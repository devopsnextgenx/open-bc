//! C ABI for the Qt picture-compare tab. Add `mod image_compare_ffi;` to the
//! crate root (the symbols are `#[no_mangle]`, so nothing needs re-exporting).
//!
//! Threading contract:
//! * All control functions are non-blocking and may be called from the GUI thread.
//! * The event callback runs on a Tokio worker thread, never the GUI thread. It
//!   must copy what it needs and return quickly (the Qt side queues the copy to
//!   the GUI thread). It must not call back into this API.
//! * After `openbc_image_compare_destroy` returns, the callback is not running
//!   and will never run again, so `user` may be freed immediately.
//! * Pointers inside an event are valid only for the duration of the callback.

use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

use openbc_core::image_compare::{
    BackendKind, CompareMode, DeltaRange, ImageCompareOptions, PixelOffset, ResizePolicy, Verdict,
    PROBE_CELLS,
};
use openbc_engine::image_compare::{
    spawn_image_compare, ImageCompareConfig, ImageCompareEvent, ImageCompareHandle, ImageFileInfo,
    LocalFsSource, Side,
};
use tokio::runtime::{Builder, Runtime};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

// ----- shared runtime ---------------------------------------------

static RUNTIME: OnceLock<Runtime> = OnceLock::new();

fn runtime() -> &'static Runtime {
    RUNTIME.get_or_init(|| {
        Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("openbc-image")
            .enable_all()
            .build()
            .expect("failed to start image compare runtime")
    })
}

// ----- C layout (mirrored in qt/image_compare_api.h) -------------------------

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OpenbcImageCompareOptions {
    pub mode: u8,          // 0 tolerance, 1 range, 2 blend
    pub tolerance: u8,
    pub range_min: u8,
    pub range_max: u8,
    pub blend_percent: u8,
    pub resize: u8,        // 0 auto, 1 native, 2 fit-left, 3 fit-right, 4 fit-larger, 5 fit-smaller
    pub compare_alpha: u8,
    pub reserved: u8,
    pub offset_x: i32,
    pub offset_y: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OpenbcStr {
    pub ptr: *const u8,
    pub len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OpenbcImageInfo {
    pub path: OpenbcStr,
    pub format: OpenbcStr,
    pub size_bytes: u64,
    pub modified_unix_ms: i64,
    pub width: u32,
    pub height: u32,
    pub bits_per_pixel: u32,
    pub has_modified: u8,
    pub reserved: [u8; 3],
}

pub const KIND_LOADING: u32 = 1;
pub const KIND_LOADED: u32 = 2;
pub const KIND_RESULT: u32 = 3;
pub const KIND_PATH_ERROR: u32 = 4;
pub const KIND_FAILED: u32 = 5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct OpenbcImageCompareEvent {
    pub kind: u32,
    pub side: u32, // 0 left, 1 right (PATH_ERROR)
    pub seq: u64,

    pub left: OpenbcImageInfo,  // LOADED
    pub right: OpenbcImageInfo, // LOADED
    pub left_rgba: *const u8,   // LOADED: left.width * left.height * 4 bytes
    pub right_rgba: *const u8,  // LOADED

    pub path: OpenbcStr,    // PATH_ERROR
    pub message: OpenbcStr, // PATH_ERROR, FAILED

    pub options: OpenbcImageCompareOptions, // RESULT (options actually used)
    pub verdict: u32,                       // RESULT: 0 binary-same, 1 pixels-same, 2 similar, 3 different
    pub backend: u32,                       // RESULT: 0 cpu, 1 gpu
    pub binary_identical: u8,               // LOADED, RESULT
    pub max_delta: u8,
    pub has_overlap: u8,
    pub reserved: u8,
    pub overlay_width: u32,
    pub overlay_height: u32,
    pub overlay_rgba: *const u8,
    pub overlap_x0: u32,
    pub overlap_y0: u32,
    pub overlap_x1: u32,
    pub overlap_y1: u32,
    pub right_width: u32, // RESULT: right image size after the resize policy
    pub right_height: u32,
    pub compared_pixels: u64,
    pub identical_pixels: u64,
    pub similar_pixels: u64,
    pub different_pixels: u64,
    pub sum_delta: u64,
}

#[repr(C)]
pub struct OpenbcPixelProbe {
    pub x: i32,
    pub y: i32,
    pub left_present: [u8; PROBE_CELLS],
    pub right_present: [u8; PROBE_CELLS],
    pub left_rgba: [u8; PROBE_CELLS * 4],
    pub right_rgba: [u8; PROBE_CELLS * 4],
}

pub type OpenbcImageCompareCallback = extern "C" fn(user: *mut c_void, event: *const OpenbcImageCompareEvent);

// ----- conversions -------------------------------------------------------------

fn options_from_c(o: &OpenbcImageCompareOptions) -> ImageCompareOptions {
    ImageCompareOptions {
        mode: match o.mode {
            1 => CompareMode::Range,
            2 => CompareMode::Blend,
            _ => CompareMode::Tolerance,
        },
        tolerance: o.tolerance,
        range: DeltaRange { min: o.range_min, max: o.range_max },
        blend_percent: o.blend_percent,
        offset: PixelOffset { x: o.offset_x, y: o.offset_y },
        resize: match o.resize {
            1 => ResizePolicy::Native,
            2 => ResizePolicy::FitToLeft,
            3 => ResizePolicy::FitToRight,
            4 => ResizePolicy::FitToLarger,
            5 => ResizePolicy::FitToSmaller,
            _ => ResizePolicy::Auto,
        },
        compare_alpha: o.compare_alpha != 0,
    }
    .sanitized()
}

fn options_to_c(o: &ImageCompareOptions) -> OpenbcImageCompareOptions {
    OpenbcImageCompareOptions {
        mode: match o.mode {
            CompareMode::Tolerance => 0,
            CompareMode::Range => 1,
            CompareMode::Blend => 2,
        },
        tolerance: o.tolerance,
        range_min: o.range.min,
        range_max: o.range.max,
        blend_percent: o.blend_percent,
        resize: match o.resize {
            ResizePolicy::Auto => 0,
            ResizePolicy::Native => 1,
            ResizePolicy::FitToLeft => 2,
            ResizePolicy::FitToRight => 3,
            ResizePolicy::FitToLarger => 4,
            ResizePolicy::FitToSmaller => 5,
        },
        compare_alpha: o.compare_alpha as u8,
        reserved: 0,
        offset_x: o.offset.x,
        offset_y: o.offset.y,
    }
}

fn str_of(s: &str) -> OpenbcStr {
    OpenbcStr { ptr: s.as_ptr(), len: s.len() }
}

fn info_of(i: &ImageFileInfo) -> OpenbcImageInfo {
    OpenbcImageInfo {
        path: str_of(&i.path),
        format: str_of(i.format.as_deref().unwrap_or("")),
        size_bytes: i.size_bytes,
        modified_unix_ms: i.modified_unix_ms.unwrap_or(0),
        width: i.width,
        height: i.height,
        bits_per_pixel: i.bits_per_pixel as u32,
        has_modified: i.modified_unix_ms.is_some() as u8,
        reserved: [0; 3],
    }
}

fn blank_event(kind: u32, seq: u64) -> OpenbcImageCompareEvent {
    // SAFETY: every field is an integer, a raw pointer or a struct of those;
    // all-zero (null pointers, zero lengths) is a valid value for all of them.
    let mut e: OpenbcImageCompareEvent = unsafe { std::mem::zeroed() };
    e.kind = kind;
    e.seq = seq;
    e
}

/// Build the C event on the stack and run `f` while all borrowed pointers are alive.
fn with_c_event(event: &ImageCompareEvent, f: impl FnOnce(&OpenbcImageCompareEvent)) {
    match event {
        ImageCompareEvent::Loading { seq } => f(&blank_event(KIND_LOADING, *seq)),
        ImageCompareEvent::Loaded { seq, left, right, left_image, right_image, binary_identical } => {
            let mut e = blank_event(KIND_LOADED, *seq);
            e.left = info_of(left);
            e.right = info_of(right);
            e.left_rgba = left_image.as_bytes().as_ptr();
            e.right_rgba = right_image.as_bytes().as_ptr();
            e.binary_identical = *binary_identical as u8;
            f(&e)
        }
        ImageCompareEvent::Result { seq, output, binary_identical } => {
            let mut e = blank_event(KIND_RESULT, *seq);
            e.options = options_to_c(&output.options);
            e.verdict = match output.verdict {
                Verdict::BinarySame => 0,
                Verdict::PixelsSame => 1,
                Verdict::Similar => 2,
                Verdict::Different => 3,
            };
            e.backend = match output.backend {
                BackendKind::Cpu => 0,
                BackendKind::Gpu => 1,
            };
            e.binary_identical = *binary_identical as u8;
            e.max_delta = output.stats.max_delta;
            e.overlay_width = output.overlay.width();
            e.overlay_height = output.overlay.height();
            e.overlay_rgba = output.overlay.as_bytes().as_ptr();
            if let Some(r) = output.overlap {
                e.has_overlap = 1;
                (e.overlap_x0, e.overlap_y0, e.overlap_x1, e.overlap_y1) = (r.x0, r.y0, r.x1, r.y1);
            }
            (e.right_width, e.right_height) = output.right_size;
            e.compared_pixels = output.stats.compared_pixels;
            e.identical_pixels = output.stats.identical_pixels;
            e.similar_pixels = output.stats.similar_pixels;
            e.different_pixels = output.stats.different_pixels;
            e.sum_delta = output.stats.sum_delta;
            f(&e)
        }
        ImageCompareEvent::PathError { seq, side, path, message } => {
            let mut e = blank_event(KIND_PATH_ERROR, *seq);
            e.side = (*side == Side::Right) as u32;
            e.path = str_of(path);
            e.message = str_of(message);
            f(&e)
        }
        ImageCompareEvent::Failed { seq, message } => {
            let mut e = blank_event(KIND_FAILED, *seq);
            e.message = str_of(message);
            f(&e)
        }
    }
}

// ----- handle ------------------------------------------------------------------

pub struct OpenbcImageCompare {
    handle: ImageCompareHandle,
    /// `true` once destroyed. The bridge holds this lock while invoking the
    /// callback, so `destroy` can wait out an in-flight call.
    gate: Arc<Mutex<bool>>,
    bridge: JoinHandle<()>,
}

/// Create a session. Returns null if `callback` is null.
///
/// # Safety
/// `options` must be null or point to a valid options struct. `user` is passed
/// back verbatim to `callback` and must stay valid until `destroy` returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_create(
    callback: Option<OpenbcImageCompareCallback>,
    user: *mut c_void,
    options: *const OpenbcImageCompareOptions,
) -> *mut OpenbcImageCompare {
    let Some(callback) = callback else { return std::ptr::null_mut() };
    let initial = if options.is_null() {
        ImageCompareOptions::default()
    } else {
        options_from_c(unsafe { &*options })
    };
    let rt = runtime();
    // Small bound: events can carry multi-megabyte overlays; backpressure is intended.
    let (tx, mut rx) = mpsc::channel::<ImageCompareEvent>(4);
    let gate = Arc::new(Mutex::new(false));
    let bridge_gate = gate.clone();
    let user_addr = user as usize;

    let bridge = rt.spawn(async move {
        while let Some(event) = rx.recv().await {
            let destroyed = bridge_gate.lock().unwrap_or_else(|p| p.into_inner());
            if *destroyed {
                break;
            }
            with_c_event(&event, |c| callback(user_addr as *mut c_void, c));
        }
    });

    let handle = spawn_image_compare(
        rt.handle(),
        Arc::new(LocalFsSource),
        tx,
        initial,
        ImageCompareConfig::default(),
    );
    Box::into_raw(Box::new(OpenbcImageCompare { handle, gate, bridge }))
}

/// # Safety
/// `h` must come from `openbc_image_compare_create` and not be destroyed yet.
/// After this returns the callback is guaranteed not to be running.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_destroy(h: *mut OpenbcImageCompare) {
    if h.is_null() {
        return;
    }
    let session = unsafe { Box::from_raw(h) };
    *session.gate.lock().unwrap_or_else(|p| p.into_inner()) = true;
    session.handle.close();
    session.bridge.abort();
}

unsafe fn utf8(ptr: *const u8, len: usize) -> String {
    if ptr.is_null() || len == 0 {
        return String::new();
    }
    String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(ptr, len) }).into_owned()
}

/// Returns 0 on success, 1 if the command queue is full, 2 if the session ended.
fn code(r: Result<(), openbc_engine::image_compare::HandleError>) -> i32 {
    match r {
        Ok(()) => 0,
        Err(openbc_engine::image_compare::HandleError::Busy) => 1,
        Err(openbc_engine::image_compare::HandleError::Closed) => 2,
    }
}

/// # Safety
/// `h` must be live; string pointers must be valid for their lengths (UTF-8).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_open(
    h: *mut OpenbcImageCompare,
    left: *const u8,
    left_len: usize,
    right: *const u8,
    right_len: usize,
) -> i32 {
    let Some(s) = (unsafe { h.as_ref() }) else { return 2 };
    code(s.handle.open(unsafe { utf8(left, left_len) }, unsafe { utf8(right, right_len) }))
}

/// # Safety
/// `h` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_reload(h: *mut OpenbcImageCompare) -> i32 {
    match unsafe { h.as_ref() } {
        Some(s) => code(s.handle.reload()),
        None => 2,
    }
}

/// # Safety
/// `h` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_swap(h: *mut OpenbcImageCompare) -> i32 {
    match unsafe { h.as_ref() } {
        Some(s) => code(s.handle.swap()),
        None => 2,
    }
}

/// # Safety
/// `h` must be live and `options` valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_set_options(
    h: *mut OpenbcImageCompare,
    options: *const OpenbcImageCompareOptions,
) {
    if let (Some(s), Some(o)) = (unsafe { h.as_ref() }, unsafe { options.as_ref() }) {
        s.handle.set_options(options_from_c(o));
    }
}

/// Fill `out` with the 9x9 neighbourhoods at canvas coordinate (x, y).
/// Returns 1 if a compare result is available, 0 otherwise.
///
/// # Safety
/// `h` must be live and `out` valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn openbc_image_compare_probe(
    h: *mut OpenbcImageCompare,
    x: i32,
    y: i32,
    out: *mut OpenbcPixelProbe,
) -> i32 {
    let (Some(s), Some(out)) = (unsafe { h.as_ref() }, unsafe { out.as_mut() }) else { return 0 };
    let Some(p) = s.handle.probe(x, y) else { return 0 };
    out.x = x;
    out.y = y;
    for i in 0..PROBE_CELLS {
        for (cells, present, rgba) in [
            (&p.left, &mut out.left_present, &mut out.left_rgba),
            (&p.right, &mut out.right_present, &mut out.right_rgba),
        ] {
            match cells[i] {
                Some(px) => {
                    present[i] = 1;
                    rgba[i * 4..i * 4 + 4].copy_from_slice(&px);
                }
                None => {
                    present[i] = 0;
                    rgba[i * 4..i * 4 + 4].fill(0);
                }
            }
        }
    }
    1
}

#[cfg(test)]
mod layout_tests {
    use super::*;
    use std::mem::{offset_of, size_of};

    /// These numbers are what `g++` reports for qt/image_compare_api.h on a
    /// 64-bit target. If either side changes a field, this test (and the
    /// static_asserts in the header) must be updated together.
    #[test]
    fn matches_cpp_header_layout() {
        assert_eq!(size_of::<OpenbcImageCompareOptions>(), 16);
        assert_eq!(size_of::<OpenbcStr>(), 16);
        assert_eq!(size_of::<OpenbcImageInfo>(), 64);
        assert_eq!(size_of::<OpenbcImageCompareEvent>(), 304);
        assert_eq!(size_of::<OpenbcPixelProbe>(), 820);
        assert_eq!(offset_of!(OpenbcImageCompareEvent, options), 192);
        assert_eq!(offset_of!(OpenbcImageCompareEvent, overlay_rgba), 232);
        assert_eq!(offset_of!(OpenbcImageCompareEvent, compared_pixels), 264);
    }

    #[test]
    fn option_conversion_round_trips_and_sanitizes() {
        let c = OpenbcImageCompareOptions {
            mode: 2, tolerance: 7, range_min: 200, range_max: 10, blend_percent: 250,
            resize: 4, compare_alpha: 1, reserved: 0, offset_x: -3, offset_y: 9,
        };
        let o = options_from_c(&c);
        assert_eq!((o.range.min, o.range.max, o.blend_percent), (10, 200, 100)); // swapped + clamped
        let back = options_to_c(&o);
        assert_eq!((back.mode, back.resize, back.offset_x, back.offset_y), (2, 4, -3, 9));
    }
}

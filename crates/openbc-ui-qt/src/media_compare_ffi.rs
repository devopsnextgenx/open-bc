//! C ABI for the Media Compare tab (container/tag metadata only). See `ffi_common`.

use std::ffi::{c_char, c_void, CStr};
use std::sync::Arc;

use openbc_engine::media_compare::{spawn_media_compare, CoverEdit, MediaCommand, MediaCompareHandle, MediaEvent, MediaSave, MediaView, TagEdit};
use openbc_engine::media_compare::is_editable;

use crate::ffi_common::*;

pub struct MediaSession {
    handle: MediaCompareHandle,
    sink: Sink,
}

type ViewHandle = Arc<MediaView>;

/// # Safety
/// See `openbc_table_session_create`.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_session_create(left: *const c_char, right: *const c_char, callback: EventCallback, user: *mut c_void) -> *mut MediaSession {
    let sink = Sink::new(callback, user);
    let (handle, mut events) = spawn_media_compare(runtime().handle(), local_source(), path_arg(left), path_arg(right));
    let pump = sink.clone();
    std::thread::Builder::new()
        .name("openbc-media-events".into())
        .spawn(move || {
            while let Some(event) = events.blocking_recv() {
                match event {
                    MediaEvent::Status(m) => pump.message(EV_STATUS, &m),
                    MediaEvent::Result(v) => pump.handle(EV_RESULT, Box::new(v)),
                }
            }
        })
        .ok();
    Box::into_raw(Box::new(MediaSession { handle, sink }))
}

/// # Safety
/// `s` from create, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_session_destroy(s: *mut MediaSession) {
    if let Some(session) = (!s.is_null()).then(|| Box::from_raw(s)) {
        session.sink.close();
    }
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_reload(s: *const MediaSession) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(MediaCommand::Reload))
}

/// # Safety
/// `v` from a result event, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_view_destroy(v: *mut ViewHandle) {
    if !v.is_null() {
        drop(Box::from_raw(v));
    }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_view_rows(v: *const ViewHandle) -> usize {
    handle_ref!(v, 0).rows.len()
}

/// `field`: 0 group, 1 key, 2 left value, 3 right value (null when absent on that side).
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_row_text(v: *const ViewHandle, row: usize, field: u8, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    let text = view.rows.get(row).and_then(|r| match field {
        0 => Some(r.group.as_str()),
        1 => Some(r.key.as_str()),
        2 => r.left.as_deref(),
        _ => r.right.as_deref(),
    });
    out_opt_str(text, len)
}

/// bit0 values differ, bit1 informational (excluded from difference counts), bit2 editable tag row.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_row_flags(v: *const ViewHandle, row: usize) -> u32 {
    handle_ref!(v, 0).rows.get(row).map_or(0, |r| u32::from(r.differs) | u32::from(r.informational) << 1 | u32::from(is_editable(&r.group, &r.key)) << 2)
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_side_error(v: *const ViewHandle, side: u8, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(view.errors[usize::from(side != 0)].as_deref(), len)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_side_present(v: *const ViewHandle, side: u8) -> u8 {
    u8::from(handle_ref!(v, 0).present[usize::from(side != 0)])
}

/// Embedded cover art of one side (raw jpeg/png/... bytes, not NUL-terminated). Returns null with
/// `*len == 0` when the side has no picture. The pointer stays valid until the view is destroyed.
///
/// # Safety
/// `v` valid or null; `len` writable or null.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_side_cover(v: *const ViewHandle, side: u8, len: *mut usize) -> *const u8 {
    let write_len = |n: usize| {
        if !len.is_null() {
            *len = n;
        }
    };
    if v.is_null() {
        write_len(0);
        return std::ptr::null();
    }
    let view: &MediaView = &**v;
    match view.covers[usize::from(side != 0)].as_deref() {
        Some(bytes) if !bytes.is_empty() => {
            write_len(bytes.len());
            bytes.as_ptr()
        }
        _ => {
            write_len(0);
            std::ptr::null()
        }
    }
}
unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() { String::new() } else { CStr::from_ptr(p).to_string_lossy().into_owned() }
}

/// Writes edited tags (and optionally the cover) of one side, then the engine re-reads both files
/// and emits a normal Result event (a failure shows up as `errors[side]` = "Save failed: ...").
/// `groups`/`keys`/`values` are parallel arrays of `count` NUL-terminated UTF-8 strings; a null
/// `values[i]` removes that tag. `cover_mode`: 0 keep, 1 replace with `cover[..cover_len]`, 2 remove.
/// Returns 0 when the command could not be queued.
///
/// # Safety
/// `s` valid; the arrays hold `count` valid entries (values may be null); `cover` is valid for
/// `cover_len` bytes when `cover_mode == 1`.
#[no_mangle]
pub unsafe extern "C" fn openbc_media_save(
    s: *const MediaSession,
    side: u8,
    count: usize,
    groups: *const *const c_char,
    keys: *const *const c_char,
    values: *const *const c_char,
    cover_mode: u8,
    cover: *const u8,
    cover_len: usize,
) -> u8 {
    let session = handle_ref!(s, 0);
    let edits = (0..count)
        .map(|i| TagEdit {
            group: cstr(*groups.add(i)),
            key: cstr(*keys.add(i)),
            value: { let v = *values.add(i); if v.is_null() { None } else { Some(cstr(v)) } },
        })
        .collect();
    let cover = match cover_mode {
        1 if !cover.is_null() && cover_len > 0 => CoverEdit::Replace(std::slice::from_raw_parts(cover, cover_len).to_vec()),
        2 => CoverEdit::Remove,
        _ => CoverEdit::Keep,
    };
    u8::from(session.handle.send(MediaCommand::Save(MediaSave { side: usize::from(side != 0), edits, cover })))
}
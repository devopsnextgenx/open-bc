//! C ABI for the 3D Compare tab (`model3d_compare_api.h` is the matching header).
//!
//! The callback runs on a dedicated Rust thread. Every pointer inside an event is valid ONLY
//! for the duration of the call: the Qt side deep-copies what it keeps and re-posts to the GUI
//! thread. `dev_token` / `changes_token` change only when the corresponding data changed, so
//! the UI can skip copying large arrays on tolerance / range ticks.
//!
//! All compute access goes through `openbc-engine`; this crate does not depend on
//! `openbc-compute`.
use openbc_core::model3d::*;
use openbc_engine::model3d_compare::*;
use std::ffi::c_void;
use std::sync::Arc;
use tokio::sync::mpsc;

#[repr(C)] #[derive(Clone, Copy)] pub struct Openbc3dStr { pub ptr: *const u8, pub len: usize }
impl Openbc3dStr { fn of(s: &str) -> Self { Self { ptr: s.as_ptr(), len: s.len() } } }

#[repr(C)] #[derive(Clone, Copy)]
pub struct Openbc3dOptions {
    pub tolerance: f32, pub range: f32, pub offset: [f32; 3],
    pub alignment: u8, pub prefer_gpu: u8, pub compute_changes: u8, pub _pad: u8,
}
#[repr(C)] #[derive(Clone, Copy)]
pub struct Openbc3dMesh { pub positions: *const f32, pub vertex_count: usize, pub indices: *const u32, pub index_count: usize, pub edges: *const u32, pub edge_count: usize }
#[repr(C)] #[derive(Clone, Copy)]
pub struct Openbc3dFileInfo { pub path: Openbc3dStr, pub format: Openbc3dStr, pub size_bytes: u64, pub modified_unix_ms: i64, pub has_modified: u8 }
#[repr(C)] #[derive(Clone, Copy)]
pub struct Openbc3dStats { pub triangles: u64, pub vertices: u64, pub area: f64, pub volume: f64, pub size_bytes: u64, pub bmin: [f32; 3], pub bmax: [f32; 3], pub watertight: u8 }
#[repr(C)] #[derive(Clone, Copy)]
pub struct Openbc3dSide { pub info: Openbc3dFileInfo, pub stats: Openbc3dStats, pub mesh: Openbc3dMesh }
#[repr(C)] #[derive(Clone, Copy)]
pub struct Openbc3dRegion { pub side: u8, pub triangles: u32, pub area: f64, pub max_deviation: f32, pub bmin: [f32; 3], pub bmax: [f32; 3] }

pub const K_LOADING: u32 = 0; pub const K_LOADED: u32 = 1; pub const K_RESULT: u32 = 2;
pub const K_PATH_ERROR: u32 = 3; pub const K_FAILED: u32 = 4; pub const K_NOTICE: u32 = 5;

#[repr(C)]
pub struct Openbc3dEvent {
    pub kind: u32, pub side: u32,
    pub path: Openbc3dStr, pub message: Openbc3dStr,
    // LOADED
    pub left_present: u8, pub right_present: u8, pub left: Openbc3dSide, pub right: Openbc3dSide,
    // RESULT
    pub options: Openbc3dOptions, pub backend: u32, pub display_shift: [f32; 3],
    pub b_positions: *const f32, pub b_vertex_count: usize,
    pub dev_a: *const f32, pub dev_a_count: usize, pub dev_b: *const f32, pub dev_b_count: usize, pub dev_token: u64,
    pub max_deviation: f32, pub mean_deviation: f32, pub surface_changed: f32, pub tolerance: f32, pub range: f32,
    pub histogram: *const f32, pub histogram_count: usize,
    pub regions: *const Openbc3dRegion, pub region_count: usize, pub regions_total: u32,
    pub has_changes: u8, pub changes_truncated: u8, pub changes_token: u64,
    pub removed_mm3: f64, pub added_mm3: f64, pub cell_size: f32,
    pub removed: Openbc3dMesh, pub added: Openbc3dMesh,
    pub align_ms: f32, pub deviation_ms: f32, pub analysis_ms: f32, pub changes_ms: f32,
}

#[repr(C)] pub struct Openbc3dProbe { pub point: [f32; 3], pub deviation: f32, pub side: u8, pub triangle: u32 }

pub type Openbc3dCallback = extern "C" fn(user: *mut c_void, ev: *const Openbc3dEvent);

pub struct Openbc3dCompare {
    rt: tokio::runtime::Runtime,
    handle: Option<Model3dCompareHandle>,
    forwarder: Option<std::thread::JoinHandle<()>>,
    /// Opaque engine-owned environment (GPU adapter info, registered formats).
    env: Model3dEnv,
}

struct SendPtr(*mut c_void);
unsafe impl Send for SendPtr {}

fn to_opts(o: &Openbc3dOptions) -> Model3dCompareOptions {
    Model3dCompareOptions {
        tolerance_mm: o.tolerance, range_mm: o.range, alignment: AlignmentMode::from_u8(o.alignment), offset: o.offset,
        prefer_gpu: o.prefer_gpu != 0, compute_changes: o.compute_changes != 0,
    }
}
fn from_opts(o: &Model3dCompareOptions) -> Openbc3dOptions {
    Openbc3dOptions { tolerance: o.tolerance_mm, range: o.range_mm, offset: o.offset, alignment: o.alignment as u8, prefer_gpu: o.prefer_gpu as u8, compute_changes: o.compute_changes as u8, _pad: 0 }
}
fn mesh_ref(m: &Mesh, edges: &[u32]) -> Openbc3dMesh {
    Openbc3dMesh { positions: m.positions.as_ptr() as *const f32, vertex_count: m.positions.len(), indices: m.indices.as_ptr(), index_count: m.indices.len(), edges: edges.as_ptr(), edge_count: edges.len() }
}
fn side_ref(p: &LoadedSidePayload) -> Openbc3dSide {
    Openbc3dSide {
        info: Openbc3dFileInfo { path: Openbc3dStr::of(&p.info.path), format: Openbc3dStr::of(&p.info.format), size_bytes: p.info.size_bytes, modified_unix_ms: p.info.modified_unix_ms.unwrap_or(0), has_modified: p.info.modified_unix_ms.is_some() as u8 },
        stats: Openbc3dStats { triangles: p.stats.triangles, vertices: p.stats.vertices, area: p.stats.area, volume: p.stats.volume, size_bytes: p.stats.size_bytes, bmin: p.stats.bbox.min, bmax: p.stats.bbox.max, watertight: p.stats.watertight as u8 },
        mesh: mesh_ref(&p.mesh, &p.edges),
    }
}

fn dispatch(cb: Openbc3dCallback, user: *mut c_void, ev: Model3dEvent) {
    // SAFETY: Openbc3dEvent is plain data (integers, floats, raw pointers); all-zero is valid.
    let mut c: Openbc3dEvent = unsafe { std::mem::zeroed() };
    let (mut path, mut message) = (String::new(), String::new());
    let regions: Vec<Openbc3dRegion>;
    let out_holder: Option<Box<Model3dOutput>>;
    match ev {
        Model3dEvent::Loading => c.kind = K_LOADING,
        Model3dEvent::Loaded { left, right } => {
            c.kind = K_LOADED;
            if let Some(l) = &left { c.left_present = 1; c.left = side_ref(l); }
            if let Some(r) = &right { c.right_present = 1; c.right = side_ref(r); }
            cb(user, &c);
            return;
        }
        Model3dEvent::PathError { side, path: p, message: m } => { c.kind = K_PATH_ERROR; c.side = side as u32; path = p; message = m; }
        Model3dEvent::Notice(m) => { c.kind = K_NOTICE; message = m; }
        Model3dEvent::Failed(m) => { c.kind = K_FAILED; message = m; }
        Model3dEvent::Result(out) => {
            c.kind = K_RESULT;
            c.options = from_opts(&out.options);
            c.backend = out.backend as u32;
            c.display_shift = out.display_shift;
            if let Some(b) = &out.b_positions { c.b_positions = b.as_ptr() as *const f32; c.b_vertex_count = b.len(); }
            c.dev_a = out.dev_a.as_ptr(); c.dev_a_count = out.dev_a.len();
            c.dev_b = out.dev_b.as_ptr(); c.dev_b_count = out.dev_b.len();
            c.dev_token = Arc::as_ptr(&out.dev_a) as usize as u64;
            c.max_deviation = out.summary.max_deviation; c.mean_deviation = out.summary.mean_deviation;
            c.surface_changed = out.summary.surface_changed; c.tolerance = out.summary.tolerance; c.range = out.summary.range;
            c.histogram = out.histogram.as_ptr(); c.histogram_count = out.histogram.len();
            regions = out.regions.iter().map(|r| Openbc3dRegion { side: r.side, triangles: r.triangles, area: r.area, max_deviation: r.max_deviation, bmin: r.bbox.min, bmax: r.bbox.max }).collect();
            c.regions = regions.as_ptr(); c.region_count = regions.len(); c.regions_total = out.regions_total;
            if let Some(ch) = &out.changes {
                c.has_changes = 1; c.changes_truncated = ch.truncated as u8; c.changes_token = Arc::as_ptr(ch) as usize as u64;
                c.removed_mm3 = ch.removed_mm3; c.added_mm3 = ch.added_mm3; c.cell_size = ch.cell_size_mm;
                c.removed = mesh_ref(&ch.removed, &[]); c.added = mesh_ref(&ch.added, &[]);
            }
            c.align_ms = out.timings.align_ms; c.deviation_ms = out.timings.deviation_ms;
            c.analysis_ms = out.timings.analysis_ms; c.changes_ms = out.timings.changes_ms;
            out_holder = Some(out);
            c.path = Openbc3dStr::of(&path); c.message = Openbc3dStr::of(&message);
            cb(user, &c);
            drop(out_holder);
            return;
        }
    }
    c.path = Openbc3dStr::of(&path);
    c.message = Openbc3dStr::of(&message);
    cb(user, &c);
}

#[no_mangle]
pub extern "C" fn openbc_model3d_create(cb: Openbc3dCallback, user: *mut c_void, options: *const Openbc3dOptions) -> *mut Openbc3dCompare {
    let env = Model3dEnv::shared();
    let Ok(rt) = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build() else { return std::ptr::null_mut() };
    let (tx, mut rx) = mpsc::channel(EVENT_CAPACITY);
    let initial = if options.is_null() { Model3dCompareOptions::default() } else { to_opts(unsafe { &*options }) };
    let handle = spawn(
        rt.handle(),
        Model3dCompareConfig { env: env.clone(), source: Arc::new(LocalFs { max_file_bytes: 2 << 30 }) },
        tx, initial,
    );
    let user = SendPtr(user);
    let forwarder = std::thread::Builder::new().name("openbc-3d-events".into()).spawn(move || {
        let user = user;
        while let Some(ev) = rx.blocking_recv() { dispatch(cb, user.0, ev); }
    }).ok();
    Box::into_raw(Box::new(Openbc3dCompare { rt, handle: Some(handle), forwarder, env }))
}

/// Blocks until the actor and the forwarder thread ended: no callback runs after this returns.
#[no_mangle]
pub extern "C" fn openbc_model3d_destroy(h: *mut Openbc3dCompare) {
    if h.is_null() { return; }
    let mut me = unsafe { Box::from_raw(h) };
    if let Some(mut handle) = me.handle.take() { me.rt.block_on(handle.shutdown()); }
    if let Some(f) = me.forwarder.take() { let _ = f.join(); }
}

fn with<R>(h: *mut Openbc3dCompare, f: impl FnOnce(&Model3dCompareHandle) -> R) -> Option<R> {
    unsafe { h.as_ref() }.and_then(|m| m.handle.as_ref()).map(f)
}
unsafe fn s(p: *const u8, n: usize) -> String { String::from_utf8_lossy(std::slice::from_raw_parts(p, n)).into_owned() }

#[no_mangle] pub extern "C" fn openbc_model3d_set_options(h: *mut Openbc3dCompare, o: *const Openbc3dOptions) {
    if o.is_null() { return; }
    let o = to_opts(unsafe { &*o });
    with(h, |m| m.set_options(o));
}
#[no_mangle] pub extern "C" fn openbc_model3d_open(h: *mut Openbc3dCompare, l: *const u8, ln: usize, r: *const u8, rn: usize) {
    let (l, r) = unsafe { (s(l, ln), s(r, rn)) };
    with(h, |m| m.open(l, r));
}
#[no_mangle] pub extern "C" fn openbc_model3d_reload(h: *mut Openbc3dCompare) { with(h, |m| m.reload()); }
#[no_mangle] pub extern "C" fn openbc_model3d_swap(h: *mut Openbc3dCompare) { with(h, |m| m.swap()); }
/// 0 = refine alignment (ICP), 1 = align parts individually, 2 = reset refinement.
#[no_mangle] pub extern "C" fn openbc_model3d_align(h: *mut Openbc3dCompare, cmd: u32) {
    let c = match cmd { 0 => AlignCommand::RefineIcp, 1 => AlignCommand::AlignParts, _ => AlignCommand::ResetRefinement };
    with(h, |m| m.align(c));
}
/// `origin` and `dir` are 3 floats each in display space; `dir` must be normalised.
#[no_mangle] pub extern "C" fn openbc_model3d_probe(h: *mut Openbc3dCompare, side: u8, origin: *const f32, dir: *const f32, out: *mut Openbc3dProbe) -> u8 {
    if origin.is_null() || dir.is_null() || out.is_null() { return 0; }
    let (o, d) = unsafe { ([*origin, *origin.add(1), *origin.add(2)], [*dir, *dir.add(1), *dir.add(2)]) };
    match with(h, |m| m.probe(side, o, d)).flatten() {
        Some(p) => { unsafe { *out = Openbc3dProbe { point: p.point, deviation: p.deviation, side: p.side, triangle: p.triangle }; } 1 }
        None => 0,
    }
}
fn copy_str(text: &str, buf: *mut u8, cap: usize) -> usize {
    let n = text.len().min(cap);
    if !buf.is_null() { unsafe { std::ptr::copy_nonoverlapping(text.as_ptr(), buf, n); } }
    text.len()
}
/// Name of the GPU adapter in use ("" when CPU only). Returns the full length.
#[no_mangle] pub extern "C" fn openbc_model3d_gpu_name(h: *mut Openbc3dCompare, buf: *mut u8, cap: usize) -> usize {
    let name = unsafe { h.as_ref() }.and_then(|m| m.env.gpu_name()).unwrap_or_default();
    copy_str(&name, buf, cap)
}
/// Registered extensions, ';'-separated ("stl;obj;ply;..."), so the Qt file dialog follows the registry.
#[no_mangle] pub extern "C" fn openbc_model3d_extensions(h: *mut Openbc3dCompare, buf: *mut u8, cap: usize) -> usize {
    let list = unsafe { h.as_ref() }.map(|m| m.env.extensions()).unwrap_or_default();
    copy_str(&list, buf, cap)
}
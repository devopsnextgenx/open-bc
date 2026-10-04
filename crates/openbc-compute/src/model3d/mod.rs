//! 3D model compare in the compute layer.
//!
//! `Session` owns everything a compare tab needs between option changes and re-runs only the
//! stages an option invalidates:
//!
//! ```text
//! load (parse A || parse B, weld, stats, components, edges, BVH)      on file change
//!  -> align   (mode / offset / ICP / per-part, rebuild B's BVH)       on alignment change
//!   -> deviation fields (A->B and B->A signed distance, GPU or CPU)   on alignment change
//!    -> analysis (summary, histogram, regions)                        on tolerance / range
//!    -> changes  (voxel removed / added volumes + boundary meshes)    on alignment / tolerance
//! ```
//! Threading: everything here is synchronous and CPU-bound; callers run it on Rayon (the engine
//! does) and cancel cooperatively through `CancelToken`. Inside, Rayon parallelises parsing
//! (per chunk), BVH construction (fork-join), distance queries (per chunk), voxelisation (per
//! row) and analysis. The GPU path is used only through `Backends`.
pub mod align;
pub mod analysis;
pub mod backend;
pub mod bvh;
pub mod deviation;
pub mod formats;
#[cfg(feature = "gpu")]
pub mod gpu;
pub mod volume;
#[cfg(test)]
mod tests;

pub use backend::{Backends, DeviationExecutor, GpuError, GpuPolicy, GPU_DEFAULT_BYTES, GPU_FLOOR_BYTES};
pub use formats::builtin_registry;

use align::{AlignState, PartAlign};
use bvh::Bvh;
use deviation::{check, sample_points, split, Deviation};
use openbc_core::model3d::mesh::*;
use openbc_core::model3d::*;
use std::sync::Arc;
use std::time::Instant;

#[derive(Debug, Clone)]
pub enum ComputeError { Cancelled, NotLoaded, Message(String) }
impl std::fmt::Display for ComputeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self { Self::Cancelled => write!(f, "cancelled"), Self::NotLoaded => write!(f, "both models must be loaded"), Self::Message(m) => write!(f, "{m}") }
    }
}
impl std::error::Error for ComputeError {}

/// File bytes plus metadata, produced by the engine's async I/O.
pub struct RawModel { pub info: FileInfo, pub bytes: Vec<u8> }

pub struct Side {
    pub info: FileInfo,
    pub mesh: Arc<Mesh>,
    pub edges: Arc<Vec<u32>>,
    pub stats: MeshStats,
    pub comps: Arc<Components>,
    pub bvh: Arc<Bvh>,
}
impl Side {
    pub fn payload(&self) -> LoadedSidePayload {
        LoadedSidePayload { info: self.info.clone(), stats: self.stats.clone(), mesh: self.mesh.clone(), edges: self.edges.clone() }
    }
}

pub struct LoadOutcome { pub left: Result<Arc<Side>, String>, pub right: Result<Arc<Side>, String> }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AlignCommand { RefineIcp, AlignParts, ResetRefinement }

/// What the UI's synchronous hover / click probe needs; swapped in after every pass.
pub struct ProbeData { pub a: Arc<Bvh>, pub b: Arc<Bvh>, pub display_shift: V3 }

#[derive(Clone)]
struct AlignedCache { version: u64, mesh: Arc<Mesh>, bvh: Arc<Bvh>, display_shift: V3 }
#[derive(Clone)]
struct FieldsCache { version: u64, a: Arc<Deviation>, b: Arc<Deviation>, backend: Backend }

pub struct Session {
    registry: Arc<LoaderRegistry>,
    backends: Arc<Backends>,
    limits: LoadLimits,
    a: Option<Arc<Side>>,
    b: Option<Arc<Side>>,
    align: AlignState,
    aligned: Option<AlignedCache>,
    fields: Option<FieldsCache>,
    occupancy: Option<(u64, Arc<volume::Occupancy>)>,
    changes: Option<(u64, u32, Arc<ChangesOutput>)>,
    sent_b_version: Option<u64>,
}

impl Session {
    pub fn new(registry: Arc<LoaderRegistry>, backends: Arc<Backends>) -> Self {
        Self {
            registry, backends, limits: LoadLimits::default(), a: None, b: None,
            align: AlignState::new(AlignmentMode::Original, [0.0; 3], 0),
            aligned: None, fields: None, occupancy: None, changes: None, sent_b_version: None,
        }
    }

    pub fn is_ready(&self) -> bool { self.a.is_some() && self.b.is_some() }
    pub fn backends(&self) -> &Backends { &self.backends }
    pub fn registry(&self) -> &LoaderRegistry { &self.registry }

    fn reset_caches(&mut self) {
        self.align = AlignState::new(self.align.mode, self.align.offset, self.align.version + 1);
        self.aligned = None; self.fields = None; self.occupancy = None; self.changes = None; self.sent_b_version = None;
    }

    // ---------------------------------------------------------------------------- load

    fn load_side(&self, raw: RawModel, cancel: &CancelToken) -> Result<Arc<Side>, String> {
        let name = std::path::Path::new(&raw.info.path).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| raw.info.path.clone());
        let loader = self.registry.find(&name, &raw.bytes[..raw.bytes.len().min(512)])
            .ok_or_else(|| format!("No loader registered for \"{name}\""))?;
        let ctx = LoadContext { file_name: &name, limits: &self.limits, cancel };
        let mesh = loader.load(&raw.bytes, &ctx).map_err(|e| e.to_string())?;
        let size = raw.info.size_bytes;
        // Independent per-model work runs concurrently; BVH build is itself fork-join parallel.
        let ((stats, comps), (edges, bvh)) = rayon::join(
            || (mesh.stats(size), mesh.components()),
            || rayon::join(|| mesh.unique_edges(), || Bvh::build(&mesh)),
        );
        let mut info = raw.info;
        info.format = loader.display_name().to_string();
        Ok(Arc::new(Side { info, mesh: Arc::new(mesh), edges: Arc::new(edges), stats, comps: Arc::new(comps), bvh: Arc::new(bvh) }))
    }

    /// Parses both models concurrently. A failure on one side never affects the other.
    pub fn load_pair(&mut self, left: Result<RawModel, String>, right: Result<RawModel, String>, cancel: &CancelToken) -> LoadOutcome {
        let me = &*self;
        let (l, r) = rayon::join(
            || left.and_then(|m| me.load_side(m, cancel)),
            || right.and_then(|m| me.load_side(m, cancel)),
        );
        self.a = l.as_ref().ok().cloned();
        self.b = r.as_ref().ok().cloned();
        self.reset_caches();
        LoadOutcome { left: l, right: r }
    }

    pub fn payloads(&self) -> (Option<LoadedSidePayload>, Option<LoadedSidePayload>) {
        (self.a.as_ref().map(|s| s.payload()), self.b.as_ref().map(|s| s.payload()))
    }

    pub fn swap(&mut self) {
        std::mem::swap(&mut self.a, &mut self.b);
        self.reset_caches();
    }

    pub fn probe_data(&self) -> Option<Arc<ProbeData>> {
        let (a, al) = (self.a.as_ref()?, self.aligned.as_ref()?);
        Some(Arc::new(ProbeData { a: a.bvh.clone(), b: al.bvh.clone(), display_shift: al.display_shift }))
    }

    // -------------------------------------------------------------------------- compare

    fn build_aligned(a: &Side, b: &Side, st: &AlignState) -> AlignedCache {
        let (mesh, display_shift) = align::apply_alignment(&b.mesh, &a.stats, &b.stats, st);
        let bvh = Bvh::build(&mesh);
        AlignedCache { version: st.version, mesh: Arc::new(mesh), bvh: Arc::new(bvh), display_shift }
    }

    fn apply_command(cmd: AlignCommand, a: &Side, b: &Side, st: &mut AlignState, cancel: &CancelToken) -> Result<Option<String>, ComputeError> {
        let mut global = st.clone();
        global.parts = None;
        let (g, _) = align::apply_alignment(&b.mesh, &a.stats, &b.stats, &global);
        let mut notice = None;
        match cmd {
            AlignCommand::ResetRefinement => { st.refine = Rigid::IDENTITY; st.parts = None; }
            AlignCommand::RefineIcp => {
                let q = sample_points(&g.positions, &g.indices);
                let pts = align::sample_stride(&q, 20_000);
                let delta = align::icp(&pts, &a.bvh, 30, cancel)?;
                st.refine = st.refine.then(&delta);
                st.parts = None;
            }
            AlignCommand::AlignParts => {
                if a.comps.count < 2 && b.comps.count < 2 {
                    notice = Some("Both models are a single connected part - nothing to align individually.".to_string());
                } else {
                    let pa: PartAlign = align::align_parts(&a.mesh, &a.comps, &g, &b.comps, cancel)?;
                    notice = Some(if pa.matched == 0 { "No matching parts were found.".to_string() } else { format!("Aligned {} matching part(s) individually.", pa.matched) });
                    st.parts = Some(Arc::new(pa));
                }
            }
        }
        st.version += 1;
        Ok(notice)
    }

    pub fn compare(&mut self, options: &Model3dCompareOptions, cmd: Option<AlignCommand>, cancel: &CancelToken) -> Result<Model3dOutput, ComputeError> {
        let opts = options.sanitized();
        let (a, b) = match (&self.a, &self.b) { (Some(a), Some(b)) => (a.clone(), b.clone()), _ => return Err(ComputeError::NotLoaded) };
        let mut timings = Timings::default();

        // ---- alignment (+ optional ICP / part command) -----------------------------------
        let t0 = Instant::now();
        let mut st = self.align.clone();
        if st.mode != opts.alignment || st.offset != opts.offset { st = AlignState::new(opts.alignment, opts.offset, st.version + 1); }
        let mut notice = None;
        if let Some(c) = cmd { notice = Self::apply_command(c, &a, &b, &mut st, cancel)?; }
        let aligned = match &self.aligned {
            Some(c) if c.version == st.version => c.clone(),
            _ => Self::build_aligned(&a, &b, &st),
        };
        check(cancel)?;
        self.align = st;
        self.aligned = Some(aligned.clone());
        timings.align_ms = t0.elapsed().as_secs_f32() * 1e3;

        // ---- deviation fields ------------------------------------------------------------
        let t1 = Instant::now();
        let fields = match &self.fields {
            Some(f) if f.version == aligned.version => f.clone(),
            _ => {
                let largest = a.info.size_bytes.max(b.info.size_bytes);
                let qa = sample_points(&a.mesh.positions, &a.mesh.indices);
                let (va, ba) = self.backends.signed_distances(&aligned.bvh, &qa, largest, opts.prefer_gpu, cancel)?;
                let qb = sample_points(&aligned.mesh.positions, &aligned.mesh.indices);
                let (vb, bb) = self.backends.signed_distances(&a.bvh, &qb, largest, opts.prefer_gpu, cancel)?;
                let backend = if ba == Backend::Gpu || bb == Backend::Gpu { Backend::Gpu } else { Backend::Cpu };
                let f = FieldsCache {
                    version: aligned.version,
                    a: Arc::new(split(va, a.mesh.positions.len())),
                    b: Arc::new(split(vb, aligned.mesh.positions.len())),
                    backend,
                };
                self.fields = Some(f.clone());
                f
            }
        };
        timings.deviation_ms = t1.elapsed().as_secs_f32() * 1e3;
        check(cancel)?;

        // ---- analysis --------------------------------------------------------------------
        let t2 = Instant::now();
        let an = analysis::analyze(
            (&a.mesh.positions, &a.mesh.indices), (&aligned.mesh.positions, &aligned.mesh.indices),
            &fields.a, &fields.b, opts.tolerance_mm, opts.range_mm,
        );
        timings.analysis_ms = t2.elapsed().as_secs_f32() * 1e3;
        check(cancel)?;

        // ---- changes (volumetric) --------------------------------------------------------
        let t3 = Instant::now();
        let changes = if opts.compute_changes {
            let tol_bits = opts.tolerance_mm.to_bits();
            match &self.changes {
                Some((v, t, c)) if *v == aligned.version && *t == tol_bits => Some(c.clone()),
                _ => {
                    let occ = match &self.occupancy {
                        Some((v, o)) if *v == aligned.version => o.clone(),
                        _ => {
                            let o = Arc::new(volume::occupancy(&a.bvh, &aligned.bvh, volume::DEFAULT_MAX_CELLS, cancel)?);
                            self.occupancy = Some((aligned.version, o.clone()));
                            o
                        }
                    };
                    let c = Arc::new(volume::changes(&occ, &a.bvh, &aligned.bvh, opts.tolerance_mm, volume::DEFAULT_MAX_QUADS, cancel)?);
                    self.changes = Some((aligned.version, tol_bits, c.clone()));
                    Some(c)
                }
            }
        } else { None };
        timings.changes_ms = t3.elapsed().as_secs_f32() * 1e3;

        let b_positions = (self.sent_b_version != Some(aligned.version)).then(|| Arc::new(aligned.mesh.positions.clone()));
        self.sent_b_version = Some(aligned.version);

        Ok(Model3dOutput {
            options: opts,
            backend: fields.backend,
            display_shift: aligned.display_shift,
            b_positions,
            dev_a: fields.a.vertex.clone(),
            dev_b: fields.b.vertex.clone(),
            summary: an.summary,
            histogram: an.histogram,
            regions: an.regions,
            regions_total: an.regions_total,
            changes,
            timings,
            notice,
        })
    }
}

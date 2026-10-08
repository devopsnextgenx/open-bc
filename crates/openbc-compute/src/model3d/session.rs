//! `Session`: the incremental compare pipeline (load -> align -> deviation -> analysis -> changes).
//!
//! Each stage keeps a cache keyed by the alignment `version`, so an option change only re-runs
//! the stages it invalidates (see the diagram in the parent module docs).
use super::align::{self, AlignState, PartAlign};
use super::analysis;
use super::backend::Backends;
use super::bvh::Bvh;
use super::deviation::{sample_points, split, Deviation};
use super::error::{check, ComputeError};
use super::volume;
use openbc_core::model3d::mesh::*;
use openbc_core::model3d::*;
use std::sync::Arc;
use std::time::Instant;

/// Bytes of a file handed to loader sniffing.
const SNIFF_BYTES: usize = 512;
/// ICP budget for the global "refine" command.
const ICP_MAX_POINTS: usize = 20_000;
const ICP_ITERATIONS: usize = 30;

/// File bytes plus metadata, produced by the engine's async I/O.
pub struct RawModel {
    pub info: FileInfo,
    pub bytes: Vec<u8>,
}

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
        LoadedSidePayload {
            info: self.info.clone(),
            stats: self.stats.clone(),
            mesh: self.mesh.clone(),
            edges: self.edges.clone(),
        }
    }
}

pub struct LoadOutcome {
    pub left: Result<Arc<Side>, String>,
    pub right: Result<Arc<Side>, String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AlignCommand {
    RefineIcp,
    AlignParts,
    ResetRefinement,
}

/// What the UI's synchronous hover / click probe needs; swapped in after every pass.
pub struct ProbeData {
    pub a: Arc<Bvh>,
    pub b: Arc<Bvh>,
    pub display_shift: V3,
}

#[derive(Clone)]
struct AlignedCache {
    version: u64,
    mesh: Arc<Mesh>,
    bvh: Arc<Bvh>,
    display_shift: V3,
}

#[derive(Clone)]
struct FieldsCache {
    version: u64,
    a: Arc<Deviation>,
    b: Arc<Deviation>,
    backend: Backend,
}

struct OccupancyCache {
    version: u64,
    grid: Arc<volume::Occupancy>,
}

struct ChangesCache {
    version: u64,
    /// `f32::to_bits` of the tolerance the volumes were filtered with.
    tolerance_bits: u32,
    output: Arc<ChangesOutput>,
}

pub struct Session {
    registry: Arc<LoaderRegistry>,
    backends: Arc<Backends>,
    limits: LoadLimits,
    a: Option<Arc<Side>>,
    b: Option<Arc<Side>>,
    align: AlignState,
    aligned: Option<AlignedCache>,
    fields: Option<FieldsCache>,
    occupancy: Option<OccupancyCache>,
    changes: Option<ChangesCache>,
    sent_b_version: Option<u64>,
}

fn elapsed_ms(since: Instant) -> f32 {
    since.elapsed().as_secs_f32() * 1e3
}

impl Session {
    pub fn new(registry: Arc<LoaderRegistry>, backends: Arc<Backends>) -> Self {
        Self {
            registry,
            backends,
            limits: LoadLimits::default(),
            a: None,
            b: None,
            align: AlignState::new(AlignmentMode::Original, [0.0; 3], 0),
            aligned: None,
            fields: None,
            occupancy: None,
            changes: None,
            sent_b_version: None,
        }
    }

    pub fn is_ready(&self) -> bool {
        self.a.is_some() && self.b.is_some()
    }

    pub fn backends(&self) -> &Backends {
        &self.backends
    }

    pub fn registry(&self) -> &LoaderRegistry {
        &self.registry
    }

    fn reset_caches(&mut self) {
        self.align = AlignState::new(self.align.mode, self.align.offset, self.align.version + 1);
        self.aligned = None;
        self.fields = None;
        self.occupancy = None;
        self.changes = None;
        self.sent_b_version = None;
    }

    // ---------------------------------------------------------------------------- load

    fn load_side(&self, raw: RawModel, cancel: &CancelToken) -> Result<Arc<Side>, String> {
        let name = std::path::Path::new(&raw.info.path)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| raw.info.path.clone());
        let head = &raw.bytes[..raw.bytes.len().min(SNIFF_BYTES)];
        let loader = self
            .registry
            .find(&name, head)
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
        Ok(Arc::new(Side {
            info,
            mesh: Arc::new(mesh),
            edges: Arc::new(edges),
            stats,
            comps: Arc::new(comps),
            bvh: Arc::new(bvh),
        }))
    }

    /// Parses both models concurrently. A failure on one side never affects the other.
    pub fn load_pair(
        &mut self,
        left: Result<RawModel, String>,
        right: Result<RawModel, String>,
        cancel: &CancelToken,
    ) -> LoadOutcome {
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

    /// Runs (or reuses) every stage for `options` and assembles the UI payload.
    pub fn compare(
        &mut self,
        options: &Model3dCompareOptions,
        cmd: Option<AlignCommand>,
        cancel: &CancelToken,
    ) -> Result<Model3dOutput, ComputeError> {
        let opts = options.sanitized();
        let (a, b) = match (&self.a, &self.b) {
            (Some(a), Some(b)) => (a.clone(), b.clone()),
            _ => return Err(ComputeError::NotLoaded),
        };
        let mut timings = Timings::default();

        let t = Instant::now();
        let (aligned, notice) = self.stage_align(&a, &b, &opts, cmd, cancel)?;
        timings.align_ms = elapsed_ms(t);

        let t = Instant::now();
        let fields = self.stage_fields(&a, &b, &aligned, opts.prefer_gpu, cancel)?;
        timings.deviation_ms = elapsed_ms(t);
        check(cancel)?;

        let t = Instant::now();
        let an = analysis::analyze(
            (&a.mesh.positions, &a.mesh.indices),
            (&aligned.mesh.positions, &aligned.mesh.indices),
            &fields.a,
            &fields.b,
            opts.tolerance_mm,
            opts.range_mm,
        );
        timings.analysis_ms = elapsed_ms(t);
        check(cancel)?;

        let t = Instant::now();
        let changes = if opts.compute_changes {
            Some(self.stage_changes(&a, &aligned, opts.tolerance_mm, cancel)?)
        } else {
            None
        };
        timings.changes_ms = elapsed_ms(t);

        let b_positions = (self.sent_b_version != Some(aligned.version))
            .then(|| Arc::new(aligned.mesh.positions.clone()));
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

    /// Alignment (+ optional ICP / part command); rebuilds B's BVH when the version moved.
    fn stage_align(
        &mut self,
        a: &Side,
        b: &Side,
        opts: &Model3dCompareOptions,
        cmd: Option<AlignCommand>,
        cancel: &CancelToken,
    ) -> Result<(AlignedCache, Option<String>), ComputeError> {
        let mut st = self.align.clone();
        if st.mode != opts.alignment || st.offset != opts.offset {
            st = AlignState::new(opts.alignment, opts.offset, st.version + 1);
        }
        let notice = match cmd {
            Some(c) => Self::apply_command(c, a, b, &mut st, cancel)?,
            None => None,
        };
        let aligned = match &self.aligned {
            Some(c) if c.version == st.version => c.clone(),
            _ => Self::build_aligned(a, b, &st),
        };
        check(cancel)?;
        self.align = st;
        self.aligned = Some(aligned.clone());
        Ok((aligned, notice))
    }

    fn build_aligned(a: &Side, b: &Side, st: &AlignState) -> AlignedCache {
        let (mesh, display_shift) = align::apply_alignment(&b.mesh, &a.stats, &b.stats, st);
        let bvh = Bvh::build(&mesh);
        AlignedCache { version: st.version, mesh: Arc::new(mesh), bvh: Arc::new(bvh), display_shift }
    }

    fn apply_command(
        cmd: AlignCommand,
        a: &Side,
        b: &Side,
        st: &mut AlignState,
        cancel: &CancelToken,
    ) -> Result<Option<String>, ComputeError> {
        // B under the global alignment only (per-part adjustments dropped).
        let mut global = st.clone();
        global.parts = None;
        let (g, _) = align::apply_alignment(&b.mesh, &a.stats, &b.stats, &global);

        let notice = match cmd {
            AlignCommand::ResetRefinement => {
                st.refine = Rigid::IDENTITY;
                st.parts = None;
                None
            }
            AlignCommand::RefineIcp => {
                let queries = sample_points(&g.positions, &g.indices);
                let points = align::sample_stride(&queries, ICP_MAX_POINTS);
                let delta = align::icp(&points, &a.bvh, ICP_ITERATIONS, cancel)?;
                st.refine = st.refine.then(&delta);
                st.parts = None;
                None
            }
            AlignCommand::AlignParts => Self::align_parts_command(a, b, &g, st, cancel)?,
        };
        st.version += 1;
        Ok(notice)
    }

    fn align_parts_command(
        a: &Side,
        b: &Side,
        b_global: &Mesh,
        st: &mut AlignState,
        cancel: &CancelToken,
    ) -> Result<Option<String>, ComputeError> {
        if a.comps.count < 2 && b.comps.count < 2 {
            return Ok(Some("Both models are a single connected part - nothing to align individually.".to_string()));
        }
        let parts: PartAlign = align::align_parts(&a.mesh, &a.comps, b_global, &b.comps, cancel)?;
        let notice = if parts.matched == 0 {
            "No matching parts were found.".to_string()
        } else {
            format!("Aligned {} matching part(s) individually.", parts.matched)
        };
        st.parts = Some(Arc::new(parts));
        Ok(Some(notice))
    }

    /// Signed-distance fields A->B and B->A (GPU or CPU); independent of tolerance.
    fn stage_fields(
        &mut self,
        a: &Side,
        b: &Side,
        aligned: &AlignedCache,
        prefer_gpu: bool,
        cancel: &CancelToken,
    ) -> Result<FieldsCache, ComputeError> {
        if let Some(f) = &self.fields {
            if f.version == aligned.version {
                return Ok(f.clone());
            }
        }
        let largest = a.info.size_bytes.max(b.info.size_bytes);
        let qa = sample_points(&a.mesh.positions, &a.mesh.indices);
        let (va, ba) = self.backends.signed_distances(&aligned.bvh, &qa, largest, prefer_gpu, cancel)?;
        let qb = sample_points(&aligned.mesh.positions, &aligned.mesh.indices);
        let (vb, bb) = self.backends.signed_distances(&a.bvh, &qb, largest, prefer_gpu, cancel)?;
        let backend = if ba == Backend::Gpu || bb == Backend::Gpu { Backend::Gpu } else { Backend::Cpu };
        let fields = FieldsCache {
            version: aligned.version,
            a: Arc::new(split(va, a.mesh.positions.len())),
            b: Arc::new(split(vb, aligned.mesh.positions.len())),
            backend,
        };
        self.fields = Some(fields.clone());
        Ok(fields)
    }

    /// Voxel removed / added volumes; the occupancy grid is reused across tolerance changes.
    fn stage_changes(
        &mut self,
        a: &Side,
        aligned: &AlignedCache,
        tolerance_mm: f32,
        cancel: &CancelToken,
    ) -> Result<Arc<ChangesOutput>, ComputeError> {
        let tolerance_bits = tolerance_mm.to_bits();
        if let Some(c) = &self.changes {
            if c.version == aligned.version && c.tolerance_bits == tolerance_bits {
                return Ok(c.output.clone());
            }
        }
        let grid = self.occupancy_grid(a, aligned, cancel)?;
        let output = Arc::new(volume::changes(
            &grid,
            &a.bvh,
            &aligned.bvh,
            tolerance_mm,
            volume::DEFAULT_MAX_QUADS,
            cancel,
        )?);
        self.changes = Some(ChangesCache { version: aligned.version, tolerance_bits, output: output.clone() });
        Ok(output)
    }

    fn occupancy_grid(
        &mut self,
        a: &Side,
        aligned: &AlignedCache,
        cancel: &CancelToken,
    ) -> Result<Arc<volume::Occupancy>, ComputeError> {
        if let Some(c) = &self.occupancy {
            if c.version == aligned.version {
                return Ok(c.grid.clone());
            }
        }
        let grid = Arc::new(volume::occupancy(&a.bvh, &aligned.bvh, volume::DEFAULT_MAX_CELLS, cancel)?);
        self.occupancy = Some(OccupancyCache { version: aligned.version, grid: grid.clone() });
        Ok(grid)
    }
}

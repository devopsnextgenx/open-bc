//! Owned result types the compute layer produces and the engine forwards to the UI.
use super::mesh::{Aabb, Mesh, MeshStats, V3};
use super::options::Model3dCompareOptions;
use std::sync::Arc;

pub const HISTOGRAM_BINS: usize = 64;

#[derive(Clone, Debug, Default)]
pub struct FileInfo {
    pub path: String,
    pub format: String,
    pub size_bytes: u64,
    pub modified_unix_ms: Option<i64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Backend { Cpu = 0, Gpu = 1 }

#[derive(Clone, Copy, Debug, Default)]
pub struct Timings {
    pub align_ms: f32,
    pub deviation_ms: f32,
    pub analysis_ms: f32,
    pub changes_ms: f32,
}

/// A connected patch of surface whose deviation exceeds the tolerance.
#[derive(Clone, Debug)]
pub struct Region {
    /// 0 = on A, 1 = on B.
    pub side: u8,
    pub area: f64,
    pub max_deviation: f32,
    pub triangles: u32,
    /// In display space (A frame, B after alignment).
    pub bbox: Aabb,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Summary {
    pub max_deviation: f32,
    pub mean_deviation: f32,
    /// 0..1, area weighted over both surfaces.
    pub surface_changed: f32,
    pub tolerance: f32,
    pub range: f32,
}

#[derive(Clone, Debug, Default)]
pub struct ChangesOutput {
    pub removed_mm3: f64,
    pub added_mm3: f64,
    pub cell_size_mm: f32,
    /// The boundary meshes were capped; the volumes are still exact for the grid.
    pub truncated: bool,
    /// Material in A that is not in B (drawn red).
    pub removed: Mesh,
    /// Material in B that is not in A (drawn blue).
    pub added: Mesh,
}

#[derive(Clone, Debug)]
pub struct Model3dOutput {
    /// The options this pass actually used.
    pub options: Model3dCompareOptions,
    pub backend: Backend,
    /// Both meshes are drawn translated by this (CenterAtOrigin moves A as well).
    pub display_shift: V3,
    /// Aligned positions of B; `None` when unchanged since the last result that carried them.
    pub b_positions: Option<Arc<Vec<V3>>>,
    /// Signed deviation per vertex: > 0 means the surface lies outside the other model.
    pub dev_a: Arc<Vec<f32>>,
    pub dev_b: Arc<Vec<f32>>,
    pub summary: Summary,
    /// Area-weighted histogram over [-range, +range].
    pub histogram: Vec<f32>,
    pub regions: Vec<Region>,
    pub regions_total: u32,
    pub changes: Option<Arc<ChangesOutput>>,
    pub timings: Timings,
    /// Set by align commands that could not do anything useful ("only one part", ...).
    pub notice: Option<String>,
}

#[derive(Clone, Debug)]
pub struct LoadedSidePayload {
    pub info: FileInfo,
    pub stats: MeshStats,
    pub mesh: Arc<Mesh>,
    pub edges: Arc<Vec<u32>>,
}

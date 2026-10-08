//! Compute layer of OpenBC: every CPU (Rayon) / GPU (wgpu) heavy lifting routine behind a
//! small, backend-agnostic API. No `wgpu` type crosses a public boundary.
//!
//! ```text
//! lib.rs ........ module map + root re-exports (this file holds no logic)
//! gpu_policy .... the one GPU size policy (1 MiB floor, 4 MiB default) every domain uses
//! dispatcher .... ComputeDispatcher: content hashing HAL (CPU pool + optional GPU marker)
//! hex ........... byte-range comparison for hex compare
//! table ......... CSV/TSV + spreadsheet decoding and parallel row diffing
//! media ......... container metadata (audio via lofty; MP4/MOV and AVI header walkers)
//! image ......... decode, resize and pixel compare (CPU kernel + GPU contract)
//! model3d ....... mesh loaders, alignment, deviation fields, analysis, volumetric changes
//! ```
//!
//! Each domain keeps its own error type (`ComputeError`, `ImageComputeError`, `TableError`,
//! `MediaError`, `model3d::ComputeError`) because callers match on them independently.

pub mod gpu_policy;
pub mod hex;
pub mod image;
pub mod media;
pub mod model3d;
pub mod table;

mod dispatcher;

pub use dispatcher::{
    BackendKind, ComputeDispatcher, ComputeError, HashResult, GPU_HARD_FLOOR_BYTES, GPU_MIN_BYTES,
};

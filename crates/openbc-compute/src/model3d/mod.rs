//! 3D model compare in the compute layer.
//!
//! [`Session`] owns everything a compare tab needs between option changes and re-runs only the
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
//!
//! File map: `session` (the pipeline above), `align` / `deviation` / `analysis` / `volume`
//! (one per stage), `bvh` (spatial index), `backend` + `gpu` (distance-query execution),
//! `formats` (loaders), `geom` + `error` (shared helpers).
pub mod align;
pub mod analysis;
pub mod backend;
pub mod bvh;
pub mod deviation;
pub mod formats;
#[cfg(feature = "gpu")]
pub mod gpu;
pub mod volume;

mod error;
mod geom;
mod session;
#[cfg(test)]
mod tests;

pub use backend::{Backends, DeviationExecutor, GpuError, GpuPolicy, GPU_DEFAULT_BYTES, GPU_FLOOR_BYTES};
pub use error::ComputeError;
pub use formats::builtin_registry;
pub use session::{AlignCommand, LoadOutcome, ProbeData, RawModel, Session, Side};

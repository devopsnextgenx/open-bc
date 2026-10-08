//! 3D model compare: domain types only (no I/O, no threads, no GPU).
//!
//! * `mesh`    - triangle mesh, pure statistics / topology helpers; also re-exports the
//!               vector math, `Aabb`, `Rigid` and `FxMap` it is built from
//! * `options` - everything the UI can change (tolerance, alignment, ...)
//! * `result`  - owned, send-able result types produced by the compute layer
//! * `loader`  - the `MeshLoader` extension point and the `LoaderRegistry`
pub mod loader;
pub mod mesh;
pub mod options;
pub mod result;

// Implementation detail of `mesh`; its public items are re-exported from there.
mod bounds;
mod hash;
mod math;
mod rigid;

pub use loader::{CancelToken, LoadContext, LoadError, LoadLimits, LoaderRegistry, MeshLoader};
pub use mesh::{Aabb, Components, Mesh, MeshStats, Rigid, V3};
pub use options::{AlignmentMode, Model3dCompareOptions};
pub use result::*;

//! Compute layer of OpenBC: every CPU (Rayon) / GPU (wgpu) heavy lifting routine behind a
//! small, backend-agnostic API. No `wgpu` type crosses a public boundary.
//!
//! ```text
//! lib.rs ........ module map + root re-exports (this file holds no logic)
//! gpu_policy .... the one GPU size policy (1 MiB floor, 4 MiB default) every domain uses
//! dispatcher .... ComputeDispatcher: content hashing HAL (CPU pool + optional GPU marker)
//! digest ........ MD5 / SHA-1 / SHA-256 / SHA-512 content digests (HashAlgorithm)
//! binary ........ LIEF extraction of PE/ELF/Mach-O into openbc-core's BinaryModel (feature `binary`)
//! hex ........... byte-range comparison for hex compare
//! table ......... CSV/TSV + spreadsheet decoding and parallel row diffing
//! audio_compare . decoded PCM, multi-resolution peaks and aligned sample deltas
//! spectral_diff . STFT-based spectral comparison and RGB heat-map rendering
//! media ......... container metadata (audio via lofty; MP4/MOV and AVI header walkers)
//! image ......... decode, resize and pixel compare (CPU kernel + GPU contract)
//! model3d ....... mesh loaders, alignment, deviation fields, analysis, volumetric changes
//! ```
//!
//! Each domain keeps its own error type (`ComputeError`, `ImageComputeError`, `TableError`,
//! `MediaError`, `model3d::ComputeError`) because callers match on them independently.

pub mod gpu_policy;
pub mod audio_compare;
#[cfg(feature = "binary")]
pub mod binary;
pub mod digest;
pub mod hex;
pub mod image;
pub mod media;
pub mod model3d;
pub mod spectral_diff;
pub mod table;

mod dispatcher;

pub use dispatcher::{
    BackendKind, ComputeDispatcher, ComputeError, HashResult, GPU_HARD_FLOOR_BYTES, GPU_MIN_BYTES,
};

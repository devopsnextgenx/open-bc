//! Image compare compute: decode, align (resize) and compare, with CPU (Rayon)
//! and optional GPU execution behind one policy-driven entry point.
//!
//! Callers only see [`ImageCompute`]; no backend type leaks out.
//!
//! ```text
//! decode ...... bytes -> RGBA (bounded dimensions / allocation)
//! resize ...... bilinear alignment of the two sides
//! kernel ...... reference CPU compare (row-parallel, cooperative cancel)
//! gpu ......... executor contract + WGSL (no wgpu types)
//! policy ...... when the GPU is eligible
//! pipeline .... align -> try GPU -> CPU fallback -> verdict
//! dispatch .... `ImageCompute`: spawns the above on a Rayon pool
//! ```

mod cancel;
mod decode;
mod dispatch;
mod error;
mod gpu;
mod kernel;
mod pipeline;
mod policy;
mod resize;

pub use cancel::CancelToken;
pub use decode::{decode_image, DecodedImage, MAX_DECODE_ALLOC, MAX_DIMENSION};
pub use dispatch::{CompareDone, ImageCompute, PairDecode};
pub use error::ImageComputeError;
pub use gpu::{GpuCompareParams, GpuImageExecutor, GpuImageJob, GpuImageOutput, IMAGE_COMPARE_WGSL};
pub use kernel::compare_cpu;
pub use policy::{ImageComputePolicy, GPU_HARD_FLOOR_BYTES};
pub use resize::resize_bilinear;

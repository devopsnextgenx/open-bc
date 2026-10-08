//! Compile-time contract for downstream crates (engine, ui-qt FFI, ...).
//!
//! Every path below existed before the internal reorganisation. If a refactor moves or
//! renames one, this file stops compiling instead of a consumer.
#![allow(unused_imports)]

use std::io::Cursor;

// crate root
use openbc_compute::{BackendKind, ComputeDispatcher, ComputeError, HashResult, GPU_HARD_FLOOR_BYTES, GPU_MIN_BYTES};

// hex
use openbc_compute::hex::{compare_chunk, select_backend, ByteBackend, GPU_NEVER_BELOW_BYTES};

// table
use openbc_compute::table::{
    decode_workbook, diff_tables_parallel, is_delimited_extension, is_spreadsheet_extension, TableError,
};

// media
use openbc_compute::media::{format_duration, read_media_metadata, MediaError, MediaInfo, ReadSeek};

// image
use openbc_compute::image::{
    compare_cpu, decode_image, resize_bilinear, CancelToken, CompareDone, DecodedImage, GpuCompareParams,
    GpuImageExecutor, GpuImageJob, GpuImageOutput, ImageCompute, ImageComputeError, ImageComputePolicy,
    PairDecode, GPU_HARD_FLOOR_BYTES as IMAGE_GPU_HARD_FLOOR_BYTES, IMAGE_COMPARE_WGSL, MAX_DECODE_ALLOC,
    MAX_DIMENSION,
};

// model3d
use openbc_compute::model3d::{
    align, analysis, backend, builtin_registry, bvh, deviation, formats, volume, AlignCommand, Backends,
    ComputeError as Model3dComputeError, DeviationExecutor, GpuError, GpuPolicy, LoadOutcome, ProbeData,
    RawModel, Session, Side, GPU_DEFAULT_BYTES, GPU_FLOOR_BYTES,
};
use openbc_compute::model3d::deviation::check as deviation_check;
use openbc_compute::model3d::formats::par_weld;

#[test]
fn constants_keep_their_types_and_values() {
    let _: usize = GPU_MIN_BYTES;
    let _: usize = GPU_HARD_FLOOR_BYTES;
    let _: usize = openbc_compute::hex::GPU_MIN_BYTES;
    let _: usize = GPU_NEVER_BELOW_BYTES;
    let _: u64 = IMAGE_GPU_HARD_FLOOR_BYTES;
    let _: u64 = GPU_FLOOR_BYTES;
    let _: u64 = GPU_DEFAULT_BYTES;
    assert_eq!((GPU_MIN_BYTES, GPU_HARD_FLOOR_BYTES), (4 << 20, 1 << 20));
    assert_eq!(openbc_compute::hex::GPU_MIN_BYTES, GPU_MIN_BYTES);
    assert_eq!(GPU_NEVER_BELOW_BYTES, GPU_HARD_FLOOR_BYTES);
    assert_eq!((IMAGE_GPU_HARD_FLOOR_BYTES, GPU_FLOOR_BYTES, GPU_DEFAULT_BYTES), (1 << 20, 1 << 20, 4 << 20));
}

#[test]
fn hash_dispatcher_and_hex_backend() {
    let r: HashResult = ComputeDispatcher::new(1).unwrap().hash_bytes(b"abc");
    assert_eq!(r.backend, BackendKind::Cpu);
    assert_eq!(select_backend(1 << 30, true), ByteBackend::Cpu);
    assert!(compare_chunk(b"same", b"same", 0).is_empty());
}

#[test]
fn error_display_strings_are_unchanged() {
    assert_eq!(ImageComputeError::Cancelled.to_string(), "cancelled");
    assert_eq!(ImageComputeError::Decode("x".into()).to_string(), "cannot decode image: x");
    assert_eq!(ImageComputeError::TooLarge { width: 3, height: 4 }.to_string(), "image too large: 3x4");
    assert_eq!(ImageComputeError::WorkerDropped.to_string(), "compute worker dropped the job");
    assert_eq!(Model3dComputeError::Cancelled.to_string(), "cancelled");
    assert_eq!(Model3dComputeError::NotLoaded.to_string(), "both models must be loaded");
    assert_eq!(Model3dComputeError::Message("m".into()).to_string(), "m");
    assert_eq!(TableError::Unsupported("png".into()).to_string(), "unsupported table format '.png'");
    assert_eq!(MediaError::Unsupported.to_string(), "unrecognised media container");
    let _: fn(usize) -> Result<ComputeDispatcher, ComputeError> = ComputeDispatcher::new;
}

#[test]
fn media_and_table_entry_points() {
    assert!(matches!(read_media_metadata(&mut Cursor::new(vec![0u8; 64])), Err(MediaError::Unsupported)));
    assert_eq!(format_duration(5.0), "0:00:05.000");
    assert!(is_delimited_extension("csv") && is_spreadsheet_extension("xlsx"));
    assert!(matches!(decode_workbook(Vec::new(), "bin", 1), Err(TableError::Unsupported(_))));
}

#[test]
fn image_policy_and_cancel_token() {
    let p = ImageComputePolicy::default();
    assert!(!p.gpu_eligible(IMAGE_GPU_HARD_FLOOR_BYTES - 1, 0));
    let c = CancelToken::new();
    assert!(!c.is_cancelled());
    c.cancel();
    assert!(c.is_cancelled());
    assert!(IMAGE_COMPARE_WGSL.contains("@compute"));
}

#[test]
fn model3d_registry_and_policy() {
    let _registry = builtin_registry();
    assert!(!GpuPolicy::default().eligible(GPU_DEFAULT_BYTES - 1));
    assert!(Backends::cpu_only().gpu_name().is_none());
    assert!(deviation_check(&openbc_core::model3d::CancelToken::new()).is_ok());
}

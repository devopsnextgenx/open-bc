//! C-ABI shims consumed by the Qt (C++) layer, one file per compare viewer.
//!
//! File naming mirrors the C++ side: `src/ffi/<viewer>_compare.rs` pairs with
//! `cpp/viewers/<viewer>/<viewer>_compare_viewer.h`. Shared helpers live in `common.rs`.
//! Exported symbol names (`openbc_*`) are part of the C++/Rust contract and are
//! independent of this module layout.

pub(crate) mod common;
pub(crate) mod model3d_compare;
pub(crate) mod picture_compare;
pub(crate) mod resource_watch;
pub(crate) mod vfs_bridge;

pub mod hex_compare;
pub mod media_compare;
pub mod registry_compare;
pub mod table_compare;
//! Async orchestration between the UI and `openbc-core` / `openbc-compute` / `openbc-vfs`.
//!
//! * Folder compare: [`folder_compare`] (one level, optional content/timestamp checks),
//!   [`scan`] (level scanning with bounded background hashing) and [`fs_ops`] (copy / mirror /
//!   delete / rename through the VFS). Their items are re-exported at the crate root.
//! * Per-tab compare actors, one module each: [`text_runner`], [`table_compare`],
//!   [`registry_compare`], [`hex_compare`], [`media_compare`], [`image_compare`] and
//!   [`model3d_compare`]. The table/registry/media actors share the loop in
//!   [`common::BatchedActor`]; all of them use the channel and I/O plumbing in [`common`].
//! * Reload tracking: [`resource_watch`] (per-tab stale detection on top of `openbc-vfs::observe`).

#[macro_use]
mod metrics;

pub mod common;
pub mod hex_compare;
pub mod image_compare;
pub mod media_compare;
pub mod model3d_compare;
pub mod registry_compare;
pub mod resource_watch;
pub mod table_compare;
pub mod text_runner;

mod folder_compare;
mod fs_ops;
mod scan;

pub use folder_compare::{
    compare_folder_level, compare_folder_level_with_options, compare_folder_sides_with_options,
};
pub use fs_ops::{
    copy_entry, create_directory, delete_entry, mirror_entry, read_file, rename_entry, write_file,
};
pub use scan::{compare_file_content_by_hash, scan_level, ScanError, ScanEvent};

#[cfg(test)]
mod tests;
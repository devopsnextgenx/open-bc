//! Background image compare pipeline.
//!
//! Flow (see ARCHITECTURE.md): bytes arrive through an [`ImageByteSource`]
//! (Tokio I/O), decode/resize/compare run on Rayon via `openbc-compute`, and
//! owned [`ImageCompareEvent`]s flow to the UI over a bounded
//! `tokio::sync::mpsc` channel. Slider/offset changes re-run only the compare
//! phase and supersede any in-flight pass.

mod events;
mod session;
mod source;

pub use events::{ImageCompareEvent, ImageFileInfo, Side};
pub use session::{
    install_image_compute, spawn_image_compare, HandleError, ImageCompareConfig, ImageCompareHandle,
};
pub use source::{ImageByteSource, LocalFsSource, SourceFuture, SourceRead};

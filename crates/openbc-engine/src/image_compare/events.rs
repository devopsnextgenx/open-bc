use std::sync::Arc;

use openbc_core::image_compare::{ImageCompareOutput, RgbaImage};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Side {
    Left,
    Right,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ImageFileInfo {
    pub path: String,
    pub size_bytes: u64,
    pub modified_unix_ms: Option<i64>,
    pub format: Option<String>,
    pub width: u32,
    pub height: u32,
    pub bits_per_pixel: u16,
}

/// Owned progress/result messages. `seq` increases with every load or compare
/// pass; the UI may ignore anything older than the newest `seq` it has seen.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum ImageCompareEvent {
    /// Files are being read and decoded.
    Loading { seq: u64 },
    /// Both sides decoded. Carries the native-size images for the two preview panes.
    Loaded {
        seq: u64,
        left: ImageFileInfo,
        right: ImageFileInfo,
        left_image: Arc<RgbaImage>,
        right_image: Arc<RgbaImage>,
        binary_identical: bool,
    },
    /// A compare pass finished (also emitted after every option change).
    Result { seq: u64, output: ImageCompareOutput, binary_identical: bool },
    /// One side could not be read or decoded. The other side's work is unaffected.
    PathError { seq: u64, side: Side, path: String, message: String },
    /// The compare pass itself failed.
    Failed { seq: u64, message: String },
}

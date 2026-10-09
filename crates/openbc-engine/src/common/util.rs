//! Small helpers shared by the compare actors.

use std::fmt::Display;
use std::path::Path;

use serde::{Deserialize, Serialize};

use super::runtime::rayon_run;
use super::source::ByteSource;

/// Which pane of a two-way compare something belongs to. Shared by every compare tab
/// (`image_compare::Side` is this same type).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Side {
    Left,
    Right,
}

impl Side {
    pub fn index(self) -> usize {
        self as usize
    }

    pub fn label(self) -> &'static str {
        if self == Side::Left { "Left" } else { "Right" }
    }
}

/// Lower-case extension without the dot.
pub fn extension_of(path: &Path) -> String {
    path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase()
}

/// An empty path means "no file on that side".
pub fn is_unset(path: &Path) -> bool {
    path.as_os_str().is_empty()
}

/// `path: message` error text (preserves path and operation context).
pub fn context(path: &Path, operation: &str, err: impl Display) -> String {
    format!("{}: {operation}: {err}", path.display())
}

/// Reads `path` (bounded by `max_bytes`) and parses it on Rayon.
///
/// `Ok(None)` for an unset path. Errors are `path: read: ...` for I/O and
/// `path: <stage>: ...` for parse failures (or a lost worker), ready to show in the UI.
pub async fn load_parsed<T, E>(
    source: &dyn ByteSource,
    path: &Path,
    max_bytes: u64,
    stage: &'static str,
    parse: impl FnOnce(Vec<u8>) -> Result<T, E> + Send + 'static,
) -> Result<Option<T>, String>
where
    T: Send + 'static,
    E: Display + Send + 'static,
{
    if is_unset(path) {
        return Ok(None);
    }
    let bytes = source.read_all(path, max_bytes).await.map_err(|e| context(path, "read", e))?;
    match rayon_run(move || parse(bytes)).await {
        Ok(Ok(value)) => Ok(Some(value)),
        Ok(Err(e)) => Err(context(path, stage, e)),
        Err(e) => Err(context(path, stage, e)),
    }
}

/// Writes `bytes` to `target`, with `path: save: ...` error text.
pub async fn write_bytes(source: &dyn ByteSource, target: &Path, bytes: &[u8]) -> Result<(), String> {
    source.write_all(target, bytes).await.map_err(|e| context(target, "save", e))
}

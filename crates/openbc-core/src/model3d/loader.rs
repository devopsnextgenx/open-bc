//! The extension point for new 3D formats.
//!
//! A format is one `MeshLoader`. Register it on a `LoaderRegistry`; the UI file filter,
//! the engine and the compute layer pick it up without further changes. Later
//! registrations win over earlier ones for the same extension, so an application can
//! replace a built-in loader (e.g. plug a real STEP tessellator over the placeholder).
use super::mesh::Mesh;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

#[derive(Clone, Default)]
pub struct CancelToken(Arc<AtomicBool>);
impl CancelToken {
    pub fn new() -> Self { Self::default() }
    pub fn cancel(&self) { self.0.store(true, Ordering::Relaxed); }
    pub fn is_cancelled(&self) -> bool { self.0.load(Ordering::Relaxed) }
}

#[derive(Clone, Copy, Debug)]
pub struct LoadLimits {
    pub max_triangles: usize,
}
impl Default for LoadLimits {
    fn default() -> Self { Self { max_triangles: 60_000_000 } }
}

#[derive(Debug, Clone)]
pub enum LoadError {
    /// No loader for this extension, or the loader needs a plugin that is not installed.
    Unsupported(String),
    Malformed(String),
    TooLarge(String),
    Cancelled,
}
impl std::fmt::Display for LoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Unsupported(m) => write!(f, "Unsupported format: {m}"),
            Self::Malformed(m) => write!(f, "Malformed file: {m}"),
            Self::TooLarge(m) => write!(f, "Too large: {m}"),
            Self::Cancelled => write!(f, "Cancelled"),
        }
    }
}
impl std::error::Error for LoadError {}

pub struct LoadContext<'a> {
    pub file_name: &'a str,
    pub limits: &'a LoadLimits,
    pub cancel: &'a CancelToken,
}

pub trait MeshLoader: Send + Sync {
    fn id(&self) -> &'static str;
    fn display_name(&self) -> &'static str;
    /// Lower-case extensions without the dot.
    fn extensions(&self) -> &'static [&'static str];
    /// Content sniffing for files whose extension is unknown or wrong.
    fn sniff(&self, _head: &[u8]) -> bool { false }
    /// Must return millimetres, finite coordinates, triangles only. May use Rayon.
    /// Should poll `ctx.cancel` at coarse intervals.
    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError>;
}

#[derive(Default, Clone)]
pub struct LoaderRegistry {
    loaders: Vec<Arc<dyn MeshLoader>>,
}

impl LoaderRegistry {
    pub fn new() -> Self { Self::default() }

    pub fn register(&mut self, loader: Arc<dyn MeshLoader>) { self.loaders.push(loader); }

    pub fn find(&self, file_name: &str, head: &[u8]) -> Option<Arc<dyn MeshLoader>> {
        let ext = file_name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).unwrap_or_default();
        self.loaders.iter().rev()
            .find(|l| l.extensions().iter().any(|e| e.eq_ignore_ascii_case(&ext)))
            .or_else(|| self.loaders.iter().rev().find(|l| l.sniff(head)))
            .cloned()
    }

    /// (display name, extensions) groups for file dialogs.
    pub fn groups(&self) -> Vec<(&'static str, &'static [&'static str])> {
        self.loaders.iter().map(|l| (l.display_name(), l.extensions())).collect()
    }

    pub fn extensions(&self) -> Vec<String> {
        let mut v: Vec<String> = self.loaders.iter().flat_map(|l| l.extensions().iter().map(|e| e.to_string())).collect();
        v.sort();
        v.dedup();
        v
    }
}

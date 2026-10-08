//! Built-in loaders. Every format is one `MeshLoader`; `builtin_registry()` wires them up.
//! To add a format: implement `MeshLoader` (here, or in your own crate) and call
//! `registry.register(Arc::new(MyLoader))` - nothing else in the stack changes.
use openbc_core::model3d::*;
use std::sync::Arc;

mod cad;
mod common;
mod obj;
mod off;
mod ply;
mod stl;
#[cfg(feature = "fmt-gltf")]
mod gltf_fmt;
#[cfg(feature = "fmt-3mf")]
mod threemf;
#[cfg(feature = "fmt-step")]
mod step;

/// Later registrations win, so the placeholder goes first.
pub fn builtin_registry() -> LoaderRegistry {
    let mut r = LoaderRegistry::new();
    r.register(Arc::new(cad::CadPlaceholder));
    r.register(Arc::new(stl::StlLoader));
    r.register(Arc::new(obj::ObjLoader));
    r.register(Arc::new(ply::PlyLoader));
    r.register(Arc::new(off::OffLoader));
    #[cfg(feature = "fmt-gltf")]
    r.register(Arc::new(gltf_fmt::GltfLoader));
    #[cfg(feature = "fmt-3mf")]
    r.register(Arc::new(threemf::ThreeMfLoader));
    #[cfg(feature = "fmt-step")]
    r.register(Arc::new(step::StepLoader));
    r
}

pub use common::par_weld;
pub(crate) use common::{check_cancel, fan_triangulate, finish, malformed, parse_v3, poll_cancel};

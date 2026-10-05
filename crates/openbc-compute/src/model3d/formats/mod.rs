//! Built-in loaders. Every format is one `MeshLoader`; `builtin_registry()` wires them up.
//! To add a format: implement `MeshLoader` (here, or in your own crate) and call
//! `registry.register(Arc::new(MyLoader))` - nothing else in the stack changes.
use openbc_core::model3d::*;
use rayon::prelude::*;
use std::sync::Arc;

mod cad;
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

/// Shared tail of every loader: limits, validation, parallel weld.
pub(crate) fn finish(mut mesh: Mesh, ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
    if ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
    if mesh.triangle_count() == 0 { return Err(LoadError::Malformed("file contains no triangles".into())); }
    if mesh.triangle_count() > ctx.limits.max_triangles {
        return Err(LoadError::TooLarge(format!("{} triangles (limit {})", mesh.triangle_count(), ctx.limits.max_triangles)));
    }
    mesh.validate().map_err(LoadError::Malformed)?;
    par_weld(&mut mesh);
    if mesh.triangle_count() == 0 { return Err(LoadError::Malformed("only degenerate triangles".into())); }
    Ok(mesh)
}

/// Sort-based vertex weld: quantise, parallel sort, one linear scan. Same result as
/// `Mesh::weld` but scales with cores (the hash-map version is single threaded).
pub fn par_weld(mesh: &mut Mesh) {
    let bb = mesh.aabb();
    if bb.is_empty() { return; }
    let inv = 1.0 / (bb.diagonal() as f64 * 1e-6).max(1e-12);
    let mut keyed: Vec<((i64, i64, i64), u32)> = mesh.positions.par_iter().enumerate()
        .map(|(i, p)| (((p[0] as f64 * inv).round() as i64, (p[1] as f64 * inv).round() as i64, (p[2] as f64 * inv).round() as i64), i as u32))
        .collect();
    keyed.par_sort_unstable();
    let mut remap = vec![0u32; mesh.positions.len()];
    let mut out: Vec<V3> = Vec::with_capacity(mesh.positions.len() / 2 + 1);
    let mut prev = None;
    for (k, i) in &keyed {
        if prev != Some(*k) { out.push(mesh.positions[*i as usize]); prev = Some(*k); }
        remap[*i as usize] = (out.len() - 1) as u32;
    }
    let tris: Vec<[u32; 3]> = mesh.indices.par_chunks_exact(3)
        .filter_map(|t| {
            let (a, b, c) = (remap[t[0] as usize], remap[t[1] as usize], remap[t[2] as usize]);
            (a != b && b != c && a != c).then_some([a, b, c])
        })
        .collect();
    mesh.positions = out;
    mesh.indices = tris.into_iter().flatten().collect();
}

pub(crate) fn malformed(m: impl Into<String>) -> LoadError { LoadError::Malformed(m.into()) }

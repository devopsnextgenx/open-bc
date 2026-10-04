use openbc_core::model3d::*;

/// STEP / IGES / BREP are B-rep formats: they must be *tessellated* by a CAD kernel
/// (OpenCascade, ...). This crate deliberately has no kernel dependency, so the built-in
/// entry only produces a clear error. An application that links a kernel registers its own
/// `MeshLoader` for these extensions after `builtin_registry()`; it wins automatically.
pub struct CadPlaceholder;

impl MeshLoader for CadPlaceholder {
    fn id(&self) -> &'static str { "cad-placeholder" }
    fn display_name(&self) -> &'static str { "CAD (needs kernel plugin)" }
    fn extensions(&self) -> &'static [&'static str] { &["step", "stp", "iges", "igs", "brep"] }
    fn load(&self, _bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        Err(LoadError::Unsupported(format!(
            "{}: B-rep CAD files need a tessellation plugin (register a MeshLoader for step/stp/iges/igs/brep)",
            ctx.file_name
        )))
    }
}

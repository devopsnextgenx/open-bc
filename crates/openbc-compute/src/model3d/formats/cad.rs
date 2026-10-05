use openbc_core::model3d::*;

/// B-rep CAD formats must be *tessellated* by a CAD kernel. This crate has no full kernel
/// dependency, so the built-in entry only produces a clear error. An application that links a
/// kernel (OpenCascade, ...) registers its own `MeshLoader` after `builtin_registry()`; it wins
/// automatically.
///
/// With the `fmt-step` feature the truck-based STEP loader handles `step`/`stp`, so the
/// placeholder only claims the formats nothing else can read yet.
pub struct CadPlaceholder;

const WITH_STEP_LOADER: &[&str] = &["iges", "igs", "brep"];
const WITHOUT_STEP_LOADER: &[&str] = &["step", "stp", "iges", "igs", "brep"];

impl MeshLoader for CadPlaceholder {
    fn id(&self) -> &'static str { "cad-placeholder" }
    fn display_name(&self) -> &'static str { "CAD (needs kernel plugin)" }
    fn extensions(&self) -> &'static [&'static str] {
        if cfg!(feature = "fmt-step") { WITH_STEP_LOADER } else { WITHOUT_STEP_LOADER }
    }
    fn load(&self, _bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        Err(LoadError::Unsupported(format!(
            "{}: this CAD format is not supported yet (IGES / BREP need a tessellation plugin; \
             register a MeshLoader for iges/igs/brep{})",
            ctx.file_name,
            if cfg!(feature = "fmt-step") { "" } else { "/step/stp, or build with the `fmt-step` feature" }
        )))
    }
}
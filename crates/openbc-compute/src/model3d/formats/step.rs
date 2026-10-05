//! STEP via truck (cargo feature `fmt-step`). Tessellates every shell; units assumed mm.
use super::{finish, malformed};
use openbc_core::model3d::*;
use truck_meshalgo::prelude::*;
use truck_stepio::r#in::Table;

pub struct StepLoader;

/// Chord tolerance in mm.
const TOL: f64 = 0.05;

impl MeshLoader for StepLoader {
    fn id(&self) -> &'static str { "step" }
    fn display_name(&self) -> &'static str { "STEP (truck)" }
    fn extensions(&self) -> &'static [&'static str] { &["step", "stp"] }
    fn sniff(&self, head: &[u8]) -> bool { head.starts_with(b"ISO-10303-21") }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let text = String::from_utf8_lossy(bytes).into_owned();
        // truck can panic on unsupported geometry, and a panic inside rayon::spawn aborts
        // the whole process. Contain it and report it as a normal load error.
        let result = std::panic::catch_unwind(|| -> Result<Mesh, String> {
            let table = Table::from_step(&text).ok_or("not a parsable STEP file")?;
            let mut mesh = Mesh::default();
            for shell in table.shell.values() {
                let cshell = table.to_compressed_shell(shell).map_err(|e| format!("shell: {e:?}"))?;
                let mut poly = cshell.robust_triangulation(TOL).to_polygon();
                poly.triangulate();
                let base = mesh.positions.len() as u32;
                mesh.positions.extend(poly.positions().iter().map(|p| [p.x as f32, p.y as f32, p.z as f32]));
                for t in poly.faces().tri_faces() {
                    mesh.indices.extend(t.iter().map(|v| base + v.pos as u32));
                }
            }
            Ok(mesh)
        });
        match result {
            Ok(Ok(m)) => finish(m, ctx),
            Ok(Err(e)) => Err(malformed(e)),
            Err(_) => Err(malformed("STEP tessellation panicked (unsupported geometry)")),
        }
    }
}
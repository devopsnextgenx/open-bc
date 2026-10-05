//! STEP via truck (cargo feature `fmt-step`, on by default). Tessellates every shell; units
//! assumed mm. Assembly placements are not applied (truck-stepio reads shells as-is).
use super::{finish, malformed};
use openbc_core::model3d::*;
use std::panic::{catch_unwind, AssertUnwindSafe};
use truck_meshalgo::prelude::*;
use truck_stepio::r#in::Table;

pub struct StepLoader;

/// Chord tolerance in mm.
const TOL: f64 = 0.05;

impl MeshLoader for StepLoader {
    fn id(&self) -> &'static str { "step" }
    fn display_name(&self) -> &'static str { "STEP (truck)" }
    fn extensions(&self) -> &'static [&'static str] { &["step", "stp"] }
    fn sniff(&self, head: &[u8]) -> bool {
        let t = head.iter().position(|b| !b.is_ascii_whitespace()).unwrap_or(0);
        head[t..].starts_with(b"ISO-10303-21")
    }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let text = String::from_utf8_lossy(bytes).into_owned();
        // truck can panic on unsupported geometry, and a panic inside rayon::spawn aborts the
        // whole process. Contain it per shell: one bad shell must not lose the whole model.
        let outer = catch_unwind(AssertUnwindSafe(|| -> Result<Mesh, LoadError> {
            let table = Table::from_step(&text).ok_or_else(|| malformed("not a parsable STEP file"))?;
            if table.shell.is_empty() {
                return Err(malformed("STEP file contains no shells (nothing to tessellate)"));
            }
            let mut mesh = Mesh::default();
            let (mut failed, mut first_err) = (0usize, None::<String>);
            for shell in table.shell.values() {
                if ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
                let one = catch_unwind(AssertUnwindSafe(|| -> Result<(Vec<V3>, Vec<u32>), String> {
                    let cshell = table.to_compressed_shell(shell).map_err(|e| format!("shell: {e:?}"))?;
                    let mut poly = cshell.robust_triangulation(TOL).to_polygon();
                    poly.triangulate();
                    let pos: Vec<V3> = poly.positions().iter().map(|p| [p.x as f32, p.y as f32, p.z as f32]).collect();
                    let idx: Vec<u32> = poly.faces().tri_faces().iter().flat_map(|t| t.iter().map(|v| v.pos as u32)).collect();
                    Ok((pos, idx))
                }));
                match one {
                    Ok(Ok((pos, idx))) => {
                        let base = mesh.positions.len() as u32;
                        mesh.positions.extend(pos);
                        mesh.indices.extend(idx.into_iter().map(|i| base + i));
                    }
                    Ok(Err(e)) => { failed += 1; first_err.get_or_insert(e); }
                    Err(_) => { failed += 1; first_err.get_or_insert_with(|| "tessellation panicked (unsupported geometry)".into()); }
                }
            }
            if mesh.triangle_count() == 0 {
                return Err(malformed(format!(
                    "no shell could be tessellated ({failed} failed: {})",
                    first_err.unwrap_or_else(|| "empty result".into())
                )));
            }
            if failed > 0 {
                eprintln!("step: {failed} shell(s) skipped ({})", first_err.unwrap_or_default());
            }
            Ok(mesh)
        }));
        match outer {
            Ok(Ok(m)) => finish(m, ctx),
            Ok(Err(e)) => Err(e),
            Err(_) => Err(malformed("STEP parsing panicked (unsupported file)")),
        }
    }
}
//! glTF 2.0 / GLB (cargo feature `fmt-gltf`, uses the `gltf` crate). Scene-graph transforms
//! are baked in; glTF is metres, converted to mm.
use super::{finish, malformed};
use openbc_core::model3d::*;

pub struct GltfLoader;

type M4 = [[f32; 4]; 4];
fn mul(a: &M4, b: &M4) -> M4 {
    let mut r = [[0f32; 4]; 4];
    for c in 0..4 { for rr in 0..4 { for k in 0..4 { r[c][rr] += a[k][rr] * b[c][k]; } } }
    r
}

fn visit(node: gltf::Node, parent: &M4, bufs: &[gltf::buffer::Data], out: &mut Mesh) {
    let world = mul(parent, &node.transform().matrix());
    if let Some(mesh) = node.mesh() {
        for prim in mesh.primitives().filter(|p| p.mode() == gltf::mesh::Mode::Triangles) {
            let reader = prim.reader(|b| Some(&bufs[b.index()]));
            let Some(pos) = reader.read_positions() else { continue };
            let base = out.positions.len() as u32;
            let n = out.positions.len();
            out.positions.extend(pos.map(|p| [
                (world[0][0] * p[0] + world[1][0] * p[1] + world[2][0] * p[2] + world[3][0]) * 1000.0,
                (world[0][1] * p[0] + world[1][1] * p[1] + world[2][1] * p[2] + world[3][1]) * 1000.0,
                (world[0][2] * p[0] + world[1][2] * p[1] + world[2][2] * p[2] + world[3][2]) * 1000.0,
            ]));
            let added = (out.positions.len() - n) as u32;
            match reader.read_indices() {
                Some(i) => out.indices.extend(i.into_u32().map(|v| v + base)),
                None => out.indices.extend(base..base + added),
            }
        }
    }
    for c in node.children() { visit(c, &world, bufs, out); }
}

impl MeshLoader for GltfLoader {
    fn id(&self) -> &'static str { "gltf" }
    fn display_name(&self) -> &'static str { "glTF / GLB" }
    fn extensions(&self) -> &'static [&'static str] { &["glb", "gltf"] }
    fn sniff(&self, head: &[u8]) -> bool { head.starts_with(b"glTF") }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let (doc, bufs, _) = gltf::import_slice(bytes).map_err(|e| malformed(e.to_string()))?;
        let id: M4 = [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]];
        let mut mesh = Mesh::default();
        for scene in doc.scenes() { for n in scene.nodes() { visit(n, &id, &bufs, &mut mesh); } }
        finish(mesh, ctx)
    }
}

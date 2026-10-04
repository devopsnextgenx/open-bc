//! 3MF (cargo feature `fmt-3mf`, uses `zip` + `roxmltree`). Build items and their
//! transforms are honoured; component references are expanded one level deep.
use super::{finish, malformed};
use openbc_core::model3d::*;
use std::io::Read;

pub struct ThreeMfLoader;

fn unit_to_mm(u: &str) -> f32 {
    match u { "micron" => 0.001, "centimeter" => 10.0, "inch" => 25.4, "foot" => 304.8, "meter" => 1000.0, _ => 1.0 }
}

impl MeshLoader for ThreeMfLoader {
    fn id(&self) -> &'static str { "3mf" }
    fn display_name(&self) -> &'static str { "3MF" }
    fn extensions(&self) -> &'static [&'static str] { &["3mf"] }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let mut zip = zip::ZipArchive::new(std::io::Cursor::new(bytes)).map_err(|e| malformed(e.to_string()))?;
        let name = (0..zip.len()).filter_map(|i| zip.by_index(i).ok().map(|f| f.name().to_string()))
            .find(|n| n.to_ascii_lowercase().ends_with(".model")).ok_or_else(|| malformed("no .model part"))?;
        let mut xml = String::new();
        zip.by_name(&name).map_err(|e| malformed(e.to_string()))?.read_to_string(&mut xml).map_err(|e| malformed(e.to_string()))?;
        let doc = roxmltree::Document::parse(&xml).map_err(|e| malformed(e.to_string()))?;
        let root = doc.root_element();
        let scale = unit_to_mm(root.attribute("unit").unwrap_or("millimeter"));

        let object = |id: &str| doc.descendants().find(|n| n.has_tag_name("object") && n.attribute("id") == Some(id));
        let mesh_of = |obj: roxmltree::Node| -> Option<Mesh> {
            let m = obj.children().find(|n| n.has_tag_name("mesh"))?;
            let mut out = Mesh::default();
            for v in m.descendants().filter(|n| n.has_tag_name("vertex")) {
                let f = |k| v.attribute(k).and_then(|s| s.parse::<f32>().ok()).unwrap_or(0.0);
                out.positions.push([f("x"), f("y"), f("z")]);
            }
            for t in m.descendants().filter(|n| n.has_tag_name("triangle")) {
                let i = |k| t.attribute(k).and_then(|s| s.parse::<u32>().ok()).unwrap_or(0);
                out.indices.extend_from_slice(&[i("v1"), i("v2"), i("v3")]);
            }
            Some(out)
        };
        let xf = |s: Option<&str>| -> Rigid {
            // 3MF: 12 numbers m00 m01 m02 m10 m11 m12 m20 m21 m22 tx ty tz, row-vector convention.
            let v: Vec<f64> = s.unwrap_or("").split_ascii_whitespace().filter_map(|t| t.parse().ok()).collect();
            if v.len() != 12 { return Rigid::IDENTITY; }
            Rigid { r: [[v[0], v[3], v[6]], [v[1], v[4], v[7]], [v[2], v[5], v[8]]], t: [v[9], v[10], v[11]] }
        };

        let mut out = Mesh::default();
        let mut add = |obj: roxmltree::Node, t: &Rigid, out: &mut Mesh| {
            if let Some(mut m) = mesh_of(obj) { m.transform(t); out.append(&m); }
            for c in obj.descendants().filter(|n| n.has_tag_name("component")) {
                if let Some(o) = c.attribute("objectid").and_then(object) {
                    if let Some(mut m) = mesh_of(o) { m.transform(&xf(c.attribute("transform")).then(t)); out.append(&m); }
                }
            }
        };
        for item in doc.descendants().filter(|n| n.has_tag_name("item")) {
            if let Some(o) = item.attribute("objectid").and_then(object) { add(o, &xf(item.attribute("transform")), &mut out); }
        }
        out.scale(scale);
        finish(out, ctx)
    }
}

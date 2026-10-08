use super::{fan_triangulate, finish, malformed, parse_v3, poll_cancel};
use openbc_core::model3d::*;

pub struct OffLoader;

impl MeshLoader for OffLoader {
    fn id(&self) -> &'static str { "off" }
    fn display_name(&self) -> &'static str { "OFF" }
    fn extensions(&self) -> &'static [&'static str] { &["off"] }
    fn sniff(&self, head: &[u8]) -> bool { head.starts_with(b"OFF") || head.starts_with(b"COFF") }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let text = String::from_utf8_lossy(bytes);
        let mut lines = text.lines().map(str::trim).filter(|l| !l.is_empty() && !l.starts_with('#'));
        let first = lines.next().ok_or_else(|| malformed("empty file"))?;
        let rest = first.trim_start_matches(|c: char| c.is_ascii_alphabetic());
        let counts_line = if rest.trim().is_empty() { lines.next().ok_or_else(|| malformed("missing counts"))? } else { rest };
        let mut c = counts_line.split_ascii_whitespace().map(|t| t.parse::<usize>());
        let (nv, nf) = match (c.next(), c.next()) { (Some(Ok(a)), Some(Ok(b))) => (a, b), _ => return Err(malformed("bad counts")) };
        let mut mesh = Mesh::default();
        for _ in 0..nv {
            let l = lines.next().ok_or_else(|| malformed("truncated vertices"))?;
            mesh.positions.push(parse_v3(&mut l.split_ascii_whitespace()).ok_or_else(|| malformed("bad vertex"))?);
        }
        for n in 0..nf {
            poll_cancel(ctx, n)?;
            let l = lines.next().ok_or_else(|| malformed("truncated faces"))?;
            let mut it = l.split_ascii_whitespace();
            let k: usize = it.next().and_then(|t| t.parse().ok()).ok_or_else(|| malformed("bad face"))?;
            let f: Vec<u32> = it.take(k).map(|t| t.parse().map_err(|_| malformed("bad face index"))).collect::<Result<_, _>>()?;
            if f.len() != k { return Err(malformed("short face")); }
            fan_triangulate(&f, &mut mesh.indices);
        }
        finish(mesh, ctx)
    }
}

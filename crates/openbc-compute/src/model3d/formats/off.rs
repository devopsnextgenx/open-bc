use super::{finish, malformed};
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
            let mut it = l.split_ascii_whitespace();
            let mut v = [0f32; 3];
            for k in &mut v { *k = it.next().and_then(|t| t.parse().ok()).ok_or_else(|| malformed("bad vertex"))?; }
            mesh.positions.push(v);
        }
        for n in 0..nf {
            if n % 65536 == 0 && ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
            let l = lines.next().ok_or_else(|| malformed("truncated faces"))?;
            let mut it = l.split_ascii_whitespace();
            let k: usize = it.next().and_then(|t| t.parse().ok()).ok_or_else(|| malformed("bad face"))?;
            let f: Vec<u32> = it.take(k).map(|t| t.parse().map_err(|_| malformed("bad face index"))).collect::<Result<_, _>>()?;
            if f.len() != k { return Err(malformed("short face")); }
            for j in 1..k.saturating_sub(1) { mesh.indices.extend_from_slice(&[f[0], f[j], f[j + 1]]); }
        }
        finish(mesh, ctx)
    }
}

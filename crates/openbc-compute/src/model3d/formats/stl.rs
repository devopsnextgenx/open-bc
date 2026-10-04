use super::{finish, malformed};
use openbc_core::model3d::*;
use rayon::prelude::*;

pub struct StlLoader;

impl MeshLoader for StlLoader {
    fn id(&self) -> &'static str { "stl" }
    fn display_name(&self) -> &'static str { "STL" }
    fn extensions(&self) -> &'static [&'static str] { &["stl"] }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let mesh = if is_binary(bytes) { binary(bytes, ctx)? } else { ascii(bytes, ctx)? };
        finish(mesh, ctx)
    }
}

fn is_binary(b: &[u8]) -> bool {
    if b.len() < 84 { return false; }
    let n = u32::from_le_bytes([b[80], b[81], b[82], b[83]]) as usize;
    if 84 + n * 50 == b.len() { return true; }
    // Some writers start binary files with "solid" in the header; ASCII files contain "facet".
    let head = &b[..b.len().min(2048)];
    let looks_ascii = b.starts_with(b"solid") && head.windows(5).any(|w| w == b"facet");
    !looks_ascii && b.len() >= 84 + n * 50
}

fn binary(bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
    let n = u32::from_le_bytes([bytes[80], bytes[81], bytes[82], bytes[83]]) as usize;
    if n > ctx.limits.max_triangles { return Err(LoadError::TooLarge(format!("{n} triangles"))); }
    if 3 * n > u32::MAX as usize { return Err(LoadError::TooLarge("index space exhausted".into())); }
    let body = &bytes[84..84 + n * 50];
    let rd = |r: &[u8], o: usize| f32::from_le_bytes([r[o], r[o + 1], r[o + 2], r[o + 3]]);
    let mut positions = vec![[0f32; 3]; n * 3];
    const REC: usize = 4096;
    body.par_chunks(50 * REC).zip(positions.par_chunks_mut(3 * REC)).try_for_each(|(src, dst)| {
        if ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
        for (rec, out) in src.chunks_exact(50).zip(dst.chunks_exact_mut(3)) {
            for v in 0..3 {
                let o = 12 + v * 12;
                out[v] = [rd(rec, o), rd(rec, o + 4), rd(rec, o + 8)];
            }
        }
        Ok(())
    })?;
    let indices: Vec<u32> = (0..(n * 3) as u32).into_par_iter().collect();
    Ok(Mesh { positions, indices })
}

fn ascii(bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
    let text = String::from_utf8_lossy(bytes);
    let mut it = text.split_ascii_whitespace();
    let mut positions: Vec<V3> = Vec::new();
    while let Some(tok) = it.next() {
        if tok.eq_ignore_ascii_case("vertex") {
            let mut v = [0f32; 3];
            for c in &mut v {
                *c = it.next().ok_or_else(|| malformed("truncated vertex"))?.parse().map_err(|_| malformed("bad number in vertex"))?;
            }
            positions.push(v);
            if positions.len() % (1 << 16) == 0 && ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
        }
    }
    if positions.len() % 3 != 0 { return Err(malformed("vertex count is not a multiple of 3")); }
    let indices = (0..positions.len() as u32).collect();
    Ok(Mesh { positions, indices })
}

use super::{finish, malformed};
use openbc_core::model3d::*;
use rayon::prelude::*;

pub struct ObjLoader;

impl MeshLoader for ObjLoader {
    fn id(&self) -> &'static str { "obj" }
    fn display_name(&self) -> &'static str { "Wavefront OBJ" }
    fn extensions(&self) -> &'static [&'static str] { &["obj"] }

    /// Two parallel passes over newline-aligned chunks: (1) count `v` lines so every chunk
    /// knows its global vertex base (needed for negative / relative indices), (2) parse.
    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let chunks = split_lines(bytes, rayon::current_num_threads() * 4);
        let counts: Vec<usize> = chunks.par_iter().map(|c| lines(c).filter(|l| is_tag(l, b'v')).count()).collect();
        let mut base = Vec::with_capacity(counts.len());
        let mut total = 0usize;
        for c in &counts { base.push(total); total += c; }

        let parts: Vec<(Vec<V3>, Vec<u32>)> = chunks.par_iter().zip(base.par_iter())
            .map(|(c, &b)| parse_chunk(c, b, total, ctx))
            .collect::<Result<_, _>>()?;

        let mut mesh = Mesh::default();
        mesh.positions.reserve(total);
        for (p, i) in parts { mesh.positions.extend(p); mesh.indices.extend(i); }
        finish(mesh, ctx)
    }
}

fn split_lines(b: &[u8], parts: usize) -> Vec<&[u8]> {
    let target = (b.len() / parts.max(1)).max(1 << 20);
    let (mut out, mut start) = (Vec::new(), 0);
    while start < b.len() {
        let mut end = (start + target).min(b.len());
        while end < b.len() && b[end - 1] != b'\n' { end += 1; }
        out.push(&b[start..end]);
        start = end;
    }
    out
}

fn lines(c: &[u8]) -> impl Iterator<Item = &[u8]> {
    c.split(|&b| b == b'\n').map(|l| {
        let l = l.strip_suffix(b"\r").unwrap_or(l);
        let s = l.iter().position(|b| !b.is_ascii_whitespace()).unwrap_or(l.len());
        &l[s..]
    })
}
fn is_tag(l: &[u8], tag: u8) -> bool { l.len() > 1 && l[0] == tag && (l[1] == b' ' || l[1] == b'\t') }

fn parse_chunk(c: &[u8], base: usize, total: usize, ctx: &LoadContext<'_>) -> Result<(Vec<V3>, Vec<u32>), LoadError> {
    let (mut pos, mut idx): (Vec<V3>, Vec<u32>) = (Vec::new(), Vec::new());
    let mut face: Vec<u32> = Vec::with_capacity(8);
    for (n, line) in lines(c).enumerate() {
        if n % 65536 == 0 && ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
        if is_tag(line, b'v') {
            let Ok(s) = std::str::from_utf8(&line[2..]) else { continue };
            let mut it = s.split_ascii_whitespace();
            let mut v = [0f32; 3];
            for k in &mut v { *k = it.next().and_then(|t| t.parse().ok()).ok_or_else(|| malformed("bad vertex line"))?; }
            pos.push(v);
        } else if is_tag(line, b'f') {
            let Ok(s) = std::str::from_utf8(&line[2..]) else { continue };
            face.clear();
            for tok in s.split_ascii_whitespace() {
                let head = tok.split('/').next().unwrap_or("");
                let i: i64 = head.parse().map_err(|_| malformed("bad face index"))?;
                let g = if i > 0 { i - 1 } else { (base + pos.len()) as i64 + i };
                if g < 0 || g as usize >= total { return Err(malformed("face index out of range")); }
                face.push(g as u32);
            }
            for k in 1..face.len().saturating_sub(1) { idx.extend_from_slice(&[face[0], face[k], face[k + 1]]); }
        }
    }
    Ok((pos, idx))
}

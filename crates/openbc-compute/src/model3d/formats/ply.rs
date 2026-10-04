use super::{finish, malformed};
use openbc_core::model3d::*;
use rayon::prelude::*;

pub struct PlyLoader;

#[derive(Clone, Copy)]
enum Ty { I8, U8, I16, U16, I32, U32, F32, F64 }
impl Ty {
    fn parse(s: &str) -> Option<Ty> {
        Some(match s {
            "char" | "int8" => Ty::I8, "uchar" | "uint8" => Ty::U8,
            "short" | "int16" => Ty::I16, "ushort" | "uint16" => Ty::U16,
            "int" | "int32" => Ty::I32, "uint" | "uint32" => Ty::U32,
            "float" | "float32" => Ty::F32, "double" | "float64" => Ty::F64,
            _ => return None,
        })
    }
    fn size(self) -> usize { match self { Ty::I8 | Ty::U8 => 1, Ty::I16 | Ty::U16 => 2, Ty::I32 | Ty::U32 | Ty::F32 => 4, Ty::F64 => 8 } }
}

enum Kind { Scalar(Ty), List(Ty, Ty) }
struct Prop { name: String, kind: Kind }
struct Element { name: String, count: usize, props: Vec<Prop> }

fn read_bin(ty: Ty, d: &[u8], p: usize, le: bool) -> Option<f64> {
    macro_rules! rd { ($t:ty, $n:expr) => {{
        let a: [u8; $n] = d.get(p..p + $n)?.try_into().ok()?;
        (if le { <$t>::from_le_bytes(a) } else { <$t>::from_be_bytes(a) }) as f64
    }}; }
    Some(match ty {
        Ty::I8 => rd!(i8, 1), Ty::U8 => rd!(u8, 1), Ty::I16 => rd!(i16, 2), Ty::U16 => rd!(u16, 2),
        Ty::I32 => rd!(i32, 4), Ty::U32 => rd!(u32, 4), Ty::F32 => rd!(f32, 4), Ty::F64 => rd!(f64, 8),
    })
}

trait Src {
    fn next(&mut self, ty: Ty) -> Option<f64>;
    /// Fast path for fixed-stride vertex blocks; returns true when handled.
    fn bulk_vertices(&mut self, _e: &Element, _out: &mut Vec<V3>) -> bool { false }
}
struct AsciiSrc<'a>(std::str::SplitAsciiWhitespace<'a>);
impl Src for AsciiSrc<'_> {
    fn next(&mut self, _: Ty) -> Option<f64> { self.0.next()?.parse().ok() }
}
struct BinSrc<'a> { d: &'a [u8], pos: usize, le: bool }
impl Src for BinSrc<'_> {
    fn next(&mut self, ty: Ty) -> Option<f64> {
        let v = read_bin(ty, self.d, self.pos, self.le)?;
        self.pos += ty.size();
        Some(v)
    }
    fn bulk_vertices(&mut self, e: &Element, out: &mut Vec<V3>) -> bool {
        let mut off = 0;
        let (mut ox, mut oy, mut oz) = (None, None, None);
        for p in &e.props {
            let Kind::Scalar(t) = p.kind else { return false };
            match p.name.as_str() { "x" => ox = Some((off, t)), "y" => oy = Some((off, t)), "z" => oz = Some((off, t)), _ => {} }
            off += t.size();
        }
        let (Some(x), Some(y), Some(z)) = (ox, oy, oz) else { return false };
        let stride = off;
        let Some(end) = self.pos.checked_add(stride * e.count) else { return false };
        if end > self.d.len() { return false; }
        let (d, le) = (self.d, self.le);
        let block = &d[self.pos..end];
        let rd = |r: &[u8], (o, t): (usize, Ty)| read_bin(t, r, o, le).unwrap_or(0.0) as f32;
        out.par_extend(block.par_chunks_exact(stride).map(|r| [rd(r, x), rd(r, y), rd(r, z)]));
        self.pos = end;
        true
    }
}

fn read_all<S: Src>(src: &mut S, elems: &[Element], mesh: &mut Mesh, ctx: &LoadContext<'_>) -> Result<(), LoadError> {
    for e in elems {
        if ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
        match e.name.as_str() {
            "vertex" => {
                if src.bulk_vertices(e, &mut mesh.positions) { continue; }
                for _ in 0..e.count {
                    let mut v = [0f32; 3];
                    for p in &e.props {
                        let Kind::Scalar(t) = p.kind else { return Err(malformed("list property on vertex")) };
                        let x = src.next(t).ok_or_else(|| malformed("truncated vertex data"))? as f32;
                        match p.name.as_str() { "x" => v[0] = x, "y" => v[1] = x, "z" => v[2] = x, _ => {} }
                    }
                    mesh.positions.push(v);
                }
            }
            _ => {
                let is_face = e.name == "face";
                let mut face: Vec<u32> = Vec::new();
                for n in 0..e.count {
                    if n % 65536 == 0 && ctx.cancel.is_cancelled() { return Err(LoadError::Cancelled); }
                    face.clear();
                    for p in &e.props {
                        match p.kind {
                            Kind::Scalar(t) => { src.next(t).ok_or_else(|| malformed("truncated element"))?; }
                            Kind::List(ct, it) => {
                                let k = src.next(ct).ok_or_else(|| malformed("truncated list"))? as usize;
                                let keep = is_face && (p.name == "vertex_indices" || p.name == "vertex_index");
                                for _ in 0..k {
                                    let v = src.next(it).ok_or_else(|| malformed("truncated list"))?;
                                    if keep { face.push(v as u32); }
                                }
                            }
                        }
                    }
                    for j in 1..face.len().saturating_sub(1) { mesh.indices.extend_from_slice(&[face[0], face[j], face[j + 1]]); }
                }
            }
        }
    }
    Ok(())
}

impl MeshLoader for PlyLoader {
    fn id(&self) -> &'static str { "ply" }
    fn display_name(&self) -> &'static str { "PLY" }
    fn extensions(&self) -> &'static [&'static str] { &["ply"] }
    fn sniff(&self, head: &[u8]) -> bool { head.starts_with(b"ply") }

    fn load(&self, bytes: &[u8], ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
        let marker = b"end_header";
        let hp = bytes.windows(marker.len()).position(|w| w == marker).ok_or_else(|| malformed("no end_header"))?;
        let mut body_start = hp + marker.len();
        if bytes.get(body_start) == Some(&b'\r') { body_start += 1; }
        if bytes.get(body_start) == Some(&b'\n') { body_start += 1; }
        let header = std::str::from_utf8(&bytes[..hp]).map_err(|_| malformed("non-UTF8 header"))?;

        let (mut format, mut elems): (Option<&str>, Vec<Element>) = (None, Vec::new());
        for line in header.lines().map(str::trim) {
            let t: Vec<&str> = line.split_ascii_whitespace().collect();
            match t.as_slice() {
                ["format", f, ..] => format = Some(f),
                ["element", name, n] => elems.push(Element { name: name.to_string(), count: n.parse().map_err(|_| malformed("bad element count"))?, props: vec![] }),
                ["property", "list", ct, it, name] => {
                    let (Some(ct), Some(it)) = (Ty::parse(ct), Ty::parse(it)) else { return Err(malformed("bad list type")) };
                    elems.last_mut().ok_or_else(|| malformed("property before element"))?.props.push(Prop { name: name.to_string(), kind: Kind::List(ct, it) });
                }
                ["property", ty, name] => {
                    let ty = Ty::parse(ty).ok_or_else(|| malformed("bad property type"))?;
                    elems.last_mut().ok_or_else(|| malformed("property before element"))?.props.push(Prop { name: name.to_string(), kind: Kind::Scalar(ty) });
                }
                _ => {}
            }
        }
        let body = &bytes[body_start..];
        let mut mesh = Mesh::default();
        match format {
            Some("ascii") => {
                let text = String::from_utf8_lossy(body);
                read_all(&mut AsciiSrc(text.split_ascii_whitespace()), &elems, &mut mesh, ctx)?;
            }
            Some(f @ ("binary_little_endian" | "binary_big_endian")) => {
                read_all(&mut BinSrc { d: body, pos: 0, le: f == "binary_little_endian" }, &elems, &mut mesh, ctx)?;
            }
            _ => return Err(malformed("unknown PLY format")),
        }
        finish(mesh, ctx)
    }
}

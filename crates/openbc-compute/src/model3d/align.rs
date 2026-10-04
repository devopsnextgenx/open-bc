//! Alignment of B onto A: quick modes, manual offset, ICP refinement and per-part alignment.
//! All of it produces rigid transforms in A's frame; deviation fields are recomputed after.
use super::bvh::Bvh;
use super::ComputeError;
use openbc_core::model3d::mesh::*;
use openbc_core::model3d::*;
use rayon::prelude::*;
use std::sync::Arc;

/// Per-part adjustments applied to B after the global alignment.
#[derive(Clone)]
pub struct PartAlign {
    pub vertex_part: Vec<u32>,
    pub extra: Vec<Rigid>,
    pub matched: usize,
}

#[derive(Clone)]
pub struct AlignState {
    pub mode: AlignmentMode,
    pub offset: [f32; 3],
    /// ICP result, accumulated on top of mode + offset.
    pub refine: Rigid,
    pub parts: Option<Arc<PartAlign>>,
    /// Bumps on every change; keys the cached BVH / deviation fields.
    pub version: u64,
}

impl AlignState {
    pub fn new(mode: AlignmentMode, offset: [f32; 3], version: u64) -> Self {
        Self { mode, offset, refine: Rigid::IDENTITY, parts: None, version }
    }
}

/// (translation applied to B, shift applied to the *display* of both models).
pub fn base_translation(mode: AlignmentMode, a: &MeshStats, b: &MeshStats) -> (V3, V3) {
    match mode {
        AlignmentMode::Original => ([0.0; 3], [0.0; 3]),
        AlignmentMode::CenterAtOrigin => {
            let (ca, cb) = (a.bbox.center(), b.bbox.center());
            (sub(ca, cb), mul(ca, -1.0))
        }
        AlignmentMode::BoundingBoxCorners => (sub(a.bbox.min, b.bbox.min), [0.0; 3]),
        AlignmentMode::Centroids => (sub(a.centroid, b.centroid), [0.0; 3]),
    }
}

/// B in A's frame under `st`.
pub fn apply_alignment(b: &Mesh, a_stats: &MeshStats, b_stats: &MeshStats, st: &AlignState) -> (Mesh, V3) {
    let (shift, display) = base_translation(st.mode, a_stats, b_stats);
    let t = add(shift, st.offset);
    let total = Rigid::translation([t[0] as f64, t[1] as f64, t[2] as f64]).then(&st.refine);
    let mut m = b.clone();
    match &st.parts {
        None => m.positions.par_iter_mut().for_each(|p| *p = total.apply(*p)),
        Some(pa) => {
            let extra = &pa.extra;
            m.positions.par_iter_mut().zip(pa.vertex_part.par_iter()).for_each(|(p, &part)| {
                let q = total.apply(*p);
                *p = if (part as usize) < extra.len() { extra[part as usize].apply(q) } else { q };
            });
        }
    }
    (m, display)
}

// ---------------------------------------------------------------------------------- ICP

/// Rigid ICP (point-to-surface, trimmed). `points` are B's current positions; the returned
/// transform moves them onto `target`. Starts from wherever `points` already are.
pub fn icp(points: &[V3], target: &Bvh, iterations: usize, cancel: &CancelToken) -> Result<Rigid, ComputeError> {
    let diag = target.bounds.diagonal().max(1e-9) as f64;
    let mut cur: Vec<V3> = points.to_vec();
    let mut total = Rigid::IDENTITY;
    for _ in 0..iterations {
        if cancel.is_cancelled() { return Err(ComputeError::Cancelled); }
        let pairs: Vec<(V3, V3, f32)> = cur.par_iter().map(|&p| match target.closest(p) {
            Some(h) => (p, h.point, h.signed.abs()),
            None => (p, p, f32::INFINITY),
        }).collect();
        let mut d: Vec<f32> = pairs.iter().map(|x| x.2).collect();
        if d.is_empty() { break; }
        let k = ((d.len() as f32 * 0.85) as usize).min(d.len() - 1);
        d.select_nth_unstable_by(k, |a, b| a.total_cmp(b));
        let thr = d[k];
        let sel: Vec<&(V3, V3, f32)> = pairs.iter().filter(|x| x.2.is_finite() && x.2 <= thr).collect();
        if sel.len() < 3 { break; }
        let Some(delta) = horn(sel.iter().map(|x| (x.0, x.1))) else { break };
        cur.par_iter_mut().for_each(|p| *p = delta.apply(*p));
        total = total.then(&delta);
        let tn = (delta.t[0].powi(2) + delta.t[1].powi(2) + delta.t[2].powi(2)).sqrt();
        let rot = (delta.r[0][0] + delta.r[1][1] + delta.r[2][2] - 3.0).abs();
        if tn < 1e-7 * diag && rot < 1e-12 { break; }
    }
    Ok(total)
}

/// Horn's closed-form absolute orientation (unit quaternion from the dominant eigenvector).
fn horn(pairs: impl Iterator<Item = (V3, V3)> + Clone) -> Option<Rigid> {
    let (mut n, mut cp, mut cq) = (0f64, [0f64; 3], [0f64; 3]);
    for (p, q) in pairs.clone() { n += 1.0; for k in 0..3 { cp[k] += p[k] as f64; cq[k] += q[k] as f64; } }
    if n < 3.0 { return None; }
    for k in 0..3 { cp[k] /= n; cq[k] /= n; }
    let mut s = [[0f64; 3]; 3];
    for (p, q) in pairs {
        for i in 0..3 { for j in 0..3 { s[i][j] += (p[i] as f64 - cp[i]) * (q[j] as f64 - cq[j]); } }
    }
    let (sxx, sxy, sxz, syx, syy, syz, szx, szy, szz) = (s[0][0], s[0][1], s[0][2], s[1][0], s[1][1], s[1][2], s[2][0], s[2][1], s[2][2]);
    let nm = [
        [sxx + syy + szz, syz - szy, szx - sxz, sxy - syx],
        [syz - szy, sxx - syy - szz, sxy + syx, szx + sxz],
        [szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy],
        [sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz],
    ];
    let (vals, vecs) = jacobi4(nm);
    let j = (0..4).max_by(|&a, &b| vals[a].total_cmp(&vals[b]))?;
    let (w, x, y, z) = (vecs[0][j], vecs[1][j], vecs[2][j], vecs[3][j]);
    let r = [
        [w * w + x * x - y * y - z * z, 2.0 * (x * y - w * z), 2.0 * (x * z + w * y)],
        [2.0 * (x * y + w * z), w * w - x * x + y * y - z * z, 2.0 * (y * z - w * x)],
        [2.0 * (x * z - w * y), 2.0 * (y * z + w * x), w * w - x * x - y * y + z * z],
    ];
    let mut t = [0f64; 3];
    for i in 0..3 { t[i] = cq[i] - (r[i][0] * cp[0] + r[i][1] * cp[1] + r[i][2] * cp[2]); }
    Some(Rigid { r, t })
}

fn jacobi4(mut a: [[f64; 4]; 4]) -> ([f64; 4], [[f64; 4]; 4]) {
    let mut v = [[0f64; 4]; 4];
    for i in 0..4 { v[i][i] = 1.0; }
    for _ in 0..64 {
        let (mut p, mut q, mut mx) = (0, 1, 0f64);
        for i in 0..4 { for j in i + 1..4 { if a[i][j].abs() > mx { mx = a[i][j].abs(); p = i; q = j; } } }
        if mx < 1e-14 { break; }
        let theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        let t = theta.signum() / (theta.abs() + (theta * theta + 1.0).sqrt());
        let c = 1.0 / (t * t + 1.0).sqrt();
        let s = t * c;
        for k in 0..4 { let (x, y) = (a[k][p], a[k][q]); a[k][p] = c * x - s * y; a[k][q] = s * x + c * y; }
        for k in 0..4 { let (x, y) = (a[p][k], a[q][k]); a[p][k] = c * x - s * y; a[q][k] = s * x + c * y; }
        for k in 0..4 { let (x, y) = (v[k][p], v[k][q]); v[k][p] = c * x - s * y; v[k][q] = s * x + c * y; }
    }
    ([a[0][0], a[1][1], a[2][2], a[3][3]], v)
}

/// Evenly strided subset of at most `max` points.
pub fn sample_stride(points: &[V3], max: usize) -> Vec<V3> {
    let step = (points.len() / max.max(1)).max(1);
    points.iter().step_by(step).copied().collect()
}

// ------------------------------------------------------------------------ per-part alignment

struct PartInfo { bbox: Aabb, area: f64 }

fn part_infos(mesh: &Mesh, comps: &Components) -> Vec<PartInfo> {
    let mut v: Vec<PartInfo> = (0..comps.count).map(|_| PartInfo { bbox: Aabb::EMPTY, area: 0.0 }).collect();
    for (i, &c) in comps.tri_comp.iter().enumerate() {
        let t = mesh.triangle(i);
        let p = &mut v[c as usize];
        for x in t { p.bbox.extend(x); }
        p.area += 0.5 * len(cross(sub(t[1], t[0]), sub(t[2], t[0]))) as f64;
    }
    v
}

fn signature(p: &PartInfo) -> [f64; 4] {
    let s = p.bbox.size();
    let mut e = [s[0] as f64, s[1] as f64, s[2] as f64];
    e.sort_by(|a, b| a.total_cmp(b));
    [p.area.max(1e-9), e[0].max(1e-6), e[1].max(1e-6), e[2].max(1e-6)]
}

fn tris_by_comp(comps: &Components) -> Vec<Vec<u32>> {
    let mut v = vec![Vec::new(); comps.count];
    for (i, &c) in comps.tri_comp.iter().enumerate() { v[c as usize].push(i as u32); }
    v
}

fn submesh(mesh: &Mesh, tris: &[u32]) -> Mesh {
    let mut map: FxMap<u32, u32> = FxMap::default();
    let mut out = Mesh::default();
    for &t in tris {
        for k in 0..3 {
            let vi = mesh.indices[t as usize * 3 + k];
            let id = *map.entry(vi).or_insert_with(|| { out.positions.push(mesh.positions[vi as usize]); (out.positions.len() - 1) as u32 });
            out.indices.push(id);
        }
    }
    out
}

/// Matches every disconnected part of B to the most similar part of A (area + sorted bbox
/// extents) and fits it individually (centre translation, then trimmed ICP), so re-arranged
/// plates compare as if nothing moved. Parts without a good match keep the global alignment.
pub fn align_parts(a: &Mesh, ca: &Components, b_world: &Mesh, cb: &Components, cancel: &CancelToken) -> Result<PartAlign, ComputeError> {
    const MAX_PARTS: usize = 2000;
    let (ia, ib) = (part_infos(a, ca), part_infos(b_world, cb));
    let top = |v: &[PartInfo]| { let mut ids: Vec<usize> = (0..v.len()).collect(); ids.sort_by(|&x, &y| v[y].area.total_cmp(&v[x].area)); ids.truncate(MAX_PARTS); ids };
    let (ta, tb) = (top(&ia), top(&ib));
    let mut cand: Vec<(f64, usize, usize)> = Vec::new();
    for &j in &tb {
        let sb = signature(&ib[j]);
        for &i in &ta {
            let sa = signature(&ia[i]);
            let cost: f64 = (0..4).map(|k| (sa[k] / sb[k]).ln().abs()).sum();
            if cost < 0.6 { cand.push((cost, i, j)); }
        }
    }
    cand.sort_by(|x, y| x.0.total_cmp(&y.0));
    let (mut used_a, mut used_b) = (vec![false; ia.len()], vec![false; ib.len()]);
    let mut pairs = Vec::new();
    for (_, i, j) in cand { if !used_a[i] && !used_b[j] { used_a[i] = true; used_b[j] = true; pairs.push((i, j)); } }

    let (ta_tris, tb_tris) = (tris_by_comp(ca), tris_by_comp(cb));
    let fitted: Vec<(usize, Rigid)> = pairs.par_iter().map(|&(i, j)| -> Result<(usize, Rigid), ComputeError> {
        let a_part = submesh(a, &ta_tris[i]);
        let b_part = submesh(b_world, &tb_tris[j]);
        let bvh = Bvh::build(&a_part);
        let (ca_, cb_) = (ia[i].bbox.center(), ib[j].bbox.center());
        let t0 = Rigid::translation([(ca_[0] - cb_[0]) as f64, (ca_[1] - cb_[1]) as f64, (ca_[2] - cb_[2]) as f64]);
        let pts: Vec<V3> = sample_stride(&b_part.positions, 4000).into_iter().map(|p| t0.apply(p)).collect();
        let delta = icp(&pts, &bvh, 15, cancel)?;
        Ok((j, t0.then(&delta)))
    }).collect::<Result<_, _>>()?;

    let mut extra = vec![Rigid::IDENTITY; cb.count];
    for (j, r) in &fitted { extra[*j] = *r; }
    Ok(PartAlign { vertex_part: b_world.vertex_components(cb), extra, matched: fitted.len() })
}

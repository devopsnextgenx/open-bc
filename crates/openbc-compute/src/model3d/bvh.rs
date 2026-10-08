//! Flat bounding-volume hierarchy over a triangle mesh.
//!
//! Layout (also uploaded verbatim to the GPU): `nodes[0]` is the root. A node with
//! `count == 0` is internal: its left child is `index + 1`, its right child is `a`.
//! A node with `count > 0` is a leaf covering `tris[a .. a + count]`. Triangles are stored
//! reordered (`tris`), `tri_ids` maps back to the mesh's triangle index.
use super::geom::centroid;
use openbc_core::model3d::mesh::*;
use rayon::prelude::*;

pub const LEAF_SIZE: usize = 4;
const PAR_THRESHOLD: usize = 8192;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Node { pub min: V3, pub a: u32, pub max: V3, pub count: u32 }

impl Node {
    /// Triangle range `tris[a .. a + count]` covered by a leaf.
    #[inline]
    pub fn leaf_range(&self) -> std::ops::Range<usize> { self.a as usize..(self.a + self.count) as usize }
}

pub struct Bvh {
    pub nodes: Vec<Node>,
    pub tris: Vec<[V3; 3]>,
    pub tri_ids: Vec<u32>,
    pub bounds: Aabb,
}

#[derive(Clone, Copy, Debug)]
pub struct Hit {
    /// Signed distance: > 0 when the query lies on the side the nearest triangle's normal faces.
    pub signed: f32,
    pub point: V3,
    /// Triangle index in the source mesh.
    pub tri: u32,
}

#[derive(Clone, Copy)]
struct Item { min: V3, max: V3, c: V3 }

impl Bvh {
    pub fn build(mesh: &Mesh) -> Bvh {
        let n = mesh.triangle_count();
        let items: Vec<Item> = (0..n).into_par_iter().map(|i| {
            let t = mesh.triangle(i);
            let (mut mn, mut mx) = (t[0], t[0]);
            for v in &t[1..] { for k in 0..3 { mn[k] = mn[k].min(v[k]); mx[k] = mx[k].max(v[k]); } }
            Item { min: mn, max: mx, c: centroid(t[0], t[1], t[2]) }
        }).collect();
        let mut ids: Vec<u32> = (0..n as u32).collect();
        let nodes = if n == 0 { Vec::new() } else { build_range(&mut ids, &items) };
        let tris: Vec<[V3; 3]> = ids.par_iter().map(|&i| mesh.triangle(i as usize)).collect();
        let bounds = nodes.first().map(|nd| Aabb { min: nd.min, max: nd.max }).unwrap_or(Aabb::EMPTY);
        Bvh { nodes, tris, tri_ids: ids, bounds }
    }

    pub fn is_empty(&self) -> bool { self.nodes.is_empty() }

    /// Closest point on the surface, with a signed distance (see `Hit::signed`).
    pub fn closest(&self, p: V3) -> Option<Hit> {
        if self.nodes.is_empty() { return None; }
        let (mut best, mut best_k, mut best_q) = (f32::INFINITY, 0usize, p);
        let mut stack = [0u32; 64];
        let mut sp = 1usize;
        while sp > 0 {
            sp -= 1;
            let ni = stack[sp] as usize;
            let nd = &self.nodes[ni];
            if box_d2(p, nd) >= best { continue; }
            if nd.count > 0 {
                for k in nd.leaf_range() {
                    let (q, d2) = closest_on_tri(p, &self.tris[k]);
                    if d2 < best { best = d2; best_k = k; best_q = q; }
                }
            } else {
                let (l, r) = (ni + 1, nd.a as usize);
                let (dl, dr) = (box_d2(p, &self.nodes[l]), box_d2(p, &self.nodes[r]));
                // push the farther child first so the nearer one is popped next
                let (far, far_d, near, near_d) = if dl < dr { (r, dr, l, dl) } else { (l, dl, r, dr) };
                if far_d < best { stack[sp] = far as u32; sp += 1; }
                if near_d < best { stack[sp] = near as u32; sp += 1; }
            }
        }
        if !best.is_finite() { return None; }
        let t = &self.tris[best_k];
        let n = cross(sub(t[1], t[0]), sub(t[2], t[0]));
        let s = if dot(sub(p, best_q), n) < 0.0 { -1.0 } else { 1.0 };
        Some(Hit { signed: s * best.sqrt(), point: best_q, tri: self.tri_ids[best_k] })
    }

    /// Nearest ray/triangle intersection (double sided). Returns (t, source triangle).
    pub fn raycast(&self, o: V3, d: V3) -> Option<(f32, u32)> {
        if self.nodes.is_empty() { return None; }
        let inv = [1.0 / d[0], 1.0 / d[1], 1.0 / d[2]];
        let (mut best_t, mut best_k) = (f32::INFINITY, usize::MAX);
        let mut stack = [0u32; 64];
        let mut sp = 1usize;
        while sp > 0 {
            sp -= 1;
            let ni = stack[sp] as usize;
            let nd = &self.nodes[ni];
            if !ray_box(o, inv, nd, best_t) { continue; }
            if nd.count > 0 {
                for k in nd.leaf_range() {
                    if let Some(t) = ray_tri(o, d, &self.tris[k]) { if t < best_t { best_t = t; best_k = k; } }
                }
            } else { push_children(&mut stack, &mut sp, ni, nd); }
        }
        (best_k != usize::MAX).then(|| (best_t, self.tri_ids[best_k]))
    }

    /// All x positions where the line (x, y, z) with fixed (y, z) crosses the surface.
    /// Used for scanline voxelisation; callers jitter y/z so edges are never hit exactly.
    pub fn x_hits(&self, y: f32, z: f32, out: &mut Vec<f32>) {
        out.clear();
        if self.nodes.is_empty() { return; }
        let mut stack = [0u32; 64];
        let mut sp = 1usize;
        while sp > 0 {
            sp -= 1;
            let ni = stack[sp] as usize;
            let nd = &self.nodes[ni];
            if y < nd.min[1] || y > nd.max[1] || z < nd.min[2] || z > nd.max[2] { continue; }
            if nd.count > 0 {
                for k in nd.leaf_range() {
                    let t = &self.tris[k];
                    let (ay, az) = (t[0][1], t[0][2]);
                    let (e1y, e1z, e2y, e2z) = (t[1][1] - ay, t[1][2] - az, t[2][1] - ay, t[2][2] - az);
                    let det = e1y * e2z - e2y * e1z;
                    if det.abs() < 1e-20 { continue; }
                    let (py, pz) = (y - ay, z - az);
                    let u = (py * e2z - e2y * pz) / det;
                    let v = (e1y * pz - py * e1z) / det;
                    if u >= 0.0 && v >= 0.0 && u + v <= 1.0 {
                        out.push(t[0][0] + u * (t[1][0] - t[0][0]) + v * (t[2][0] - t[0][0]));
                    }
                }
            } else { push_children(&mut stack, &mut sp, ni, nd); }
        }
        out.sort_unstable_by(|a, b| a.total_cmp(b));
    }

    /// Triangles as 3 x vec4 (xyz + pad) for GPU storage buffers.
    pub fn gpu_triangles(&self) -> Vec<[f32; 4]> {
        self.tris.iter().flat_map(|t| [[t[0][0], t[0][1], t[0][2], 0.0], [t[1][0], t[1][1], t[1][2], 0.0], [t[2][0], t[2][1], t[2][2], 0.0]]).collect()
    }
}

fn build_range(ids: &mut [u32], items: &[Item]) -> Vec<Node> {
    let (mut bmin, mut bmax) = ([f32::INFINITY; 3], [f32::NEG_INFINITY; 3]);
    let (mut cmin, mut cmax) = (bmin, bmax);
    for &i in ids.iter() {
        let it = &items[i as usize];
        for k in 0..3 {
            bmin[k] = bmin[k].min(it.min[k]); bmax[k] = bmax[k].max(it.max[k]);
            cmin[k] = cmin[k].min(it.c[k]); cmax[k] = cmax[k].max(it.c[k]);
        }
    }
    let ext = [cmax[0] - cmin[0], cmax[1] - cmin[1], cmax[2] - cmin[2]];
    let axis = if ext[0] >= ext[1] && ext[0] >= ext[2] { 0 } else if ext[1] >= ext[2] { 1 } else { 2 };
    let total = ids.len();
    if total <= LEAF_SIZE || (ext[axis] <= 0.0 && total <= 16) {
        return vec![Node { min: bmin, a: 0, max: bmax, count: total as u32 }];
    }
    let mid = total / 2;
    if ext[axis] > 0.0 {
        ids.select_nth_unstable_by(mid, |&x, &y| items[x as usize].c[axis].total_cmp(&items[y as usize].c[axis]));
    }
    let (l, r) = ids.split_at_mut(mid);
    let (ln, rn) = if total >= PAR_THRESHOLD {
        rayon::join(|| build_range(l, items), || build_range(r, items))
    } else {
        (build_range(l, items), build_range(r, items))
    };
    let ll = ln.len();
    let mut out = Vec::with_capacity(1 + ll + rn.len());
    out.push(Node { min: bmin, a: (1 + ll) as u32, max: bmax, count: 0 });
    out.extend(ln.into_iter().map(|mut n| { if n.count == 0 { n.a += 1; } n }));
    out.extend(rn.into_iter().map(|mut n| { if n.count == 0 { n.a += (1 + ll) as u32; } else { n.a += mid as u32; } n }));
    out
}

/// Pushes both children of an internal node (right first, so the left one is visited next).
#[inline]
fn push_children(stack: &mut [u32; 64], sp: &mut usize, ni: usize, nd: &Node) {
    stack[*sp] = nd.a;
    stack[*sp + 1] = (ni + 1) as u32;
    *sp += 2;
}

#[inline]
fn box_d2(p: V3, n: &Node) -> f32 {
    let mut d2 = 0.0;
    for k in 0..3 {
        let d = (n.min[k] - p[k]).max(p[k] - n.max[k]).max(0.0);
        d2 += d * d;
    }
    d2
}

#[inline]
fn ray_box(o: V3, inv: V3, n: &Node, tmax: f32) -> bool {
    let (mut t0, mut t1) = (0.0f32, tmax);
    for k in 0..3 {
        let (a, b) = ((n.min[k] - o[k]) * inv[k], (n.max[k] - o[k]) * inv[k]);
        let (lo, hi) = if a < b { (a, b) } else { (b, a) };
        // NaN (0 * inf) comparisons are false, which keeps the slab: conservative and correct.
        if lo > t0 { t0 = lo; }
        if hi < t1 { t1 = hi; }
        if t0 > t1 { return false; }
    }
    true
}

#[inline]
fn ray_tri(o: V3, d: V3, t: &[V3; 3]) -> Option<f32> {
    let (e1, e2) = (sub(t[1], t[0]), sub(t[2], t[0]));
    let p = cross(d, e2);
    let det = dot(e1, p);
    if det.abs() < 1e-20 { return None; }
    let inv = 1.0 / det;
    let s = sub(o, t[0]);
    let u = dot(s, p) * inv;
    if !(0.0..=1.0).contains(&u) { return None; }
    let q = cross(s, e1);
    let v = dot(d, q) * inv;
    if v < 0.0 || u + v > 1.0 { return None; }
    let tt = dot(e2, q) * inv;
    (tt > 1e-6).then_some(tt)
}

/// Ericson, "Real-Time Collision Detection" 5.1.5. Returns (closest point, squared distance).
#[inline]
pub fn closest_on_tri(p: V3, t: &[V3; 3]) -> (V3, f32) {
    let (a, b, c) = (t[0], t[1], t[2]);
    let (ab, ac, ap) = (sub(b, a), sub(c, a), sub(p, a));
    let d = |q: V3| { let v = sub(p, q); dot(v, v) };
    let (d1, d2) = (dot(ab, ap), dot(ac, ap));
    if d1 <= 0.0 && d2 <= 0.0 { return (a, d(a)); }
    let bp = sub(p, b);
    let (d3, d4) = (dot(ab, bp), dot(ac, bp));
    if d3 >= 0.0 && d4 <= d3 { return (b, d(b)); }
    let vc = d1 * d4 - d3 * d2;
    if vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0 {
        let q = add(a, mul(ab, d1 / (d1 - d3)));
        return (q, d(q));
    }
    let cp = sub(p, c);
    let (d5, d6) = (dot(ab, cp), dot(ac, cp));
    if d6 >= 0.0 && d5 <= d6 { return (c, d(c)); }
    let vb = d5 * d2 - d1 * d6;
    if vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0 {
        let q = add(a, mul(ac, d2 / (d2 - d6)));
        return (q, d(q));
    }
    let va = d3 * d6 - d5 * d4;
    if va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0 {
        let q = add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
        return (q, d(q));
    }
    let denom = 1.0 / (va + vb + vc);
    let q = add(add(a, mul(ab, vb * denom)), mul(ac, vc * denom));
    (q, d(q))
}

//! Triangle mesh + the small amount of pure geometry the whole stack shares.
//! All units are millimetres (loaders convert on the way in).
use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};

pub type V3 = [f32; 3];

#[inline] pub fn sub(a: V3, b: V3) -> V3 { [a[0] - b[0], a[1] - b[1], a[2] - b[2]] }
#[inline] pub fn add(a: V3, b: V3) -> V3 { [a[0] + b[0], a[1] + b[1], a[2] + b[2]] }
#[inline] pub fn mul(a: V3, s: f32) -> V3 { [a[0] * s, a[1] * s, a[2] * s] }
#[inline] pub fn dot(a: V3, b: V3) -> f32 { a[0] * b[0] + a[1] * b[1] + a[2] * b[2] }
#[inline] pub fn cross(a: V3, b: V3) -> V3 {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
#[inline] pub fn len(a: V3) -> f32 { dot(a, a).sqrt() }

/// Cheap multiplicative hasher for integer keys (vertex welding, edge maps).
#[derive(Default)]
pub struct FxHasher(u64);
impl Hasher for FxHasher {
    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes { self.write_u64(b as u64); }
    }
    fn write_u64(&mut self, i: u64) { self.0 = (self.0.rotate_left(5) ^ i).wrapping_mul(0x517c_c1b7_2722_0a95); }
    fn write_i64(&mut self, i: i64) { self.write_u64(i as u64); }
    fn finish(&self) -> u64 { self.0 }
}
pub type FxMap<K, V> = HashMap<K, V, BuildHasherDefault<FxHasher>>;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Aabb { pub min: V3, pub max: V3 }

impl Aabb {
    pub const EMPTY: Aabb = Aabb { min: [f32::INFINITY; 3], max: [f32::NEG_INFINITY; 3] };
    pub fn is_empty(&self) -> bool { self.min[0] > self.max[0] }
    pub fn extend(&mut self, p: V3) {
        for k in 0..3 { self.min[k] = self.min[k].min(p[k]); self.max[k] = self.max[k].max(p[k]); }
    }
    pub fn union(&self, o: &Aabb) -> Aabb {
        let mut r = *self;
        if !o.is_empty() { r.extend(o.min); r.extend(o.max); }
        r
    }
    pub fn center(&self) -> V3 { mul(add(self.min, self.max), 0.5) }
    pub fn size(&self) -> V3 { sub(self.max, self.min) }
    pub fn diagonal(&self) -> f32 { if self.is_empty() { 0.0 } else { len(self.size()) } }
}

/// Rigid transform p' = R p + t, kept in f64 so chained alignments do not drift.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rigid { pub r: [[f64; 3]; 3], pub t: [f64; 3] }

impl Rigid {
    pub const IDENTITY: Rigid = Rigid { r: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]], t: [0.0; 3] };
    pub fn translation(t: [f64; 3]) -> Rigid { Rigid { r: Self::IDENTITY.r, t } }
    #[inline]
    pub fn apply(&self, p: V3) -> V3 {
        let (x, y, z) = (p[0] as f64, p[1] as f64, p[2] as f64);
        let r = &self.r;
        [
            (r[0][0] * x + r[0][1] * y + r[0][2] * z + self.t[0]) as f32,
            (r[1][0] * x + r[1][1] * y + r[1][2] * z + self.t[1]) as f32,
            (r[2][0] * x + r[2][1] * y + r[2][2] * z + self.t[2]) as f32,
        ]
    }
    /// `self` first, then `next`.
    pub fn then(&self, next: &Rigid) -> Rigid {
        let mut r = [[0.0; 3]; 3];
        for i in 0..3 { for j in 0..3 { for k in 0..3 { r[i][j] += next.r[i][k] * self.r[k][j]; } } }
        let mut t = next.t;
        for i in 0..3 { for k in 0..3 { t[i] += next.r[i][k] * self.t[k]; } }
        Rigid { r, t }
    }
    pub fn inverse(&self) -> Rigid {
        let mut r = [[0.0; 3]; 3];
        for i in 0..3 { for j in 0..3 { r[i][j] = self.r[j][i]; } }
        let mut t = [0.0; 3];
        for i in 0..3 { for k in 0..3 { t[i] -= r[i][k] * self.t[k]; } }
        Rigid { r, t }
    }
}

#[derive(Clone, Debug, Default)]
pub struct Mesh {
    pub positions: Vec<V3>,
    /// Three indices per triangle.
    pub indices: Vec<u32>,
}

#[derive(Clone, Debug)]
pub struct MeshStats {
    pub triangles: u64,
    pub vertices: u64,
    pub area: f64,
    pub volume: f64,
    pub watertight: bool,
    pub bbox: Aabb,
    pub centroid: V3,
    pub size_bytes: u64,
}

/// Connected components (triangles sharing a vertex), ids in first-seen order.
#[derive(Clone, Debug, Default)]
pub struct Components { pub tri_comp: Vec<u32>, pub count: usize }

impl Mesh {
    pub fn vertex_count(&self) -> usize { self.positions.len() }
    pub fn triangle_count(&self) -> usize { self.indices.len() / 3 }
    #[inline]
    pub fn triangle(&self, i: usize) -> [V3; 3] {
        let k = i * 3;
        [self.positions[self.indices[k] as usize], self.positions[self.indices[k + 1] as usize], self.positions[self.indices[k + 2] as usize]]
    }
    pub fn memory_bytes(&self) -> u64 { (self.positions.len() * 12 + self.indices.len() * 4) as u64 }

    pub fn aabb(&self) -> Aabb {
        let mut b = Aabb::EMPTY;
        for &p in &self.positions { b.extend(p); }
        b
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.indices.len() % 3 != 0 { return Err("index count is not a multiple of 3".into()); }
        let n = self.positions.len() as u32;
        if self.indices.iter().any(|&i| i >= n) { return Err("triangle index out of range".into()); }
        if self.positions.iter().any(|p| !(p[0].is_finite() && p[1].is_finite() && p[2].is_finite())) {
            return Err("non-finite vertex coordinate".into());
        }
        Ok(())
    }

    pub fn scale(&mut self, s: f32) { for p in &mut self.positions { *p = mul(*p, s); } }

    pub fn transform(&mut self, t: &Rigid) { for p in &mut self.positions { *p = t.apply(*p); } }

    pub fn append(&mut self, other: &Mesh) {
        let base = self.positions.len() as u32;
        self.positions.extend_from_slice(&other.positions);
        self.indices.extend(other.indices.iter().map(|i| i + base));
    }

    /// Merges coincident vertices (1e-6 of the bbox diagonal) and drops degenerate triangles.
    /// STL and many OBJ files are "triangle soup"; every adjacency-based feature needs this.
    pub fn weld(&mut self) {
        let bb = self.aabb();
        if bb.is_empty() { return; }
        let inv = 1.0 / (bb.diagonal() as f64 * 1e-6).max(1e-12);
        let mut map: FxMap<(i64, i64, i64), u32> = FxMap::default();
        map.reserve(self.positions.len() / 2 + 1);
        let mut out: Vec<V3> = Vec::with_capacity(self.positions.len() / 2 + 1);
        let mut remap: Vec<u32> = Vec::with_capacity(self.positions.len());
        for p in &self.positions {
            let key = ((p[0] as f64 * inv).round() as i64, (p[1] as f64 * inv).round() as i64, (p[2] as f64 * inv).round() as i64);
            let id = *map.entry(key).or_insert_with(|| { out.push(*p); (out.len() - 1) as u32 });
            remap.push(id);
        }
        let mut idx = Vec::with_capacity(self.indices.len());
        for t in self.indices.chunks_exact(3) {
            let (a, b, c) = (remap[t[0] as usize], remap[t[1] as usize], remap[t[2] as usize]);
            if a != b && b != c && a != c { idx.extend_from_slice(&[a, b, c]); }
        }
        self.positions = out;
        self.indices = idx;
    }

    pub fn stats(&self, size_bytes: u64) -> MeshStats {
        let (mut area, mut vol) = (0f64, 0f64);
        for t in self.indices.chunks_exact(3) {
            let p = |i: u32| { let v = self.positions[i as usize]; [v[0] as f64, v[1] as f64, v[2] as f64] };
            let (a, b, c) = (p(t[0]), p(t[1]), p(t[2]));
            let cr = |x: [f64; 3], y: [f64; 3]| [x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]];
            let n = cr([b[0] - a[0], b[1] - a[1], b[2] - a[2]], [c[0] - a[0], c[1] - a[1], c[2] - a[2]]);
            area += 0.5 * (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt();
            let bc = cr(b, c);
            vol += (a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2]) / 6.0;
        }
        let mut cen = [0f64; 3];
        for p in &self.positions { for k in 0..3 { cen[k] += p[k] as f64; } }
        let nv = self.positions.len().max(1) as f64;
        MeshStats {
            triangles: self.triangle_count() as u64,
            vertices: self.positions.len() as u64,
            area,
            volume: vol.abs(),
            watertight: self.is_watertight(),
            bbox: self.aabb(),
            centroid: [(cen[0] / nv) as f32, (cen[1] / nv) as f32, (cen[2] / nv) as f32],
            size_bytes,
        }
    }

    /// Every undirected edge shared by exactly two triangles.
    pub fn is_watertight(&self) -> bool {
        if self.indices.is_empty() { return false; }
        let mut keys = self.unique_edge_keys(true);
        keys.sort_unstable();
        let mut i = 0;
        while i < keys.len() {
            let mut j = i;
            while j < keys.len() && keys[j] == keys[i] { j += 1; }
            if j - i != 2 { return false; }
            i = j;
        }
        true
    }

    fn unique_edge_keys(&self, keep_duplicates: bool) -> Vec<u64> {
        let mut keys = Vec::with_capacity(self.indices.len());
        for t in self.indices.chunks_exact(3) {
            for (a, b) in [(t[0], t[1]), (t[1], t[2]), (t[2], t[0])] {
                keys.push(((a.min(b) as u64) << 32) | a.max(b) as u64);
            }
        }
        if !keep_duplicates { keys.sort_unstable(); keys.dedup(); }
        keys
    }

    /// Unique edges as index pairs (for the wireframe view).
    pub fn unique_edges(&self) -> Vec<u32> {
        self.unique_edge_keys(false).into_iter().flat_map(|k| [(k >> 32) as u32, (k & 0xffff_ffff) as u32]).collect()
    }

    pub fn components(&self) -> Components {
        let nv = self.positions.len();
        let mut parent: Vec<u32> = (0..nv as u32).collect();
        fn find(p: &mut [u32], mut x: u32) -> u32 {
            while p[x as usize] != x { p[x as usize] = p[p[x as usize] as usize]; x = p[x as usize]; }
            x
        }
        for t in self.indices.chunks_exact(3) {
            for (a, b) in [(t[0], t[1]), (t[1], t[2])] {
                let (ra, rb) = (find(&mut parent, a), find(&mut parent, b));
                if ra != rb { parent[rb as usize] = ra; }
            }
        }
        let mut ids: FxMap<u32, u32> = FxMap::default();
        let mut tri_comp = Vec::with_capacity(self.triangle_count());
        for t in self.indices.chunks_exact(3) {
            let r = find(&mut parent, t[0]);
            let next = ids.len() as u32;
            tri_comp.push(*ids.entry(r).or_insert(next));
        }
        Components { count: ids.len(), tri_comp }
    }

    /// Component id per vertex (`u32::MAX` for unreferenced vertices).
    pub fn vertex_components(&self, comps: &Components) -> Vec<u32> {
        let mut v = vec![u32::MAX; self.positions.len()];
        for (t, c) in self.indices.chunks_exact(3).zip(&comps.tri_comp) {
            for &i in t { v[i as usize] = *c; }
        }
        v
    }
}

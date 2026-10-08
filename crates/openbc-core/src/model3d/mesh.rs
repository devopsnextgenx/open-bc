//! Triangle mesh and the pure topology/statistics helpers the whole stack shares.
//! All units are millimetres (loaders convert on the way in).
//!
//! The vector math, hasher, bounding box and rigid transform live in sibling modules and
//! are re-exported here so `model3d::mesh::*` keeps its original surface.

pub use super::bounds::Aabb;
pub use super::hash::{FxHasher, FxMap};
pub use super::math::{add, cross, dot, len, mul, sub, V3};
pub use super::rigid::Rigid;

use super::math::{cross64, dot64, sub64, widen};

/// Indexed triangle mesh.
#[derive(Clone, Debug, Default)]
pub struct Mesh {
    /// Vertex positions.
    pub positions: Vec<V3>,
    /// Three indices per triangle.
    pub indices: Vec<u32>,
}

/// Summary figures for one loaded mesh.
#[derive(Clone, Debug)]
pub struct MeshStats {
    /// Triangle count.
    pub triangles: u64,
    /// Vertex count.
    pub vertices: u64,
    /// Total surface area.
    pub area: f64,
    /// Enclosed volume (absolute value of the signed volume).
    pub volume: f64,
    /// Every edge is shared by exactly two triangles.
    pub watertight: bool,
    /// Bounding box.
    pub bbox: Aabb,
    /// Mean of the vertex positions.
    pub centroid: V3,
    /// Size of the source file in bytes.
    pub size_bytes: u64,
}

/// Connected components (triangles sharing a vertex), ids in first-seen order.
#[derive(Clone, Debug, Default)]
pub struct Components {
    /// Component id of each triangle.
    pub tri_comp: Vec<u32>,
    /// Number of components.
    pub count: usize,
}

impl Mesh {
    /// Number of vertices.
    pub fn vertex_count(&self) -> usize {
        self.positions.len()
    }

    /// Number of triangles.
    pub fn triangle_count(&self) -> usize {
        self.indices.len() / 3
    }

    /// Corner positions of triangle `i`.
    #[inline]
    pub fn triangle(&self, i: usize) -> [V3; 3] {
        let k = i * 3;
        let corner = |n: usize| self.positions[self.indices[k + n] as usize];
        [corner(0), corner(1), corner(2)]
    }

    /// Approximate heap footprint of positions and indices.
    pub fn memory_bytes(&self) -> u64 {
        (self.positions.len() * 12 + self.indices.len() * 4) as u64
    }

    /// Bounding box of all vertices.
    pub fn aabb(&self) -> Aabb {
        let mut b = Aabb::EMPTY;
        for &p in &self.positions {
            b.extend(p);
        }
        b
    }

    /// Checks index bounds, triangle structure and finite coordinates.
    pub fn validate(&self) -> Result<(), String> {
        if self.indices.len() % 3 != 0 {
            return Err("index count is not a multiple of 3".into());
        }
        let n = self.positions.len() as u32;
        if self.indices.iter().any(|&i| i >= n) {
            return Err("triangle index out of range".into());
        }
        if self.positions.iter().any(|p| !p.iter().all(|c| c.is_finite())) {
            return Err("non-finite vertex coordinate".into());
        }
        Ok(())
    }

    /// Uniformly scales all positions.
    pub fn scale(&mut self, s: f32) {
        for p in &mut self.positions {
            *p = mul(*p, s);
        }
    }

    /// Applies a rigid transform to all positions.
    pub fn transform(&mut self, t: &Rigid) {
        for p in &mut self.positions {
            *p = t.apply(*p);
        }
    }

    /// Appends `other`, re-basing its indices.
    pub fn append(&mut self, other: &Mesh) {
        let base = self.positions.len() as u32;
        self.positions.extend_from_slice(&other.positions);
        self.indices.extend(other.indices.iter().map(|i| i + base));
    }

    /// Merges coincident vertices (1e-6 of the bbox diagonal) and drops degenerate triangles.
    /// STL and many OBJ files are "triangle soup"; every adjacency-based feature needs this.
    pub fn weld(&mut self) {
        let bb = self.aabb();
        if bb.is_empty() {
            return;
        }
        let inv = 1.0 / (bb.diagonal() as f64 * 1e-6).max(1e-12);
        let mut map: FxMap<(i64, i64, i64), u32> = FxMap::default();
        map.reserve(self.positions.len() / 2 + 1);
        let mut out: Vec<V3> = Vec::with_capacity(self.positions.len() / 2 + 1);
        let mut remap: Vec<u32> = Vec::with_capacity(self.positions.len());
        for p in &self.positions {
            let key = (
                (p[0] as f64 * inv).round() as i64,
                (p[1] as f64 * inv).round() as i64,
                (p[2] as f64 * inv).round() as i64,
            );
            let id = *map.entry(key).or_insert_with(|| {
                out.push(*p);
                (out.len() - 1) as u32
            });
            remap.push(id);
        }
        let mut idx = Vec::with_capacity(self.indices.len());
        for t in self.indices.chunks_exact(3) {
            let (a, b, c) = (remap[t[0] as usize], remap[t[1] as usize], remap[t[2] as usize]);
            if a != b && b != c && a != c {
                idx.extend_from_slice(&[a, b, c]);
            }
        }
        self.positions = out;
        self.indices = idx;
    }

    /// Surface area, volume, topology and bounds in one pass over the triangles.
    pub fn stats(&self, size_bytes: u64) -> MeshStats {
        let (mut area, mut vol) = (0f64, 0f64);
        for t in self.indices.chunks_exact(3) {
            let corner = |i: u32| widen(self.positions[i as usize]);
            let (a, b, c) = (corner(t[0]), corner(t[1]), corner(t[2]));
            let n = cross64(sub64(b, a), sub64(c, a));
            area += 0.5 * dot64(n, n).sqrt();
            vol += dot64(a, cross64(b, c)) / 6.0;
        }
        let mut cen = [0f64; 3];
        for p in &self.positions {
            let w = widen(*p);
            for k in 0..3 {
                cen[k] += w[k];
            }
        }
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
        if self.indices.is_empty() {
            return false;
        }
        let mut keys = self.edge_keys();
        keys.sort_unstable();
        let mut i = 0;
        while i < keys.len() {
            let mut j = i;
            while j < keys.len() && keys[j] == keys[i] {
                j += 1;
            }
            if j - i != 2 {
                return false;
            }
            i = j;
        }
        true
    }

    /// One packed key per triangle edge (three per triangle), duplicates included.
    fn edge_keys(&self) -> Vec<u64> {
        let mut keys = Vec::with_capacity(self.indices.len());
        for t in self.indices.chunks_exact(3) {
            for (a, b) in [(t[0], t[1]), (t[1], t[2]), (t[2], t[0])] {
                keys.push(((a.min(b) as u64) << 32) | a.max(b) as u64);
            }
        }
        keys
    }

    /// Unique edges as index pairs (for the wireframe view).
    pub fn unique_edges(&self) -> Vec<u32> {
        let mut keys = self.edge_keys();
        keys.sort_unstable();
        keys.dedup();
        keys.into_iter().flat_map(|k| [(k >> 32) as u32, (k & 0xffff_ffff) as u32]).collect()
    }

    /// Connected components, joining triangles that share a vertex.
    pub fn components(&self) -> Components {
        let nv = self.positions.len();
        let mut parent: Vec<u32> = (0..nv as u32).collect();
        fn find(p: &mut [u32], mut x: u32) -> u32 {
            while p[x as usize] != x {
                p[x as usize] = p[p[x as usize] as usize];
                x = p[x as usize];
            }
            x
        }
        for t in self.indices.chunks_exact(3) {
            for (a, b) in [(t[0], t[1]), (t[1], t[2])] {
                let (ra, rb) = (find(&mut parent, a), find(&mut parent, b));
                if ra != rb {
                    parent[rb as usize] = ra;
                }
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
            for &i in t {
                v[i as usize] = *c;
            }
        }
        v
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Unit tetrahedron with outward-facing winding.
    fn tetra() -> Mesh {
        Mesh {
            positions: vec![[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
            indices: vec![0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3],
        }
    }

    #[test]
    fn tetrahedron_is_closed_with_expected_volume() {
        let m = tetra();
        assert!(m.validate().is_ok());
        assert!(m.is_watertight());
        assert_eq!(m.unique_edges().len(), 12); // 6 edges
        let s = m.stats(0);
        assert!((s.volume - 1.0 / 6.0).abs() < 1e-9);
        assert_eq!(m.components().count, 1);
    }

    #[test]
    fn weld_merges_triangle_soup() {
        let mut m = Mesh {
            positions: vec![[0.0; 3], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0; 3], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]],
            indices: vec![0, 1, 2, 3, 4, 5],
        };
        m.weld();
        assert_eq!(m.vertex_count(), 4);
        assert_eq!(m.triangle_count(), 2);
    }
}

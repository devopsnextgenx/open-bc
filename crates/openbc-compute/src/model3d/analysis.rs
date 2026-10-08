//! Everything derived from the deviation fields + tolerance: summary numbers, histogram and
//! the connected regions that exceed the tolerance. Cheap enough to re-run on every
//! tolerance-slider tick (O(triangles), no distance queries).
use super::deviation::Deviation;
use super::geom::tri_area;
use openbc_core::model3d::mesh::*;
use openbc_core::model3d::*;
use rayon::prelude::*;

pub struct Analysis {
    pub summary: Summary,
    pub histogram: Vec<f32>,
    pub regions: Vec<Region>,
    pub regions_total: u32,
}

pub const MAX_REGIONS: usize = 256;
const HUGE: f32 = 1.0e6;

/// Running totals over both models.
#[derive(Default)]
struct Acc {
    area: f64,
    abs_area: f64,
    changed_area: f64,
    max_dev: f32,
    hist: Vec<f64>,
}

/// Per-triangle numbers derived from its three vertex deviations and its centroid deviation.
struct TriMetric {
    area: f32,
    mean_abs: f32,
    mean_signed: f32,
    max_abs: f32,
}

/// Union-find over vertex indices (path halving); joins vertices of over-tolerance triangles.
struct DisjointSet(Vec<u32>);

impl DisjointSet {
    fn new(n: usize) -> Self {
        Self((0..n as u32).collect())
    }

    fn find(&mut self, mut x: u32) -> u32 {
        let p = &mut self.0;
        while p[x as usize] != x {
            p[x as usize] = p[p[x as usize] as usize];
            x = p[x as usize];
        }
        x
    }

    fn union(&mut self, a: u32, b: u32) {
        let (ra, rb) = (self.find(a), self.find(b));
        if ra != rb {
            self.0[rb as usize] = ra;
        }
    }
}

/// NaN -> 0, everything clamped to +-`HUGE` so one bad sample cannot poison the sums.
fn finite(v: f32) -> f32 {
    if v.is_nan() {
        0.0
    } else {
        v.clamp(-HUGE, HUGE)
    }
}

fn triangle_metrics(positions: &[V3], indices: &[u32], dev: &Deviation) -> Vec<TriMetric> {
    (0..indices.len() / 3)
        .into_par_iter()
        .map(|i| {
            let t = &indices[i * 3..i * 3 + 3];
            let (a, b, c) = (positions[t[0] as usize], positions[t[1] as usize], positions[t[2] as usize]);
            let s = [
                finite(dev.vertex[t[0] as usize]),
                finite(dev.vertex[t[1] as usize]),
                finite(dev.vertex[t[2] as usize]),
                finite(dev.centroid[i]),
            ];
            TriMetric {
                area: tri_area(a, b, c),
                mean_abs: s.iter().map(|x| x.abs()).sum::<f32>() / 4.0,
                mean_signed: s.iter().sum::<f32>() / 4.0,
                max_abs: s.iter().fold(0f32, |m, x| m.max(x.abs())),
            }
        })
        .collect()
}

/// Folds one model into the running totals / histogram and returns its over-tolerance regions.
fn analyze_side(
    positions: &[V3],
    indices: &[u32],
    dev: &Deviation,
    tol: f32,
    range: f32,
    side_id: u8,
    acc: &mut Acc,
) -> Vec<Region> {
    let metrics = triangle_metrics(positions, indices, dev);
    let bins = HISTOGRAM_BINS;
    if acc.hist.is_empty() {
        acc.hist = vec![0.0; bins];
    }

    let mut sets = DisjointSet::new(positions.len());
    for (i, m) in metrics.iter().enumerate() {
        let area = m.area as f64;
        acc.area += area;
        acc.abs_area += area * m.mean_abs as f64;
        acc.max_dev = acc.max_dev.max(m.max_abs);
        let bin = (((m.mean_signed / range).clamp(-1.0, 1.0) + 1.0) * 0.5 * (bins as f32 - 1.0)).round() as usize;
        acc.hist[bin.min(bins - 1)] += area;
        if m.mean_abs > tol {
            acc.changed_area += area;
            let t = &indices[i * 3..i * 3 + 3];
            sets.union(t[0], t[1]);
            sets.union(t[0], t[2]);
        }
    }

    let mut by_root: FxMap<u32, Region> = FxMap::default();
    for (i, m) in metrics.iter().enumerate() {
        if m.mean_abs <= tol {
            continue;
        }
        let t = &indices[i * 3..i * 3 + 3];
        let root = sets.find(t[0]);
        let r = by_root.entry(root).or_insert(Region {
            side: side_id,
            area: 0.0,
            max_deviation: 0.0,
            triangles: 0,
            bbox: Aabb::EMPTY,
        });
        r.area += m.area as f64;
        r.max_deviation = r.max_deviation.max(m.max_abs);
        r.triangles += 1;
        for &v in t {
            r.bbox.extend(positions[v as usize]);
        }
    }
    by_root.into_values().collect()
}

pub fn analyze(a: (&[V3], &[u32]), b: (&[V3], &[u32]), da: &Deviation, db: &Deviation, tol: f32, range: f32) -> Analysis {
    let mut acc = Acc::default();
    let mut regions = analyze_side(a.0, a.1, da, tol, range, 0, &mut acc);
    regions.extend(analyze_side(b.0, b.1, db, tol, range, 1, &mut acc));
    regions.sort_by(|x, y| y.area.total_cmp(&x.area));
    let regions_total = regions.len() as u32;
    regions.truncate(MAX_REGIONS);
    let total = acc.area.max(1e-30);
    Analysis {
        summary: Summary {
            max_deviation: acc.max_dev,
            mean_deviation: (acc.abs_area / total) as f32,
            surface_changed: (acc.changed_area / total) as f32,
            tolerance: tol,
            range,
        },
        histogram: acc.hist.iter().map(|&v| v as f32).collect(),
        regions,
        regions_total,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn disjoint_set_joins_and_keeps_others_apart() {
        let mut s = DisjointSet::new(5);
        s.union(0, 1);
        s.union(1, 2);
        assert_eq!(s.find(0), s.find(2));
        assert_ne!(s.find(0), s.find(3));
    }

    #[test]
    fn finite_clamps_and_zeroes_nan() {
        assert_eq!(finite(f32::NAN), 0.0);
        assert_eq!(finite(f32::INFINITY), HUGE);
        assert_eq!(finite(-2.5), -2.5);
    }
}

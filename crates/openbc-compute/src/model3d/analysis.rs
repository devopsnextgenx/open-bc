//! Everything derived from the deviation fields + tolerance: summary numbers, histogram and
//! the connected regions that exceed the tolerance. Cheap enough to re-run on every
//! tolerance-slider tick (O(triangles), no distance queries).
use super::deviation::Deviation;
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

#[derive(Default)]
struct Acc { area: f64, abs_area: f64, changed_area: f64, max_dev: f32, hist: Vec<f64> }

fn fin(v: f32) -> f32 { if v.is_nan() { 0.0 } else { v.clamp(-HUGE, HUGE) } }

fn side(positions: &[V3], indices: &[u32], dev: &Deviation, tol: f32, range: f32, side_id: u8, acc: &mut Acc, regions: &mut Vec<Region>) {
    let nt = indices.len() / 3;
    // per triangle: (area, mean |dev|, mean signed dev, max |dev|) - parallel
    let tri: Vec<(f32, f32, f32, f32)> = (0..nt).into_par_iter().map(|i| {
        let t = &indices[i * 3..i * 3 + 3];
        let (a, b, c) = (positions[t[0] as usize], positions[t[1] as usize], positions[t[2] as usize]);
        let area = 0.5 * len(cross(sub(b, a), sub(c, a)));
        let s = [fin(dev.vertex[t[0] as usize]), fin(dev.vertex[t[1] as usize]), fin(dev.vertex[t[2] as usize]), fin(dev.centroid[i])];
        let abs: f32 = s.iter().map(|x| x.abs()).sum::<f32>() / 4.0;
        let signed: f32 = s.iter().sum::<f32>() / 4.0;
        let mx = s.iter().fold(0f32, |m, x| m.max(x.abs()));
        (area, abs, signed, mx)
    }).collect();

    let bins = HISTOGRAM_BINS;
    if acc.hist.is_empty() { acc.hist = vec![0.0; bins]; }
    let mut parent: Vec<u32> = (0..positions.len() as u32).collect();
    fn find(p: &mut [u32], mut x: u32) -> u32 {
        while p[x as usize] != x { p[x as usize] = p[p[x as usize] as usize]; x = p[x as usize]; }
        x
    }
    for (i, &(area, abs, signed, mx)) in tri.iter().enumerate() {
        let area = area as f64;
        acc.area += area;
        acc.abs_area += area * abs as f64;
        acc.max_dev = acc.max_dev.max(mx);
        let bin = (((signed / range).clamp(-1.0, 1.0) + 1.0) * 0.5 * (bins as f32 - 1.0)).round() as usize;
        acc.hist[bin.min(bins - 1)] += area;
        if abs > tol {
            acc.changed_area += area;
            let t = &indices[i * 3..i * 3 + 3];
            let (r0, r1) = (find(&mut parent, t[0]), find(&mut parent, t[1]));
            if r0 != r1 { parent[r1 as usize] = r0; }
            let (r0, r2) = (find(&mut parent, t[0]), find(&mut parent, t[2]));
            if r0 != r2 { parent[r2 as usize] = r0; }
        }
    }
    let mut by_root: FxMap<u32, Region> = FxMap::default();
    for (i, &(area, abs, _, mx)) in tri.iter().enumerate() {
        if abs <= tol { continue; }
        let t = &indices[i * 3..i * 3 + 3];
        let root = find(&mut parent, t[0]);
        let r = by_root.entry(root).or_insert(Region { side: side_id, area: 0.0, max_deviation: 0.0, triangles: 0, bbox: Aabb::EMPTY });
        r.area += area as f64;
        r.max_deviation = r.max_deviation.max(mx);
        r.triangles += 1;
        for &v in t { r.bbox.extend(positions[v as usize]); }
    }
    regions.extend(by_root.into_values());
}

pub fn analyze(a: (&[V3], &[u32]), b: (&[V3], &[u32]), da: &Deviation, db: &Deviation, tol: f32, range: f32) -> Analysis {
    let mut acc = Acc::default();
    let mut regions = Vec::new();
    side(a.0, a.1, da, tol, range, 0, &mut acc, &mut regions);
    side(b.0, b.1, db, tol, range, 1, &mut acc, &mut regions);
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

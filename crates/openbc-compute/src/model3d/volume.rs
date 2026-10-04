//! "Changes" view: material removed from A / added in B.
//!
//! Both models are scan-converted onto one shared voxel grid (scanline parity: for every
//! (y, z) column the BVH returns the x positions where the surface is crossed, and the
//! spans between pairs are inside). Rows are independent, so the work is a plain Rayon
//! parallel loop. Cells that are only "different" because of voxel aliasing are discarded
//! by requiring the cell centre to be farther than the tolerance from the other surface.
//! Volumes are therefore accurate to about one cell size; `cell_size_mm` is reported.
use super::bvh::Bvh;
use super::ComputeError;
use openbc_core::model3d::mesh::*;
use openbc_core::model3d::*;
use rayon::prelude::*;

pub const DEFAULT_MAX_CELLS: u64 = 1 << 24;
pub const DEFAULT_MAX_QUADS: usize = 500_000;
// Fractions of a cell: keeps the scan columns off axis-aligned edges.
const JY: f32 = 0.0123;
const JZ: f32 = 0.0071;

pub struct Occupancy {
    origin: V3,
    h: f32,
    n: [usize; 3],
    wpr: usize,
    /// In A but not B, per cell (bit x of row z * ny + y).
    removed: Vec<u64>,
    /// In B but not A.
    added: Vec<u64>,
}

fn set_range(w: &mut [u64], lo: usize, hi: usize) {
    if lo >= hi { return; }
    let (wl, wh) = (lo / 64, (hi - 1) / 64);
    for i in wl..=wh {
        let start = if i == wl { lo % 64 } else { 0 };
        let end = if i == wh { (hi - 1) % 64 + 1 } else { 64 };
        let mask = if end - start == 64 { !0u64 } else { ((1u64 << (end - start)) - 1) << start };
        w[i] |= mask;
    }
}

fn fill(hits: &[f32], ox: f32, h: f32, nx: usize, words: &mut [u64]) {
    for pair in hits.chunks_exact(2) {  // an odd trailing hit (open mesh) is ignored
        let lo = ((pair[0] - ox) / h - 0.5).ceil().max(0.0) as usize;
        let hi = (((pair[1] - ox) / h - 0.5).ceil().max(0.0) as usize).min(nx);
        set_range(words, lo, hi);
    }
}

pub fn occupancy(a: &Bvh, b: &Bvh, max_cells: u64, cancel: &CancelToken) -> Result<Occupancy, ComputeError> {
    let bb = a.bounds.union(&b.bounds);
    if bb.is_empty() { return Err(ComputeError::Message("empty models".into())); }
    let ext = bb.size();
    let max_ext = ext[0].max(ext[1]).max(ext[2]).max(1e-6);
    let prod = (ext[0].max(1e-6) * ext[1].max(1e-6) * ext[2].max(1e-6)) as f64;
    let mut h = ((prod / max_cells as f64).cbrt() as f32).max(max_ext / 2048.0);
    let dims = |h: f32| [(ext[0] / h).ceil() as usize + 2, (ext[1] / h).ceil() as usize + 2, (ext[2] / h).ceil() as usize + 2];
    let mut n = dims(h);
    for _ in 0..256 {
        if (n[0] as u64) * (n[1] as u64) * (n[2] as u64) <= max_cells { break; }
        h *= 1.1;
        n = dims(h);
    }
    let origin = sub(bb.min, [h, h, h]);
    let (nx, ny, nz) = (n[0], n[1], n[2]);
    let wpr = (nx + 63) / 64;
    let mut removed = vec![0u64; ny * nz * wpr];
    let mut added = vec![0u64; ny * nz * wpr];

    removed.par_chunks_mut(wpr).zip(added.par_chunks_mut(wpr)).enumerate().for_each_init(
        || (Vec::<f32>::new(), Vec::<f32>::new(), vec![0u64; wpr], vec![0u64; wpr]),
        |(ha, hb, wa, wb), (row, (rm, ad))| {
            if cancel.is_cancelled() { return; }
            let (iy, iz) = (row % ny, row / ny);
            let y = origin[1] + (iy as f32 + 0.5 + JY) * h;
            let z = origin[2] + (iz as f32 + 0.5 + JZ) * h;
            a.x_hits(y, z, ha);
            b.x_hits(y, z, hb);
            wa.fill(0);
            wb.fill(0);
            fill(ha, origin[0], h, nx, wa);
            fill(hb, origin[0], h, nx, wb);
            for w in 0..wpr { rm[w] = wa[w] & !wb[w]; ad[w] = wb[w] & !wa[w]; }
        },
    );
    if cancel.is_cancelled() { return Err(ComputeError::Cancelled); }
    Ok(Occupancy { origin, h, n, wpr, removed, added })
}

/// Keeps only cells whose centre is farther than `tol` from `other`.
fn filter(raw: &[u64], occ: &Occupancy, other: &Bvh, tol: f32, cancel: &CancelToken) -> Result<Vec<u64>, ComputeError> {
    let mut out = vec![0u64; raw.len()];
    let (ny, wpr, h) = (occ.n[1], occ.wpr, occ.h);
    out.par_chunks_mut(wpr).zip(raw.par_chunks(wpr)).enumerate().for_each(|(row, (dst, src))| {
        if src.iter().all(|&w| w == 0) || cancel.is_cancelled() { return; }
        let (iy, iz) = (row % ny, row / ny);
        for (wi, &word) in src.iter().enumerate() {
            let mut bits = word;
            while bits != 0 {
                let b = bits.trailing_zeros() as usize;
                bits &= bits - 1;
                let x = wi * 64 + b;
                let c = [occ.origin[0] + (x as f32 + 0.5) * h, occ.origin[1] + (iy as f32 + 0.5) * h, occ.origin[2] + (iz as f32 + 0.5) * h];
                let d = other.closest(c).map(|hit| hit.signed.abs()).unwrap_or(f32::INFINITY);
                if d > tol { dst[wi] |= 1u64 << b; }
            }
        }
    });
    if cancel.is_cancelled() { Err(ComputeError::Cancelled) } else { Ok(out) }
}

fn get(bits: &[u64], occ: &Occupancy, x: i64, y: i64, z: i64) -> bool {
    let (nx, ny, nz) = (occ.n[0] as i64, occ.n[1] as i64, occ.n[2] as i64);
    if x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz { return false; }
    let row = (z * ny + y) as usize;
    bits[row * occ.wpr + (x as usize) / 64] >> (x as usize % 64) & 1 == 1
}

/// Boundary faces of the set as unwelded quads (flat-shaded by the renderer).
fn surface(bits: &[u64], occ: &Occupancy, max_quads: usize) -> (Mesh, bool) {
    let (ny, nz, wpr, h) = (occ.n[1], occ.n[2], occ.wpr, occ.h);
    let per_z: Vec<Vec<[V3; 4]>> = (0..nz).into_par_iter().map(|iz| {
        let mut q = Vec::new();
        for iy in 0..ny {
            let base = (iz * ny + iy) * wpr;
            for wi in 0..wpr {
                let mut w = bits[base + wi];
                while w != 0 {
                    let b = w.trailing_zeros() as usize;
                    w &= w - 1;
                    let x = (wi * 64 + b) as i64;
                    let c0 = [occ.origin[0] + x as f32 * h, occ.origin[1] + iy as f32 * h, occ.origin[2] + iz as f32 * h];
                    for axis in 0..3usize {
                        for s in 0..2i64 {
                            let mut nb = [x, iy as i64, iz as i64];
                            nb[axis] += if s == 1 { 1 } else { -1 };
                            if get(bits, occ, nb[0], nb[1], nb[2]) { continue; }
                            let (u, v) = ((axis + 1) % 3, (axis + 2) % 3);
                            let mut p = c0;
                            if s == 1 { p[axis] += h; }
                            let (mut pu, mut pv, mut puv) = (p, p, p);
                            pu[u] += h; pv[v] += h; puv[u] += h; puv[v] += h;
                            q.push([p, pu, puv, pv]);
                        }
                    }
                }
            }
        }
        q
    }).collect();

    let mut mesh = Mesh::default();
    let mut truncated = false;
    let mut quads = 0usize;
    'outer: for slice in per_z {
        for q in slice {
            if quads >= max_quads { truncated = true; break 'outer; }
            let base = mesh.positions.len() as u32;
            mesh.positions.extend_from_slice(&q);
            mesh.indices.extend_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
            quads += 1;
        }
    }
    (mesh, truncated)
}

pub fn changes(occ: &Occupancy, a: &Bvh, b: &Bvh, tol: f32, max_quads: usize, cancel: &CancelToken) -> Result<ChangesOutput, ComputeError> {
    let removed = filter(&occ.removed, occ, b, tol, cancel)?;
    let added = filter(&occ.added, occ, a, tol, cancel)?;
    let cell = (occ.h as f64).powi(3);
    let count = |v: &[u64]| v.par_iter().map(|w| w.count_ones() as u64).sum::<u64>();
    let (removed_mm3, added_mm3) = (count(&removed) as f64 * cell, count(&added) as f64 * cell);
    let ((rm, t1), (ad, t2)) = rayon::join(|| surface(&removed, occ, max_quads / 2), || surface(&added, occ, max_quads / 2));
    if cancel.is_cancelled() { return Err(ComputeError::Cancelled); }
    Ok(ChangesOutput { removed_mm3, added_mm3, cell_size_mm: occ.h, truncated: t1 || t2, removed: rm, added: ad })
}

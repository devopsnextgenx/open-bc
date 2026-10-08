//! Helpers shared by every loader: the common tail (`finish`), vertex welding, error
//! shorthands, cancellation polling, polygon fan triangulation and vertex parsing.
use openbc_core::model3d::*;
use rayon::prelude::*;

/// Loops that poll for cancellation do so every this many items.
pub(crate) const CANCEL_POLL_INTERVAL: usize = 65_536;

pub(crate) fn malformed(m: impl Into<String>) -> LoadError {
    LoadError::Malformed(m.into())
}

/// `Err(Cancelled)` if the load was cancelled.
pub(crate) fn check_cancel(ctx: &LoadContext<'_>) -> Result<(), LoadError> {
    if ctx.cancel.is_cancelled() {
        Err(LoadError::Cancelled)
    } else {
        Ok(())
    }
}

/// Cheap in-loop poll: only looks at the token when `n` is a multiple of the interval.
pub(crate) fn poll_cancel(ctx: &LoadContext<'_>, n: usize) -> Result<(), LoadError> {
    if n % CANCEL_POLL_INTERVAL == 0 {
        check_cancel(ctx)
    } else {
        Ok(())
    }
}

/// Appends the triangle fan `(f0, fk, fk+1)` of a convex polygon to `out`.
pub(crate) fn fan_triangulate(face: &[u32], out: &mut Vec<u32>) {
    for k in 1..face.len().saturating_sub(1) {
        out.extend_from_slice(&[face[0], face[k], face[k + 1]]);
    }
}

/// Next three whitespace-separated tokens as a vertex; `None` if any is missing or not a float.
pub(crate) fn parse_v3<'a>(tokens: &mut impl Iterator<Item = &'a str>) -> Option<V3> {
    let mut v = [0f32; 3];
    for c in &mut v {
        *c = tokens.next()?.parse().ok()?;
    }
    Some(v)
}

/// Shared tail of every loader: limits, validation, parallel weld.
pub(crate) fn finish(mut mesh: Mesh, ctx: &LoadContext<'_>) -> Result<Mesh, LoadError> {
    check_cancel(ctx)?;
    if mesh.triangle_count() == 0 {
        return Err(malformed("file contains no triangles"));
    }
    if mesh.triangle_count() > ctx.limits.max_triangles {
        return Err(LoadError::TooLarge(format!(
            "{} triangles (limit {})",
            mesh.triangle_count(),
            ctx.limits.max_triangles
        )));
    }
    mesh.validate().map_err(LoadError::Malformed)?;
    par_weld(&mut mesh);
    if mesh.triangle_count() == 0 {
        return Err(malformed("only degenerate triangles"));
    }
    Ok(mesh)
}

/// Sort-based vertex weld: quantise, parallel sort, one linear scan. Same result as
/// `Mesh::weld` but scales with cores (the hash-map version is single threaded).
pub fn par_weld(mesh: &mut Mesh) {
    let bb = mesh.aabb();
    if bb.is_empty() {
        return;
    }
    let inv = 1.0 / (bb.diagonal() as f64 * 1e-6).max(1e-12);
    let mut keyed: Vec<((i64, i64, i64), u32)> = mesh
        .positions
        .par_iter()
        .enumerate()
        .map(|(i, p)| {
            let q = |c: f32| (c as f64 * inv).round() as i64;
            ((q(p[0]), q(p[1]), q(p[2])), i as u32)
        })
        .collect();
    keyed.par_sort_unstable();
    let mut remap = vec![0u32; mesh.positions.len()];
    let mut out: Vec<V3> = Vec::with_capacity(mesh.positions.len() / 2 + 1);
    let mut prev = None;
    for (k, i) in &keyed {
        if prev != Some(*k) {
            out.push(mesh.positions[*i as usize]);
            prev = Some(*k);
        }
        remap[*i as usize] = (out.len() - 1) as u32;
    }
    let tris: Vec<[u32; 3]> = mesh
        .indices
        .par_chunks_exact(3)
        .filter_map(|t| {
            let (a, b, c) = (remap[t[0] as usize], remap[t[1] as usize], remap[t[2] as usize]);
            (a != b && b != c && a != c).then_some([a, b, c])
        })
        .collect();
    mesh.positions = out;
    mesh.indices = tris.into_iter().flatten().collect();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fan_triangulates_polygons_and_ignores_degenerate_faces() {
        let mut out = Vec::new();
        fan_triangulate(&[0, 1, 2, 3], &mut out);
        assert_eq!(out, [0, 1, 2, 0, 2, 3]);
        out.clear();
        fan_triangulate(&[0, 1], &mut out);
        fan_triangulate(&[], &mut out);
        assert!(out.is_empty());
    }

    #[test]
    fn parse_v3_reads_three_floats_or_nothing() {
        assert_eq!(parse_v3(&mut "1 2.5 -3 extra".split_ascii_whitespace()), Some([1.0, 2.5, -3.0]));
        assert_eq!(parse_v3(&mut "1 2".split_ascii_whitespace()), None);
        assert_eq!(parse_v3(&mut "1 x 3".split_ascii_whitespace()), None);
    }

    #[test]
    fn weld_merges_duplicate_vertices() {
        let mut mesh = Mesh {
            positions: vec![
                [0., 0., 0.], [1., 0., 0.], [1., 1., 0.],
                [0., 0., 0.], [1., 1., 0.], [0., 1., 0.],
            ],
            indices: vec![0, 1, 2, 3, 4, 5],
        };
        par_weld(&mut mesh);
        assert_eq!(mesh.positions.len(), 4);
        assert_eq!(mesh.triangle_count(), 2);
    }
}

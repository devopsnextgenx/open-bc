//! Small triangle helpers shared by BVH construction, deviation sampling, analysis and
//! per-part alignment (each used to carry its own copy of this arithmetic).
use openbc_core::model3d::mesh::*;

/// Triangle centroid.
#[inline]
pub(crate) fn centroid(a: V3, b: V3, c: V3) -> V3 {
    [(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0]
}

/// Triangle area.
#[inline]
pub(crate) fn tri_area(a: V3, b: V3, c: V3) -> f32 {
    0.5 * len(cross(sub(b, a), sub(c, a)))
}

/// Widens a single-precision vector for use with the `f64` rigid transforms.
#[inline]
pub(crate) fn to_f64(v: V3) -> [f64; 3] {
    [v[0] as f64, v[1] as f64, v[2] as f64]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn right_triangle_area_and_centroid() {
        let (a, b, c) = ([0.0, 0.0, 0.0], [4.0, 0.0, 0.0], [0.0, 3.0, 0.0]);
        assert_eq!(tri_area(a, b, c), 6.0);
        let g = centroid(a, b, c);
        assert!((g[0] - 4.0 / 3.0).abs() < 1e-6 && (g[1] - 1.0).abs() < 1e-6 && g[2] == 0.0);
    }

    #[test]
    fn widening_is_exact() {
        assert_eq!(to_f64([1.5, -2.0, 0.25]), [1.5, -2.0, 0.25]);
    }
}

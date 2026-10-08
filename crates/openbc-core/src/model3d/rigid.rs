//! Rigid transforms.

use super::math::V3;

/// Rigid transform p' = R p + t, kept in f64 so chained alignments do not drift.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Rigid {
    /// Rotation matrix (row-major).
    pub r: [[f64; 3]; 3],
    /// Translation.
    pub t: [f64; 3],
}

impl Rigid {
    /// The identity transform.
    pub const IDENTITY: Rigid =
        Rigid { r: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]], t: [0.0; 3] };

    /// A pure translation.
    pub fn translation(t: [f64; 3]) -> Rigid {
        Rigid { r: Self::IDENTITY.r, t }
    }

    /// Transforms a point.
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
        for i in 0..3 {
            for j in 0..3 {
                for k in 0..3 {
                    r[i][j] += next.r[i][k] * self.r[k][j];
                }
            }
        }
        let mut t = next.t;
        for i in 0..3 {
            for k in 0..3 {
                t[i] += next.r[i][k] * self.t[k];
            }
        }
        Rigid { r, t }
    }

    /// The inverse transform (valid because `r` is orthonormal).
    pub fn inverse(&self) -> Rigid {
        let mut r = [[0.0; 3]; 3];
        for i in 0..3 {
            for j in 0..3 {
                r[i][j] = self.r[j][i];
            }
        }
        let mut t = [0.0; 3];
        for i in 0..3 {
            for k in 0..3 {
                t[i] -= r[i][k] * self.t[k];
            }
        }
        Rigid { r, t }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn inverse_undoes_then_composition() {
        let a = Rigid::translation([1.0, 2.0, 3.0]);
        let b = Rigid { r: [[0.0, -1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]], t: [0.5, 0.0, -1.0] };
        let both = a.then(&b);
        let p = [4.0, -2.0, 7.0];
        let back = both.inverse().apply(both.apply(p));
        for k in 0..3 {
            assert!((back[k] - p[k]).abs() < 1e-5);
        }
    }
}

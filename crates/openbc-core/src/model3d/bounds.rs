//! Axis-aligned bounding box.

use super::math::{add, len, mul, sub, V3};

/// Axis-aligned bounding box; [`Aabb::EMPTY`] is the identity for [`Aabb::union`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Aabb {
    /// Minimum corner.
    pub min: V3,
    /// Maximum corner.
    pub max: V3,
}

impl Aabb {
    /// A box containing nothing.
    pub const EMPTY: Aabb = Aabb { min: [f32::INFINITY; 3], max: [f32::NEG_INFINITY; 3] };

    /// True when no point has been added.
    pub fn is_empty(&self) -> bool {
        self.min[0] > self.max[0]
    }

    /// Grows the box to contain `p`.
    pub fn extend(&mut self, p: V3) {
        for k in 0..3 {
            self.min[k] = self.min[k].min(p[k]);
            self.max[k] = self.max[k].max(p[k]);
        }
    }

    /// Smallest box containing both; an empty `o` leaves `self` unchanged.
    pub fn union(&self, o: &Aabb) -> Aabb {
        let mut r = *self;
        if !o.is_empty() {
            r.extend(o.min);
            r.extend(o.max);
        }
        r
    }

    /// Midpoint of the corners.
    pub fn center(&self) -> V3 {
        mul(add(self.min, self.max), 0.5)
    }

    /// Edge lengths.
    pub fn size(&self) -> V3 {
        sub(self.max, self.min)
    }

    /// Length of the main diagonal (0 when empty).
    pub fn diagonal(&self) -> f32 {
        if self.is_empty() {
            0.0
        } else {
            len(self.size())
        }
    }
}

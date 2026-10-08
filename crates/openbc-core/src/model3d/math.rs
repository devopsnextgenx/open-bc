//! Minimal vector math on `[f32; 3]`, plus `f64` helpers for accumulation.

/// A 3D point or vector in millimetres.
pub type V3 = [f32; 3];

/// Double-precision vector used where sums of many terms must not lose precision.
pub(super) type V3d = [f64; 3];

/// `a - b`.
#[inline]
pub fn sub(a: V3, b: V3) -> V3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

/// `a + b`.
#[inline]
pub fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}

/// `a * s`.
#[inline]
pub fn mul(a: V3, s: f32) -> V3 {
    [a[0] * s, a[1] * s, a[2] * s]
}

/// Dot product.
#[inline]
pub fn dot(a: V3, b: V3) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

/// Cross product.
#[inline]
pub fn cross(a: V3, b: V3) -> V3 {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

/// Euclidean length.
#[inline]
pub fn len(a: V3) -> f32 {
    dot(a, a).sqrt()
}

/// Widens a single-precision vector.
#[inline]
pub(super) fn widen(a: V3) -> V3d {
    [a[0] as f64, a[1] as f64, a[2] as f64]
}

/// `a - b` in double precision.
#[inline]
pub(super) fn sub64(a: V3d, b: V3d) -> V3d {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

/// Dot product in double precision.
#[inline]
pub(super) fn dot64(a: V3d, b: V3d) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

/// Cross product in double precision.
#[inline]
pub(super) fn cross64(a: V3d, b: V3d) -> V3d {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

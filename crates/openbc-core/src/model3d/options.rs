//! Options the UI can change at any time. They travel on a latest-wins channel.

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AlignmentMode {
    /// Compare in file coordinates (same CAD origin).
    Original = 0,
    /// Move both bounding-box centres to the origin.
    CenterAtOrigin = 1,
    /// Move B so the bbox minimum corners coincide.
    BoundingBoxCorners = 2,
    /// Move B so the vertex centroids coincide.
    Centroids = 3,
}

impl AlignmentMode {
    pub fn from_u8(v: u8) -> Self {
        match v { 1 => Self::CenterAtOrigin, 2 => Self::BoundingBoxCorners, 3 => Self::Centroids, _ => Self::Original }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Model3dCompareOptions {
    /// Deviations at or below this are "unchanged" (mm).
    pub tolerance_mm: f32,
    /// Heat-map range is +/- this value (mm); also the clamp for the "max >=" read-out.
    pub range_mm: f32,
    pub alignment: AlignmentMode,
    /// Extra offset applied to B on top of the alignment mode (mm).
    pub offset: [f32; 3],
    pub prefer_gpu: bool,
    /// Volumetric "Changes" view (removed / added material).
    pub compute_changes: bool,
}

impl Default for Model3dCompareOptions {
    fn default() -> Self {
        Self { tolerance_mm: 0.05, range_mm: 1.0, alignment: AlignmentMode::Original, offset: [0.0; 3], prefer_gpu: true, compute_changes: true }
    }
}

impl Model3dCompareOptions {
    pub fn sanitized(mut self) -> Self {
        let fin = |v: f32, d: f32| if v.is_finite() { v } else { d };
        self.tolerance_mm = fin(self.tolerance_mm, 0.05).clamp(1e-6, 1e4);
        self.range_mm = fin(self.range_mm, 1.0).clamp(1e-4, 1e4).max(self.tolerance_mm * 1.0001);
        for o in &mut self.offset { *o = fin(*o, 0.0).clamp(-1e6, 1e6); }
        self
    }
    /// Options that force the alignment (and therefore the deviation fields) to be recomputed.
    pub fn alignment_changed(&self, other: &Self) -> bool {
        self.alignment != other.alignment || self.offset != other.offset
    }
}

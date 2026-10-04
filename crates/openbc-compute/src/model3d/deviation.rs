//! Deviation fields: signed distance from every vertex and triangle centroid of one model to
//! the surface of the other. Independent of tolerance, so tolerance changes never recompute it.
use super::ComputeError;
use openbc_core::model3d::*;
use rayon::prelude::*;
use std::sync::Arc;

pub struct Deviation {
    /// Per vertex (signed, unclamped, `INFINITY` if the other mesh is empty).
    pub vertex: Arc<Vec<f32>>,
    /// Per triangle centroid.
    pub centroid: Vec<f32>,
}

/// Vertices first, then triangle centroids (one batch = one GPU upload).
pub fn sample_points(positions: &[V3], indices: &[u32]) -> Vec<V3> {
    let mut q = positions.to_vec();
    q.par_extend(indices.par_chunks_exact(3).map(|t| {
        let (a, b, c) = (positions[t[0] as usize], positions[t[1] as usize], positions[t[2] as usize]);
        [(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0, (a[2] + b[2] + c[2]) / 3.0]
    }));
    q
}

pub fn split(mut all: Vec<f32>, vertices: usize) -> Deviation {
    let centroid = all.split_off(vertices);
    Deviation { vertex: Arc::new(all), centroid }
}

pub fn check(cancel: &CancelToken) -> Result<(), ComputeError> {
    if cancel.is_cancelled() { Err(ComputeError::Cancelled) } else { Ok(()) }
}

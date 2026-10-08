//! Deviation fields: signed distance from every vertex and triangle centroid of one model to
//! the surface of the other. Independent of tolerance, so tolerance changes never recompute it.
use super::geom::centroid;
use openbc_core::model3d::*;
use rayon::prelude::*;
use std::sync::Arc;

/// Cancellation check, re-exported here for existing callers of `deviation::check`.
pub use super::error::check;

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
        centroid(positions[t[0] as usize], positions[t[1] as usize], positions[t[2] as usize])
    }));
    q
}

/// Splits a batch produced from [`sample_points`] back into per-vertex and per-centroid parts.
pub fn split(mut all: Vec<f32>, vertices: usize) -> Deviation {
    let centroid = all.split_off(vertices);
    Deviation { vertex: Arc::new(all), centroid }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sample_points_are_vertices_then_centroids_and_split_inverts_it() {
        let positions = vec![[0.0, 0.0, 0.0], [3.0, 0.0, 0.0], [0.0, 3.0, 0.0]];
        let q = sample_points(&positions, &[0, 1, 2]);
        assert_eq!(q.len(), 4);
        assert_eq!(q[3], [1.0, 1.0, 0.0]);
        let d = split(vec![1.0, 2.0, 3.0, 4.0], 3);
        assert_eq!((d.vertex.len(), d.centroid.len()), (3, 1));
    }
}

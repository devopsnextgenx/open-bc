//! Unit tests (not run in the authoring environment - no Rust toolchain was available there).
use super::*;
use bvh::Bvh;

pub(crate) fn cube(min: V3, size: f32) -> Mesh {
    let p = |x: f32, y: f32, z: f32| [min[0] + x * size, min[1] + y * size, min[2] + z * size];
    let positions = vec![p(0., 0., 0.), p(1., 0., 0.), p(1., 1., 0.), p(0., 1., 0.), p(0., 0., 1.), p(1., 0., 1.), p(1., 1., 1.), p(0., 1., 1.)];
    // outward-facing, counter-clockwise
    let indices = vec![
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
        1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7,
    ];
    Mesh { positions, indices }
}

fn raw(name: &str, m: &Mesh) -> RawModel {
    // binary STL round trip exercises the loader as well
    let mut b = vec![0u8; 80];
    b.extend_from_slice(&(m.triangle_count() as u32).to_le_bytes());
    for i in 0..m.triangle_count() {
        b.extend_from_slice(&[0u8; 12]);
        for v in m.triangle(i) { for c in v { b.extend_from_slice(&c.to_le_bytes()); } }
        b.extend_from_slice(&[0u8; 2]);
    }
    RawModel { info: FileInfo { path: name.into(), size_bytes: b.len() as u64, ..Default::default() }, bytes: b }
}

#[test]
fn cube_stats() {
    let s = cube([0.; 3], 10.0).stats(0);
    assert!((s.volume - 1000.0).abs() < 1e-3 && (s.area - 600.0).abs() < 1e-3 && s.watertight);
}

#[test]
fn closest_matches_brute_force() {
    let m = cube([0.; 3], 10.0);
    let bvh = Bvh::build(&m);
    for p in [[5., 5., 5.], [-3., 4., 2.], [12., 12., 12.], [5., 5., 10.5]] {
        let h = bvh.closest(p).unwrap();
        let brute = (0..m.triangle_count()).map(|i| bvh::closest_on_tri(p, &m.triangle(i)).1).fold(f32::INFINITY, f32::min).sqrt();
        assert!((h.signed.abs() - brute).abs() < 1e-4);
    }
    assert!(bvh.closest([5., 5., 5.]).unwrap().signed < 0.0); // inside
    assert!(bvh.closest([20., 5., 5.]).unwrap().signed > 0.0); // outside
}

#[test]
fn shifted_cube_deviation_and_changes() {
    let (a, b) = (cube([0.; 3], 10.0), cube([1., 0., 0.], 10.0));
    let mut s = Session::new(Arc::new(builtin_registry()), Arc::new(Backends::cpu_only()));
    let out = s.load_pair(Ok(raw("a.stl", &a)), Ok(raw("b.stl", &b)), &CancelToken::new());
    assert!(out.left.is_ok() && out.right.is_ok());
    let r = s.compare(&Model3dCompareOptions::default(), None, &CancelToken::new()).unwrap();
    assert!((r.summary.max_deviation - 1.0).abs() < 0.05);
    let c = r.changes.unwrap();
    assert!((c.removed_mm3 - 100.0).abs() < 15.0 && (c.added_mm3 - 100.0).abs() < 15.0);
    // aligning bbox corners removes the difference
    let o = Model3dCompareOptions { alignment: AlignmentMode::BoundingBoxCorners, ..Default::default() };
    let r = s.compare(&o, None, &CancelToken::new()).unwrap();
    assert!(r.summary.max_deviation < 1e-3 && r.regions_total == 0);
}

#[test]
fn icp_recovers_translation() {
    let a = cube([0.; 3], 10.0);
    let bvh = Bvh::build(&a);
    let shifted = cube([0.4, -0.3, 0.2], 10.0);
    let r = align::icp(&shifted.positions, &bvh, 30, &CancelToken::new()).unwrap();
    let p = r.apply(shifted.positions[0]);
    assert!(len(sub(p, a.positions[0])) < 0.05);
}

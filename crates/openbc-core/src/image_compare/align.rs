use serde::{Deserialize, Serialize};

use super::options::ResizePolicy;

/// Half-open pixel rectangle `[x0, x1) x [y0, y1)` in canvas coordinates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Rect {
    pub x0: u32,
    pub y0: u32,
    pub x1: u32,
    pub y1: u32,
}

/// Target sizes for the left image (which defines the canvas) and the right one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResizePlan {
    pub left_target: (u32, u32),
    pub right_target: (u32, u32),
}

fn area(d: (u32, u32)) -> u64 {
    d.0 as u64 * d.1 as u64
}

fn aspect_matches(l: (u32, u32), r: (u32, u32)) -> bool {
    let a = l.0 as u64 * r.1 as u64;
    let b = r.0 as u64 * l.1 as u64;
    a.abs_diff(b) * 100 <= a.max(b)
}

pub fn plan_resize(left: (u32, u32), right: (u32, u32), policy: ResizePolicy) -> ResizePlan {
    let native = ResizePlan { left_target: left, right_target: right };
    if area(left) == 0 || area(right) == 0 {
        return native;
    }
    let same = |t| ResizePlan { left_target: t, right_target: t };
    match policy {
        ResizePolicy::Native => native,
        ResizePolicy::FitToLeft => same(left),
        ResizePolicy::FitToRight => same(right),
        ResizePolicy::FitToLarger => same(if area(left) >= area(right) { left } else { right }),
        ResizePolicy::FitToSmaller => same(if area(left) <= area(right) { left } else { right }),
        ResizePolicy::Auto => {
            if aspect_matches(left, right) {
                same(left)
            } else {
                native
            }
        }
    }
}

/// Where the right image lands on the canvas, clipped to the canvas.
pub fn overlap_rect(canvas: (u32, u32), right: (u32, u32), offset: (i32, i32)) -> Option<Rect> {
    let x0 = (offset.0 as i64).max(0);
    let y0 = (offset.1 as i64).max(0);
    let x1 = (offset.0 as i64 + right.0 as i64).min(canvas.0 as i64);
    let y1 = (offset.1 as i64 + right.1 as i64).min(canvas.1 as i64);
    (x1 > x0 && y1 > y0).then(|| Rect { x0: x0 as u32, y0: y0 as u32, x1: x1 as u32, y1: y1 as u32 })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn auto_scales_only_when_aspect_matches() {
        // 600x1044 vs 462x923 (screenshot case): ratios differ by >1% -> native.
        let p = plan_resize((600, 1044), (462, 923), ResizePolicy::Auto);
        assert_eq!(p.right_target, (462, 923));
        let p = plan_resize((600, 1000), (300, 500), ResizePolicy::Auto);
        assert_eq!(p.right_target, (600, 1000));
    }

    #[test]
    fn fit_policies() {
        let (l, r) = ((100, 100), (50, 50));
        assert_eq!(plan_resize(l, r, ResizePolicy::FitToLarger).right_target, l);
        assert_eq!(plan_resize(l, r, ResizePolicy::FitToSmaller).left_target, r);
        assert_eq!(plan_resize(l, r, ResizePolicy::FitToRight).left_target, r);
    }

    #[test]
    fn overlap_clips_and_rejects() {
        assert_eq!(
            overlap_rect((100, 100), (50, 50), (27, 42)),
            Some(Rect { x0: 27, y0: 42, x1: 77, y1: 92 })
        );
        assert_eq!(
            overlap_rect((100, 100), (50, 50), (-10, 80)),
            Some(Rect { x0: 0, y0: 80, x1: 40, y1: 100 })
        );
        assert_eq!(overlap_rect((100, 100), (50, 50), (100, 0)), None);
        assert_eq!(overlap_rect((100, 100), (50, 50), (-50, 0)), None);
    }
}

//! Myers O(ND) longest-common-subsequence over row signatures.

/// Myers O(ND) LCS; returns ascending `(a_index, b_index)` matches, or `None` past `max_d`.
pub(super) fn myers_matches(a: &[u64], b: &[u64], max_d: usize) -> Option<Vec<(usize, usize)>> {
    let (n, m) = (a.len() as i32, b.len() as i32);
    if n == 0 || m == 0 {
        return Some(Vec::new());
    }
    let max_d = max_d.min((n + m) as usize) as i32;
    let off = max_d + 1;
    let at = |k: i32| (k + off) as usize;
    let mut v = vec![0i32; (2 * max_d + 3) as usize];
    let mut trace: Vec<Vec<i32>> = Vec::new();
    'search: for d in 0..=max_d {
        trace.push(v.clone());
        for k in (-d..=d).step_by(2) {
            let mut x = if k == -d || (k != d && v[at(k - 1)] < v[at(k + 1)]) { v[at(k + 1)] } else { v[at(k - 1)] + 1 };
            let mut y = x - k;
            while x < n && y < m && a[x as usize] == b[y as usize] {
                x += 1;
                y += 1;
            }
            v[at(k)] = x;
            if x >= n && y >= m {
                break 'search;
            }
        }
        if d == max_d {
            return None;
        }
    }
    let (mut x, mut y) = (n, m);
    let mut matches = Vec::new();
    for d in (0..trace.len() as i32).rev() {
        let vd = &trace[d as usize];
        let k = x - y;
        let prev_k = if k == -d || (k != d && vd[at(k - 1)] < vd[at(k + 1)]) { k + 1 } else { k - 1 };
        let (prev_x, prev_y) = (vd[at(prev_k)], vd[at(prev_k)] - prev_k);
        while x > prev_x && y > prev_y {
            x -= 1;
            y -= 1;
            matches.push((x as usize, y as usize));
        }
        if d > 0 {
            (x, y) = (prev_x, prev_y);
        }
    }
    matches.reverse();
    Some(matches)
}

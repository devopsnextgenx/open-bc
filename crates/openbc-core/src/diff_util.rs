//! Helpers shared by the row-oriented diff modules (tables, registry).

/// Maximal runs of consecutive rows that are *not* equal, as inclusive `(start, end)`.
pub(crate) fn changed_runs<T>(rows: &[T], is_equal: impl Fn(&T) -> bool) -> Vec<(usize, usize)> {
    let mut runs = Vec::new();
    let mut i = 0;
    while i < rows.len() {
        if is_equal(&rows[i]) {
            i += 1;
            continue;
        }
        let start = i;
        while i < rows.len() && !is_equal(&rows[i]) {
            i += 1;
        }
        runs.push((start, i - 1));
    }
    runs
}

#[cfg(test)]
mod tests {
    use super::changed_runs;

    #[test]
    fn groups_consecutive_changes() {
        let rows = [true, false, false, true, false, true, true, false];
        assert_eq!(changed_runs(&rows, |r| *r), vec![(1, 2), (4, 4), (7, 7)]);
        assert!(changed_runs(&[true, true], |r| *r).is_empty());
        assert!(changed_runs::<bool>(&[], |r| *r).is_empty());
    }
}

//! Binary compare actor: parse two executables with LIEF, diff them, publish the result tree.
//!
//! One actor per Binary Compare tab, following the same shape as [`crate::hex_compare`]:
//! commands arrive on a bounded channel, events leave on a bounded channel, and all CPU-bound
//! work (parsing, hashing, diffing) runs on Rayon, never on a Tokio worker.
//!
//! * [`BinaryCommand::Reload`] re-parses both files (the UI sends it after a file changed).
//! * [`BinaryCommand::ShowUnchanged`] only rebuilds the tree from the cached diff: no re-parse.
//!
//! Each side is parsed independently, so an unreadable or unsupported file is reported against
//! its own path (`errors[side]`) without hiding the other side's problem.
//!
//! The actor exists in every build. Without the `binary` feature (no LIEF) it reports
//! "built without binary compare support" per side instead of failing to link.
//!
//! LIEF parses from a local path, so remote (VFS) sources are not supported yet; such a path
//! yields a per-side error instead of a silent local read of the wrong file.

use std::path::PathBuf;
use std::sync::Arc;

use openbc_core::binary_compare::{build_tree, flatten, BinaryDiff, ChangeState, DiffCounts, FlatRow};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

/// Options for parsing, independent of whether LIEF is compiled in.
#[derive(Clone, Debug)]
pub struct BinaryOptions {
    /// Minimum characters for an extracted string.
    pub min_string_len: usize,
    /// Refuse files above this size.
    pub max_file_bytes: u64,
}

impl Default for BinaryOptions {
    fn default() -> Self {
        Self { min_string_len: 4, max_file_bytes: 1024 * 1024 * 1024 }
    }
}

/// Commands accepted by the actor.
#[derive(Debug)]
pub enum BinaryCommand {
    /// Parse and diff both files again.
    Reload,
    /// Show (`true`) or hide (`false`) unchanged leaves; re-uses the cached diff.
    ShowUnchanged(bool),
}

/// Everything the UI needs to display one finished comparison.
#[derive(Debug, Default)]
pub struct BinarySummary {
    /// Pre-order tree rows (empty when a side failed).
    pub rows: Vec<FlatRow>,
    /// Counts over every compared element.
    pub totals: DiffCounts,
    /// Whole-file verdict (SHA-256 of both files).
    pub file_state: Option<ChangeState>,
    /// Per-side error text, `[left, right]`.
    pub errors: [Option<String>; 2],
    /// Display labels `[left, right]`.
    pub labels: [String; 2],
    /// `true` when the rows list only changed elements.
    pub changes_only: bool,
}

/// Events emitted by the actor.
#[derive(Debug)]
pub enum BinaryEvent {
    /// Coarse progress text (`parsing`, `diffing`).
    Stage(&'static str),
    /// A finished comparison.
    Summary(Arc<BinarySummary>),
    /// Fatal actor error.
    Error(String),
}

/// Non-blocking command sender; `send` returns `false` when the actor is busy or gone.
pub type BinaryCompareHandle = CommandHandle<BinaryCommand>;

/// Start the actor for `left` (baseline, Version 1) and `right` (target, Version 2).
pub fn spawn_binary_compare(
    rt: &Handle,
    left: PathBuf,
    right: PathBuf,
    options: BinaryOptions,
) -> (BinaryCompareHandle, mpsc::Receiver<BinaryEvent>) {
    let (handle, commands, events, event_rx) = channels();
    rt.spawn(run([left, right], options, commands, events));
    (handle, event_rx)
}

enum Parsed {
    Ok(Arc<BinaryDiff>),
    Failed([Option<String>; 2], [String; 2]),
}

fn label_of(path: &std::path::Path) -> String {
    path.file_name().map_or_else(|| path.display().to_string(), |n| n.to_string_lossy().into_owned())
}

/// Parse both files (in parallel on Rayon) and diff them. Pure CPU work: call from Rayon only.
fn parse_and_diff(paths: &[PathBuf; 2], options: &BinaryOptions) -> Parsed {
    let labels = [label_of(&paths[0]), label_of(&paths[1])];
    let mut errors: [Option<String>; 2] = [None, None];
    for i in 0..2 {
        if is_unset(&paths[i]) {
            errors[i] = Some("no file selected".to_owned());
        }
    }
    if errors.iter().any(Option::is_some) {
        return Parsed::Failed(errors, labels);
    }
    match backend::parse_pair(paths, options) {
        Ok(diff) => Parsed::Ok(Arc::new(diff)),
        Err(errors) => Parsed::Failed(errors, labels),
    }
}

#[cfg(feature = "binary")]
mod backend {
    use std::path::PathBuf;

    use openbc_compute::binary::{architecture_of, parse_binary, ParseOptions};
    use openbc_core::binary_compare::{diff_binaries, BinaryDiff};

    use super::BinaryOptions;

    pub(super) fn parse_pair(paths: &[PathBuf; 2], options: &BinaryOptions) -> Result<BinaryDiff, [Option<String>; 2]> {
        let options = ParseOptions {
            min_string_len: options.min_string_len,
            max_file_bytes: options.max_file_bytes,
            ..ParseOptions::default()
        };
        let (left, right) = rayon::join(|| parse_binary(&paths[0], &options), || parse_binary(&paths[1], &options));
        let (left, right) = match (left, right) {
            (Ok(l), Ok(r)) => (l, r),
            (l, r) => return Err([l.err().map(|e| e.to_string()), r.err().map(|e| e.to_string())]),
        };
        // Universal binaries: re-parse the right side for the left side's architecture so the
        // comparison is slice-to-slice.
        let right = match architecture_of(&left) {
            Some(arch) if architecture_of(&right).as_deref() != Some(arch.as_str()) => {
                let mut opts = options.clone();
                opts.prefer_arch = Some(arch);
                parse_binary(&paths[1], &opts).unwrap_or(right)
            }
            _ => right,
        };
        Ok(diff_binaries(&left, &right))
    }
}

#[cfg(not(feature = "binary"))]
mod backend {
    use std::path::PathBuf;

    use openbc_core::binary_compare::BinaryDiff;

    use super::BinaryOptions;

    pub(super) fn parse_pair(_: &[PathBuf; 2], _: &BinaryOptions) -> Result<BinaryDiff, [Option<String>; 2]> {
        let msg = "this build of OpenBC has no binary compare support (compiled without the `binary` feature / LIEF)";
        Err([Some(msg.to_owned()), Some(msg.to_owned())])
    }
}

fn summarize(diff: &BinaryDiff, show_unchanged: bool) -> BinarySummary {
    BinarySummary {
        rows: flatten(&build_tree(diff, show_unchanged)),
        totals: diff.totals(),
        file_state: Some(diff.file_state),
        errors: [None, None],
        labels: [diff.left_label.clone(), diff.right_label.clone()],
        changes_only: !show_unchanged,
    }
}

async fn run(
    paths: [PathBuf; 2],
    options: BinaryOptions,
    mut commands: mpsc::Receiver<BinaryCommand>,
    events: mpsc::Sender<BinaryEvent>,
) {
    let mut show_unchanged = true;
    let mut cached: Option<Arc<BinaryDiff>> = None;
    let mut reload = true;

    loop {
        if reload {
            reload = false;
            if !send_event(&events, BinaryEvent::Stage("parsing")).await {
                return;
            }
            let (p, o) = (paths.clone(), options.clone());
            let parsed = match rayon_run(move || parse_and_diff(&p, &o)).await {
                Ok(parsed) => parsed,
                Err(_) => {
                    if !send_event(&events, BinaryEvent::Error("binary compare worker stopped unexpectedly".into())).await {
                        return;
                    }
                    Parsed::Failed([None, None], [String::new(), String::new()])
                }
            };
            let summary = match parsed {
                Parsed::Ok(diff) => {
                    let s = summarize(&diff, show_unchanged);
                    cached = Some(diff);
                    s
                }
                Parsed::Failed(errors, labels) => {
                    cached = None;
                    BinarySummary { errors, labels, ..BinarySummary::default() }
                }
            };
            if !send_event(&events, BinaryEvent::Summary(Arc::new(summary))).await {
                return;
            }
        }

        // Drain at least one command, then everything queued, so a burst costs one rebuild.
        let Some(first) = commands.recv().await else { return };
        let mut batch = vec![first];
        while let Ok(more) = commands.try_recv() {
            batch.push(more);
        }
        let mut rebuild = false;
        for command in batch {
            match command {
                BinaryCommand::Reload => reload = true,
                BinaryCommand::ShowUnchanged(v) => {
                    rebuild |= v != show_unchanged;
                    show_unchanged = v;
                }
            }
        }
        if rebuild && !reload {
            if let Some(diff) = cached.clone() {
                let flags = show_unchanged;
                let built = rayon_run(move || summarize(&diff, flags)).await;
                if let Ok(summary) = built {
                    if !send_event(&events, BinaryEvent::Summary(Arc::new(summary))).await {
                        return;
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(feature = "binary")]
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn compares_the_test_binary_with_itself_and_toggles_unchanged() {
        let exe = std::env::current_exe().unwrap();
        let (handle, mut rx) =
            spawn_binary_compare(&Handle::current(), exe.clone(), exe, BinaryOptions::default());
        let mut full = None;
        while let Some(ev) = rx.recv().await {
            if let BinaryEvent::Summary(s) = ev {
                full = Some(s);
                break;
            }
        }
        let full = full.expect("summary");
        assert!(full.errors.iter().all(Option::is_none));
        assert_eq!(full.file_state, Some(ChangeState::Unchanged));
        assert_eq!(full.totals.changed(), 0);
        assert!(!full.rows.is_empty());

        assert!(handle.send(BinaryCommand::ShowUnchanged(false)));
        let lean = loop {
            match rx.recv().await.expect("event") {
                BinaryEvent::Summary(s) => break s,
                _ => continue,
            }
        };
        assert!(lean.changes_only);
        assert!(lean.rows.len() < full.rows.len());
    }

    #[cfg(feature = "binary")]
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn reports_errors_per_side() {
        let exe = std::env::current_exe().unwrap();
        let missing = PathBuf::from("/definitely/not/here.bin");
        let (_h, mut rx) =
            spawn_binary_compare(&Handle::current(), exe, missing, BinaryOptions::default());
        loop {
            if let Some(BinaryEvent::Summary(s)) = rx.recv().await {
                assert!(s.errors[0].is_none());
                assert!(s.errors[1].is_some());
                assert!(s.rows.is_empty());
                return;
            }
        }
    }
}

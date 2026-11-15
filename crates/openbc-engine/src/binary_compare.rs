//! Binary compare actor: parse two executables with LIEF, diff them, publish the result tree.
//!
//! One actor per Binary Compare tab, following the same shape as [`crate::hex_compare`]:
//! commands arrive on a bounded channel, events leave on a bounded channel, and all CPU-bound
//! work (parsing, hashing, diffing) runs on Rayon, never on a Tokio worker.
//!
//! * [`BinaryCommand::Reload`] re-parses both files (the UI sends it after a file changed).
//! * [`BinaryCommand::ShowUnchanged`], [`BinaryCommand::SetFilters`] and
//!   [`BinaryCommand::SetDemangle`] only rebuild the tree from the cached diff: no re-parse. Any
//!   of them that arrive while the files are still being parsed are applied before the first
//!   result is published, so a tab restored with filters on never flashes the unfiltered tree.
//! * [`BinaryCommand::Inspect`] reads the bytes of one function or section straight from disk
//!   (offset and hash recorded by the parser) and answers with a hex diff and, for x86-64
//!   code, a disassembly diff ([`BinaryEvent::Inspect`]).
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

use openbc_core::binary_compare::{
    build_tree_with, flatten, visible_counts, BinaryDiff, ChangeState, DiffCounts, FlatRow, InspectOutput,
    ViewOptions, HIDE_ALL,
};
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
    /// Granular change filters, a bit set of `openbc_core::binary_compare::HIDE_*`; re-uses the
    /// cached diff.
    SetFilters(u32),
    /// Show C++ names demangled (`true`) or raw (`false`); re-uses the cached diff.
    SetDemangle(bool),
    /// Open the hex / disassembly inspector on the function (`kind` =
    /// `NodeKind::Function.code()`) or section (`NodeKind::Section.code()`) with identity `key`.
    /// Latest wins; a reload cancels it.
    Inspect {
        /// `NodeKind` code of the row.
        kind: u8,
        /// `FlatRow::key` of the row.
        key: String,
        /// Caller-chosen request number, echoed in [`InspectSummary::seq`] so the UI can tell
        /// a stale answer from the current one.
        seq: u64,
    },
}

/// Everything the UI needs to display one finished comparison.
#[derive(Debug, Default)]
pub struct BinarySummary {
    /// Pre-order tree rows (empty when a side failed).
    pub rows: Vec<FlatRow>,
    /// Counts over every compared element that the active filters leave visible.
    pub totals: DiffCounts,
    /// Changed elements hidden by the granular filters.
    pub hidden: usize,
    /// Whole-file verdict (SHA-256 of both files).
    pub file_state: Option<ChangeState>,
    /// Per-side error text, `[left, right]`.
    pub errors: [Option<String>; 2],
    /// Display labels `[left, right]`.
    pub labels: [String; 2],
    /// `true` when the rows list only changed elements.
    pub changes_only: bool,
}

/// Answer to [`BinaryCommand::Inspect`].
#[derive(Debug, Default)]
pub struct InspectSummary {
    /// The `seq` of the request this answers.
    pub seq: u64,
    /// Hex and disassembly rows (empty when `error` is set).
    pub output: InspectOutput,
    /// Why nothing could be shown (unknown element, unreadable file, built without LIEF).
    pub error: Option<String>,
}

/// Events emitted by the actor.
#[derive(Debug)]
pub enum BinaryEvent {
    /// Coarse progress text (`parsing`, `diffing`).
    Stage(&'static str),
    /// A finished comparison.
    Summary(Arc<BinarySummary>),
    /// The inspector content for the last [`BinaryCommand::Inspect`].
    Inspect(Arc<InspectSummary>),
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

    use openbc_compute::binary::inspect::read_region;
    use openbc_compute::binary::{architecture_of, disasm, parse_binary, ParseOptions};
    use openbc_core::binary_compare::inspect::{build_output, MAX_INSPECT_BYTES};
    use openbc_core::binary_compare::{
        diff_binaries, AsmLine, BinaryDiff, FunctionInfo, InspectOutput, NodeKind, SectionInfo,
    };

    use super::{BinaryOptions, InspectSummary};

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

    struct Side {
        offset: u64,
        size: u64,
        sha256: String,
    }

    fn failure(message: &str) -> InspectSummary {
        InspectSummary { seq: 0, output: InspectOutput::default(), error: Some(message.to_owned()) }
    }

    /// Read both versions of one function or section from disk and build the hex / disassembly diff.
    pub(super) fn inspect(paths: &[PathBuf; 2], diff: &BinaryDiff, kind: u8, key: &str) -> InspectSummary {
        let mut wants_asm = false;
        let sides: [Option<Side>; 2] = if kind == NodeKind::Function.code() {
            let Some(entry) = diff.functions.iter().find(|e| e.key == key) else {
                return failure("function not found in the comparison");
            };
            wants_asm = true;
            let side = |f: &Option<FunctionInfo>| {
                f.as_ref().and_then(|f| f.file_offset.map(|offset| Side { offset, size: f.size, sha256: f.sha256.clone() }))
            };
            [side(&entry.left), side(&entry.right)]
        } else if kind == NodeKind::Section.code() {
            let Some(entry) = diff.sections.iter().find(|e| e.key == key) else {
                return failure("section not found in the comparison");
            };
            let info = entry.right.as_ref().or(entry.left.as_ref());
            wants_asm = info.is_some_and(|s| {
                let name = s.name.to_ascii_lowercase();
                s.flags.contains("EXEC") || name.contains("text") || name.contains("code")
            });
            let side = |s: &Option<SectionInfo>| {
                s.as_ref().and_then(|s| s.file_offset.map(|offset| Side { offset, size: s.size, sha256: s.sha256.clone() }))
            };
            [side(&entry.left), side(&entry.right)]
        } else {
            return failure("only functions and sections can be inspected");
        };

        let mut notes = Vec::new();
        let mut data: [Option<Vec<u8>>; 2] = [None, None];
        for (i, label) in ["Version 1", "Version 2"].iter().enumerate() {
            match &sides[i] {
                None => notes.push(format!("{label}: not present, or its file location is unknown")),
                Some(side) => match read_region(&paths[i], side.offset, side.size, &side.sha256, MAX_INSPECT_BYTES) {
                    Ok(region) => {
                        if region.truncated {
                            notes.push(format!("{label}: showing the first {} of {} bytes", region.bytes.len(), side.size));
                        } else if !region.verified {
                            notes.push(format!(
                                "{label}: the bytes on disk do not match the analysed hash (the file changed after it was \
                                 loaded, or it is a slice of a universal binary); this view may be wrong"
                            ));
                        }
                        data[i] = Some(region.bytes);
                    }
                    Err(error) => notes.push(error),
                },
            }
        }

        let mut asm: [Option<Vec<AsmLine>>; 2] = [None, None];
        if wants_asm {
            match diff.meta_value("architecture").and_then(disasm::bitness_for) {
                Some(bitness) => {
                    for i in 0..2 {
                        asm[i] = data[i].as_deref().map(|bytes| disasm::disassemble(bitness, bytes));
                    }
                }
                None => notes.push("Disassembly is available for x86-64 only; showing the hex view.".to_owned()),
            }
        }
        let output = build_output(key, data[0].as_deref(), data[1].as_deref(), asm[0].as_deref(), asm[1].as_deref(), notes);
        InspectSummary { seq: 0, output, error: None }
    }
}

#[cfg(not(feature = "binary"))]
mod backend {
    use std::path::PathBuf;

    use openbc_core::binary_compare::{BinaryDiff, InspectOutput};

    use super::{BinaryOptions, InspectSummary};

    pub(super) fn parse_pair(_: &[PathBuf; 2], _: &BinaryOptions) -> Result<BinaryDiff, [Option<String>; 2]> {
        let msg = "this build of OpenBC has no binary compare support (compiled without the `binary` feature / LIEF)";
        Err([Some(msg.to_owned()), Some(msg.to_owned())])
    }

    pub(super) fn inspect(_: &[PathBuf; 2], _: &BinaryDiff, _: u8, _: &str) -> InspectSummary {
        InspectSummary {
            seq: 0,
            output: InspectOutput::default(),
            error: Some("this build of OpenBC has no binary compare support".to_owned()),
        }
    }
}

fn summarize(diff: &BinaryDiff, view: &ViewOptions) -> BinarySummary {
    let totals = visible_counts(diff, view);
    BinarySummary {
        rows: flatten(&build_tree_with(diff, view)),
        hidden: diff.totals().changed().saturating_sub(totals.changed()),
        totals,
        file_state: Some(diff.file_state),
        errors: [None, None],
        labels: [diff.left_label.clone(), diff.right_label.clone()],
        changes_only: !view.show_unchanged,
    }
}

/// Applies one command to the actor state. Returns `true` when the tree must be rebuilt.
fn apply_command(
    command: BinaryCommand,
    view: &mut ViewOptions,
    reload: &mut bool,
    inspect: &mut Option<(u8, String, u64)>,
) -> bool {
    match command {
        BinaryCommand::Reload => {
            *reload = true;
            false
        }
        BinaryCommand::ShowUnchanged(show) => {
            let changed = view.show_unchanged != show;
            view.show_unchanged = show;
            changed
        }
        BinaryCommand::SetFilters(bits) => {
            let bits = bits & HIDE_ALL;
            let changed = view.hide != bits;
            view.hide = bits;
            changed
        }
        BinaryCommand::SetDemangle(on) => {
            let changed = view.demangle != on;
            view.demangle = on;
            changed
        }
        BinaryCommand::Inspect { kind, key, seq } => {
            *inspect = Some((kind, key, seq));
            false
        }
    }
}

async fn run(
    paths: [PathBuf; 2],
    options: BinaryOptions,
    mut commands: mpsc::Receiver<BinaryCommand>,
    events: mpsc::Sender<BinaryEvent>,
) {
    let mut view = ViewOptions::default();
    let mut cached: Option<Arc<BinaryDiff>> = None;
    let mut reload = true;
    let mut pending_inspect: Option<(u8, String, u64)> = None;

    loop {
        if reload {
            reload = false;
            pending_inspect = None; // its row may not exist any more
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
            // View settings sent while parsing are honoured by the very first result.
            while let Ok(command) = commands.try_recv() {
                apply_command(command, &mut view, &mut reload, &mut pending_inspect);
            }
            let summary = match parsed {
                Parsed::Ok(diff) => {
                    let s = summarize(&diff, &view);
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
            if reload {
                continue; // a Reload arrived while parsing
            }
            pending_inspect = None;
        }

        // Drain at least one command, then everything queued, so a burst costs one rebuild.
        let Some(first) = commands.recv().await else { return };
        let mut batch = vec![first];
        while let Ok(more) = commands.try_recv() {
            batch.push(more);
        }
        let mut rebuild = false;
        for command in batch {
            rebuild |= apply_command(command, &mut view, &mut reload, &mut pending_inspect);
        }
        if reload {
            continue;
        }
        if rebuild {
            if let Some(diff) = cached.clone() {
                let flags = view;
                let built = rayon_run(move || summarize(&diff, &flags)).await;
                if let Ok(summary) = built {
                    if !send_event(&events, BinaryEvent::Summary(Arc::new(summary))).await {
                        return;
                    }
                }
            }
        }
        if let Some((kind, key, seq)) = pending_inspect.take() {
            let mut answer = match cached.clone() {
                Some(diff) => {
                    let p = paths.clone();
                    rayon_run(move || backend::inspect(&p, &diff, kind, &key)).await.unwrap_or_else(|_| InspectSummary {
                        seq: 0,
                        output: InspectOutput::default(),
                        error: Some("inspector worker stopped unexpectedly".to_owned()),
                    })
                }
                None => InspectSummary {
                    seq: 0,
                    output: InspectOutput::default(),
                    error: Some("nothing to inspect: the comparison failed".to_owned()),
                },
            };
            answer.seq = seq;
            if !send_event(&events, BinaryEvent::Inspect(Arc::new(answer))).await {
                return;
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
    async fn inspector_reads_identical_bytes_for_a_self_compare() {
        let exe = std::env::current_exe().unwrap();
        let (handle, mut rx) =
            spawn_binary_compare(&Handle::current(), exe.clone(), exe, BinaryOptions::default());
        let summary = loop {
            if let Some(BinaryEvent::Summary(s)) = rx.recv().await {
                break s;
            }
        };
        // Stripped test binaries have no inspectable rows; nothing to assert then.
        let Some(row) = summary.rows.iter().find(|r| r.inspect && r.kind == openbc_core::binary_compare::NodeKind::Function.code()) else {
            return;
        };
        assert!(handle.send(BinaryCommand::Inspect { kind: row.kind, key: row.key.clone(), seq: 7 }));
        let answer = loop {
            if let Some(BinaryEvent::Inspect(a)) = rx.recv().await {
                break a;
            }
        };
        assert_eq!(answer.seq, 7);
        assert!(answer.error.is_none(), "{:?}", answer.error);
        assert!(!answer.output.hex.is_empty());
        assert_eq!(answer.output.differing_bytes, 0);
        assert_eq!(answer.output.left_len, answer.output.right_len);
    }

    #[cfg(feature = "binary")]
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn filters_and_demangle_sent_while_parsing_shape_the_first_result() {
        let exe = std::env::current_exe().unwrap();
        let (handle, mut rx) =
            spawn_binary_compare(&Handle::current(), exe.clone(), exe, BinaryOptions::default());
        assert!(handle.send(BinaryCommand::ShowUnchanged(false)));
        assert!(handle.send(BinaryCommand::SetFilters(openbc_core::binary_compare::HIDE_ALL)));
        let first = loop {
            if let Some(BinaryEvent::Summary(s)) = rx.recv().await {
                break s;
            }
        };
        assert!(first.changes_only, "the first result already honours the early commands");
        assert_eq!(first.hidden, 0, "a self-compare has nothing to hide");
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

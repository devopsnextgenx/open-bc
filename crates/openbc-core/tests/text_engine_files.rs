//! Engine tests against real files: mmap vs buffered I/O, millions of lines, spilled indexes,
//! change detection and a complete diff over a multi-megabyte pair.

use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::sync::Arc;

use openbc_core::text::engine::*;

struct TempDir(PathBuf);
impl TempDir {
    fn new(tag: &str) -> Self {
        let p = std::env::temp_dir().join(format!("openbc_engine_{}_{tag}", std::process::id()));
        fs::create_dir_all(&p).unwrap();
        Self(p)
    }
    fn file(&self, name: &str, bytes: &[u8]) -> PathBuf {
        let p = self.0.join(name);
        fs::File::create(&p).unwrap().write_all(bytes).unwrap();
        p
    }
}
impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn open(l: &PathBuf, r: &PathBuf, cfg: CompareConfig) -> TextCompareModel {
    TextCompareModel::open(Some(l), Some(r), cfg, &CancelToken::new(), &mut |_| {}).unwrap()
}

fn rows(m: &TextCompareModel) -> Vec<(Option<u64>, Option<u64>, DiffType)> {
    (0..m.row_count()).map(|r| m.row(r).unwrap()).map(|r| (r.left_line, r.right_line, r.kind)).collect()
}

#[test]
fn mapped_and_buffered_access_give_identical_results() {
    let dir = TempDir::new("mapvsbuf");
    let a: String = (0..90_000).map(|i| format!("line {i} ünïcode\r\n")).collect();
    let mut b = a.clone().replace("line 777 ", "LINE 777 ").replace("line 20000 ", "");
    b.push_str("tail without newline");
    let (l, r) = (dir.file("a.txt", a.as_bytes()), dir.file("b.txt", b.as_bytes()));
    let mapped = open(&l, &r, CompareConfig::default());
    let mut cfg = CompareConfig::default();
    cfg.map_policy = MapPolicy::Buffered;
    let buffered = open(&l, &r, cfg);
    assert_eq!(rows(&mapped), rows(&buffered));
    for line in [0, 776, 777, 85_000] {
        assert_eq!(
            mapped.line_text(SideId::Right, line).unwrap(),
            buffered.line_text(SideId::Right, line).unwrap()
        );
    }
    let src = MappedTextFile::open(&l).unwrap();
    assert!(src.is_mapped());
    let src = MappedTextFile::open_with(&l, MapPolicy::Buffered).unwrap();
    assert!(!src.is_mapped());
    // Reads that cross the 1 MiB block boundary of the buffered reader are stitched correctly.
    let whole = fs::read(&l).unwrap();
    let mapped_src = MappedTextFile::open(&l).unwrap();
    let off = (1 << 20) - 10;
    assert_eq!(src.read(off, 100).unwrap().as_ref(), &whole[off as usize..off as usize + 100]);
    assert_eq!(src.read(off, 3 << 20).unwrap().as_ref(), mapped_src.read(off, 3 << 20).unwrap().as_ref());
    assert!(src.read(whole.len() as u64 + 5, 10).unwrap().is_empty());
}

#[test]
fn millions_of_short_lines_with_edits_everywhere() {
    let dir = TempDir::new("millions");
    const N: usize = 2_000_000;
    let mut left = Vec::with_capacity(N * 8);
    let mut right = Vec::with_capacity(N * 8);
    let (mut inserted, mut deleted, mut changed) = (0u64, 0u64, 0u64);
    for i in 0..N {
        let line = format!("l{i}\n");
        left.extend_from_slice(line.as_bytes());
        match i {
            10 => { right.extend_from_slice(b"new-a\nnew-b\n"); inserted += 2; right.extend_from_slice(line.as_bytes()); }
            900_000 => { deleted += 1; }
            1_500_000 => { right.extend_from_slice(b"changed\n"); changed += 1; }
            _ => right.extend_from_slice(line.as_bytes()),
        }
    }
    let (l, r) = (dir.file("l.txt", &left), dir.file("r.txt", &right));
    let mut cfg = CompareConfig::default();
    cfg.index.ram_record_limit = 500_000; // force the index to spill to its mapped temp file
    let m = open(&l, &r, cfg);
    assert_eq!(m.line_count(SideId::Left), N as u64);
    assert!(m.memory_report().index_resident_bytes == 0, "index must live in the mapped spill file");
    let s = m.stats();
    assert_eq!((s.inserted_rows, s.deleted_rows, s.replaced_rows, s.blocks), (inserted, deleted, changed, 3));
    // Spot-check alignment far from the edits: 2 lines were inserted and 1 deleted before the end.
    let right_line = 1_999_999 + inserted - deleted;
    let row = m.row_for_line(SideId::Right, right_line).unwrap();
    assert_eq!(m.row(row).unwrap().left_line, Some(1_999_999));
    assert_eq!(m.line_text(SideId::Right, right_line).unwrap(), "l1999999");
    assert_eq!(m.line_text(SideId::Left, 1_999_999).unwrap(), "l1999999");
    // Only the indexes live in memory; diff index is a handful of run-length segments.
    assert!(m.memory_report().diff_bytes < 4096);
}

#[test]
fn source_changes_are_detected_and_cancel_leaves_no_state() {
    let dir = TempDir::new("change");
    let l = dir.file("a.txt", b"one\ntwo\n");
    let r = dir.file("b.txt", b"one\ntwo\n");
    let m = open(&l, &r, CompareConfig::default());
    assert!(m.verify_unchanged().is_ok());
    std::thread::sleep(std::time::Duration::from_millis(20));
    fs::OpenOptions::new().append(true).open(&l).unwrap().write_all(b"three\n").unwrap();
    assert!(matches!(m.verify_unchanged(), Err(EngineError::SourceChanged)));

    let cancel = CancelToken::new();
    cancel.cancel();
    let r2 = TextCompareModel::open(Some(&l), Some(&r), CompareConfig::default(), &cancel, &mut |_| {});
    assert!(matches!(r2, Err(EngineError::Cancelled)));
    // No spill files left behind by a cancelled / failed build.
    let leaked = fs::read_dir(std::env::temp_dir())
        .unwrap()
        .filter_map(Result::ok)
        .filter(|e| e.file_name().to_string_lossy().starts_with(&format!("openbc-lineindex-{}", std::process::id())))
        .count();
    assert_eq!(leaked, 0);
}

#[test]
fn non_regular_and_missing_inputs_fail_cleanly() {
    let dir = TempDir::new("bad");
    assert!(MappedTextFile::open(&dir.0).is_err()); // a directory
    assert!(MappedTextFile::open(dir.0.join("nope")).is_err());
    let m = TextCompareModel::open(None, None, CompareConfig::default(), &CancelToken::new(), &mut |_| {}).unwrap();
    assert_eq!(m.row_count(), 0);
}

#[test]
fn memory_source_models_work_without_files() {
    let m = TextCompareModel::from_sources(
        Arc::new(MemorySource::new(b"a\nb\nc\n".to_vec())),
        Arc::new(MemorySource::new(b"a\nc\n".to_vec())),
        CompareConfig::default(),
        &CancelToken::new(),
        &mut |_| {},
    )
    .unwrap();
    assert_eq!(m.stats().deleted_rows, 1);
}

//! Per-file-type matching policy in folder compare.

use std::fs::File;
use std::path::Path;
use std::sync::Arc;
use std::time::{Duration, SystemTime};

use openbc_core::{ComparisonStatus, EntryPath, FolderComparison, MatchPolicy};
use openbc_vfs::{AsyncVfs, LocalVfs};

use crate::compare_folder_sides_with_policy;

fn write(dir: &Path, name: &str, bytes: &[u8], mtime: SystemTime) {
    std::fs::write(dir.join(name), bytes).unwrap();
    File::options().write(true).open(dir.join(name)).unwrap().set_modified(mtime).unwrap();
}

async fn run(left: &Path, right: &Path, check: bool, ignore: bool) -> FolderComparison {
    let l: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(left));
    let r: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(right));
    compare_folder_sides_with_policy(
        l,
        Some(EntryPath("".into())),
        r,
        Some(EntryPath("".into())),
        check,
        ignore,
        &MatchPolicy::builtin(),
    )
    .await
    .unwrap()
}

fn status<'a>(c: &'a FolderComparison, name: &str) -> &'a ComparisonStatus {
    &c.entries.iter().find(|e| e.name == name).unwrap_or_else(|| panic!("no entry {name}")).status
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn policy_picks_hash_for_binaries_and_timestamps_for_text() {
    let tmp = tempfile::tempdir().unwrap();
    let (l, r) = (tmp.path().join("l"), tmp.path().join("r"));
    std::fs::create_dir_all(&l).unwrap();
    std::fs::create_dir_all(&r).unwrap();
    let t0 = SystemTime::now() - Duration::from_secs(10_000);
    let t1 = t0 + Duration::from_secs(600);

    // binary: same size, different bytes, SAME timestamp -> hash catches it
    write(&l, "diff.bin", b"AAAA", t0);
    write(&r, "diff.bin", b"AAAB", t0);
    // binary: same bytes, different timestamp -> hash says equal
    write(&l, "rebuilt.dll", b"PAYLOAD", t0);
    write(&r, "rebuilt.dll", b"PAYLOAD", t1);
    // text: same size, different bytes, same timestamp -> size/timestamp trusts it
    write(&l, "a.txt", b"1111", t0);
    write(&r, "a.txt", b"2222", t0);
    // text: same bytes, different timestamp -> size/timestamp reports a time difference
    write(&l, "b.rs", b"fn main(){}", t0);
    write(&r, "b.rs", b"fn main(){}", t1);
    // different size always differs, before any read
    write(&l, "c.exe", b"12", t0);
    write(&r, "c.exe", b"123", t0);

    let c = run(&l, &r, false, false).await;
    assert_eq!(*status(&c, "diff.bin"), ComparisonStatus::Different);
    assert_eq!(*status(&c, "rebuilt.dll"), ComparisonStatus::Equal);
    assert_eq!(*status(&c, "a.txt"), ComparisonStatus::Equal);
    assert!(!matches!(status(&c, "b.rs"), ComparisonStatus::Equal | ComparisonStatus::Different));
    assert_eq!(*status(&c, "c.exe"), ComparisonStatus::Different);

    // explicit "compare contents" overrides the text rule: hash every file
    let forced = run(&l, &r, true, false).await;
    assert_eq!(*status(&forced, "a.txt"), ComparisonStatus::Different);
    assert_eq!(*status(&forced, "b.rs"), ComparisonStatus::Equal);

    // explicit "ignore timestamps": size only
    let ignored = run(&l, &r, false, true).await;
    assert_eq!(*status(&ignored, "b.rs"), ComparisonStatus::Equal);
    assert_eq!(*status(&ignored, "diff.bin"), ComparisonStatus::Equal);
}

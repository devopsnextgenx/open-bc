//! End-to-end tests of archives-as-folders against real archives made by real tools
//! (`fixtures/gen_fixtures.sh`). Everything goes through `ArchiveAwareVfs` over `LocalVfs`, i.e.
//! exactly the path the application takes.
#![cfg(feature = "archive-core")]

use std::path::PathBuf;
use std::sync::Arc;

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use openbc_vfs::archive::{ArchiveAwareVfs, ArchiveOptions};
use openbc_vfs::{read_small_file, AsyncVfs, LocalVfs, VfsError};
use tokio::io::AsyncRead;

fn fixtures() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures")
}

fn vfs() -> ArchiveAwareVfs {
    ArchiveAwareVfs::new(Arc::new(LocalVfs::new(fixtures())), ArchiveOptions::default())
}

fn p(s: &str) -> EntryPath {
    EntryPath(PathBuf::from(s))
}

async fn names(v: &dyn AsyncVfs, path: &str) -> Vec<String> {
    let mut n: Vec<String> = v.read_dir(&p(path)).await.unwrap().into_iter().map(|e| e.name).collect();
    n.sort();
    n
}

async fn text(v: &dyn AsyncVfs, path: &str) -> String {
    String::from_utf8(read_small_file(v, &p(path)).await.unwrap()).unwrap()
}

/// The tree every `sample.*` archive holds.
async fn check_sample(v: &dyn AsyncVfs, archive: &str, with_empty_dir: bool) {
    let root = names(v, archive).await;
    let expect: Vec<&str> = if with_empty_dir { vec!["data", "empty", "readme.txt", "src"] } else { vec!["data", "readme.txt", "src"] };
    assert_eq!(root, expect, "{archive}");
    assert_eq!(text(v, &format!("{archive}/readme.txt")).await, "hello archive\n", "{archive}");
    assert_eq!(text(v, &format!("{archive}/src/main.rs")).await, "fn main() {}\n", "{archive}");
    assert_eq!(text(v, &format!("{archive}/src/lib/util.rs")).await, "pub fn util() {}\n", "{archive}");
    let blob = read_small_file(v, &p(&format!("{archive}/data/blob.bin"))).await.unwrap();
    assert_eq!(blob.len(), 1000);
    assert!(blob.iter().enumerate().all(|(i, b)| *b == (i % 251) as u8), "{archive}");
    let sub = v.read_dir(&p(&format!("{archive}/src"))).await.unwrap();
    let main = sub.iter().find(|e| e.name == "main.rs").unwrap();
    assert!(!main.metadata.is_dir && main.metadata.size == 13, "{archive}");
    // Entry paths stay in the outer namespace so callers can keep navigating with them.
    assert_eq!(main.path.0, PathBuf::from(format!("{archive}/src/main.rs")));
    assert!(sub.iter().find(|e| e.name == "lib").unwrap().metadata.is_dir);
    assert!(v.stat(&p(&format!("{archive}/src"))).await.unwrap().is_dir);
    assert!(v.stat(&p(&format!("{archive}/nope"))).await.is_err());
    assert!(v.read_dir(&p(&format!("{archive}/readme.txt"))).await.is_err(), "a file is not a folder");
}

#[tokio::test]
async fn every_format_exposes_the_same_tree() {
    let v = vfs();
    for a in ["sample.zip", "sample.tar", "sample.tar.gz", "sample.tgz", "sample.tar.bz2", "sample.tar.xz", "sample.tar.zst", "sample.tar.lz4"] {
        check_sample(&v, a, true).await;
    }
    check_sample(&v, "sample.7z", true).await;
    check_sample(&v, "sample.rar", true).await;
    // cab has no directory entries (the folders are implied by the file names).
    check_sample(&v, "sample.cab", false).await;
}

#[tokio::test]
async fn archive_file_itself_stays_a_file() {
    let v = vfs();
    let m = v.stat(&p("sample.zip")).await.unwrap();
    assert!(!m.is_dir, "copy/delete must keep treating the archive as a file");
    assert_eq!(m.size, std::fs::metadata(fixtures().join("sample.zip")).unwrap().len());
    let bytes = read_small_file(&v, &p("sample.zip")).await.unwrap();
    assert_eq!(&bytes[..2], b"PK");
    // And the parent listing shows it as a plain file.
    let listing = v.read_dir(&p("")).await.unwrap();
    assert!(!listing.iter().find(|e| e.name == "sample.zip").unwrap().metadata.is_dir);
}

#[tokio::test]
async fn single_file_compressions_show_one_file() {
    let v = vfs();
    for a in ["hello.txt.gz", "hello.txt.bz2", "hello.txt.xz", "hello.txt.zst"] {
        assert_eq!(names(&v, a).await, ["hello.txt"], "{a}");
        assert_eq!(text(&v, &format!("{a}/hello.txt")).await, "hello single\n", "{a}");
    }
}

#[tokio::test]
async fn deb_is_an_ar_of_tarballs_and_nesting_works() {
    let v = vfs();
    assert_eq!(names(&v, "sample.deb").await, ["control.tar.gz", "data.tar.gz", "debian-binary"]);
    assert_eq!(text(&v, "sample.deb/debian-binary").await, "2.0\n");
    assert_eq!(text(&v, "sample.deb/data.tar.gz/src/main.rs").await, "fn main() {}\n");
}

#[tokio::test]
async fn archives_nest_zip_in_7z_style() {
    let v = vfs();
    assert_eq!(names(&v, "outer.zip").await, ["inner.7z", "top.txt"]);
    assert!(!v.stat(&p("outer.zip/inner.7z")).await.unwrap().is_dir, "nested archive is a file until entered");
    check_sample(&v, "outer.zip/inner.7z", true).await;
}

#[tokio::test]
async fn hostile_entry_names_never_surface() {
    let v = vfs();
    let n = names(&v, "evil.zip").await;
    assert_eq!(n, ["abs.txt", "ok.txt", "sub"], "`..` entries dropped, absolute and backslash names flattened");
    assert_eq!(names(&v, "evil.zip/sub").await, ["win.txt"]);
    assert!(v.stat(&p("evil.zip/../evil.txt")).await.is_err());
}

#[tokio::test]
async fn archive_structure_operations_remain_read_only() {
    let v = vfs();
    assert!(matches!(v.create_dir(&p("sample.zip/d")).await, Err(VfsError::Unsupported(_))));
    assert!(matches!(v.remove_file(&p("sample.zip/readme.txt")).await, Err(VfsError::Unsupported(_))));
    assert!(matches!(v.rename(&p("sample.zip/readme.txt"), &p("sample.zip/r2")).await, Err(VfsError::Unsupported(_))));
}

#[cfg(feature = "archive-zip")]
#[tokio::test]
async fn local_zip_members_can_be_saved_in_place() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::copy(fixtures().join("sample.zip"), dir.path().join("sample.zip")).unwrap();
    let v = ArchiveAwareVfs::new(Arc::new(LocalVfs::new(dir.path())), ArchiveOptions::default());

    v.write_file(&p("sample.zip/src/main.rs"), b"fn changed() {}\n").await.unwrap();

    assert_eq!(text(&v, "sample.zip/src/main.rs").await, "fn changed() {}\n");
    assert_eq!(text(&v, "sample.zip/readme.txt").await, "hello archive\n");
}

#[tokio::test]
async fn non_zip_archive_members_remain_read_only() {
    let v = vfs();
    assert!(matches!(v.write_file(&p("sample.tar/readme.txt"), b"x").await, Err(VfsError::Unsupported(_))));
}

#[tokio::test]
async fn limited_reads_respect_the_limit() {
    let v = vfs();
    let path = p("sample.zip/data/blob.bin");
    assert!(v.read_file_limited(&path, 999).await.unwrap().is_none());
    assert_eq!(v.read_file_limited(&path, 1000).await.unwrap().unwrap().len(), 1000);
}

#[tokio::test]
async fn a_real_directory_with_an_archive_name_is_just_a_directory() {
    let dir = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(dir.path().join("backup.zip/inner")).unwrap();
    std::fs::write(dir.path().join("backup.zip/inner/f.txt"), "real").unwrap();
    let v = ArchiveAwareVfs::new(Arc::new(LocalVfs::new(dir.path())), ArchiveOptions::default());
    assert_eq!(names(&v, "backup.zip").await, ["inner"]);
    assert_eq!(text(&v, "backup.zip/inner/f.txt").await, "real");
}

#[tokio::test]
async fn replacing_the_archive_on_disk_is_noticed() {
    let dir = tempfile::tempdir().unwrap();
    let target = dir.path().join("moving.zip");
    std::fs::copy(fixtures().join("sample.zip"), &target).unwrap();
    let v = ArchiveAwareVfs::new(Arc::new(LocalVfs::new(dir.path())), ArchiveOptions::default());
    assert!(names(&v, "moving.zip").await.contains(&"readme.txt".to_owned()));
    std::fs::copy(fixtures().join("evil.zip"), &target).unwrap(); // different size
    assert_eq!(names(&v, "moving.zip").await, ["abs.txt", "ok.txt", "sub"]);
}

/// A provider that is not backed by local files (like SFTP): forces the copy-to-temp path.
struct NotLocal(LocalVfs);

#[async_trait]
impl AsyncVfs for NotLocal {
    async fn read_dir(&self, p: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        self.0.read_dir(p).await
    }
    async fn stat(&self, p: &EntryPath) -> Result<EntryMetadata, VfsError> {
        self.0.stat(p).await
    }
    async fn open_file(&self, p: &EntryPath) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        self.0.open_file(p).await
    }
}

#[tokio::test]
async fn remote_style_providers_work_and_respect_the_copy_limit() {
    let remote = Arc::new(NotLocal(LocalVfs::new(fixtures())));
    let v = ArchiveAwareVfs::new(remote.clone(), ArchiveOptions::default());
    check_sample(&v, "sample.7z", true).await;
    let tiny = ArchiveAwareVfs::new(remote, ArchiveOptions { max_spill_bytes: 100, ..ArchiveOptions::default() });
    let err = tiny.read_dir(&p("sample.zip")).await.unwrap_err().to_string();
    assert!(err.contains("copy limit"), "{err}");
}

#[tokio::test]
async fn expansion_limit_stops_bombs_but_listing_still_works_where_cheap() {
    let opts = ArchiveOptions { max_expanded_bytes: 10, ..ArchiveOptions::default() };
    let v = ArchiveAwareVfs::new(Arc::new(LocalVfs::new(fixtures())), opts);
    let err = v.read_dir(&p("sample.tar.gz")).await.unwrap_err().to_string();
    assert!(err.contains("refusing") || err.contains("expands"), "{err}");
    // 7z lists from its header; only reading content needs the expansion.
    assert!(names(&v, "sample.7z").await.contains(&"readme.txt".to_owned()));
    let err = read_small_file(&v, &p("sample.7z/readme.txt")).await.unwrap_err().to_string();
    assert!(err.contains("limit"), "{err}");
}

#[tokio::test]
async fn temp_files_are_removed_when_the_provider_is_dropped() {
    let tmp = tempfile::tempdir().unwrap();
    let opts = ArchiveOptions { temp_dir: Some(tmp.path().to_path_buf()), ..ArchiveOptions::default() };
    {
        let v = ArchiveAwareVfs::new(Arc::new(LocalVfs::new(fixtures())), opts);
        assert_eq!(text(&v, "sample.tar.xz/readme.txt").await, "hello archive\n");
        assert_eq!(text(&v, "sample.rar/readme.txt").await, "hello archive\n");
        assert!(std::fs::read_dir(tmp.path()).unwrap().count() >= 2, "expansion lives in temp while open");
    }
    assert_eq!(std::fs::read_dir(tmp.path()).unwrap().count(), 0, "temp folders must not leak");
}

#[tokio::test]
async fn plain_paths_pass_straight_through() {
    let v = vfs();
    assert!(names(&v, "").await.contains(&"sample.zip".to_owned()));
    assert!(v.stat(&p("does-not-exist.txt")).await.is_err());
    assert!(v.stat(&p("missing.zip/inner.txt")).await.is_err());
}

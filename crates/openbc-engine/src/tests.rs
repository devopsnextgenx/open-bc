//! End-to-end check of folder compare plus the file operations through `LocalVfs`.

use super::{
    compare_folder_level_with_options, copy_entry, create_directory, delete_entry, mirror_entry, read_file,
    rename_entry, write_file,
};
use openbc_core::{ComparisonStatus, EntryPath};
use openbc_vfs::{AsyncVfs, LocalVfs};
use std::sync::Arc;

#[tokio::test]
async fn compares_and_mutates_files_through_engine_and_vfs() {
    let unique = format!(
        "openbc-engine-test-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    );
    let root = std::env::temp_dir().join(unique);
    let left_root = root.join("left");
    let right_root = root.join("right");
    tokio::fs::create_dir_all(&left_root).await.unwrap();
    tokio::fs::create_dir_all(&right_root).await.unwrap();
    let left: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(&left_root));
    let right: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(&right_root));

    let file = EntryPath("sample.txt".into());
    write_file(left.as_ref(), &file, b"same").await.unwrap();
    write_file(right.as_ref(), &file, b"same").await.unwrap();
    let equal = compare_folder_level_with_options(
        Arc::clone(&left),
        EntryPath("".into()),
        Arc::clone(&right),
        EntryPath("".into()),
        true,
        false,
    )
    .await
    .unwrap();
    assert_eq!(equal.entries[0].status, ComparisonStatus::Equal);

    write_file(right.as_ref(), &file, b"diff").await.unwrap();
    let different = compare_folder_level_with_options(
        Arc::clone(&left),
        EntryPath("".into()),
        Arc::clone(&right),
        EntryPath("".into()),
        true,
        false,
    )
    .await
    .unwrap();
    assert_eq!(different.entries[0].status, ComparisonStatus::Different);
    assert_eq!(read_file(left.as_ref(), &file).await.unwrap(), b"same");

    let directory = EntryPath("created".into());
    create_directory(left.as_ref(), &directory).await.unwrap();
    let nested_file = EntryPath("created/nested.txt".into());
    write_file(left.as_ref(), &nested_file, b"nested")
        .await
        .unwrap();
    let copied_directory = EntryPath("copied".into());
    copy_entry(left.as_ref(), &directory, right.as_ref(), &copied_directory)
        .await
        .unwrap();
    assert_eq!(
        read_file(right.as_ref(), &EntryPath("copied/nested.txt".into()))
            .await
            .unwrap(),
        b"nested"
    );
    let mirror_directory = EntryPath("mirror".into());
    create_directory(right.as_ref(), &mirror_directory)
        .await
        .unwrap();
    write_file(
        right.as_ref(),
        &EntryPath("mirror/extra.txt".into()),
        b"extra",
    )
    .await
    .unwrap();
    mirror_entry(left.as_ref(), &directory, right.as_ref(), &mirror_directory)
        .await
        .unwrap();
    assert_eq!(
        read_file(right.as_ref(), &EntryPath("mirror/nested.txt".into()))
            .await
            .unwrap(),
        b"nested"
    );
    assert!(right
        .stat(&EntryPath("mirror/extra.txt".into()))
        .await
        .is_err());
    delete_entry(right.as_ref(), &copied_directory)
        .await
        .unwrap();
    assert!(right.stat(&copied_directory).await.is_err());
    let renamed = EntryPath("renamed.txt".into());
    rename_entry(left.as_ref(), &file, &renamed).await.unwrap();
    assert_eq!(read_file(left.as_ref(), &renamed).await.unwrap(), b"same");

    tokio::fs::remove_dir_all(root).await.unwrap();
}

/// Identical files whose timestamps differ (what a copy leaves behind) must compare Equal,
/// while same-size files with different bytes still report which side is newer.
#[tokio::test]
async fn differing_timestamps_alone_do_not_make_identical_files_different() {
    let unique = format!(
        "openbc-engine-ts-test-{}-{}",
        std::process::id(),
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
    );
    let root = std::env::temp_dir().join(unique);
    let (left_root, right_root) = (root.join("left"), root.join("right"));
    tokio::fs::create_dir_all(&left_root).await.unwrap();
    tokio::fs::create_dir_all(&right_root).await.unwrap();
    let left: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(&left_root));
    let right: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(&right_root));

    let same = EntryPath("same.txt".into());
    let changed = EntryPath("changed.txt".into());
    write_file(left.as_ref(), &same, b"abcd").await.unwrap();
    write_file(left.as_ref(), &changed, b"abcd").await.unwrap();
    // Beyond the 2 s timestamp tolerance.
    tokio::time::sleep(std::time::Duration::from_millis(2300)).await;
    write_file(right.as_ref(), &same, b"abcd").await.unwrap();
    write_file(right.as_ref(), &changed, b"abce").await.unwrap();

    let result = compare_folder_level_with_options(
        Arc::clone(&left),
        EntryPath("".into()),
        Arc::clone(&right),
        EntryPath("".into()),
        false,
        false,
    )
    .await
    .unwrap();
    let status_of = |name: &str| result.entries.iter().find(|e| e.name == name).unwrap().status.clone();
    assert_eq!(status_of("same.txt"), ComparisonStatus::Equal);
    assert_eq!(status_of("changed.txt"), ComparisonStatus::RightNewer);
    let _ = tokio::fs::remove_dir_all(&root).await;
}

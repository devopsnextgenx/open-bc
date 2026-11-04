#![warn(dead_code, unused)]
#![cfg(feature = "mem-stats")]

use async_trait::async_trait;
use openbc_core::{
    EntryMetadata, EntryPath,
    text::{ChangeKind, CompareOptions, LineDiff, highlight_to_spans},
};
use openbc_engine::text_runner::AsyncTextCompareTask;
use openbc_vfs::{AsyncVfs, LocalVfs, VfsError};
use std::{
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
    time::Instant,
};
use tokio::{io::AsyncRead, sync::mpsc};

struct ReadProgress {
    started: Instant,
    completed_reads: AtomicUsize,
    both_reads_completed_us: AtomicU64,
}

struct TimedLocalVfs {
    inner: LocalVfs,
    progress: Arc<ReadProgress>,
}

#[async_trait]
impl AsyncVfs for TimedLocalVfs {
    async fn read_dir(
        &self,
        path: &EntryPath,
    ) -> Result<Vec<openbc_core::DirectoryEntry>, VfsError> {
        self.inner.read_dir(path).await
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        self.inner.stat(path).await
    }

    async fn open_file(
        &self,
        path: &EntryPath,
    ) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        self.inner.open_file(path).await
    }

    async fn read_file_limited(
        &self,
        path: &EntryPath,
        limit: u64,
    ) -> Result<Option<Vec<u8>>, VfsError> {
        let result = self.inner.read_file_limited(path, limit).await?;
        if self.progress.completed_reads.fetch_add(1, Ordering::SeqCst) + 1 == 2 {
            self.progress.both_reads_completed_us.store(
                self.progress.started.elapsed().as_micros() as u64,
                Ordering::SeqCst,
            );
        }
        Ok(result)
    }

    fn local_path(&self, path: &EntryPath) -> Option<PathBuf> {
        self.inner.local_path(path)
    }
}

#[derive(Default)]
struct DiffStats {
    rows: usize,
    unchanged: usize,
    added: usize,
    deleted: usize,
    modified: usize,
    changed_groups: usize,
    in_changed_group: bool,
    syntax_spans: usize,
}

impl DiffStats {
    fn record(&mut self, row: &LineDiff) {
        let changed = match &row.kind {
            ChangeKind::Unchanged => {
                self.unchanged += 1;
                false
            }
            ChangeKind::Added => {
                self.added += 1;
                true
            }
            ChangeKind::Deleted => {
                self.deleted += 1;
                true
            }
            ChangeKind::Modified { .. } => {
                self.modified += 1;
                true
            }
        };
        if changed && !self.in_changed_group {
            self.changed_groups += 1;
        }
        self.in_changed_group = changed;

        if self.rows < 256 {
            self.syntax_spans += highlight_to_spans(&row.left_text, "log", true).len();
            self.syntax_spans += highlight_to_spans(&row.right_text, "log", true).len();
        }
        self.rows += 1;
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore = "manual large-file diagnostic; reads and compares the 80 MiB log pair"]
async fn autosync_logs_report_progressive_backend_behavior_and_memory() {
    let repository = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let fixture_dir = repository.join("test");
    let left_path = fixture_dir.join("autosync.left.log");
    let right_path = fixture_dir.join("autosync.right.log");
    let left_bytes = std::fs::metadata(&left_path)
        .expect("left autosync fixture")
        .len();
    let right_bytes = std::fs::metadata(&right_path)
        .expect("right autosync fixture")
        .len();
    let rust_live_before = openbc_core::mem::current_bytes();

    let started = Instant::now();
    let progress = Arc::new(ReadProgress {
        started,
        completed_reads: AtomicUsize::new(0),
        both_reads_completed_us: AtomicU64::new(0),
    });
    let vfs: Arc<dyn AsyncVfs> = Arc::new(TimedLocalVfs {
        inner: LocalVfs::new(&fixture_dir),
        progress: Arc::clone(&progress),
    });
    let task = AsyncTextCompareTask::new(2).expect("create text comparison task");
    let (sender, mut receiver) = mpsc::unbounded_channel();
    let run = tokio::spawn(async move {
        task.run(
            Arc::clone(&vfs),
            EntryPath("autosync.left.log".into()),
            vfs,
            EntryPath("autosync.right.log".into()),
            CompareOptions::default(),
            sender,
        )
        .await
    });

    let first_chunk = receiver
        .recv()
        .await
        .expect("backend should produce a first diff chunk");
    let first_chunk_us = started.elapsed().as_micros() as u64;
    let mut stats = DiffStats::default();
    for row in &first_chunk {
        stats.record(row);
    }
    drop(first_chunk);
    while let Some(chunk) = receiver.recv().await {
        for row in &chunk {
            stats.record(row);
        }
    }
    run.await
        .expect("join backend task")
        .expect("compare log pair");

    let both_reads_completed_us = progress.both_reads_completed_us.load(Ordering::SeqCst);
    let rust_live_after = openbc_core::mem::current_bytes();
    let rust_peak_process = openbc_core::mem::peak_bytes();
    let progressive = first_chunk_us < both_reads_completed_us;

    println!(
        "inputs: left={left_bytes} B right={right_bytes} B; \
         first_diff_chunk={first_chunk_us} us; both_input_reads_completed={both_reads_completed_us} us; \
         progressive_diff={progressive}"
    );
    println!(
        "diff: rows={} unchanged={} added={} deleted={} modified={} changed_groups={}; \
         log_syntax_spans_first_256_rows={}",
        stats.rows,
        stats.unchanged,
        stats.added,
        stats.deleted,
        stats.modified,
        stats.changed_groups,
        stats.syntax_spans
    );
    println!(
        "Rust allocator: live_before={rust_live_before} B live_after={rust_live_after} B \
         process_peak={rust_peak_process} B"
    );

    assert!(stats.rows > 0, "the backend returned no diff rows");
    assert!(
        stats.added + stats.deleted + stats.modified > 0,
        "the fixture pair should contain at least one unmatched or modified row"
    );
    assert!(
        progressive,
        "the first diff chunk arrived after both complete input reads; backend output is not progressive"
    );
}

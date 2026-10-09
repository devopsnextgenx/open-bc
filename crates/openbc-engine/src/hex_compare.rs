//! Hex compare actor: bounded chunked scan for differing byte runs plus on-demand windows.
//!
//! Files are never loaded whole: the scan streams 4 MiB chunks (compared on Rayon) and the
//! UI asks for small windows around what it is showing. Windows are served while the scan
//! is still running.

use std::path::PathBuf;
use std::sync::Arc;

use openbc_compute::hex::compare_chunk;
use openbc_core::hex_compare::{push_run, tail_run, ByteDiffRun, MAX_RUNS};
use tokio::runtime::Handle;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::common::*;

const SCAN_CHUNK: usize = 4 * 1024 * 1024;
/// Largest window a single request may ask for.
pub const MAX_WINDOW_BYTES: usize = 1024 * 1024;

#[derive(Debug)]
pub enum HexCommand {
    Reload,
    Window { id: u64, offset: u64, len: usize },
}

#[derive(Debug, Default)]
pub struct HexSummary {
    pub left_len: u64,
    pub right_len: u64,
    pub runs: Vec<ByteDiffRun>,
    /// More than `MAX_RUNS` runs: the list is cut short.
    pub truncated: bool,
    pub diff_bytes: u64,
    pub errors: [Option<String>; 2],
}

impl HexSummary {
    /// Counts a differing run and keeps it in the (capped, merged) run list.
    fn record(&mut self, offset: u64, len: u64) {
        self.diff_bytes += len;
        if self.runs.len() >= MAX_RUNS {
            self.truncated = true;
        } else {
            push_run(&mut self.runs, offset, len);
        }
    }

    fn has_errors(&self) -> bool {
        self.errors.iter().any(Option::is_some)
    }
}

#[derive(Debug)]
pub struct HexWindow {
    pub id: u64,
    pub offset: u64,
    pub left: Vec<u8>,
    pub right: Vec<u8>,
}

#[derive(Debug)]
pub enum HexEvent {
    Progress { done: u64, total: u64 },
    Summary(Arc<HexSummary>),
    Window(Arc<HexWindow>),
    Error(String),
}

/// Non-blocking command sender; `send` returns `false` when the actor is busy or gone.
pub type HexCompareHandle = CommandHandle<HexCommand>;

pub fn spawn_hex_compare(rt: &Handle, source: Arc<dyn ByteSource>, left: PathBuf, right: PathBuf) -> (HexCompareHandle, mpsc::Receiver<HexEvent>) {
    let (handle, commands, events, event_rx) = channels();
    rt.spawn(run(source, [left, right], commands, events));
    (handle, event_rx)
}

async fn run(source: Arc<dyn ByteSource>, paths: [PathBuf; 2], mut crx: mpsc::Receiver<HexCommand>, etx: mpsc::Sender<HexEvent>) {
    let spawn_scan = |source: &Arc<dyn ByteSource>| tokio::spawn(scan(source.clone(), paths.clone(), etx.clone()));
    let mut scanning: Option<JoinHandle<HexSummary>> = Some(spawn_scan(&source));
    loop {
        tokio::select! {
            joined = async { scanning.as_mut().expect("guarded by precondition").await }, if scanning.is_some() => {
                scanning = None;
                let done = joined.unwrap_or_else(|_| HexSummary { errors: [Some("scan stopped unexpectedly".into()), None], ..Default::default() });
                if !send_event(&etx, HexEvent::Summary(Arc::new(done))).await { return; }
            }
            command = crx.recv() => match command {
                None => break,
                Some(HexCommand::Reload) => {
                    if let Some(old) = scanning.take() { old.abort(); }
                    scanning = Some(spawn_scan(&source));
                }
                Some(HexCommand::Window { id, offset, len }) => {
                    let len = len.min(MAX_WINDOW_BYTES);
                    let read = |i: usize| {
                        let (source, path) = (source.clone(), paths[i].clone());
                        async move {
                            if is_unset(&path) { return Vec::new(); }
                            source.read_range(&path, offset, len).await.unwrap_or_default()
                        }
                    };
                    let (left, right) = tokio::join!(read(0), read(1));
                    if !send_event(&etx, HexEvent::Window(Arc::new(HexWindow { id, offset, left, right }))).await { return; }
                }
            },
        }
    }
    if let Some(h) = scanning {
        h.abort();
    }
}

async fn scan(source: Arc<dyn ByteSource>, paths: [PathBuf; 2], etx: mpsc::Sender<HexEvent>) -> HexSummary {
    let mut summary = HexSummary::default();
    let mut lens = [0u64; 2];
    for i in 0..2 {
        if is_unset(&paths[i]) {
            continue;
        }
        match source.stat(&paths[i]).await {
            Ok(st) => lens[i] = st.size,
            Err(e) => summary.errors[i] = Some(context(&paths[i], "stat", e)),
        }
    }
    (summary.left_len, summary.right_len) = (lens[0], lens[1]);
    let common = if summary.has_errors() { 0 } else { lens[0].min(lens[1]) };
    let mut offset = 0u64;
    while offset < common {
        let want = SCAN_CHUNK.min((common - offset) as usize);
        let (l, r) = tokio::join!(source.read_range(&paths[0], offset, want), source.read_range(&paths[1], offset, want));
        let (l, r) = match (l, r) {
            (Ok(l), Ok(r)) if l.len() == want && r.len() == want => (l, r),
            (l, r) => {
                let err = |res: std::io::Result<Vec<u8>>, i: usize| res.err().map(|e| context(&paths[i], "read", e));
                (summary.errors[0], summary.errors[1]) = (err(l, 0), err(r, 1));
                break;
            }
        };
        let runs = match rayon_run(move || compare_chunk(&l, &r, offset)).await {
            Ok(runs) => runs,
            Err(_) => break,
        };
        for run in runs {
            summary.record(run.offset, run.len);
        }
        offset += want as u64;
        // Progress is advisory: drop it rather than stall the scan on a slow UI.
        let _ = etx.try_send(HexEvent::Progress { done: offset, total: common });
    }
    if let Some(tail) = tail_run(lens[0], lens[1]).filter(|_| !summary.has_errors()) {
        summary.record(tail.offset, tail.len);
    }
    summary
}

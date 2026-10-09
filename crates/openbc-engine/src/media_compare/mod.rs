//! Media compare actor: header/tag metadata only (never the audio/video payload).

use std::path::{Path, PathBuf};
use std::sync::Arc;

use async_trait::async_trait;
use openbc_compute::media::read_media_metadata;
use openbc_core::meta_compare::{diff_meta, MetaDiffRow, MetaRow};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

mod format;
mod write;

use format::{format_utc, human_size};
pub use write::is_editable;

/// One tag change; `value: None` removes the tag. `group`/`key` are the row's display names.
#[derive(Debug, Clone)]
pub struct TagEdit {
    pub group: String,
    pub key: String,
    pub value: Option<String>,
}

#[derive(Clone)]
pub enum CoverEdit {
    Keep,
    Replace(Vec<u8>),
    Remove,
}

// Manual Debug so logging a command never dumps image bytes.
impl std::fmt::Debug for CoverEdit {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Keep => f.write_str("Keep"),
            Self::Replace(b) => write!(f, "Replace({} bytes)", b.len()),
            Self::Remove => f.write_str("Remove"),
        }
    }
}

/// Edits for one side (0 left, 1 right); the file is rewritten, then both sides are re-read.
#[derive(Debug, Clone)]
pub struct MediaSave {
    pub side: usize,
    pub edits: Vec<TagEdit>,
    pub cover: CoverEdit,
}

#[derive(Debug)]
pub enum MediaCommand {
    Reload,
    Save(MediaSave),
}

pub struct MediaView {
    pub rows: Vec<MetaDiffRow>,
    pub errors: [Option<String>; 2],
    pub present: [bool; 2],
    /// Raw bytes of each side's first embedded picture (cover art), if any.
    pub covers: [Option<Vec<u8>>; 2],
}

// Manual Debug so logging a view never dumps image bytes.
impl std::fmt::Debug for MediaView {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MediaView")
            .field("rows", &self.rows.len())
            .field("errors", &self.errors)
            .field("present", &self.present)
            .field("covers", &self.covers.each_ref().map(|c| c.as_ref().map(Vec::len)))
            .finish()
    }
}

#[derive(Debug)]
pub enum MediaEvent {
    Status(String),
    Result(Arc<MediaView>),
}

/// Non-blocking command sender; `send` returns `false` when the actor is busy or gone.
pub type MediaCompareHandle = CommandHandle<MediaCommand>;

pub fn spawn_media_compare(rt: &Handle, source: Arc<dyn ByteSource>, left: PathBuf, right: PathBuf) -> (MediaCompareHandle, mpsc::Receiver<MediaEvent>) {
    spawn_batched(rt, |etx| Actor { source, paths: [left, right], save_errors: [None, None], etx })
}

/// Everything loaded for one side.
struct LoadedSide {
    rows: Vec<MetaRow>,
    cover: Option<Vec<u8>>,
    error: Option<String>,
}

impl LoadedSide {
    fn empty() -> Self {
        Self { rows: Vec::new(), cover: None, error: None }
    }

    fn failed(rows: Vec<MetaRow>, error: String) -> Self {
        Self { rows, cover: None, error: Some(error) }
    }
}

struct Actor {
    source: Arc<dyn ByteSource>,
    paths: [PathBuf; 2],
    /// "Save failed: ..." per side, shown by the UI in that side's error line after the re-read.
    save_errors: [Option<String>; 2],
    etx: mpsc::Sender<MediaEvent>,
}

// There is no separate load step: every batch of commands (including the very first, empty
// one) ends in `publish`, which re-reads both files. Queued reloads therefore collapse into
// one re-read while saves still run in order.
#[async_trait]
impl BatchedActor for Actor {
    type Command = MediaCommand;

    async fn init(&mut self) -> bool {
        true
    }

    async fn apply(&mut self, command: MediaCommand) -> bool {
        if let MediaCommand::Save(save) = command {
            self.save(save).await;
        }
        true
    }

    async fn publish(&mut self) -> bool {
        if !send_event(&self.etx, MediaEvent::Status("Reading media headers...".into())).await {
            return false;
        }
        // Both sides load concurrently; a failure on one side leaves the other intact.
        let (l, r) = tokio::join!(load_side(&self.source, &self.paths[0]), load_side(&self.source, &self.paths[1]));
        let diff = rayon_run({
            let (a, b) = (l.rows.clone(), r.rows.clone());
            move || diff_meta(&a, &b)
        })
        .await;
        let Ok(rows) = diff else { return false };
        let view = MediaView {
            rows,
            errors: [self.save_errors[0].take().or(l.error), self.save_errors[1].take().or(r.error)],
            present: [!is_unset(&self.paths[0]), !is_unset(&self.paths[1])],
            covers: [l.cover, r.cover],
        };
        send_event(&self.etx, MediaEvent::Result(Arc::new(view))).await
    }
}

impl Actor {
    async fn save(&mut self, save: MediaSave) {
        let side = usize::from(save.side != 0);
        let path = self.paths[side].clone();
        let result = if is_unset(&path) {
            Err("no file selected".to_string())
        } else {
            rayon_run(move || write::apply(&path, &save)).await.map_err(|e| e.to_string()).and_then(|r| r)
        };
        self.save_errors[side] = result.err().map(|e| format!("Save failed: {e}"));
    }
}

async fn load_side(source: &Arc<dyn ByteSource>, path: &Path) -> LoadedSide {
    if is_unset(path) {
        return LoadedSide::empty();
    }
    let file_row = |key: &str, value: String| MetaRow { group: "File".into(), key: key.into(), value };
    let mut rows = vec![file_row("Name", path.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned()))];
    match source.stat(path).await {
        Ok(st) => {
            rows.push(file_row("Size", format!("{} ({} bytes)", human_size(st.size), st.size)));
            if let Some(t) = st.modified {
                rows.push(file_row("Modified", format_utc(t)));
            }
        }
        Err(e) => return LoadedSide::failed(rows, context(path, "stat", e)),
    }
    let (src, p) = (source.clone(), path.to_path_buf());
    let parsed = rayon_run(move || match src.open_reader(&p) {
        Ok(mut reader) => read_media_metadata(&mut *reader).map_err(|e| context(&p, "read metadata", e)),
        Err(e) => Err(context(&p, "open", e)),
    })
    .await;
    match parsed {
        Ok(Ok(mut info)) => {
            rows.append(&mut info.rows);
            LoadedSide { rows, cover: info.cover, error: None }
        }
        Ok(Err(message)) => LoadedSide::failed(rows, message),
        Err(e) => LoadedSide::failed(rows, e.to_string()),
    }
}

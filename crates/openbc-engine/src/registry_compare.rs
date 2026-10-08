//! Registry compare actor: loads `.reg` exports, diffs keys/values, applies copy edits.

use std::path::PathBuf;
use std::sync::Arc;

use openbc_core::registry_compare::{apply_reg_copy, diff_reg, group_reg_rows, parse_reg, write_reg, RegCompareOptions, RegDiffRow, RegFile};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

const MAX_REG_BYTES: u64 = 256 * 1024 * 1024;

#[derive(Debug)]
pub enum RegistryCommand {
    SetOptions(RegCompareOptions),
    Reload,
    Copy { start: usize, end: usize, to_right: bool },
    /// `None` saves in place; `Some` writes a new `.reg` (UTF-16, v5.00).
    Save { side: Side, path: Option<PathBuf> },
}

#[derive(Clone, Debug, Default)]
pub struct RegSideInfo {
    pub error: Option<String>,
    pub loaded: bool,
    pub dirty: bool,
}

#[derive(Debug)]
pub struct RegistryView {
    pub rows: Vec<RegDiffRow>,
    /// Inclusive `(start, end)` row ranges, one per copy arrow.
    pub groups: Vec<(usize, usize)>,
    pub sides: [RegSideInfo; 2],
}

#[derive(Debug)]
pub enum RegistryEvent {
    Status(String),
    Result(Arc<RegistryView>),
    Saved { side: Side, path: String },
    Error(String),
}

#[derive(Clone)]
pub struct RegistryCompareHandle {
    tx: mpsc::Sender<RegistryCommand>,
}

impl RegistryCompareHandle {
    pub fn send(&self, command: RegistryCommand) -> bool {
        self.tx.try_send(command).is_ok()
    }
}

pub fn spawn_registry_compare(
    rt: &Handle,
    source: Arc<dyn ByteSource>,
    left: PathBuf,
    right: PathBuf,
) -> (RegistryCompareHandle, mpsc::Receiver<RegistryEvent>) {
    let (ctx, crx) = mpsc::channel(COMMAND_CHANNEL_CAPACITY);
    let (etx, erx) = mpsc::channel(EVENT_CHANNEL_CAPACITY);
    let side = |path| RegSide { path, file: None, error: None, dirty: false };
    let actor = Actor { source, sides: [side(left), side(right)], options: RegCompareOptions::default(), last_rows: Vec::new(), etx };
    rt.spawn(actor.run(crx));
    (RegistryCompareHandle { tx: ctx }, erx)
}

struct RegSide {
    path: PathBuf,
    file: Option<RegFile>,
    error: Option<String>,
    dirty: bool,
}

struct Actor {
    source: Arc<dyn ByteSource>,
    sides: [RegSide; 2],
    options: RegCompareOptions,
    last_rows: Vec<RegDiffRow>,
    etx: mpsc::Sender<RegistryEvent>,
}

impl Actor {
    async fn emit(&self, event: RegistryEvent) -> bool {
        self.etx.send(event).await.is_ok()
    }

    async fn run(mut self, mut crx: mpsc::Receiver<RegistryCommand>) {
        if !self.reload().await || !self.emit_result().await {
            return;
        }
        while let Some(first) = crx.recv().await {
            let mut batch = vec![first];
            while let Ok(next) = crx.try_recv() {
                batch.push(next);
            }
            for command in batch {
                let ok = match command {
                    RegistryCommand::SetOptions(o) => {
                        self.options = o;
                        true
                    }
                    RegistryCommand::Reload => self.reload().await,
                    RegistryCommand::Copy { start, end, to_right } => {
                        self.copy(start, end, to_right);
                        true
                    }
                    RegistryCommand::Save { side, path } => self.save(side, path).await,
                };
                if !ok {
                    return;
                }
            }
            if !self.emit_result().await {
                return;
            }
        }
    }

    async fn reload(&mut self) -> bool {
        for idx in 0..2 {
            let path = self.sides[idx].path.clone();
            if !self.emit(RegistryEvent::Status(format!("Loading {}...", path.display()))).await {
                return false;
            }
            let loaded: Result<Option<RegFile>, String> = if path.as_os_str().is_empty() {
                Ok(None)
            } else {
                match self.source.read_all(&path, MAX_REG_BYTES).await {
                    Err(e) => Err(context(&path, "read", e)),
                    Ok(bytes) => match rayon_run(move || parse_reg(&bytes)).await {
                        Ok(Ok(file)) => Ok(Some(file)),
                        Ok(Err(e)) => Err(context(&path, "parse", e)),
                        Err(e) => Err(context(&path, "parse", e)),
                    },
                }
            };
            let side = &mut self.sides[idx];
            side.dirty = false;
            match loaded {
                Ok(file) => (side.file, side.error) = (file, None),
                Err(e) => (side.file, side.error) = (None, Some(e)),
            }
        }
        true
    }

    async fn emit_result(&mut self) -> bool {
        let (l, r) = (self.sides[0].file.clone().unwrap_or_default(), self.sides[1].file.clone().unwrap_or_default());
        let options = self.options.clone();
        let rows = match rayon_run(move || diff_reg(&l, &r, &options)).await {
            Ok(rows) => rows,
            Err(e) => return self.emit(RegistryEvent::Error(e.to_string())).await,
        };
        self.last_rows = rows.clone();
        let info = |s: &RegSide| RegSideInfo { error: s.error.clone(), loaded: s.file.is_some(), dirty: s.dirty };
        let view = RegistryView { groups: group_reg_rows(&rows), rows, sides: [info(&self.sides[0]), info(&self.sides[1])] };
        self.emit(RegistryEvent::Result(Arc::new(view))).await
    }

    fn copy(&mut self, start: usize, end: usize, to_right: bool) {
        let dst = usize::from(to_right);
        for side in &mut self.sides {
            side.file.get_or_insert_with(RegFile::default);
        }
        let (mut l, mut r) = (self.sides[0].file.take().unwrap_or_default(), self.sides[1].file.take().unwrap_or_default());
        apply_reg_copy(&mut l, &mut r, &self.last_rows, start, end, to_right);
        (self.sides[0].file, self.sides[1].file) = (Some(l), Some(r));
        self.sides[dst].dirty = true;
    }

    async fn save(&mut self, side: Side, path: Option<PathBuf>) -> bool {
        let state = &self.sides[side.index()];
        let Some(file) = state.file.clone() else {
            return self.emit(RegistryEvent::Error(format!("{} side has nothing to save", side.label()))).await;
        };
        let in_place = path.is_none();
        let target = path.unwrap_or_else(|| state.path.clone());
        let bytes = match rayon_run(move || write_reg(&file)).await {
            Ok(b) => b,
            Err(e) => return self.emit(RegistryEvent::Error(e.to_string())).await,
        };
        match self.source.write_all(&target, &bytes).await {
            Ok(()) => {
                if in_place {
                    self.sides[side.index()].dirty = false;
                }
                self.emit(RegistryEvent::Saved { side, path: target.display().to_string() }).await
            }
            Err(e) => self.emit(RegistryEvent::Error(context(&target, "save", e))).await,
        }
    }
}

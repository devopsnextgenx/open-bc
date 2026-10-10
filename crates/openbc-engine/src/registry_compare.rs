//! Registry compare actor: loads `.reg` exports, diffs keys/values, applies copy edits.

use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::Arc;

use async_trait::async_trait;
use openbc_core::registry_compare::{apply_reg_copy, diff_reg, group_reg_rows, parse_reg, set_reg_value_text, write_reg, RegCompareOptions, RegDiffRow, RegFile, RegRowKind};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

const MAX_REG_BYTES: u64 = 256 * 1024 * 1024;

#[derive(Debug)]
pub enum RegistryCommand {
    SetOptions(RegCompareOptions),
    Reload,
    Copy { start: usize, end: usize, to_right: bool },
    /// Type new data into the value of diff row `row` on `side`, in the form `RegValue::display`
    /// shows it. The value keeps its kind; the side turns dirty.
    SetValue { side: Side, row: usize, text: String },
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
    /// Per side: `(lower-cased key, lower-cased value name or empty for the key row)` touched by
    /// typing or a copy since load / last save.
    pub edited: [HashSet<(String, String)>; 2],
}

#[derive(Debug)]
pub enum RegistryEvent {
    Status(String),
    Result(Arc<RegistryView>),
    Saved { side: Side, path: String },
    Error(String),
}

/// Non-blocking command sender; `send` returns `false` when the actor is busy or gone.
pub type RegistryCompareHandle = CommandHandle<RegistryCommand>;

pub fn spawn_registry_compare(
    rt: &Handle,
    source: Arc<dyn ByteSource>,
    left: PathBuf,
    right: PathBuf,
) -> (RegistryCompareHandle, mpsc::Receiver<RegistryEvent>) {
    let side = |path| RegSide { path, file: None, error: None, dirty: false, edited: HashSet::new() };
    spawn_batched(rt, |etx| Actor { source, sides: [side(left), side(right)], options: RegCompareOptions::default(), last_rows: Vec::new(), etx })
}

struct RegSide {
    path: PathBuf,
    file: Option<RegFile>,
    error: Option<String>,
    dirty: bool,
    edited: HashSet<(String, String)>,
}

fn edit_id(key: &str, name: Option<&str>) -> (String, String) {
    (key.to_lowercase(), name.map(str::to_lowercase).unwrap_or_default())
}

struct Actor {
    source: Arc<dyn ByteSource>,
    sides: [RegSide; 2],
    options: RegCompareOptions,
    last_rows: Vec<RegDiffRow>,
    etx: mpsc::Sender<RegistryEvent>,
}

#[async_trait]
impl BatchedActor for Actor {
    type Command = RegistryCommand;

    async fn init(&mut self) -> bool {
        self.reload().await
    }

    async fn apply(&mut self, command: RegistryCommand) -> bool {
        match command {
            RegistryCommand::SetOptions(options) => {
                self.options = options;
                true
            }
            RegistryCommand::Reload => self.reload().await,
            RegistryCommand::Copy { start, end, to_right } => {
                self.copy(start, end, to_right);
                true
            }
            RegistryCommand::SetValue { side, row, text } => self.set_value(side, row, text).await,
            RegistryCommand::Save { side, path } => self.save(side, path).await,
        }
    }

    async fn publish(&mut self) -> bool {
        let (l, r) = (self.sides[0].file.clone().unwrap_or_default(), self.sides[1].file.clone().unwrap_or_default());
        let options = self.options.clone();
        let rows = match rayon_run(move || diff_reg(&l, &r, &options)).await {
            Ok(rows) => rows,
            Err(e) => return self.emit(RegistryEvent::Error(e.to_string())).await,
        };
        self.last_rows = rows.clone();
        let info = |s: &RegSide| RegSideInfo { error: s.error.clone(), loaded: s.file.is_some(), dirty: s.dirty };
        let view = RegistryView { groups: group_reg_rows(&rows), rows, sides: [info(&self.sides[0]), info(&self.sides[1])], edited: [self.sides[0].edited.clone(), self.sides[1].edited.clone()] };
        self.emit(RegistryEvent::Result(Arc::new(view))).await
    }
}

impl Actor {
    async fn emit(&self, event: RegistryEvent) -> bool {
        send_event(&self.etx, event).await
    }

    async fn load_side(&self, idx: usize) -> Result<Option<RegFile>, String> {
        let parse = |bytes: Vec<u8>| parse_reg(&bytes);
        load_parsed(self.source.as_ref(), &self.sides[idx].path, MAX_REG_BYTES, "parse", parse).await
    }

    async fn reload(&mut self) -> bool {
        for idx in 0..2 {
            if !self.emit(RegistryEvent::Status(format!("Loading {}...", self.sides[idx].path.display()))).await {
                return false;
            }
            let loaded = self.load_side(idx).await;
            let side = &mut self.sides[idx];
            side.dirty = false;
            side.edited.clear();
            match loaded {
                Ok(file) => (side.file, side.error) = (file, None),
                Err(e) => (side.file, side.error) = (None, Some(e)),
            }
        }
        true
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
        for row in self.last_rows.iter().skip(start).take(end.saturating_sub(start) + 1) {
            if row.kind != RegRowKind::Equal {
                self.sides[dst].edited.insert(edit_id(&row.key, row.name.as_deref()));
            }
        }
    }

    async fn set_value(&mut self, side: Side, row: usize, text: String) -> bool {
        let idx = side.index();
        let Some(diff) = self.last_rows.get(row).cloned() else { return true };
        let present = if idx == 0 { diff.left.is_some() } else { diff.right.is_some() };
        let (Some(name), true) = (diff.name.as_deref(), present) else {
            let msg = format!("That row has no value on the {} side; copy it across first", side.label());
            return self.emit(RegistryEvent::Error(msg)).await;
        };
        let Some(file) = self.sides[idx].file.as_mut() else {
            return self.emit(RegistryEvent::Error(format!("{} side has nothing to edit", side.label()))).await;
        };
        match set_reg_value_text(file, &diff.key, name, &text) {
            Ok(changed) => {
                if changed {
                    self.sides[idx].dirty = true;
                    self.sides[idx].edited.insert(edit_id(&diff.key, Some(name)));
                }
                true
            }
            Err(message) => self.emit(RegistryEvent::Error(message)).await,
        }
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
        match write_bytes(self.source.as_ref(), &target, &bytes).await {
            Ok(()) => {
                if in_place {
                    self.sides[side.index()].dirty = false;
                    self.sides[side.index()].edited.clear();
                }
                self.emit(RegistryEvent::Saved { side, path: target.display().to_string() }).await
            }
            Err(message) => self.emit(RegistryEvent::Error(message)).await,
        }
    }
}

//! Table compare actor: loads CSV/spreadsheets, aligns rows, applies copy edits, saves CSV.

use std::path::PathBuf;
use std::sync::Arc;

use openbc_compute::table::{decode_workbook, diff_tables_parallel};
use openbc_core::table_compare::csv::write_csv;
use openbc_core::table_compare::{apply_copy, group_changed, DiffGroup, SourceFormat, Table, TableCompareOptions, TableDiffRow, Workbook};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

#[derive(Clone, Debug)]
pub struct TableCompareConfig {
    pub max_file_bytes: u64,
    pub max_cells: usize,
}

impl Default for TableCompareConfig {
    fn default() -> Self {
        Self { max_file_bytes: 256 * 1024 * 1024, max_cells: 20_000_000 }
    }
}

#[derive(Debug)]
pub enum TableCommand {
    SetOptions(TableCompareOptions),
    /// Sheet indexes; `None` keeps the current selection.
    SelectSheets { left: Option<usize>, right: Option<usize> },
    Reload,
    /// Make the other side match for diff rows `start..=end`.
    Copy { start: usize, end: usize, to_right: bool },
    /// `path: None` saves in place (CSV only); `Some` exports the selected sheet as CSV.
    Save { side: Side, path: Option<PathBuf> },
}

#[derive(Clone, Debug, Default)]
pub struct SideInfo {
    pub sheets: Vec<String>,
    pub selected: Option<usize>,
    pub error: Option<String>,
    pub loaded: bool,
    pub editable: bool,
    pub dirty: bool,
}

/// Immutable snapshot handed to the UI.
#[derive(Debug)]
pub struct TableView {
    pub left: Arc<Table>,
    pub right: Arc<Table>,
    pub diff: Vec<TableDiffRow>,
    pub groups: Vec<DiffGroup>,
    pub sides: [SideInfo; 2],
    pub columns: usize,
}

#[derive(Debug)]
pub enum TableEvent {
    Status(String),
    Result(Arc<TableView>),
    Saved { side: Side, path: String },
    Error(String),
}

#[derive(Clone)]
pub struct TableCompareHandle {
    tx: mpsc::Sender<TableCommand>,
}

impl TableCompareHandle {
    /// Non-blocking; `false` when the actor is busy (queue full) or gone.
    pub fn send(&self, command: TableCommand) -> bool {
        self.tx.try_send(command).is_ok()
    }
}

/// Starts one table-compare actor. Empty paths mean "no file on that side".
pub fn spawn_table_compare(
    rt: &Handle,
    config: TableCompareConfig,
    source: Arc<dyn ByteSource>,
    left: PathBuf,
    right: PathBuf,
) -> (TableCompareHandle, mpsc::Receiver<TableEvent>) {
    let (ctx, crx) = mpsc::channel(COMMAND_CHANNEL_CAPACITY);
    let (etx, erx) = mpsc::channel(EVENT_CHANNEL_CAPACITY);
    let actor = Actor {
        config,
        source,
        sides: [SideState::new(left), SideState::new(right)],
        options: TableCompareOptions::default(),
        last_diff: Vec::new(),
        etx,
    };
    rt.spawn(actor.run(crx));
    (TableCompareHandle { tx: ctx }, erx)
}

struct SideState {
    path: PathBuf,
    workbook: Option<Workbook>,
    selected: Option<usize>,
    error: Option<String>,
    dirty: bool,
}

impl SideState {
    fn new(path: PathBuf) -> Self {
        Self { path, workbook: None, selected: None, error: None, dirty: false }
    }

    fn table(&self) -> Arc<Table> {
        match (&self.workbook, self.selected) {
            (Some(w), Some(i)) if i < w.sheets.len() => w.sheets[i].clone(),
            _ => Arc::new(Table::default()),
        }
    }

    /// Moves the selected sheet out (leaving an empty one) so `Arc::make_mut` can edit it
    /// without a copy unless a UI snapshot still shares it.
    fn take_table(&mut self) -> Arc<Table> {
        match (&mut self.workbook, self.selected) {
            (Some(w), Some(i)) if i < w.sheets.len() => std::mem::take(&mut w.sheets[i]),
            _ => Arc::new(Table::default()),
        }
    }

    fn put_table(&mut self, table: Arc<Table>) {
        if let (Some(w), Some(i)) = (&mut self.workbook, self.selected) {
            if i < w.sheets.len() {
                w.sheets[i] = table;
            }
        }
    }

    fn info(&self) -> SideInfo {
        SideInfo {
            sheets: self.workbook.as_ref().map(Workbook::sheet_names).unwrap_or_default(),
            selected: self.selected,
            error: self.error.clone(),
            loaded: self.workbook.is_some(),
            editable: matches!(self.workbook.as_ref().map(|w| w.format), Some(SourceFormat::Csv { .. })),
            dirty: self.dirty,
        }
    }
}

struct Actor {
    config: TableCompareConfig,
    source: Arc<dyn ByteSource>,
    sides: [SideState; 2],
    options: TableCompareOptions,
    last_diff: Vec<TableDiffRow>,
    etx: mpsc::Sender<TableEvent>,
}

impl Actor {
    async fn emit(&self, event: TableEvent) -> bool {
        self.etx.send(event).await.is_ok()
    }

    async fn run(mut self, mut crx: mpsc::Receiver<TableCommand>) {
        if !self.reload().await || !self.emit_result().await {
            return;
        }
        while let Some(first) = crx.recv().await {
            // Coalesce: everything queued while we were busy is handled before one re-diff.
            let mut batch = vec![first];
            while let Ok(next) = crx.try_recv() {
                batch.push(next);
            }
            let mut rediff = false;
            for command in batch {
                let ok = match command {
                    TableCommand::SetOptions(o) => {
                        self.options = o;
                        rediff = true;
                        true
                    }
                    TableCommand::SelectSheets { left, right } => {
                        for (side, pick) in self.sides.iter_mut().zip([left, right]) {
                            let count = side.workbook.as_ref().map_or(0, |w| w.sheets.len());
                            if let Some(i) = pick.filter(|&i| i < count) {
                                side.selected = Some(i);
                            }
                        }
                        rediff = true;
                        true
                    }
                    TableCommand::Reload => {
                        rediff = true;
                        self.reload().await
                    }
                    TableCommand::Copy { start, end, to_right } => {
                        rediff = true;
                        self.copy(start, end, to_right).await
                    }
                    TableCommand::Save { side, path } => {
                        rediff = true; // refresh the dirty flags
                        self.save(side, path).await
                    }
                };
                if !ok {
                    return;
                }
            }
            if rediff && !self.emit_result().await {
                return;
            }
        }
    }

    async fn load_side(&self, idx: usize) -> Result<Option<Workbook>, String> {
        let path = self.sides[idx].path.clone();
        if path.as_os_str().is_empty() {
            return Ok(None);
        }
        let bytes = self.source.read_all(&path, self.config.max_file_bytes).await.map_err(|e| context(&path, "read", e))?;
        let (ext, max_cells) = (extension_of(&path), self.config.max_cells);
        let decoded = rayon_run(move || decode_workbook(bytes, &ext, max_cells)).await.map_err(|e| context(&path, "decode", e))?;
        decoded.map(Some).map_err(|e| context(&path, "decode", e))
    }

    async fn reload(&mut self) -> bool {
        for idx in 0..2 {
            if !self.emit(TableEvent::Status(format!("Loading {}...", self.sides[idx].path.display()))).await {
                return false;
            }
            let loaded = self.load_side(idx).await; // one unreadable side never blocks the other
            let side = &mut self.sides[idx];
            (side.dirty, side.error) = (false, None);
            match loaded {
                Ok(wb) => (side.workbook, side.selected) = (wb.filter(|w| !w.sheets.is_empty()), Some(0)),
                Err(e) => (side.workbook, side.selected, side.error) = (None, None, Some(e)),
            }
            if side.workbook.is_none() {
                side.selected = None;
            }
        }
        // Pair the right sheet with the left sheet of the same name when there is one.
        let left_name = self.sides[0].selected.and_then(|i| self.sides[0].workbook.as_ref().map(|w| w.sheets[i].name.clone()));
        if let (Some(name), Some(w)) = (left_name, self.sides[1].workbook.as_ref()) {
            self.sides[1].selected = w.find(&name).or(self.sides[1].selected);
        }
        true
    }

    async fn emit_result(&mut self) -> bool {
        let (left, right) = (self.sides[0].table(), self.sides[1].table());
        let (l, r, o) = (left.clone(), right.clone(), self.options.clone());
        let diff = match rayon_run(move || diff_tables_parallel(&l, &r, &o)).await {
            Ok(d) => d,
            Err(e) => return self.emit(TableEvent::Error(e.to_string())).await,
        };
        self.last_diff = diff.clone();
        let view = TableView {
            columns: left.columns().max(right.columns()),
            groups: group_changed(&diff),
            diff,
            sides: [self.sides[0].info(), self.sides[1].info()],
            left,
            right,
        };
        self.emit(TableEvent::Result(Arc::new(view))).await
    }

    async fn copy(&mut self, start: usize, end: usize, to_right: bool) -> bool {
        let dst = usize::from(to_right);
        if self.sides[dst].workbook.is_none() {
            // Copying onto a missing file creates an in-memory CSV that Save writes out.
            let format = match self.sides[1 - dst].workbook.as_ref().map(|w| w.format) {
                Some(SourceFormat::Csv { delimiter }) => SourceFormat::Csv { delimiter },
                _ => SourceFormat::Spreadsheet,
            };
            let sheet = Arc::new(Table { name: "Sheet1".into(), rows: Vec::new() });
            (self.sides[dst].workbook, self.sides[dst].selected) = (Some(Workbook { format, sheets: vec![sheet] }), Some(0));
        }
        let rows = std::mem::take(&mut self.last_diff);
        let (mut l, mut r) = (self.sides[0].take_table(), self.sides[1].take_table());
        let (lm, rm) = (Arc::make_mut(&mut l), Arc::make_mut(&mut r));
        apply_copy(lm, rm, &rows, start, end, to_right);
        self.sides[0].put_table(l);
        self.sides[1].put_table(r);
        self.sides[dst].dirty = true;
        true
    }

    async fn save(&mut self, side: Side, path: Option<PathBuf>) -> bool {
        let state = &self.sides[side.index()];
        let Some(workbook) = &state.workbook else {
            return self.emit(TableEvent::Error(format!("{} side has nothing to save", side.label()))).await;
        };
        let delimiter = match workbook.format {
            SourceFormat::Csv { delimiter } => delimiter,
            SourceFormat::Spreadsheet if path.is_some() => b',',
            SourceFormat::Spreadsheet => {
                let msg = format!("{} is a spreadsheet and is read-only here; use Save As to export the sheet as CSV", state.path.display());
                return self.emit(TableEvent::Error(msg)).await;
            }
        };
        let in_place = path.is_none();
        let target = path.unwrap_or_else(|| state.path.clone());
        let table = state.table();
        let bytes = match rayon_run(move || write_csv(&table.rows, delimiter)).await {
            Ok(b) => b,
            Err(e) => return self.emit(TableEvent::Error(e.to_string())).await,
        };
        match self.source.write_all(&target, &bytes).await {
            Ok(()) => {
                if in_place {
                    self.sides[side.index()].dirty = false;
                }
                self.emit(TableEvent::Saved { side, path: target.display().to_string() }).await
            }
            Err(e) => self.emit(TableEvent::Error(context(&target, "save", e))).await,
        }
    }
}

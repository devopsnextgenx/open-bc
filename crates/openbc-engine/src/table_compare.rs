//! Table compare actor: loads CSV/spreadsheets, aligns rows, applies copy edits, saves CSV.

use std::collections::HashSet;
use std::path::PathBuf;
use std::sync::Arc;

use async_trait::async_trait;
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
    /// Type `text` into one cell. `row` is a diff row (as in `TableView::diff`), `col` a column.
    /// CSV sides only; the side turns dirty and the row is reported in `TableView::edited`.
    SetCell { side: Side, row: usize, col: usize, text: String },
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
    /// Per side: 0-based sheet rows typed into since load / last save. Row numbers shift when
    /// rows are copied across, so a copy clears the destination's marks (the side stays dirty).
    pub edited: [HashSet<usize>; 2],
}

#[derive(Debug)]
pub enum TableEvent {
    Status(String),
    Result(Arc<TableView>),
    Saved { side: Side, path: String },
    Error(String),
}

/// Non-blocking command sender; `send` returns `false` when the actor is busy or gone.
pub type TableCompareHandle = CommandHandle<TableCommand>;

/// Starts one table-compare actor. Empty paths mean "no file on that side".
pub fn spawn_table_compare(
    rt: &Handle,
    config: TableCompareConfig,
    source: Arc<dyn ByteSource>,
    left: PathBuf,
    right: PathBuf,
) -> (TableCompareHandle, mpsc::Receiver<TableEvent>) {
    spawn_batched(rt, |etx| Actor {
        config,
        source,
        sides: [SideState::new(left), SideState::new(right)],
        options: TableCompareOptions::default(),
        last_diff: Vec::new(),
        etx,
    })
}

struct SideState {
    path: PathBuf,
    workbook: Option<Workbook>,
    selected: Option<usize>,
    error: Option<String>,
    dirty: bool,
    edited: HashSet<usize>,
}

impl SideState {
    fn new(path: PathBuf) -> Self {
        Self { path, workbook: None, selected: None, error: None, dirty: false, edited: HashSet::new() }
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

#[async_trait]
impl BatchedActor for Actor {
    type Command = TableCommand;

    async fn init(&mut self) -> bool {
        self.reload().await
    }

    async fn apply(&mut self, command: TableCommand) -> bool {
        match command {
            TableCommand::SetOptions(options) => {
                self.options = options;
                true
            }
            TableCommand::SelectSheets { left, right } => {
                self.select_sheets(left, right);
                true
            }
            TableCommand::Reload => self.reload().await,
            TableCommand::Copy { start, end, to_right } => {
                self.copy(start, end, to_right);
                true
            }
            TableCommand::SetCell { side, row, col, text } => self.set_cell(side, row, col, text).await,
            TableCommand::Save { side, path } => self.save(side, path).await,
        }
    }

    async fn publish(&mut self) -> bool {
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
            edited: [self.sides[0].edited.clone(), self.sides[1].edited.clone()],
            left,
            right,
        };
        self.emit(TableEvent::Result(Arc::new(view))).await
    }
}

impl Actor {
    async fn emit(&self, event: TableEvent) -> bool {
        send_event(&self.etx, event).await
    }

    fn select_sheets(&mut self, left: Option<usize>, right: Option<usize>) {
        for (side, pick) in self.sides.iter_mut().zip([left, right]) {
            let count = side.workbook.as_ref().map_or(0, |w| w.sheets.len());
            if let Some(i) = pick.filter(|&i| i < count) {
                if side.selected != Some(i) {
                    side.edited.clear();
                }
                side.selected = Some(i);
            }
        }
    }

    async fn load_side(&self, idx: usize) -> Result<Option<Workbook>, String> {
        let path = &self.sides[idx].path;
        let (ext, max_cells) = (extension_of(path), self.config.max_cells);
        let parse = move |bytes: Vec<u8>| decode_workbook(bytes, &ext, max_cells);
        load_parsed(self.source.as_ref(), path, self.config.max_file_bytes, "decode", parse).await
    }

    async fn reload(&mut self) -> bool {
        for idx in 0..2 {
            if !self.emit(TableEvent::Status(format!("Loading {}...", self.sides[idx].path.display()))).await {
                return false;
            }
            let loaded = self.load_side(idx).await; // one unreadable side never blocks the other
            let side = &mut self.sides[idx];
            (side.dirty, side.error) = (false, None);
            side.edited.clear();
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

    fn copy(&mut self, start: usize, end: usize, to_right: bool) {
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
        self.sides[dst].edited.clear();
    }

    async fn set_cell(&mut self, side: Side, row: usize, col: usize, text: String) -> bool {
        let idx = side.index();
        if !self.sides[idx].info().editable {
            let msg = format!("{} side is not an editable CSV file", side.label());
            return self.emit(TableEvent::Error(msg)).await;
        }
        let number = self.last_diff.get(row).map_or(0, |r| if idx == 0 { r.left_row } else { r.right_row }) as usize;
        if number == 0 {
            let msg = format!("That row does not exist on the {} side; copy it across first", side.label());
            return self.emit(TableEvent::Error(msg)).await;
        }
        let mut table = self.sides[idx].take_table();
        let changed = match Arc::make_mut(&mut table).rows.get_mut(number - 1) {
            Some(cells) => {
                if cells.len() <= col {
                    cells.resize(col + 1, String::new());
                }
                let differs = cells[col] != text;
                if differs {
                    cells[col] = text;
                }
                differs
            }
            None => false,
        };
        self.sides[idx].put_table(table);
        if changed {
            self.sides[idx].dirty = true;
            self.sides[idx].edited.insert(number - 1);
        }
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
        match write_bytes(self.source.as_ref(), &target, &bytes).await {
            Ok(()) => {
                if in_place {
                    self.sides[side.index()].dirty = false;
                    self.sides[side.index()].edited.clear();
                }
                self.emit(TableEvent::Saved { side, path: target.display().to_string() }).await
            }
            Err(message) => self.emit(TableEvent::Error(message)).await,
        }
    }
}

//! C ABI for the Table Compare tab (CSV / Excel / ODS). See `ffi::common` for the contract.

use std::ffi::{c_char, c_void};
use std::sync::Arc;

use openbc_core::table_compare::{RowKind, TableCompareOptions};
use openbc_engine::common::Side;
use openbc_engine::table_compare::{spawn_table_compare, TableCommand, TableCompareConfig, TableCompareHandle, TableEvent, TableView};

use super::common::*;

pub struct TableSession {
    handle: TableCompareHandle,
    sink: Sink,
}

type ViewHandle = Arc<TableView>;

/// # Safety
/// `left`/`right` null or NUL-terminated UTF-8; `callback` must stay callable until destroy.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_session_create(left: *const c_char, right: *const c_char, callback: EventCallback, user: *mut c_void) -> *mut TableSession {
    let sink = Sink::new(callback, user);
    let (handle, mut events) = spawn_table_compare(runtime().handle(), TableCompareConfig::default(), local_source(), path_arg(left), path_arg(right));
    let pump = sink.clone();
    std::thread::Builder::new()
        .name("openbc-table-events".into())
        .spawn(move || {
            while let Some(event) = events.blocking_recv() {
                match event {
                    TableEvent::Status(m) => pump.message(EV_STATUS, &m),
                    TableEvent::Error(m) => pump.message(EV_ERROR, &m),
                    TableEvent::Saved { side, path } => pump.message(EV_SAVED, &format!("{}|{path}", side.index())),
                    TableEvent::Result(view) => pump.handle(EV_RESULT, Box::new(view)),
                }
            }
        })
        .ok();
    Box::into_raw(Box::new(TableSession { handle, sink }))
}

/// # Safety
/// `s` from `openbc_table_session_create`, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_session_destroy(s: *mut TableSession) {
    if let Some(session) = (!s.is_null()).then(|| Box::from_raw(s)) {
        session.sink.close();
    }
}

/// # Safety
/// `keys` points to `key_count` readable `u32`s (or is null with count 0).
#[no_mangle]
pub unsafe extern "C" fn openbc_table_set_options(s: *const TableSession, ignore_case: u8, trim: u8, tolerance: f64, keys: *const u32, key_count: usize) -> u8 {
    let session = handle_ref!(s, 0);
    let key_columns = if keys.is_null() { Vec::new() } else { std::slice::from_raw_parts(keys, key_count).iter().map(|&k| k as usize).collect() };
    let options = TableCompareOptions { ignore_case: ignore_case != 0, trim_cells: trim != 0, numeric_tolerance: tolerance.max(0.0), key_columns };
    u8::from(session.handle.send(TableCommand::SetOptions(options)))
}

/// Pass -1 to keep a side's current sheet.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_select_sheets(s: *const TableSession, left: i32, right: i32) -> u8 {
    let session = handle_ref!(s, 0);
    let pick = |v: i32| usize::try_from(v).ok();
    u8::from(session.handle.send(TableCommand::SelectSheets { left: pick(left), right: pick(right) }))
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_reload(s: *const TableSession) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(TableCommand::Reload))
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_copy(s: *const TableSession, start: usize, end: usize, to_right: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(TableCommand::Copy { start, end, to_right: to_right != 0 }))
}

/// `path` null saves in place (CSV only); otherwise exports the sheet as CSV.
///
/// # Safety
/// `s` valid; `path` null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_save(s: *const TableSession, left: u8, path: *const c_char) -> u8 {
    let session = handle_ref!(s, 0);
    let target = (!path.is_null()).then(|| path_arg(path));
    let side = if left != 0 { Side::Left } else { Side::Right };
    u8::from(session.handle.send(TableCommand::Save { side, path: target }))
}

// ------------------------------------------------------------------------ view accessors

fn side_of(side: u8) -> usize {
    usize::from(side != 0)
}

/// # Safety
/// `v` from a result event, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_view_destroy(v: *mut ViewHandle) {
    if !v.is_null() {
        drop(Box::from_raw(v));
    }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_view_rows(v: *const ViewHandle) -> usize {
    handle_ref!(v, 0).diff.len()
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_view_columns(v: *const ViewHandle) -> usize {
    handle_ref!(v, 0).columns
}

/// 0 equal, 1 changed, 2 left only, 3 right only.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_row_kind(v: *const ViewHandle, row: usize) -> u8 {
    handle_ref!(v, 0).diff.get(row).map_or(0, |r| match r.kind {
        RowKind::Equal => 0,
        RowKind::Changed => 1,
        RowKind::LeftOnly => 2,
        RowKind::RightOnly => 3,
    })
}

/// 1-based source row number on that side, 0 when the side has no row.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_row_number(v: *const ViewHandle, row: usize, side: u8) -> u32 {
    handle_ref!(v, 0).diff.get(row).map_or(0, |r| if side == 0 { r.left_row } else { r.right_row })
}

/// Cell text; empty (length 0) for missing rows/cells.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_cell(v: *const ViewHandle, row: usize, side: u8, col: usize, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_str("", len));
    let number = view.diff.get(row).map_or(0, |r| if side == 0 { r.left_row } else { r.right_row });
    let table = if side == 0 { &view.left } else { &view.right };
    let text = (number > 0).then(|| table.rows.get(number as usize - 1).and_then(|r| r.get(col))).flatten();
    out_str(text.map_or("", String::as_str), len)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_cell_changed(v: *const ViewHandle, row: usize, col: usize) -> u8 {
    handle_ref!(v, 0).diff.get(row).map_or(0, |r| u8::from(r.changed_cols.binary_search(&(col as u32)).is_ok()))
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_group_count(v: *const ViewHandle) -> usize {
    handle_ref!(v, 0).groups.len()
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_group_start(v: *const ViewHandle, g: usize) -> usize {
    handle_ref!(v, 0).groups.get(g).map_or(0, |g| g.start)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_group_end(v: *const ViewHandle, g: usize) -> usize {
    handle_ref!(v, 0).groups.get(g).map_or(0, |g| g.end)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_sheet_count(v: *const ViewHandle, side: u8) -> usize {
    handle_ref!(v, 0).sides[side_of(side)].sheets.len()
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_sheet_name(v: *const ViewHandle, side: u8, index: usize, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_str("", len));
    out_str(view.sides[side_of(side)].sheets.get(index).map_or("", String::as_str), len)
}

/// Selected sheet index or -1.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_selected_sheet(v: *const ViewHandle, side: u8) -> i32 {
    handle_ref!(v, -1).sides[side_of(side)].selected.map_or(-1, |i| i as i32)
}

/// bit0 file loaded, bit1 editable (CSV), bit2 unsaved edits.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_side_flags(v: *const ViewHandle, side: u8) -> u32 {
    let info = &handle_ref!(v, 0).sides[side_of(side)];
    u32::from(info.loaded) | u32::from(info.editable) << 1 | u32::from(info.dirty) << 2
}

/// Load error for the side, or null.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_side_error(v: *const ViewHandle, side: u8, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(view.sides[side_of(side)].error.as_deref(), len)
}
// ------------------------------------------------------------------------ cell editing

/// Type `utf8_text` into one cell of `side` (0 left / 1 right). `row` is a diff row (the same
/// index `openbc_table_cell` takes), `col` a column. Returns 1 when the backend took the
/// command, 0 when it is busy (the caller may retry). The re-diffed view arrives as a Result.
///
/// # Safety
/// `s` valid; `utf8_text` null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_set_cell(s: *mut TableSession, side: u8, row: usize, col: usize, utf8_text: *const c_char) -> u8 {
    let session = handle_ref!(s, 0);
    let text = if utf8_text.is_null() { String::new() } else { std::ffi::CStr::from_ptr(utf8_text).to_string_lossy().into_owned() };
    let side = if side == 0 { Side::Left } else { Side::Right };
    u8::from(session.handle.send(TableCommand::SetCell { side, row, col, text }))
}

/// Non-zero when this side's row was typed into since load / last save.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_table_row_edited(v: *const ViewHandle, row: usize, side: u8) -> u8 {
    let view = handle_ref!(v, 0);
    let Some(diff) = view.diff.get(row) else { return 0 };
    let number = if side == 0 { diff.left_row } else { diff.right_row } as usize;
    u8::from(number > 0 && view.edited[usize::from(side != 0)].contains(&(number - 1)))
}

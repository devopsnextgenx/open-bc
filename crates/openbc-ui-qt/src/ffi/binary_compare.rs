//! C ABI for the Binary Compare tab. See `ffi::common` for the contract.
//!
//! One `EV_RESULT` event carries a finished comparison (`Arc<BinarySummary>`, destroyed with
//! `openbc_binary_summary_destroy`). The result is a pre-order list of tree rows; the Qt side
//! rebuilds the parent/child structure from `depth`, so no pointers into Rust data are exposed.
//! `EV_PROGRESS` carries the stage text (`parsing`) and `EV_ERROR` a fatal actor error.
//!
//! Row `kind` and `state` codes are `openbc_core::binary_compare::NodeKind::code` /
//! `ChangeState::code` (state `255` = none).

use std::ffi::{c_char, c_void};
use std::sync::Arc;

use openbc_engine::binary_compare::{
    spawn_binary_compare, BinaryCommand, BinaryCompareHandle, BinaryEvent, BinaryOptions, BinarySummary,
};

use super::common::*;

pub struct BinarySession {
    handle: BinaryCompareHandle,
    sink: Sink,
}

type SummaryHandle = Arc<BinarySummary>;

/// # Safety
/// See `openbc_table_session_create`.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_session_create(left: *const c_char, right: *const c_char, callback: EventCallback, user: *mut c_void) -> *mut BinarySession {
    let sink = Sink::new(callback, user);
    let (handle, mut events) = spawn_binary_compare(runtime().handle(), path_arg(left), path_arg(right), BinaryOptions::default());
    let pump = sink.clone();
    std::thread::Builder::new()
        .name("openbc-binary-events".into())
        .spawn(move || {
            while let Some(event) = events.blocking_recv() {
                match event {
                    BinaryEvent::Stage(stage) => pump.message(EV_PROGRESS, stage),
                    BinaryEvent::Error(m) => pump.message(EV_ERROR, &m),
                    BinaryEvent::Summary(s) => pump.handle(EV_RESULT, Box::new(s)),
                }
            }
        })
        .ok();
    Box::into_raw(Box::new(BinarySession { handle, sink }))
}

/// # Safety
/// `s` from create, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_session_destroy(s: *mut BinarySession) {
    if let Some(session) = (!s.is_null()).then(|| Box::from_raw(s)) {
        session.sink.close();
    }
}

/// Parse and diff both files again (after a file changed on disk).
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_reload(s: *const BinarySession) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(BinaryCommand::Reload))
}

/// Show (`1`) or hide (`0`) unchanged elements; answered with a new `EV_RESULT`, no re-parse.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_show_unchanged(s: *const BinarySession, show: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(BinaryCommand::ShowUnchanged(show != 0)))
}

/// # Safety
/// `v` from an `EV_RESULT` event, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_destroy(v: *mut SummaryHandle) {
    if !v.is_null() {
        drop(Box::from_raw(v));
    }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_count(v: *const SummaryHandle) -> usize {
    handle_ref!(v, 0).rows.len()
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_depth(v: *const SummaryHandle, i: usize) -> u32 {
    handle_ref!(v, 0).rows.get(i).map_or(0, |r| r.depth)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_kind(v: *const SummaryHandle, i: usize) -> u8 {
    handle_ref!(v, 0).rows.get(i).map_or(0, |r| r.kind)
}

/// Change-state code: 0 unchanged, 1 modified, 2 removed, 3 added, 255 none.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_state(v: *const SummaryHandle, i: usize) -> u8 {
    handle_ref!(v, 255).rows.get(i).map_or(255, |r| r.state)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_has_children(v: *const SummaryHandle, i: usize) -> u8 {
    u8::from(handle_ref!(v, 0).rows.get(i).is_some_and(|r| r.has_children))
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_label(v: *const SummaryHandle, i: usize, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.rows.get(i).map(|r| r.label.as_str()), len)
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_detail(v: *const SummaryHandle, i: usize, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.rows.get(i).map(|r| r.detail.as_str()), len)
}

/// Count of elements in `state` (0 unchanged, 1 modified, 2 removed, 3 added).
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_total(v: *const SummaryHandle, state: u8) -> usize {
    let t = &handle_ref!(v, 0).totals;
    match state {
        0 => t.unchanged,
        1 => t.modified,
        2 => t.removed,
        3 => t.added,
        _ => 0,
    }
}

/// Whole-file verdict: 0 identical, 1 differ, 255 unknown (a side failed).
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_file_state(v: *const SummaryHandle) -> u8 {
    handle_ref!(v, 255).file_state.map_or(255, |s| s.code())
}

/// `1` when the rows list only changed elements.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_changes_only(v: *const SummaryHandle) -> u8 {
    u8::from(handle_ref!(v, 0).changes_only)
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_error(v: *const SummaryHandle, side: u8, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.errors[usize::from(side != 0)].as_deref(), len)
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_label(v: *const SummaryHandle, side: u8, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(Some(s.labels[usize::from(side != 0)].as_str()), len)
}

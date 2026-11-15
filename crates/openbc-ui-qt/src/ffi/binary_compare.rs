//! C ABI for the Binary Compare tab. See `ffi::common` for the contract.
//!
//! One `EV_RESULT` event carries a finished comparison (`Arc<BinarySummary>`, destroyed with
//! `openbc_binary_summary_destroy`). The result is a pre-order list of tree rows; the Qt side
//! rebuilds the parent/child structure from `depth`, so no pointers into Rust data are exposed.
//! `EV_PROGRESS` carries the stage text (`parsing`) and `EV_ERROR` a fatal actor error.
//!
//! The hex / disassembly inspector answers on `EV_WINDOW` with an `Arc<InspectSummary>` handle
//! (destroyed with `openbc_binary_inspect_destroy`); it is requested with `openbc_binary_inspect`.
//!
//! Row `kind` and `state` codes are `openbc_core::binary_compare::NodeKind::code` /
//! `ChangeState::code` (state `255` = none).

use std::ffi::{c_char, c_void};
use std::sync::Arc;

use openbc_engine::binary_compare::{
    spawn_binary_compare, BinaryCommand, BinaryCompareHandle, BinaryEvent, BinaryOptions, BinarySummary,
    InspectSummary,
};
use openbc_core::binary_compare::NodeKind;

use super::common::*;

pub struct BinarySession {
    handle: BinaryCompareHandle,
    sink: Sink,
}

type SummaryHandle = Arc<BinarySummary>;
type InspectHandle = Arc<InspectSummary>;

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
                    BinaryEvent::Inspect(i) => pump.handle(EV_WINDOW, Box::new(i)),
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

/// Granular change filters: a bit set (`1` symtab/strtab, `2` debug and notes, `4` padding shifts,
/// `8` local symbols). Answered with a new `EV_RESULT`, no re-parse.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_set_filters(s: *const BinarySession, mask: u32) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(BinaryCommand::SetFilters(mask)))
}

/// Show C++ names demangled (`1`) or raw (`0`); answered with a new `EV_RESULT`, no re-parse.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_set_demangle(s: *const BinarySession, on: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(BinaryCommand::SetDemangle(on != 0)))
}

/// Open the hex / disassembly inspector for row `i` of `v` (a function or section row that
/// reports `openbc_binary_summary_row_inspectable`). Answered with an `EV_WINDOW` event.
///
/// # Safety
/// `s` and `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect(s: *const BinarySession, v: *const SummaryHandle, i: usize) -> u8 {
    let session = handle_ref!(s, 0);
    let summary = handle_ref!(v, 0);
    let Some(row) = summary.rows.get(i) else { return 0 };
    if !row.inspect || (row.kind != NodeKind::Function.code() && row.kind != NodeKind::Section.code()) {
        return 0;
    }
    u8::from(session.handle.send(BinaryCommand::Inspect { kind: row.kind, key: row.key.clone() }))
}

/// `1` when the hex / disassembly inspector can open row `i`.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_inspectable(v: *const SummaryHandle, i: usize) -> u8 {
    u8::from(handle_ref!(v, 0).rows.get(i).is_some_and(|r| r.inspect))
}

/// Raw identity of row `i` (the mangled linkage name for symbols and functions).
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_key(v: *const SummaryHandle, i: usize, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.rows.get(i).map(|r| r.key.as_str()), len)
}

/// Version 1 value of row `i` for the split viewer (empty: absent, or a group row).
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_left(v: *const SummaryHandle, i: usize, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.rows.get(i).map(|r| r.left.as_str()), len)
}

/// Version 2 value of row `i` for the split viewer (empty: absent, or a group row).
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_row_right(v: *const SummaryHandle, i: usize, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.rows.get(i).map(|r| r.right.as_str()), len)
}

/// Changed elements hidden by the granular filters.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_summary_hidden(v: *const SummaryHandle) -> usize {
    handle_ref!(v, 0).hidden
}

// ----------------------------------------------------------------------------- inspector

/// # Safety
/// `v` from an `EV_WINDOW` event of a binary session, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_destroy(v: *mut InspectHandle) {
    if !v.is_null() {
        drop(Box::from_raw(v));
    }
}

/// Why nothing can be shown (unknown element, unreadable file, ...), or null.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_error(v: *const InspectHandle, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.error.as_deref(), len)
}

/// Raw identity of the inspected element.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_title(v: *const InspectHandle, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(Some(s.output.title.as_str()), len)
}

/// Number of notes (caveats) to show above the views.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_note_count(v: *const InspectHandle) -> usize {
    handle_ref!(v, 0).output.notes.len()
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_note(v: *const InspectHandle, i: usize, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.output.notes.get(i).map(String::as_str), len)
}

/// Bytes that differ between the two versions (bytes present on one side only count).
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_differing_bytes(v: *const InspectHandle) -> usize {
    handle_ref!(v, 0).output.differing_bytes
}

/// Byte count shown for `side` (0 left, 1 right).
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_length(v: *const InspectHandle, side: u8) -> usize {
    let o = &handle_ref!(v, 0).output;
    if side == 0 { o.left_len } else { o.right_len }
}

/// Number of hex rows.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_hex_count(v: *const InspectHandle) -> usize {
    handle_ref!(v, 0).output.hex.len()
}

/// State of hex row `i`: 0 same, 1 modified, 2 left only, 3 right only.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_hex_state(v: *const InspectHandle, i: usize) -> u8 {
    handle_ref!(v, 255).output.hex.get(i).map_or(255, |r| r.state.code())
}

/// Bit `n` set: byte `n` of hex row `i` differs.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_hex_mask(v: *const InspectHandle, i: usize) -> u32 {
    handle_ref!(v, 0).output.hex.get(i).map_or(0, |r| u32::from(r.mask))
}

/// Formatted hex row `i` of `side` (0 left, 1 right); empty when that side has no bytes there.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_hex_text(v: *const InspectHandle, i: usize, side: u8, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.output.hex.get(i).map(|r| if side == 0 { r.left.as_str() } else { r.right.as_str() }), len)
}

/// `1` when a disassembly diff is available (x86 / x86-64 code).
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_asm_available(v: *const InspectHandle) -> u8 {
    u8::from(handle_ref!(v, 0).output.asm.is_some())
}

/// Number of disassembly rows.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_asm_count(v: *const InspectHandle) -> usize {
    handle_ref!(v, 0).output.asm.as_ref().map_or(0, Vec::len)
}

/// State of disassembly row `i`: 0 same, 1 modified, 2 left only, 3 right only.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_asm_state(v: *const InspectHandle, i: usize) -> u8 {
    handle_ref!(v, 255)
        .output
        .asm
        .as_ref()
        .and_then(|rows| rows.get(i))
        .map_or(255, |r| r.state.code())
}

/// Formatted disassembly row `i` of `side` (0 left, 1 right); empty when absent on that side.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_binary_inspect_asm_text(v: *const InspectHandle, i: usize, side: u8, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    let row = s.output.asm.as_ref().and_then(|rows| rows.get(i));
    out_opt_str(row.map(|r| if side == 0 { r.left.as_str() } else { r.right.as_str() }), len)
}

#[cfg(test)]
mod tests {
    use openbc_core::binary_compare::ChangeState;

    #[test]
    fn state_codes_match_the_c_header() {
        assert_eq!(ChangeState::Unchanged.code(), 0);
        assert_eq!(ChangeState::Modified.code(), 1);
        assert_eq!(ChangeState::Removed.code(), 2);
        assert_eq!(ChangeState::Added.code(), 3);
    }
}

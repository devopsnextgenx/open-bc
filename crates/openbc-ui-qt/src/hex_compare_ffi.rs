//! C ABI for the Hex Compare tab. See `ffi_common` for the contract.
//!
//! Two kinds of handles arrive: `EV_RESULT` carries a summary (sizes + difference runs,
//! destroyed with `openbc_hex_summary_destroy`) and `EV_WINDOW` a byte window for both
//! files (destroyed with `openbc_hex_window_destroy`).

use std::ffi::{c_char, c_void};
use std::sync::Arc;

use openbc_engine::hex_compare::{spawn_hex_compare, HexCommand, HexCompareHandle, HexEvent, HexSummary, HexWindow};

use crate::ffi_common::*;

pub struct HexSession {
    handle: HexCompareHandle,
    sink: Sink,
}

type SummaryHandle = Arc<HexSummary>;
type WindowHandle = Arc<HexWindow>;

/// # Safety
/// See `openbc_table_session_create`.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_session_create(left: *const c_char, right: *const c_char, callback: EventCallback, user: *mut c_void) -> *mut HexSession {
    let sink = Sink::new(callback, user);
    let (handle, mut events) = spawn_hex_compare(runtime().handle(), local_source(), path_arg(left), path_arg(right));
    let pump = sink.clone();
    std::thread::Builder::new()
        .name("openbc-hex-events".into())
        .spawn(move || {
            while let Some(event) = events.blocking_recv() {
                match event {
                    HexEvent::Progress { done, total } => pump.message(EV_PROGRESS, &format!("{done}|{total}")),
                    HexEvent::Error(m) => pump.message(EV_ERROR, &m),
                    HexEvent::Summary(s) => pump.handle(EV_RESULT, Box::new(s)),
                    HexEvent::Window(w) => pump.handle(EV_WINDOW, Box::new(w)),
                }
            }
        })
        .ok();
    Box::into_raw(Box::new(HexSession { handle, sink }))
}

/// # Safety
/// `s` from create, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_session_destroy(s: *mut HexSession) {
    if let Some(session) = (!s.is_null()).then(|| Box::from_raw(s)) {
        session.sink.close();
    }
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_reload(s: *const HexSession) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(HexCommand::Reload))
}

/// Asks for `len` bytes at `offset` of both files; answered with an `EV_WINDOW` event
/// carrying the same `id`.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_request_window(s: *const HexSession, id: u64, offset: u64, len: usize) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(HexCommand::Window { id, offset, len }))
}

/// # Safety
/// `v` from an `EV_RESULT` event, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_destroy(v: *mut SummaryHandle) {
    if !v.is_null() {
        drop(Box::from_raw(v));
    }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_size(v: *const SummaryHandle, side: u8) -> u64 {
    let s = handle_ref!(v, 0);
    if side == 0 { s.left_len } else { s.right_len }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_run_count(v: *const SummaryHandle) -> usize {
    handle_ref!(v, 0).runs.len()
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_run_offset(v: *const SummaryHandle, i: usize) -> u64 {
    handle_ref!(v, 0).runs.get(i).map_or(0, |r| r.offset)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_run_len(v: *const SummaryHandle, i: usize) -> u64 {
    handle_ref!(v, 0).runs.get(i).map_or(0, |r| r.len)
}

/// Index of the run containing `offset`, or -1.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_run_at(v: *const SummaryHandle, offset: u64) -> i64 {
    let runs = &handle_ref!(v, -1).runs;
    let i = runs.partition_point(|r| r.offset + r.len <= offset);
    runs.get(i).filter(|r| r.offset <= offset).map_or(-1, |_| i as i64)
}

/// Index of the first run starting after `offset`, or -1.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_next_run(v: *const SummaryHandle, offset: u64) -> i64 {
    let runs = &handle_ref!(v, -1).runs;
    let i = runs.partition_point(|r| r.offset <= offset);
    if i < runs.len() { i as i64 } else { -1 }
}

/// Index of the last run starting before `offset`, or -1.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_prev_run(v: *const SummaryHandle, offset: u64) -> i64 {
    let runs = &handle_ref!(v, -1).runs;
    let i = runs.partition_point(|r| r.offset < offset);
    if i > 0 { (i - 1) as i64 } else { -1 }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_truncated(v: *const SummaryHandle) -> u8 {
    u8::from(handle_ref!(v, 0).truncated)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_diff_bytes(v: *const SummaryHandle) -> u64 {
    handle_ref!(v, 0).diff_bytes
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_summary_error(v: *const SummaryHandle, side: u8, len: *mut usize) -> *const u8 {
    let s = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(s.errors[usize::from(side != 0)].as_deref(), len)
}

/// # Safety
/// `w` from an `EV_WINDOW` event, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_window_destroy(w: *mut WindowHandle) {
    if !w.is_null() {
        drop(Box::from_raw(w));
    }
}

/// # Safety
/// `w` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_window_id(w: *const WindowHandle) -> u64 {
    handle_ref!(w, 0).id
}

/// # Safety
/// `w` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_window_offset(w: *const WindowHandle) -> u64 {
    handle_ref!(w, 0).offset
}

/// Bytes of one side's window (may be shorter than requested at EOF).
///
/// # Safety
/// `w` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_hex_window_bytes(w: *const WindowHandle, side: u8, len: *mut usize) -> *const u8 {
    let w = handle_ref!(w, out_opt_str(None, len));
    let bytes = if side == 0 { &w.left } else { &w.right };
    if !len.is_null() {
        *len = bytes.len();
    }
    bytes.as_ptr()
}
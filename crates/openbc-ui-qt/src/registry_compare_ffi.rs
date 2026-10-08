//! C ABI for the Registry Compare tab (.reg exports). See `ffi_common` for the contract.

use std::ffi::{c_char, c_void};
use std::sync::Arc;

use openbc_core::registry_compare::{RegCompareOptions, RegRowKind};
use openbc_engine::common::Side;
use openbc_engine::registry_compare::{spawn_registry_compare, RegistryCommand, RegistryCompareHandle, RegistryEvent, RegistryView};

use crate::ffi_common::*;

pub struct RegistrySession {
    handle: RegistryCompareHandle,
    sink: Sink,
}

/// View plus pre-rendered value text (display strings need stable storage for the C side).
pub struct RegistryViewHandle {
    view: Arc<RegistryView>,
    shown: Vec<[Option<String>; 2]>,
}

/// # Safety
/// See `openbc_table_session_create`.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_session_create(left: *const c_char, right: *const c_char, callback: EventCallback, user: *mut c_void) -> *mut RegistrySession {
    let sink = Sink::new(callback, user);
    let (handle, mut events) = spawn_registry_compare(runtime().handle(), local_source(), path_arg(left), path_arg(right));
    let pump = sink.clone();
    std::thread::Builder::new()
        .name("openbc-registry-events".into())
        .spawn(move || {
            while let Some(event) = events.blocking_recv() {
                match event {
                    RegistryEvent::Status(m) => pump.message(EV_STATUS, &m),
                    RegistryEvent::Error(m) => pump.message(EV_ERROR, &m),
                    RegistryEvent::Saved { side, path } => pump.message(EV_SAVED, &format!("{}|{path}", side.index())),
                    RegistryEvent::Result(view) => {
                        let shown = view.rows.iter().map(|r| [r.left.as_ref().map(|v| v.display()), r.right.as_ref().map(|v| v.display())]).collect();
                        pump.handle(EV_RESULT, Box::new(RegistryViewHandle { view, shown }));
                    }
                }
            }
        })
        .ok();
    Box::into_raw(Box::new(RegistrySession { handle, sink }))
}

/// # Safety
/// `s` from create, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_session_destroy(s: *mut RegistrySession) {
    if let Some(session) = (!s.is_null()).then(|| Box::from_raw(s)) {
        session.sink.close();
    }
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_set_options(s: *const RegistrySession, ignore_data_case: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(RegistryCommand::SetOptions(RegCompareOptions { ignore_data_case: ignore_data_case != 0 })))
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_reload(s: *const RegistrySession) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(RegistryCommand::Reload))
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_copy(s: *const RegistrySession, start: usize, end: usize, to_right: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(RegistryCommand::Copy { start, end, to_right: to_right != 0 }))
}

/// # Safety
/// `s` valid; `path` null (save in place) or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_save(s: *const RegistrySession, left: u8, path: *const c_char) -> u8 {
    let session = handle_ref!(s, 0);
    let target = (!path.is_null()).then(|| path_arg(path));
    u8::from(session.handle.send(RegistryCommand::Save { side: if left != 0 { Side::Left } else { Side::Right }, path: target }))
}

/// # Safety
/// `v` from a result event, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_view_destroy(v: *mut RegistryViewHandle) {
    if !v.is_null() {
        drop(Box::from_raw(v));
    }
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_view_rows(v: *const RegistryViewHandle) -> usize {
    handle_ref!(v, 0).view.rows.len()
}

/// 0 equal, 1 changed, 2 left only, 3 right only.
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_row_kind(v: *const RegistryViewHandle, row: usize) -> u8 {
    handle_ref!(v, 0).view.rows.get(row).map_or(0, |r| match r.kind {
        RegRowKind::Equal => 0,
        RegRowKind::Changed => 1,
        RegRowKind::LeftOnly => 2,
        RegRowKind::RightOnly => 3,
    })
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_row_key(v: *const RegistryViewHandle, row: usize, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(view.view.rows.get(row).map(|r| r.key.as_str()), len)
}

/// Value name; null for the key row itself.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_row_name(v: *const RegistryViewHandle, row: usize, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(view.view.rows.get(row).and_then(|r| r.name.as_deref()), len)
}

/// Value type (`REG_SZ`...) on a side; null when that side lacks the value.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_row_type(v: *const RegistryViewHandle, row: usize, side: u8, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    let value = view.view.rows.get(row).and_then(|r| if side == 0 { r.left.as_ref() } else { r.right.as_ref() });
    out_opt_str(value.map(|v| v.kind.as_str()), len)
}

/// Human-readable data on a side; null when that side lacks the value.
///
/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_row_data(v: *const RegistryViewHandle, row: usize, side: u8, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(view.shown.get(row).and_then(|s| s[usize::from(side != 0)].as_deref()), len)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_group_count(v: *const RegistryViewHandle) -> usize {
    handle_ref!(v, 0).view.groups.len()
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_group_start(v: *const RegistryViewHandle, g: usize) -> usize {
    handle_ref!(v, 0).view.groups.get(g).map_or(0, |g| g.0)
}

/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_group_end(v: *const RegistryViewHandle, g: usize) -> usize {
    handle_ref!(v, 0).view.groups.get(g).map_or(0, |g| g.1)
}

/// bit0 loaded, bit2 unsaved edits (bit1 unused: `.reg` is always writable).
///
/// # Safety
/// `v` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_side_flags(v: *const RegistryViewHandle, side: u8) -> u32 {
    let info = &handle_ref!(v, 0).view.sides[usize::from(side != 0)];
    u32::from(info.loaded) | 2 | u32::from(info.dirty) << 2
}

/// # Safety
/// `v` valid; `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_registry_side_error(v: *const RegistryViewHandle, side: u8, len: *mut usize) -> *const u8 {
    let view = handle_ref!(v, out_opt_str(None, len));
    out_opt_str(view.view.sides[usize::from(side != 0)].error.as_deref(), len)
}
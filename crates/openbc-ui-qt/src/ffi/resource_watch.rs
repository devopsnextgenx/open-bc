//! C ABI for external-change detection (one `WatchSession` per tab).
//!
//! Contract with the Qt side (cpp/common/resource_watch.h):
//! * `openbc_watch_set_*` registers what a side loaded. Call it BEFORE reading the file so the
//!   baseline is never newer than the bytes read.
//! * `openbc_watch_rebase` = "Reload Latest" (call before re-reading), `openbc_watch_keep` =
//!   "Keep Current". Neither reads or replaces content; the viewer does that itself.
//! * Events arrive on a Rust worker thread via the usual `EventCallback` as `EV_STATUS` messages
//!   `"<stale|fresh|unavailable>|<0 left|1 right>|<detail>"`; the Qt wrapper re-posts them to the
//!   GUI thread. After `openbc_watch_destroy` returns, the callback is never invoked again.
//!
//! All sessions share one `WatchHub`, so two tabs on one file use one OS watcher / poller.
//! Every function returns 1 on success and 0 if the session is busy or the argument is invalid.

use std::ffi::{c_char, c_void};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};

use openbc_engine::common::Side;
use openbc_engine::resource_watch::{spawn_tab_watch, ResourceEvent, ResourceTarget, TabId, TabWatchHandle, WatchCommand};
use openbc_vfs::observe::{WatchConfig, WatchHub};

use super::common::*;
use super::vfs_bridge::watch_target;

pub struct WatchSession {
    handle: TabWatchHandle,
    sink: Sink,
}

fn hub() -> &'static Arc<WatchHub> {
    static HUB: OnceLock<Arc<WatchHub>> = OnceLock::new();
    HUB.get_or_init(|| Arc::new(WatchHub::new(runtime().handle().clone(), WatchConfig::default())))
}

fn side_of(side: u8) -> Side {
    if side == 0 { Side::Left } else { Side::Right }
}

fn side_index(side: Side) -> u8 {
    u8::from(side == Side::Right)
}

/// # Safety
/// `callback`/`user` follow the contract of `ffi::common::EventCallback`.
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_create(callback: EventCallback, user: *mut c_void) -> *mut WatchSession {
    static NEXT_TAB: AtomicU64 = AtomicU64::new(1);
    let sink = Sink::new(callback, user);
    let (handle, mut events) = spawn_tab_watch(runtime().handle(), Arc::clone(hub()), TabId(NEXT_TAB.fetch_add(1, Ordering::Relaxed)));
    let pump = sink.clone();
    std::thread::Builder::new()
        .name("openbc-watch-events".into())
        .spawn(move || {
            while let Some(event) = events.blocking_recv() {
                let text = match event {
                    ResourceEvent::Stale { side, change, .. } => format!("stale|{}|{}", side_index(side), change.as_str()),
                    ResourceEvent::Fresh { side, .. } => format!("fresh|{}|", side_index(side)),
                    ResourceEvent::Unavailable { side, message, .. } => format!("unavailable|{}|{message}", side_index(side)),
                };
                pump.message(EV_STATUS, &text);
            }
        })
        .ok();
    Box::into_raw(Box::new(WatchSession { handle, sink }))
}

/// Closes the tab's watch: no callback after this returns; unused watchers are released.
///
/// # Safety
/// `s` from create, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_destroy(s: *mut WatchSession) {
    if let Some(session) = (!s.is_null()).then(|| Box::from_raw(s)) {
        session.sink.close();
    }
}

/// Watches a local file (`is_dir == 0`) or one folder level (`is_dir != 0`) for `side`.
///
/// # Safety
/// `s` valid; `path` a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_set_local(s: *const WatchSession, side: u8, path: *const c_char, is_dir: u8) -> u8 {
    let session = handle_ref!(s, 0);
    let path = path_arg(path);
    if path.as_os_str().is_empty() {
        return u8::from(session.handle.send(WatchCommand::Clear { side: side_of(side) }));
    }
    let target = if is_dir != 0 { ResourceTarget::LocalDir(path) } else { ResourceTarget::LocalFile(path) };
    u8::from(session.handle.send(WatchCommand::Set { side: side_of(side), target }))
}

/// Watches a path behind VFS session `session_id` (remote profile). Polled, not push-based.
///
/// # Safety
/// `s` valid; `path` a NUL-terminated UTF-8 string (the provider-relative path).
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_set_remote(s: *const WatchSession, side: u8, session_id: i32, path: *const c_char, is_dir: u8) -> u8 {
    let session = handle_ref!(s, 0);
    let raw = path_arg(path).to_string_lossy().into_owned();
    match watch_target(session_id, &raw) {
        Ok((vfs, entry)) => u8::from(session.handle.send(WatchCommand::Set {
            side: side_of(side),
            target: ResourceTarget::Vfs { session_key: session_id.to_string(), vfs, path: entry, directory: is_dir != 0 },
        })),
        Err(_) => 0,
    }
}

/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_clear(s: *const WatchSession, side: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(WatchCommand::Clear { side: side_of(side) }))
}

/// "Reload Latest": adopt the current version as loaded. Call BEFORE re-reading the file.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_rebase(s: *const WatchSession, side: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(WatchCommand::Rebase { side: side_of(side) }))
}

/// "Keep Current": no more notices for the version seen now; a newer one notifies again.
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_watch_keep(s: *const WatchSession, side: u8) -> u8 {
    u8::from(handle_ref!(s, 0).handle.send(WatchCommand::Keep { side: side_of(side) }))
}

//! Shared C-ABI plumbing for the compare tabs (table, registry, hex, media).
//!
//! Contract with the Qt side:
//! * One session per tab. Events are delivered on a Rust worker thread through
//!   [`EventCallback`]; the Qt tab copies what it needs and re-posts to the GUI thread.
//! * `handle` arguments transfer ownership: the receiver must call the matching
//!   `*_destroy` exactly once (even if the tab died meanwhile).
//! * After `*_session_destroy` returns, the callback is never invoked again.
//! * String accessors return a pointer + length (not NUL-terminated) that stays valid
//!   while the owning view handle is alive.
//!
//! # Safety
//! Every `unsafe extern "C"` function requires its pointer arguments to be null or to
//! come from this library, and not yet destroyed.

use std::ffi::{c_char, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};

use openbc_engine::common::{ByteSource, LocalByteSource};

pub const EV_STATUS: i32 = 0;
pub const EV_RESULT: i32 = 1;
pub const EV_ERROR: i32 = 2;
pub const EV_SAVED: i32 = 3;
pub const EV_WINDOW: i32 = 4;
pub const EV_PROGRESS: i32 = 5;

/// `message` is valid only during the call; `handle` is owned by the receiver.
pub type EventCallback = extern "C" fn(user: *mut c_void, kind: i32, message: *const c_char, handle: *mut c_void);

/// Shared Tokio runtime for compare actors (replace with the app-wide runtime if one exists).
pub fn runtime() -> &'static tokio::runtime::Runtime {
    static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .thread_name("openbc-compare")
            .enable_all()
            .build()
            .expect("tokio runtime")
    })
}

pub fn local_source() -> Arc<dyn ByteSource> {
    Arc::new(LocalByteSource)
}

/// UTF-8 C string to path; null or empty means "no file".
///
/// # Safety
/// `p` must be null or a valid NUL-terminated string.
pub unsafe fn path_arg(p: *const c_char) -> PathBuf {
    if p.is_null() { PathBuf::new() } else { PathBuf::from(CStr::from_ptr(p).to_string_lossy().into_owned()) }
}

/// Writes the length and returns the data pointer of `s`.
///
/// # Safety
/// `len` must be null or writable.
pub unsafe fn out_str(s: &str, len: *mut usize) -> *const u8 {
    if !len.is_null() {
        *len = s.len();
    }
    s.as_ptr()
}

/// Like [`out_str`] but null (length 0) for `None`.
///
/// # Safety
/// `len` must be null or writable.
pub unsafe fn out_opt_str(s: Option<&str>, len: *mut usize) -> *const u8 {
    match s {
        Some(s) => out_str(s, len),
        None => {
            if !len.is_null() {
                *len = 0;
            }
            std::ptr::null()
        }
    }
}

/// Event delivery gate. The lock is held across each callback, so `close()` returns only
/// after any in-flight callback finished and none can start afterwards.
#[derive(Clone)]
pub struct Sink {
    callback: EventCallback,
    user: usize,
    open: Arc<Mutex<bool>>,
}

impl Sink {
    pub fn new(callback: EventCallback, user: *mut c_void) -> Self {
        Self { callback, user: user as usize, open: Arc::new(Mutex::new(true)) }
    }

    pub fn close(&self) {
        *self.open.lock().unwrap_or_else(|e| e.into_inner()) = false;
    }

    pub fn message(&self, kind: i32, text: &str) {
        let open = self.open.lock().unwrap_or_else(|e| e.into_inner());
        if *open {
            let c = CString::new(text.replace('\0', " ")).unwrap_or_default();
            (self.callback)(self.user as *mut c_void, kind, c.as_ptr(), std::ptr::null_mut());
        }
    }

    /// Hands `payload` to the receiver (dropped here if the sink is closed).
    pub fn handle<T>(&self, kind: i32, payload: Box<T>) {
        let open = self.open.lock().unwrap_or_else(|e| e.into_inner());
        if *open {
            (self.callback)(self.user as *mut c_void, kind, std::ptr::null(), Box::into_raw(payload).cast());
        }
    }
}

/// Dereferences a handle or returns `$default` from the enclosing function.
macro_rules! handle_ref {
    ($ptr:expr, $default:expr) => {
        match unsafe { $ptr.as_ref() } {
            Some(r) => r,
            None => return $default,
        }
    };
}
pub(crate) use handle_ref;
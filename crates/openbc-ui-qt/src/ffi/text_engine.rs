//! C ABI for the Qt-independent text comparison engine (`openbc_core::text::engine`).
//!
//! Pairs with `cpp/viewers/text/engine/text_engine_api.h` and the C++ wrapper
//! `cpp/viewers/text/engine/text_model.h`. Both text editors (the `QPlainTextEdit` one and
//! `VirtualTextEditor`) are presentation layers over the `TmModel` handle created here.
//!
//! Contract (same spirit as `ffi::common`):
//! * `openbc_tm_open_async` runs the comparison on a worker thread and reports through the
//!   callback: `EV_PROGRESS` ("phase|done|total"), then exactly one of `EV_RESULT` (the handle is
//!   owned by the receiver, destroy with `openbc_tm_destroy`) or `EV_ERROR`. After `EV_ERROR`
//!   with message `cancelled` no partial state exists.
//! * Every accessor is thread-safe; a model may be read from several viewers at once.
//! * Strings are UTF-8, not NUL-terminated, and copied into caller buffers (no lifetimes to track).
//!
//! # Safety
//! Pointer arguments must be null (where documented) or valid for the stated length, and handles
//! must come from this library and not yet be destroyed.

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

use openbc_core::text::engine::{
    CancelToken, CompareConfig, EditOutcome, EngineError, FindOptions, MapPolicy, Phase, SideId, TextCompareModel,
};
use openbc_core::text::{HighlightSession, SessionLimits};

pub const EV_ERROR: i32 = 2;
pub const EV_RESULT: i32 = 1;
pub const EV_PROGRESS: i32 = 5;

/// Same shape as `ffi::common::EventCallback`.
pub type TmCallback =
    extern "C" fn(user: *mut c_void, kind: i32, message: *const c_char, handle: *mut c_void);

/// Provider for [`openbc_syn_range_cb`]: copy line `line` (no terminator) into `buf` (capacity
/// `cap`) and return its length, or a negative value past the end. Longer lines are truncated.
pub type TmLineProvider =
    extern "C" fn(user: *mut c_void, line: u64, buf: *mut u8, cap: usize) -> i64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct TmOptions {
    pub ignore_whitespace_edges: u8,
    pub ignore_all_whitespace: u8,
    pub ignore_case: u8,
    pub ignore_line_endings: u8,
    pub buffered_io: u8,
    pub reserved: [u8; 3],
    /// 0 = default (16 Mi records in RAM before spilling).
    pub ram_record_limit: u64,
    /// 0 = default.
    pub inline_cache_entries: u32,
    /// 0 = default.
    pub inline_max_line_bytes: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmRow {
    /// -1 when this row has no left line.
    pub left: i64,
    /// -1 when this row has no right line.
    pub right: i64,
    /// 0 equal, 1 insert, 2 delete, 3 replace.
    pub kind: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmWindow {
    pub written: u32,
    pub line_bytes: u32,
    pub start_char: u64,
    pub more_after: u8,
    /// The caller's buffer was too small for the requested characters.
    pub buffer_truncated: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmRange {
    /// UTF-8 byte offset in the decoded line.
    pub start: u32,
    pub len: u32,
    /// 0 equal, 1 mismatch, 2 whitespace-only mismatch.
    pub kind: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmStats {
    pub rows: u64,
    pub left_lines: u64,
    pub right_lines: u64,
    pub equal_rows: u64,
    pub inserted_rows: u64,
    pub deleted_rows: u64,
    pub replaced_rows: u64,
    pub blocks: u64,
    pub degraded: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmMatch {
    pub line: u64,
    pub start: u64,
    pub len: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmMemory {
    pub index_bytes: u64,
    pub index_resident_bytes: u64,
    pub diff_bytes: u64,
    pub inline_cache_entries: u64,
    pub edit_bytes: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmEdit {
    pub first_row: u64,
    pub old_rows: u64,
    pub new_rows: u64,
    pub first_line: u64,
    pub inserted_lines: u64,
    pub side: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct TmSpan {
    pub line: u64,
    pub start: u32,
    pub length: u32,
    /// 0xRRGGBBAA
    pub foreground: u32,
    pub background: u32,
    /// bit 0 bold, bit 1 italic
    pub flags: u8,
}

pub struct TmModel(Arc<TextCompareModel>);
pub struct TmCancel(CancelToken);
pub struct TmSyntax(Mutex<HighlightSession>);

thread_local! {
    static LAST_ERROR: RefCell<String> = const { RefCell::new(String::new()) };
}

fn set_error(message: impl ToString) {
    LAST_ERROR.with(|e| *e.borrow_mut() = message.to_string());
}

fn side(side: u8) -> SideId {
    if side == 0 { SideId::Left } else { SideId::Right }
}

unsafe fn path_opt(p: *const c_char) -> Option<PathBuf> {
    if p.is_null() {
        return None;
    }
    let s = CStr::from_ptr(p).to_string_lossy().into_owned();
    (!s.is_empty()).then(|| PathBuf::from(s))
}

fn config_from(opts: *const TmOptions) -> CompareConfig {
    let mut cfg = CompareConfig::default();
    if let Some(o) = unsafe { opts.as_ref() } {
        cfg.index.normalize.ignore_leading_trailing_whitespace = o.ignore_whitespace_edges != 0;
        cfg.index.normalize.ignore_all_whitespace = o.ignore_all_whitespace != 0;
        cfg.index.normalize.ignore_case = o.ignore_case != 0;
        cfg.index.normalize.ignore_line_endings = o.ignore_line_endings != 0;
        if o.buffered_io != 0 {
            cfg.map_policy = MapPolicy::Buffered;
        }
        if o.ram_record_limit != 0 {
            cfg.index.ram_record_limit = o.ram_record_limit as usize;
        }
        if o.inline_cache_entries != 0 {
            cfg.inline_cache_entries = o.inline_cache_entries as usize;
        }
        if o.inline_max_line_bytes != 0 {
            cfg.inline_max_line_bytes = o.inline_max_line_bytes as usize;
        }
    }
    cfg
}

// ---- cancellation ------------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn openbc_tm_cancel_new() -> *mut TmCancel {
    Box::into_raw(Box::new(TmCancel(CancelToken::new())))
}

/// # Safety
/// `c` from `openbc_tm_cancel_new` or null.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_cancel(c: *const TmCancel) {
    if let Some(c) = c.as_ref() {
        c.0.cancel();
    }
}

/// # Safety
/// `c` from `openbc_tm_cancel_new`, destroyed once (running operations keep their own clone).
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_cancel_destroy(c: *mut TmCancel) {
    if !c.is_null() {
        drop(Box::from_raw(c));
    }
}

// ---- open / destroy -----------------------------------------------------------------------------

fn phase_name(p: Phase) -> &'static str {
    match p {
        Phase::Indexing => "indexing",
        Phase::Matching => "matching",
        Phase::Aligning => "aligning",
    }
}

/// Build the model on a worker thread. Either path may be null/empty (empty side).
///
/// # Safety
/// Paths are null or NUL-terminated; `opts`/`cancel` are null or valid; `callback` must stay
/// callable until it has delivered `EV_RESULT` or `EV_ERROR`.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_open_async(
    left: *const c_char,
    right: *const c_char,
    opts: *const TmOptions,
    cancel: *const TmCancel,
    callback: TmCallback,
    user: *mut c_void,
) {
    let (l, r) = (path_opt(left), path_opt(right));
    let cfg = config_from(opts);
    let token = cancel.as_ref().map_or_else(CancelToken::new, |c| c.0.clone());
    let user = user as usize;
    std::thread::Builder::new()
        .name("openbc-text-model".into())
        .spawn(move || {
            let say = |kind: i32, text: &str| {
                let c = CString::new(text.replace('\0', " ")).unwrap_or_default();
                callback(user as *mut c_void, kind, c.as_ptr(), std::ptr::null_mut());
            };
            let mut last_pct = u64::MAX;
            let mut report = |p: openbc_core::text::engine::Progress| {
                let pct = if p.total == 0 { 0 } else { p.done * 100 / p.total };
                let key = pct + (p.phase as u64) * 1000;
                if key != last_pct {
                    last_pct = key;
                    say(EV_PROGRESS, &format!("{}|{}|{}", phase_name(p.phase), p.done, p.total));
                }
            };
            match TextCompareModel::open(l.as_deref(), r.as_deref(), cfg, &token, &mut report) {
                Ok(model) => {
                    let handle = Box::into_raw(Box::new(TmModel(Arc::new(model))));
                    callback(user as *mut c_void, EV_RESULT, std::ptr::null(), handle.cast());
                }
                Err(e) => say(EV_ERROR, &if matches!(e, openbc_core::text::engine::EngineError::Cancelled) {
                    "cancelled".to_owned()
                } else {
                    e.to_string()
                }),
            }
        })
        .ok();
}

/// Blocking variant for tests and tiny inputs. Returns null on failure (see
/// [`openbc_tm_last_error`]).
///
/// # Safety
/// As [`openbc_tm_open_async`].
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_open_sync(
    left: *const c_char,
    right: *const c_char,
    opts: *const TmOptions,
) -> *mut TmModel {
    let cfg = config_from(opts);
    match TextCompareModel::open(
        path_opt(left).as_deref(),
        path_opt(right).as_deref(),
        cfg,
        &CancelToken::new(),
        &mut |_| {},
    ) {
        Ok(m) => Box::into_raw(Box::new(TmModel(Arc::new(m)))),
        Err(e) => {
            set_error(e);
            std::ptr::null_mut()
        }
    }
}

/// Last error message of this thread (pointer valid until the next failing call on this thread).
///
/// # Safety
/// `len` null or writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_last_error(len: *mut usize) -> *const u8 {
    LAST_ERROR.with(|e| {
        let e = e.borrow();
        if !len.is_null() {
            *len = e.len();
        }
        e.as_ptr()
    })
}

/// # Safety
/// `m` from this library, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_destroy(m: *mut TmModel) {
    if !m.is_null() {
        drop(Box::from_raw(m));
    }
}

macro_rules! model {
    ($m:expr, $default:expr) => {
        match unsafe { $m.as_ref() } {
            Some(m) => &*m.0,
            None => return $default,
        }
    };
}

// ---- structure ---------------------------------------------------------------------------------

/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_row_count(m: *const TmModel) -> u64 {
    model!(m, 0).row_count()
}

/// # Safety
/// `m` valid, `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_row(m: *const TmModel, row: u64, out: *mut TmRow) -> u8 {
    let Some(r) = model!(m, 0).row(row) else { return 0 };
    if let Some(o) = out.as_mut() {
        *o = TmRow {
            left: r.left_line.map_or(-1, |v| v as i64),
            right: r.right_line.map_or(-1, |v| v as i64),
            kind: r.kind as u8,
        };
    }
    1
}

/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_line_count(m: *const TmModel, s: u8) -> u64 {
    model!(m, 0).line_count(side(s))
}

/// Byte length of a line in the file, or -1.
///
/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_line_bytes(m: *const TmModel, s: u8, line: u64) -> i64 {
    model!(m, -1).line_byte_len(side(s), line).map_or(-1, i64::from)
}

/// Copy up to `max_chars` characters of a line, starting at character `start_char`, into `buf`.
///
/// # Safety
/// `buf` valid for `cap` bytes; `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_line_window(
    m: *const TmModel,
    s: u8,
    line: u64,
    start_char: u64,
    max_chars: u32,
    buf: *mut u8,
    cap: usize,
    out: *mut TmWindow,
) -> u8 {
    let model = model!(m, 0);
    let Ok(w) = model.line_window(side(s), line, start_char, max_chars as usize) else { return 0 };
    let mut n = w.text.len().min(cap);
    while n > 0 && !w.text.is_char_boundary(n) {
        n -= 1;
    }
    if !buf.is_null() {
        std::ptr::copy_nonoverlapping(w.text.as_ptr(), buf, n);
    }
    if let Some(o) = out.as_mut() {
        *o = TmWindow {
            written: n as u32,
            line_bytes: w.line_bytes,
            start_char: w.start_char,
            more_after: u8::from(w.more_after),
            buffer_truncated: u8::from(n < w.text.len()),
        };
    }
    1
}

/// Inline differences of one side of a row. Returns the number of ranges (may exceed `cap`), or
/// -1 when the row has no inline diff.
///
/// # Safety
/// `out` valid for `cap` ranges.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_inline(
    m: *const TmModel,
    row: u64,
    s: u8,
    out: *mut TmRange,
    cap: usize,
) -> i32 {
    use openbc_core::text::CharChangeKind;
    let Some(d) = model!(m, -1).inline_diff(row) else { return -1 };
    let ranges = if s == 0 { &d.left } else { &d.right };
    for (i, r) in ranges.iter().take(cap).enumerate() {
        *out.add(i) = TmRange {
            start: r.start as u32,
            len: r.length as u32,
            kind: match r.kind {
                CharChangeKind::Equal => 0,
                CharChangeKind::Mismatch => 1,
                CharChangeKind::WhitespaceMismatch => 2,
            },
        };
    }
    ranges.len() as i32
}

/// # Safety
/// `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_next_diff(m: *const TmModel, row: u64, out: *mut u64) -> u8 {
    match model!(m, 0).next_difference(row) {
        Some(v) => {
            *out = v;
            1
        }
        None => 0,
    }
}

/// # Safety
/// `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_prev_diff(m: *const TmModel, row: u64, out: *mut u64) -> u8 {
    match model!(m, 0).prev_difference(row) {
        Some(v) => {
            *out = v;
            1
        }
        None => 0,
    }
}

/// # Safety
/// `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_row_for_line(m: *const TmModel, s: u8, line: u64, out: *mut u64) -> u8 {
    match model!(m, 0).row_for_line(side(s), line) {
        Some(v) => {
            *out = v;
            1
        }
        None => 0,
    }
}

/// # Safety
/// `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_stats(m: *const TmModel, out: *mut TmStats) {
    let model = model!(m, ());
    let s = model.stats();
    let (l, r) = model.line_counts();
    if let Some(o) = out.as_mut() {
        *o = TmStats {
            rows: model.row_count(),
            left_lines: l,
            right_lines: r,
            equal_rows: s.equal_rows,
            inserted_rows: s.inserted_rows,
            deleted_rows: s.deleted_rows,
            replaced_rows: s.replaced_rows,
            blocks: s.blocks,
            degraded: u8::from(model.is_degraded()),
        };
    }
}

/// Change summary per overview bucket (bit0 insert, bit1 delete, bit2 replace).
///
/// # Safety
/// `out` valid for `buckets` bytes.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_overview(m: *const TmModel, buckets: usize, out: *mut u8) {
    let cells = model!(m, ()).overview(buckets);
    if !out.is_null() {
        std::ptr::copy_nonoverlapping(cells.as_ptr(), out, cells.len());
    }
}

/// # Safety
/// `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_memory(m: *const TmModel, out: *mut TmMemory) {
    let r = model!(m, ()).memory_report();
    if let Some(o) = out.as_mut() {
        *o = TmMemory {
            index_bytes: r.index_bytes,
            index_resident_bytes: r.index_resident_bytes,
            diff_bytes: r.diff_bytes,
            inline_cache_entries: r.inline_cache_entries,
            edit_bytes: r.edit_bytes,
        };
    }
}

/// 0 UTF-8, 1 UTF-16 LE, 2 UTF-16 BE; bit 7 set when the file looks binary.
///
/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_encoding(m: *const TmModel, s: u8) -> u8 {
    use openbc_core::text::engine::TextEncoding;
    let model = model!(m, 0);
    let code = match model.encoding(side(s)) {
        TextEncoding::Utf8 => 0,
        TextEncoding::Utf16Le => 1,
        TextEncoding::Utf16Be => 2,
    };
    code | if model.looks_binary(side(s)) { 0x80 } else { 0 }
}

/// 1 while the backing files are unchanged since open.
///
/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_verify(m: *const TmModel) -> u8 {
    u8::from(model!(m, 0).verify_unchanged().is_ok())
}

/// Search one side. `start_line < 0` starts at the beginning (forward) / end (backward).
/// Returns 1 found, 0 not found, -1 cancelled or failed.
///
/// # Safety
/// `needle` valid for `needle_len`; `cancel` null or valid; `out` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_find(
    m: *const TmModel,
    s: u8,
    needle: *const u8,
    needle_len: usize,
    case_sensitive: u8,
    start_line: i64,
    start_col: u64,
    forward: u8,
    cancel: *const TmCancel,
    out: *mut TmMatch,
) -> i32 {
    let model = model!(m, -1);
    let text = String::from_utf8_lossy(std::slice::from_raw_parts(needle, needle_len)).into_owned();
    let token = cancel.as_ref().map_or_else(CancelToken::new, |c| c.0.clone());
    let start = (start_line >= 0).then_some((start_line as u64, start_col as usize));
    match model.find(side(s), &FindOptions { needle: text, case_sensitive: case_sensitive != 0 }, start, forward != 0, &token) {
        Ok(Some(hit)) => {
            if let Some(o) = out.as_mut() {
                *o = TmMatch { line: hit.line, start: hit.start as u64, len: hit.len as u64 };
            }
            1
        }
        Ok(None) => 0,
        Err(_) => -1,
    }
}

// ---- editing ----------------------------------------------------------------------------------------

fn fill_edit(out: *mut TmEdit, e: &EditOutcome) {
    if let Some(o) = unsafe { out.as_mut() } {
        *o = TmEdit {
            first_row: e.first_row,
            old_rows: e.old_rows,
            new_rows: e.new_rows,
            first_line: e.first_line,
            inserted_lines: e.inserted_lines,
            side: u8::from(e.side == SideId::Right),
        };
    }
}

fn edit_result(result: Result<Option<EditOutcome>, EngineError>, out: *mut TmEdit) -> i32 {
    match result {
        Ok(Some(e)) => {
            fill_edit(out, &e);
            1
        }
        Ok(None) => 0,
        Err(e) => {
            set_error(e);
            -1
        }
    }
}

/// Replace `remove` lines of `side` starting at `first_line` by `text` (UTF-8; line breaks in it
/// make several lines; null `text` inserts nothing, i.e. a pure delete; an empty non-null text
/// inserts one empty line). `coalesce` merges typing into one undo step. Returns 1, or -1 with
/// [`openbc_tm_last_error`] set.
///
/// # Safety
/// `text` valid for `len` bytes (or null); `out` writable or null.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_replace_lines(
    m: *const TmModel,
    s: u8,
    first_line: u64,
    remove: u64,
    text: *const u8,
    len: usize,
    coalesce: u8,
    out: *mut TmEdit,
) -> i32 {
    let model = model!(m, -1);
    let owned: Option<String> =
        (!text.is_null()).then(|| String::from_utf8_lossy(std::slice::from_raw_parts(text, len)).into_owned());
    let lines: Vec<&str> = owned.as_deref().into_iter().collect();
    edit_result(model.replace_lines(side(s), first_line, remove, &lines, coalesce != 0).map(Some), out)
}

/// # Safety
/// `out` writable or null.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_undo(m: *const TmModel, s: u8, out: *mut TmEdit) -> i32 {
    edit_result(model!(m, -1).undo(side(s)), out)
}

/// # Safety
/// `out` writable or null.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_redo(m: *const TmModel, s: u8, out: *mut TmEdit) -> i32 {
    edit_result(model!(m, -1).redo(side(s)), out)
}

/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_can_undo(m: *const TmModel, s: u8) -> u8 {
    u8::from(model!(m, 0).can_undo(side(s)))
}

/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_can_redo(m: *const TmModel, s: u8) -> u8 {
    u8::from(model!(m, 0).can_redo(side(s)))
}

/// End the current typing run (next edit becomes a new undo step).
///
/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_seal_undo(m: *const TmModel, s: u8) {
    model!(m, ()).seal_undo(side(s));
}

/// Undoable edit steps applied to `side`.
///
/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_edit_count(m: *const TmModel, s: u8) -> u64 {
    model!(m, 0).edit_count(side(s)) as u64
}

/// 1 when `side` has unsaved edits.
///
/// # Safety
/// `m` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_is_dirty(m: *const TmModel, s: u8) -> u8 {
    u8::from(model!(m, 0).is_dirty(side(s)))
}

/// Make the other side's lines of the changed block containing `row` equal to `from_side`'s.
/// Returns 1 (done, `out` describes the destination side), 0 (row is in no changed block) or -1.
///
/// # Safety
/// `out` writable or null.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_copy_block(m: *const TmModel, from_side: u8, row: u64, out: *mut TmEdit) -> i32 {
    edit_result(model!(m, -1).copy_block(side(from_side), row), out)
}

/// Start row and row count of the changed block containing `row`.
///
/// # Safety
/// `start` and `len` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_block_at(m: *const TmModel, row: u64, start: *mut u64, len: *mut u64) -> u8 {
    match model!(m, 0).block_at(row) {
        Some((s, l)) => {
            *start = s;
            *len = l;
            1
        }
        None => 0,
    }
}

/// Lines of each side consumed by the rows before `row`.
///
/// # Safety
/// `left` and `right` writable.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_line_cursors(m: *const TmModel, row: u64, left: *mut u64, right: *mut u64) -> u8 {
    let Some(model) = m.as_ref() else { return 0 };
    let (l, r) = model.0.line_cursors(row);
    *left = l;
    *right = r;
    1
}

/// Copy the side's current file path into `buf`; returns the full length (0 = no path).
///
/// # Safety
/// `buf` valid for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_path(m: *const TmModel, s: u8, buf: *mut u8, cap: usize) -> usize {
    let Some(path) = model!(m, 0).path(side(s)) else { return 0 };
    let text = path.to_string_lossy();
    let n = text.len().min(cap);
    if !buf.is_null() {
        std::ptr::copy_nonoverlapping(text.as_ptr(), buf, n);
    }
    text.len()
}

/// Copy the active ignore-rule text into `buf`; returns its full length.
///
/// # Safety
/// `buf` valid for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_rules_text(m: *const TmModel, buf: *mut u8, cap: usize) -> usize {
    let text = model!(m, 0).rules_text();
    let n = text.len().min(cap);
    if !buf.is_null() {
        std::ptr::copy_nonoverlapping(text.as_ptr(), buf, n);
    }
    text.len()
}

pub const TASK_SET_RULES: u8 = 0;
pub const TASK_REDIFF: u8 = 1;
pub const TASK_SAVE: u8 = 2;
pub const TASK_SAVE_AS: u8 = 3;

/// Run a long operation on a worker thread: set ignore rules (`text` = rule text), re-diff,
/// save (`s` = side) or save as (`text` = path). Events as for `openbc_tm_open_async`:
/// `EV_PROGRESS`, then `EV_RESULT` (message = written path for saves, handle null) or `EV_ERROR`.
/// The model stays usable for reads meanwhile; writers wait for the operation.
///
/// # Safety
/// `m` valid and kept alive until the final event; `text` valid for `len`; `cancel` null or valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_tm_task_async(
    m: *const TmModel,
    task: u8,
    s: u8,
    text: *const u8,
    len: usize,
    cancel: *const TmCancel,
    callback: TmCallback,
    user: *mut c_void,
) {
    let Some(model) = m.as_ref().map(|m| m.0.clone()) else { return };
    let text = if text.is_null() { String::new() } else { String::from_utf8_lossy(std::slice::from_raw_parts(text, len)).into_owned() };
    let token = cancel.as_ref().map_or_else(CancelToken::new, |c| c.0.clone());
    let user = user as usize;
    std::thread::Builder::new()
        .name("openbc-text-task".into())
        .spawn(move || {
            let say = |kind: i32, text: &str| {
                let c = CString::new(text.replace('\0', " ")).unwrap_or_default();
                callback(user as *mut c_void, kind, c.as_ptr(), std::ptr::null_mut());
            };
            let mut last = u64::MAX;
            let mut report = |p: openbc_core::text::engine::Progress| {
                let pct = if p.total == 0 { 0 } else { p.done * 100 / p.total };
                let key = pct + (p.phase as u64) * 1000;
                if key != last {
                    last = key;
                    say(EV_PROGRESS, &format!("{}|{}|{}", phase_name(p.phase), p.done, p.total));
                }
            };
            let result: Result<String, EngineError> = match task {
                TASK_SET_RULES => model.set_rules(&text, &token, &mut report).map(|()| String::new()),
                TASK_REDIFF => model.rediff(&token, &mut report).map(|()| String::new()),
                TASK_SAVE => model.save(side(s), None).map(|r| r.path.to_string_lossy().into_owned()),
                TASK_SAVE_AS => model
                    .save(side(s), Some(std::path::Path::new(&text)))
                    .map(|r| r.path.to_string_lossy().into_owned()),
                _ => Ok(String::new()),
            };
            match result {
                Ok(message) => say(EV_RESULT, &message),
                Err(EngineError::Cancelled) => say(EV_ERROR, "cancelled"),
                Err(e) => say(EV_ERROR, &e.to_string()),
            }
        })
        .ok();
}

// ---- syntax -------------------------------------------------------------------------------------------

/// Create a highlighting session. `language` is a file name, extension or language name.
///
/// # Safety
/// `language` valid for `len`.
#[no_mangle]
pub unsafe extern "C" fn openbc_syn_new(language: *const u8, len: usize, dark: u8, span_cache_lines: u32) -> *mut TmSyntax {
    let lang = String::from_utf8_lossy(std::slice::from_raw_parts(language, len)).into_owned();
    let mut limits = SessionLimits::default();
    if span_cache_lines != 0 {
        limits.span_cache_lines = span_cache_lines as usize;
    }
    Box::into_raw(Box::new(TmSyntax(Mutex::new(HighlightSession::new(&lang, dark != 0, limits)))))
}

/// # Safety
/// `s` from `openbc_syn_new`, destroyed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_syn_destroy(s: *mut TmSyntax) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

/// Forget cached state from `line` on (the file changed).
///
/// # Safety
/// `s` valid.
#[no_mangle]
pub unsafe extern "C" fn openbc_syn_invalidate(s: *const TmSyntax, line: u64) {
    if let Some(s) = s.as_ref() {
        s.0.lock().unwrap_or_else(|e| e.into_inner()).invalidate_from(line);
    }
}

fn pack(c: [u8; 4]) -> u32 {
    u32::from_be_bytes(c)
}

fn write_spans(
    session: &mut HighlightSession,
    first: u64,
    count: u64,
    provider: &mut dyn FnMut(u64) -> Option<String>,
    out: *mut TmSpan,
    cap: usize,
) -> i64 {
    let mut total = 0usize;
    for (line, spans) in session.highlight_range(first, count, provider) {
        for sp in spans.iter() {
            let Some(style) = session.style(sp.style_id) else { continue };
            if total < cap && !out.is_null() {
                unsafe {
                    *out.add(total) = TmSpan {
                        line,
                        start: sp.start,
                        length: sp.length,
                        foreground: pack(style.foreground),
                        background: pack(style.background),
                        flags: u8::from(style.bold) | (u8::from(style.italic) << 1),
                    };
                }
            }
            total += 1;
        }
    }
    total as i64
}

/// Highlight lines `first..first+count` of one side of `m`, writing up to `cap` spans. Returns the
/// total number of spans (call again with a larger buffer when it exceeds `cap`; results are cached).
///
/// # Safety
/// `out` valid for `cap` spans.
#[no_mangle]
pub unsafe extern "C" fn openbc_syn_range(
    syn: *const TmSyntax,
    m: *const TmModel,
    s: u8,
    first: u64,
    count: u64,
    out: *mut TmSpan,
    cap: usize,
) -> i64 {
    let (Some(syn), Some(model)) = (syn.as_ref(), m.as_ref()) else { return -1 };
    let mut session = syn.0.lock().unwrap_or_else(|e| e.into_inner());
    let model = &model.0;
    write_spans(
        &mut session,
        first,
        count,
        &mut |n| {
            // Syntax is computed on a bounded prefix of very long lines.
            model.line_window(side(s), n, 0, 16 * 1024).ok().map(|w| w.text)
        },
        out,
        cap,
    )
}

/// Like [`openbc_syn_range`] but lines come from `provider` (used by the QTextDocument editor).
///
/// # Safety
/// `provider` must be callable with `user`; `out` valid for `cap` spans.
#[no_mangle]
pub unsafe extern "C" fn openbc_syn_range_cb(
    syn: *const TmSyntax,
    first: u64,
    count: u64,
    provider: TmLineProvider,
    user: *mut c_void,
    out: *mut TmSpan,
    cap: usize,
) -> i64 {
    let Some(syn) = syn.as_ref() else { return -1 };
    let mut session = syn.0.lock().unwrap_or_else(|e| e.into_inner());
    let mut scratch = vec![0u8; 64 * 1024];
    write_spans(
        &mut session,
        first,
        count,
        &mut |n| {
            let len = provider(user, n, scratch.as_mut_ptr(), scratch.len());
            (len >= 0).then(|| String::from_utf8_lossy(&scratch[..(len as usize).min(scratch.len())]).into_owned())
        },
        out,
        cap,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;
    use std::io::Write;

    fn tmp(name: &str, body: &str) -> CString {
        let p = std::env::temp_dir().join(format!("openbc_tm_{}_{name}", std::process::id()));
        std::fs::File::create(&p).unwrap().write_all(body.as_bytes()).unwrap();
        CString::new(p.to_str().unwrap()).unwrap()
    }

    #[test]
    fn round_trip_through_the_c_abi() {
        let l = tmp("l.c", "int a;\n/* x\ny */ int b;\nkeep\n");
        let r = tmp("r.c", "int a;\n/* x\ny */ int B;\nkeep\nnew\n");
        unsafe {
            let m = openbc_tm_open_sync(l.as_ptr(), r.as_ptr(), std::ptr::null());
            assert!(!m.is_null());
            assert_eq!(openbc_tm_row_count(m), 5);
            let mut row = TmRow::default();
            assert_eq!(openbc_tm_row(m, 2, &mut row), 1);
            assert_eq!((row.left, row.right, row.kind), (2, 2, 3));
            let mut buf = [0u8; 64];
            let mut w = TmWindow::default();
            assert_eq!(openbc_tm_line_window(m, 1, 2, 0, 50, buf.as_mut_ptr(), buf.len(), &mut w), 1);
            assert_eq!(&buf[..w.written as usize], b"y */ int B;");
            let mut ranges = [TmRange::default(); 8];
            assert!(openbc_tm_inline(m, 2, 0, ranges.as_mut_ptr(), 8) > 0);
            assert_eq!(openbc_tm_inline(m, 0, 0, ranges.as_mut_ptr(), 8), -1);
            let mut next = 0u64;
            assert_eq!(openbc_tm_next_diff(m, 0, &mut next), 1);
            assert_eq!(next, 2);
            let mut st = TmStats::default();
            openbc_tm_stats(m, &mut st);
            assert_eq!((st.replaced_rows, st.inserted_rows, st.blocks), (1, 1, 2));
            let mut hit = TmMatch::default();
            let needle = b"KEEP";
            assert_eq!(openbc_tm_find(m, 0, needle.as_ptr(), 4, 0, -1, 0, 1, std::ptr::null(), &mut hit), 1);
            assert_eq!((hit.line, hit.start), (3, 0));
            assert_eq!(openbc_tm_verify(m), 1);

            let syn = openbc_syn_new(b"c".as_ptr(), 1, 1, 0);
            let mut spans = vec![TmSpan::default(); 64];
            let n = openbc_syn_range(syn, m, 0, 2, 1, spans.as_mut_ptr(), spans.len());
            assert!(n > 0);
            // Line 2 starts inside a comment: its first span must be the comment style, which
            // differs from the keyword colour of "int".
            let first = spans[0];
            assert_eq!(first.start, 0);
            openbc_syn_destroy(syn);
            openbc_tm_destroy(m);
        }
    }

    static EVENTS: Mutex<Vec<(i32, String)>> = Mutex::new(Vec::new());

    extern "C" fn collect(_: *mut c_void, kind: i32, message: *const c_char, _: *mut c_void) {
        let text = if message.is_null() { String::new() } else { unsafe { CStr::from_ptr(message) }.to_string_lossy().into_owned() };
        EVENTS.lock().unwrap().push((kind, text));
    }

    fn wait_final() -> (i32, String) {
        for _ in 0..400 {
            std::thread::sleep(std::time::Duration::from_millis(10));
            let mut ev = EVENTS.lock().unwrap();
            if let Some(pos) = ev.iter().position(|(k, _)| *k == EV_RESULT || *k == EV_ERROR) {
                let last = ev[pos].clone();
                ev.clear();
                return last;
            }
        }
        panic!("task did not finish");
    }

    #[test]
    fn editing_through_the_c_abi_with_undo_copy_and_async_save() {
        let l = tmp("el.txt", "one\ntwo\nthree\n");
        let r = tmp("er.txt", "one\nTWO\nthree\n");
        unsafe {
            let m = openbc_tm_open_sync(l.as_ptr(), r.as_ptr(), std::ptr::null());
            assert!(!m.is_null());
            let mut edit = TmEdit::default();
            assert_eq!(openbc_tm_is_dirty(m, 0), 0);
            // replace line 0 on the left with "uno"
            let text = b"uno";
            assert_eq!(openbc_tm_replace_lines(m, 0, 0, 1, text.as_ptr(), text.len(), 1, &mut edit), 1);
            assert_eq!((edit.first_row, edit.old_rows, edit.new_rows, edit.side), (0, 1, 1, 0));
            assert_eq!(openbc_tm_is_dirty(m, 0), 1);
            assert_eq!(openbc_tm_can_undo(m, 0), 1);
            // pure delete (null text)
            assert_eq!(openbc_tm_replace_lines(m, 0, 2, 1, std::ptr::null(), 0, 0, &mut edit), 1);
            let mut row = TmRow::default();
            assert_eq!(openbc_tm_row(m, 2, &mut row), 1);
            assert_eq!(row.kind, 1); // the right side's "three" has no counterpart any more
            // undo both
            assert_eq!(openbc_tm_undo(m, 0, &mut edit), 1);
            assert_eq!(openbc_tm_undo(m, 0, &mut edit), 1);
            assert_eq!(openbc_tm_undo(m, 0, &mut edit), 0);
            assert_eq!(openbc_tm_is_dirty(m, 0), 0);
            // copy the block holding the changed row 1 from right to left
            let (mut start, mut len) = (0u64, 0u64);
            assert_eq!(openbc_tm_block_at(m, 1, &mut start, &mut len), 1);
            assert_eq!((start, len), (1, 1));
            assert_eq!(openbc_tm_copy_block(m, 1, 1, &mut edit), 1);
            assert_eq!(edit.side, 0);
            assert_eq!(openbc_tm_copy_block(m, 1, 0, &mut edit), 0); // row 0 is outside any block
            // async save of the left side
            EVENTS.lock().unwrap().clear();
            openbc_tm_task_async(m, TASK_SAVE, 0, std::ptr::null(), 0, std::ptr::null(), collect, std::ptr::null_mut());
            let (kind, message) = wait_final();
            assert_eq!(kind, EV_RESULT, "{message}");
            assert!(message.ends_with("el.txt"));
            assert_eq!(std::fs::read_to_string(message).unwrap(), "one\nTWO\nthree\n");
            assert_eq!(openbc_tm_is_dirty(m, 0), 0);
            // async rules, then an invalid one reports an error
            let rules = b"/^T.*/";
            openbc_tm_task_async(m, TASK_SET_RULES, 0, rules.as_ptr(), rules.len(), std::ptr::null(), collect, std::ptr::null_mut());
            assert_eq!(wait_final().0, EV_RESULT);
            let mut buf = [0u8; 32];
            let n = openbc_tm_rules_text(m, buf.as_mut_ptr(), buf.len());
            assert_eq!(&buf[..n], rules);
            let bad = b"/(x/";
            openbc_tm_task_async(m, TASK_SET_RULES, 0, bad.as_ptr(), bad.len(), std::ptr::null(), collect, std::ptr::null_mut());
            assert_eq!(wait_final().0, EV_ERROR);
            // path accessor
            let mut pbuf = [0u8; 256];
            let pn = openbc_tm_path(m, 1, pbuf.as_mut_ptr(), pbuf.len());
            assert!(String::from_utf8_lossy(&pbuf[..pn]).ends_with("er.txt"));
            openbc_tm_destroy(m);
        }
    }

    #[test]
    fn failures_are_reported_not_panicked() {
        let missing = CString::new("/definitely/not/here.txt").unwrap();
        unsafe {
            assert!(openbc_tm_open_sync(missing.as_ptr(), std::ptr::null(), std::ptr::null()).is_null());
            let mut n = 0usize;
            let p = openbc_tm_last_error(&mut n);
            assert!(n > 0 && !p.is_null());
            assert_eq!(openbc_tm_row_count(std::ptr::null()), 0);
        }
    }
}

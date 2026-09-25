use openbc_core::text::highlight_to_html as render_highlight_to_html;
use openbc_core::text::{
    compute_inline_diff, highlight_to_spans, ChangeKind, CompareOptions, HighlightSpan, InlineDiff,
    LineDiff,
};

#[cfg(feature = "qt")]
mod highlight_bridge;

pub struct DiffHandle {
    rows: Vec<LineDiff>,
}

pub struct InlineHandle {
    diff: InlineDiff,
}

pub struct HighlightHandle {
    spans: Vec<HighlightSpan>,
}

fn trace_backend(message: impl std::fmt::Display) {
    openbc_observability::log(
        "openbc-ui-qt",
        "trace_backend",
        "INFO",
        &message.to_string(),
    );
}

#[no_mangle]
pub extern "C" fn openbc_initialize_observability() -> i32 {
    match openbc_observability::init() {
        Ok(()) => 0,
        Err(_) => 1,
    }
}

#[no_mangle]
pub extern "C" fn openbc_log_message(message: *const u8, length: usize) {
    let message = unsafe { input_text(message, length) };
    openbc_observability::log("openbc-ui-qt", "qt_action", "INFO", &message);
}

unsafe fn input_text(pointer: *const u8, length: usize) -> String {
    if pointer.is_null() || length == 0 {
        return String::new();
    }
    String::from_utf8_lossy(std::slice::from_raw_parts(pointer, length)).into_owned()
}

#[no_mangle]
pub extern "C" fn openbc_compare_buffers(
    left: *const u8,
    left_length: usize,
    right: *const u8,
    right_length: usize,
    ignore_whitespace: u8,
    ignore_timestamps: u8,
    ignore_container_ids: u8,
    fuzzy_threshold: f64,
) -> *mut DiffHandle {
    trace_backend(format_args!(
        "compare start left_bytes={left_length} right_bytes={right_length}"
    ));
    let left = unsafe { input_text(left, left_length) };
    let right = unsafe { input_text(right, right_length) };
    let options = CompareOptions {
        ignore_whitespace: ignore_whitespace != 0,
        ignore_timestamps: ignore_timestamps != 0,
        ignore_container_ids: ignore_container_ids != 0,
        fuzzy_threshold,
    };
    let rows = openbc_core::text::TextCompareEngine::compare_buffers(&left, &right, &options);
    trace_backend(format_args!("compare complete rows={}", rows.len()));
    Box::into_raw(Box::new(DiffHandle { rows }))
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_destroy(handle: *mut DiffHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_len(handle: *const DiffHandle) -> usize {
    handle.as_ref().map_or(0, |value| value.rows.len())
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_kind(handle: *const DiffHandle, index: usize) -> u8 {
    let Some(row) = handle.as_ref().and_then(|value| value.rows.get(index)) else {
        return 0;
    };
    match row.kind {
        ChangeKind::Unchanged => 0,
        ChangeKind::Added => 1,
        ChangeKind::Deleted => 2,
        ChangeKind::Modified { .. } => 3,
    }
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_similarity(handle: *const DiffHandle, index: usize) -> u8 {
    let Some(row) = handle.as_ref().and_then(|value| value.rows.get(index)) else {
        return 0;
    };
    match row.kind {
        ChangeKind::Modified { similarity_score } => similarity_score,
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_left_line(handle: *const DiffHandle, index: usize) -> usize {
    handle
        .as_ref()
        .and_then(|value| value.rows.get(index))
        .and_then(|row| row.left_line_num)
        .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_right_line(handle: *const DiffHandle, index: usize) -> usize {
    handle
        .as_ref()
        .and_then(|value| value.rows.get(index))
        .and_then(|row| row.right_line_num)
        .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_left_text(
    handle: *const DiffHandle,
    index: usize,
    length: *mut usize,
) -> *const u8 {
    let Some(row) = handle.as_ref().and_then(|value| value.rows.get(index)) else {
        return std::ptr::null();
    };
    *length = row.left_text.len();
    row.left_text.as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn openbc_diff_right_text(
    handle: *const DiffHandle,
    index: usize,
    length: *mut usize,
) -> *const u8 {
    let Some(row) = handle.as_ref().and_then(|value| value.rows.get(index)) else {
        return std::ptr::null();
    };
    *length = row.right_text.len();
    row.right_text.as_ptr()
}

#[no_mangle]
pub extern "C" fn openbc_inline_diff(
    left: *const u8,
    left_length: usize,
    right: *const u8,
    right_length: usize,
) -> *mut InlineHandle {
    trace_backend(format_args!(
        "inline start left_bytes={left_length} right_bytes={right_length}"
    ));
    let left = unsafe { input_text(left, left_length) };
    let right = unsafe { input_text(right, right_length) };
    let diff = compute_inline_diff(&left, &right);
    trace_backend(format_args!(
        "inline complete left_spans={} right_spans={}",
        diff.left.len(),
        diff.right.len()
    ));
    Box::into_raw(Box::new(InlineHandle { diff }))
}

#[no_mangle]
pub unsafe extern "C" fn openbc_inline_destroy(handle: *mut InlineHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

#[no_mangle]
pub unsafe extern "C" fn openbc_inline_len(handle: *const InlineHandle, side: u8) -> usize {
    handle.as_ref().map_or(0, |value| {
        if side == 0 {
            value.diff.left.len()
        } else {
            value.diff.right.len()
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn openbc_inline_start(
    handle: *const InlineHandle,
    side: u8,
    index: usize,
) -> usize {
    handle
        .as_ref()
        .and_then(|value| {
            if side == 0 {
                value.diff.left.get(index)
            } else {
                value.diff.right.get(index)
            }
        })
        .map_or(0, |span| span.start)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_inline_length(
    handle: *const InlineHandle,
    side: u8,
    index: usize,
) -> usize {
    handle
        .as_ref()
        .and_then(|value| {
            if side == 0 {
                value.diff.left.get(index)
            } else {
                value.diff.right.get(index)
            }
        })
        .map_or(0, |span| span.length)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_inline_kind(
    handle: *const InlineHandle,
    side: u8,
    index: usize,
) -> u8 {
    handle
        .as_ref()
        .and_then(|value| {
            if side == 0 {
                value.diff.left.get(index)
            } else {
                value.diff.right.get(index)
            }
        })
        .map_or(0, |span| match span.kind {
            openbc_core::text::CharChangeKind::Equal => 0,
            openbc_core::text::CharChangeKind::Mismatch => 1,
            openbc_core::text::CharChangeKind::WhitespaceMismatch => 2,
        })
}

#[no_mangle]
pub extern "C" fn openbc_highlight_buffer(
    extension: *const u8,
    extension_length: usize,
    source: *const u8,
    source_length: usize,
) -> *mut HighlightHandle {
    let extension = unsafe { input_text(extension, extension_length) };
    let source = unsafe { input_text(source, source_length) };
    Box::into_raw(Box::new(HighlightHandle {
        spans: highlight_to_spans(&source, &extension, true),
    }))
}

#[no_mangle]
pub extern "C" fn openbc_highlight_buffer_with_theme(
    extension: *const u8,
    extension_length: usize,
    source: *const u8,
    source_length: usize,
    theme: u8,
) -> *mut HighlightHandle {
    let extension = unsafe { input_text(extension, extension_length) };
    let source = unsafe { input_text(source, source_length) };
    Box::into_raw(Box::new(HighlightHandle {
        spans: highlight_to_spans(&source, &extension, theme != 0),
    }))
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_destroy(handle: *mut HighlightHandle) {
    if !handle.is_null() {
        drop(Box::from_raw(handle));
    }
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_len(handle: *const HighlightHandle) -> usize {
    handle.as_ref().map_or(0, |value| value.spans.len())
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_line(
    handle: *const HighlightHandle,
    index: usize,
) -> usize {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .map_or(0, |span| span.line)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_start(
    handle: *const HighlightHandle,
    index: usize,
) -> usize {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .map_or(0, |span| span.start)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_length(
    handle: *const HighlightHandle,
    index: usize,
) -> usize {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .map_or(0, |span| span.length)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_foreground(
    handle: *const HighlightHandle,
    index: usize,
    channel: u8,
) -> u8 {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .and_then(|span| span.foreground.get(channel as usize))
        .copied()
        .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_background(
    handle: *const HighlightHandle,
    index: usize,
    channel: u8,
) -> u8 {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .and_then(|span| span.background.get(channel as usize))
        .copied()
        .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_bold(handle: *const HighlightHandle, index: usize) -> u8 {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .map_or(0, |span| span.bold as u8)
}

#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_italic(
    handle: *const HighlightHandle,
    index: usize,
) -> u8 {
    handle
        .as_ref()
        .and_then(|value| value.spans.get(index))
        .map_or(0, |span| span.italic as u8)
}

/// C-compatible HTML highlighting entry point for the Qt frontend.
#[no_mangle]
pub extern "C" fn openbc_highlight_to_html(
    source: *const u8,
    source_length: usize,
    language: *const u8,
    language_length: usize,
    is_dark_mode: u8,
) -> *mut std::ffi::c_char {
    let source = unsafe { input_text(source, source_length) };
    let language = unsafe { input_text(language, language_length) };
    let html = render_highlight_to_html(&source, &language, is_dark_mode != 0);
    std::ffi::CString::new(html)
        .expect("highlighted HTML cannot contain NUL bytes")
        .into_raw()
}

/// Free a string returned by [`openbc_highlight_to_html`].
#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_html_destroy(value: *mut std::ffi::c_char) {
    if !value.is_null() {
        drop(std::ffi::CString::from_raw(value));
    }
}

#[cfg(not(feature = "qt"))]
fn highlight_to_html(code: &str, language: &str, is_dark_mode: bool) -> String {
    render_highlight_to_html(code, language, is_dark_mode)
}

// Export CXX-Qt bridge modules or core logic here
pub fn init() {
    println!("Initializing openbc-ui-qt core library...");
}

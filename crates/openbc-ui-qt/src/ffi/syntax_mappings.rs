//! C-ABI for the user's syntax-highlighting mappings and the built-in language catalog.
//!
//! The mapping rules themselves live in `openbc_core::text` (registry and router); this file only
//! moves text across the boundary:
//!
//! * [`openbc_highlight_set_user_mappings`] replaces the process-wide user mappings. Every later
//!   highlight call (`openbc_highlight_buffer*`, `openbc_highlight_to_html`) honours them, so a
//!   viewer that wants to reflect a change only has to highlight again.
//! * [`openbc_highlight_catalog`] returns the built-in languages and extension tables for the
//!   Options dialog. The returned string is released with `openbc_highlight_html_destroy`, the
//!   same destructor as for highlighted HTML (both are `CString::into_raw`).
//!
//! # Safety
//! `data` must be null or point to `length` readable bytes.

use std::ffi::{c_char, CString};

use openbc_core::text::{language_catalog_text, set_user_mappings_text};

/// Replaces all user mappings with the `extension<TAB>language` lines in `data` (UTF-8).
/// Lines with an unusable extension or an unknown language are ignored.
/// Returns the number of mappings applied.
///
/// # Safety
/// `data` must be null or point to `length` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn openbc_highlight_set_user_mappings(data: *const u8, length: usize) -> usize {
    let text = if data.is_null() || length == 0 {
        String::new()
    } else {
        String::from_utf8_lossy(std::slice::from_raw_parts(data, length)).into_owned()
    };
    set_user_mappings_text(&text)
}

/// The built-in language catalog (`L`, `E` and `F` lines, see `language_catalog_text`).
/// Free with `openbc_highlight_html_destroy`.
#[no_mangle]
pub extern "C" fn openbc_highlight_catalog() -> *mut c_char {
    CString::new(language_catalog_text()).unwrap_or_default().into_raw()
}

#[cfg(test)]
mod tests {
    use super::*;
    use openbc_core::text::highlight_to_spans;

    #[test]
    fn catalog_round_trips_through_the_c_string() {
        let raw = openbc_highlight_catalog();
        assert!(!raw.is_null());
        let text = unsafe { CString::from_raw(raw) }.into_string().expect("utf-8");
        assert!(text.starts_with("L\t"));
        assert!(text.contains("\nE\trs\tRust\n"));
    }

    #[test]
    fn mappings_apply_and_clear() {
        let ext = "obc-ffi-test-ext";
        let source = "fn main() { let x = 42; }";
        let line = format!("{ext}\tRust\n.bad ext\tRust\n");
        let applied = unsafe { openbc_highlight_set_user_mappings(line.as_ptr(), line.len()) };
        assert_eq!(applied, 1);
        assert!(!highlight_to_spans(source, &format!("a.{ext}"), true).is_empty());
        assert_eq!(unsafe { openbc_highlight_set_user_mappings(std::ptr::null(), 0) }, 0);
        assert!(highlight_to_spans(source, &format!("a.{ext}"), true).is_empty());
    }
}

//! C ABI for the folder-compare matching policy (size/timestamp vs. hash algorithm per file type).
//!
//! The rules live in `openbc_core::hash_policy`; this file only moves one text line across the
//! boundary, mirroring `syntax_mappings.rs`:
//!
//! * [`openbc_match_policy_set`] installs the user's policy (from Tools > Options) process-wide.
//!   `enabled == 0` removes it, which restores the explicit-toggle behaviour of older versions.
//! * [`openbc_match_policy_builtin`] returns the built-in defaults so C++ never duplicates the
//!   extension tables. Free with `openbc_match_policy_free`.
//! * [`openbc_match_policy_method_for`] answers "how will this file name be compared?" (for status
//!   text and tooltips). Free with `openbc_match_policy_free`.
//!
//! `vfs_bridge.rs` reads the installed policy through [`current`].

use std::ffi::{c_char, CStr, CString};
use std::sync::RwLock;

use openbc_core::MatchPolicy;

static POLICY: RwLock<Option<MatchPolicy>> = RwLock::new(None);

/// The installed policy, or `None` when per-file-type matching is switched off.
pub(crate) fn current() -> Option<MatchPolicy> {
    POLICY.read().ok().and_then(|g| g.clone())
}

fn install(config: &str, enabled: bool) {
    if let Ok(mut guard) = POLICY.write() {
        *guard = enabled.then(|| MatchPolicy::from_config_string(config));
    }
}

fn to_c(text: String) -> *mut c_char {
    CString::new(text).unwrap_or_default().into_raw()
}

/// Installs (`enabled != 0`) or removes the process-wide matching policy.
///
/// # Safety
/// `config` must be null or a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn openbc_match_policy_set(config: *const c_char, enabled: u8) {
    let text = if config.is_null() { String::new() } else { CStr::from_ptr(config).to_string_lossy().into_owned() };
    install(&text, enabled != 0);
}

/// The built-in policy in its one-line serialized form. Free with `openbc_match_policy_free`.
#[no_mangle]
pub extern "C" fn openbc_match_policy_builtin() -> *mut c_char {
    to_c(MatchPolicy::builtin().to_config_string())
}

/// Method id (`size`, `md5`, `sha1`, `sha256`, `sha512`) the *installed* policy (or, when none is
/// installed, the built-in one) uses for `file_name`. Free with `openbc_match_policy_free`.
///
/// # Safety
/// `file_name` must be null or a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn openbc_match_policy_method_for(file_name: *const c_char) -> *mut c_char {
    let name = if file_name.is_null() { String::new() } else { CStr::from_ptr(file_name).to_string_lossy().into_owned() };
    let policy = current().unwrap_or_default();
    to_c(policy.method_for(&name).id().to_owned())
}

/// `1` when a per-file-type policy is installed (Tools > Options > Folder Compare is on).
#[no_mangle]
pub extern "C" fn openbc_match_policy_enabled() -> u8 {
    u8::from(current().is_some())
}

/// Releases a string returned by this module.
///
/// # Safety
/// `s` must come from this module and be freed once.
#[no_mangle]
pub unsafe extern "C" fn openbc_match_policy_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn take(p: *mut c_char) -> String {
        let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
        unsafe { openbc_match_policy_free(p) };
        s
    }

    #[test]
    fn install_query_and_remove() {
        // single test: the policy is process-wide state
        let cfg = CString::new("default=sha256;rs=size").unwrap();
        unsafe { openbc_match_policy_set(cfg.as_ptr(), 1) };
        let rs = CString::new("lib.rs").unwrap();
        let other = CString::new("blob.zzz").unwrap();
        assert_eq!(take(unsafe { openbc_match_policy_method_for(rs.as_ptr()) }), "size");
        assert_eq!(take(unsafe { openbc_match_policy_method_for(other.as_ptr()) }), "sha256");
        assert!(current().is_some());
        assert_eq!(openbc_match_policy_enabled(), 1);

        unsafe { openbc_match_policy_set(cfg.as_ptr(), 0) };
        assert!(current().is_none());
        assert_eq!(openbc_match_policy_enabled(), 0);
        assert_eq!(take(unsafe { openbc_match_policy_method_for(other.as_ptr()) }), "md5", "built-in default");
        assert!(take(openbc_match_policy_builtin()).starts_with("default=md5;"));
    }
}

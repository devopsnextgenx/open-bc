//! FFI surface that turns a UI connection profile into a live [`openbc_vfs`]
//! session. Qt calls these from worker threads; each call `block_on`s the
//! shared Tokio runtime so the GUI thread never waits on the network.

use openbc_core::EntryPath;
use openbc_vfs::ftp::{FtpConfig, FtpSecurity};
use openbc_vfs::network::NetworkShareConfig;
use openbc_vfs::sftp::{SftpAuth, SftpConfig};
use openbc_vfs::{build_vfs, read_small_file, AsyncVfs, ConnectionProfile, VfsError};
use serde::Deserialize;
use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::c_char;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

struct VfsSession {
    vfs: Arc<dyn AsyncVfs>,
    network_rooted: bool,
}

struct SessionTable {
    next_id: i32,
    sessions: HashMap<i32, VfsSession>,
}

fn runtime() -> &'static tokio::runtime::Runtime {
    static RUNTIME: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RUNTIME.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .worker_threads(2)
            .thread_name("openbc-vfs")
            .build()
            .expect("failed to start the openbc-vfs Tokio runtime")
    })
}

fn sessions() -> &'static Mutex<SessionTable> {
    static SESSIONS: OnceLock<Mutex<SessionTable>> = OnceLock::new();
    SESSIONS.get_or_init(|| {
        Mutex::new(SessionTable {
            next_id: 1,
            sessions: HashMap::new(),
        })
    })
}

fn lock_sessions() -> std::sync::MutexGuard<'static, SessionTable> {
    sessions()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[derive(Debug, Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct VfsProfileJson {
    #[serde(default)]
    protocol: String,
    #[serde(default)]
    host: String,
    #[serde(default)]
    port: u16,
    #[serde(default)]
    username: String,
    #[serde(default)]
    password: String,
    #[serde(default)]
    key_file: String,
    #[serde(default)]
    passphrase: String,
    #[serde(default)]
    share_name: String,
    #[serde(default)]
    mount_path: String,
    #[serde(default)]
    remote_root: String,
    #[serde(default = "default_passive")]
    passive_mode: bool,
}

fn default_passive() -> bool {
    true
}

fn error_string(message: impl Into<String>) -> *mut c_char {
    match CString::new(message.into()) {
        Ok(value) => value.into_raw(),
        Err(_) => CString::new("error message contained an interior NUL")
            .expect("static error")
            .into_raw(),
    }
}

fn ok_string() -> *mut c_char {
    std::ptr::null_mut()
}

fn parse_profile(json: &str) -> Result<(ConnectionProfile, bool), String> {
    let profile: VfsProfileJson =
        serde_json::from_str(json).map_err(|err| format!("invalid connection profile: {err}"))?;
    connection_from_json(profile)
}

fn connection_from_json(profile: VfsProfileJson) -> Result<(ConnectionProfile, bool), String> {
    match profile.protocol.as_str() {
        "sftp" => {
            let auth = if !profile.key_file.is_empty() {
                SftpAuth::PrivateKey {
                    username: profile.username,
                    private_key: PathBuf::from(profile.key_file),
                    passphrase: if profile.passphrase.is_empty() {
                        None
                    } else {
                        Some(profile.passphrase)
                    },
                }
            } else {
                SftpAuth::Password {
                    username: profile.username,
                    password: profile.password,
                }
            };
            Ok((
                ConnectionProfile::Sftp(SftpConfig {
                    host: profile.host,
                    port: if profile.port == 0 { 22 } else { profile.port },
                    auth,
                    // Root at / so UI listing paths (absolute remote paths) map 1:1.
                    root: PathBuf::from("/"),
                    connect_timeout_secs: 15,
                }),
                false,
            ))
        }
        "ftp" | "ftps-explicit" => Ok((
            ConnectionProfile::Ftp(FtpConfig {
                host: profile.host,
                port: if profile.port == 0 { 21 } else { profile.port },
                security: if profile.protocol == "ftp" {
                    FtpSecurity::Plain
                } else {
                    FtpSecurity::ExplicitTls
                },
                username: if profile.username.is_empty() {
                    "anonymous".into()
                } else {
                    profile.username
                },
                password: profile.password,
                root: "/".into(),
                passive_mode: profile.passive_mode,
                connect_timeout_secs: 15,
            }),
            false,
        )),
        "ftps-implicit" => Err(
            "Implicit FTPS is not supported by openbc-vfs; use explicit FTPS or SFTP instead"
                .into(),
        ),
        "smb" | "network-drive" => {
            let mount = if !profile.mount_path.is_empty() {
                PathBuf::from(profile.mount_path)
            } else if !profile.host.is_empty() && !profile.share_name.is_empty() {
                PathBuf::from(format!("//{}/{}", profile.host, profile.share_name))
            } else {
                return Err(
                    "network drive needs a mount path, or a host and share name".into(),
                );
            };
            Ok((
                ConnectionProfile::Network(NetworkShareConfig {
                    host: profile.host,
                    share_name: profile.share_name,
                    mount_path: mount,
                }),
                true,
            ))
        }
        other => Err(format!("unknown protocol '{other}'")),
    }
}

fn vfs_error_message(err: VfsError) -> String {
    err.to_string()
}

fn insert_session(session: VfsSession) -> i32 {
    let mut table = lock_sessions();
    let id = table.next_id;
    table.next_id = table.next_id.saturating_add(1);
    table.sessions.insert(id, session);
    id
}

fn listing_path(raw: &str, network_rooted: bool) -> EntryPath {
    let trimmed = raw.trim();
    if network_rooted {
        let relative = trimmed
            .trim_start_matches(['/', '\\'])
            .replace('\\', "/");
        if relative.is_empty() || relative == "." {
            return EntryPath(PathBuf::new());
        }
        return EntryPath(PathBuf::from(relative));
    }
    if trimmed.is_empty() || trimmed == "." {
        return EntryPath(PathBuf::new());
    }
    EntryPath(PathBuf::from(trimmed))
}

fn mtime_ms(time: Option<SystemTime>) -> i64 {
    time.and_then(|value| value.duration_since(UNIX_EPOCH).ok())
        .map(|duration| duration.as_millis() as i64)
        .unwrap_or(0)
}

/// Connect using a JSON profile. On success writes a session id and returns
/// null; on failure returns an allocated error string.
///
/// ```
/// // The Qt client serializes RemoteProfile to JSON and calls this.
/// assert!(true);
/// ```
#[no_mangle]
pub extern "C" fn openbc_vfs_connect(
    json: *const u8,
    json_length: usize,
    session_id: *mut i32,
) -> *mut c_char {
    let json = unsafe { crate::input_text(json, json_length) };
    match connect_inner(&json) {
        Ok(id) => {
            if !session_id.is_null() {
                unsafe { *session_id = id };
            }
            ok_string()
        }
        Err(message) => error_string(message),
    }
}

fn connect_inner(json: &str) -> Result<i32, String> {
    let (profile, network_rooted) = parse_profile(json)?;
    let vfs = runtime()
        .block_on(build_vfs(profile))
        .map_err(vfs_error_message)?;
    Ok(insert_session(VfsSession {
        vfs: Arc::from(vfs),
        network_rooted,
    }))
}

fn session_handle(session_id: i32) -> Result<(Arc<dyn AsyncVfs>, bool), String> {
    let table = lock_sessions();
    let session = table
        .sessions
        .get(&session_id)
        .ok_or_else(|| format!("no VFS session {session_id}"))?;
    Ok((Arc::clone(&session.vfs), session.network_rooted))
}

/// Drop a session created by [`openbc_vfs_connect`].
#[no_mangle]
pub extern "C" fn openbc_vfs_disconnect(session_id: i32) {
    lock_sessions().sessions.remove(&session_id);
}

/// List one directory level. Writes a JSON array to `listing` on success.
#[no_mangle]
pub extern "C" fn openbc_vfs_list(
    session_id: i32,
    path: *const u8,
    path_length: usize,
    listing: *mut *mut c_char,
) -> *mut c_char {
    let path = unsafe { crate::input_text(path, path_length) };
    match list_inner(session_id, &path) {
        Ok(json) => {
            if !listing.is_null() {
                unsafe { *listing = error_string(json) };
            }
            ok_string()
        }
        Err(message) => error_string(message),
    }
}

fn list_inner(session_id: i32, path: &str) -> Result<String, String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    let entry = listing_path(path, network_rooted);
    let entries = runtime()
        .block_on(vfs.read_dir(&entry))
        .map_err(vfs_error_message)?;
    let mut items = Vec::with_capacity(entries.len());
    for entry in entries {
        items.push(serde_json::json!({
            "name": entry.name,
            "isDir": entry.metadata.is_dir,
            "size": entry.metadata.size,
            "mtimeMs": mtime_ms(entry.metadata.modified),
        }));
    }
    serde_json::to_string(&items).map_err(|err| err.to_string())
}

/// Stat a path. Returns 1 for a directory, 0 for a file, -1 on error.
#[no_mangle]
pub extern "C" fn openbc_vfs_stat(
    session_id: i32,
    path: *const u8,
    path_length: usize,
    error: *mut *mut c_char,
) -> i32 {
    let path = unsafe { crate::input_text(path, path_length) };
    match stat_inner(session_id, &path) {
        Ok(is_dir) => {
            if !error.is_null() {
                unsafe { *error = std::ptr::null_mut() };
            }
            i32::from(is_dir)
        }
        Err(message) => {
            if !error.is_null() {
                unsafe { *error = error_string(message) };
            }
            -1
        }
    }
}

fn stat_inner(session_id: i32, path: &str) -> Result<bool, String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    let entry = listing_path(path, network_rooted);
    let metadata = runtime()
        .block_on(vfs.stat(&entry))
        .map_err(vfs_error_message)?;
    Ok(metadata.is_dir)
}

/// Read a file into an allocated buffer. Returns null on error.
#[no_mangle]
pub extern "C" fn openbc_vfs_read(
    session_id: i32,
    path: *const u8,
    path_length: usize,
    out_length: *mut usize,
    error: *mut *mut c_char,
) -> *mut u8 {
    let path = unsafe { crate::input_text(path, path_length) };
    match read_inner(session_id, &path) {
        Ok(bytes) => {
            if !error.is_null() {
                unsafe { *error = std::ptr::null_mut() };
            }
            let length = bytes.len();
            if !out_length.is_null() {
                unsafe { *out_length = length };
            }
            let mut boxed = bytes.into_boxed_slice();
            let pointer = boxed.as_mut_ptr();
            std::mem::forget(boxed);
            pointer
        }
        Err(message) => {
            if !error.is_null() {
                unsafe { *error = error_string(message) };
            }
            if !out_length.is_null() {
                unsafe { *out_length = 0 };
            }
            std::ptr::null_mut()
        }
    }
}

fn read_inner(session_id: i32, path: &str) -> Result<Vec<u8>, String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    let entry = listing_path(path, network_rooted);
    runtime()
        .block_on(read_small_file(vfs.as_ref(), &entry))
        .map_err(vfs_error_message)
}

/// Free a string returned by the VFS FFI.
#[no_mangle]
pub unsafe extern "C" fn openbc_vfs_string_destroy(value: *mut c_char) {
    if !value.is_null() {
        drop(CString::from_raw(value));
    }
}

/// Free a buffer returned by [`openbc_vfs_read`].
#[no_mangle]
pub unsafe extern "C" fn openbc_vfs_buffer_destroy(value: *mut u8, length: usize) {
    if !value.is_null() && length > 0 {
        drop(Vec::from_raw_parts(value, length, length));
    } else if !value.is_null() {
        drop(Vec::from_raw_parts(value, 0, 0));
    }
}

#[cfg(test)]
mod tests {
    use super::{connect_inner, listing_path, parse_profile};
    use std::fs;
    use std::path::PathBuf;

    #[test]
    fn rejects_implicit_ftps() {
        let err = parse_profile(r#"{"protocol":"ftps-implicit","host":"example"}"#)
            .unwrap_err();
        assert!(err.contains("Implicit FTPS"));
    }

    #[test]
    fn lists_a_local_mount() {
        let dir = std::env::temp_dir().join("openbc-vfs-bridge-test");
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("hello.txt"), b"hi").unwrap();
        let json = format!(
            r#"{{"protocol":"network-drive","mountPath":{}}}"#,
            serde_json::to_string(&dir.to_string_lossy().into_owned()).unwrap()
        );
        let id = connect_inner(&json).expect("connect local mount");
        let listing = super::list_inner(id, "").expect("list");
        assert!(listing.contains("hello.txt"), "{listing}");
        super::openbc_vfs_disconnect(id);
        let _ = fs::remove_file(dir.join("hello.txt"));
        let _ = fs::remove_dir(&dir);
    }

    #[test]
    fn network_listing_path_strips_root() {
        assert_eq!(listing_path("/", true).0, PathBuf::new());
        assert_eq!(listing_path("/foo/bar", true).0, PathBuf::from("foo/bar"));
    }
}

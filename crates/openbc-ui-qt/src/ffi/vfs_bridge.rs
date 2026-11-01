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
        "local" => Ok((
            ConnectionProfile::Local {
                root: if profile.mount_path.is_empty() {
                    PathBuf::from("/")
                } else {
                    PathBuf::from(profile.mount_path)
                },
            },
            false,
        )),
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
                return Err("network drive needs a mount path, or a host and share name".into());
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
        let relative = trimmed.trim_start_matches(['/', '\\']).replace('\\', "/");
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

/// Provider + provider-relative path for a session id and the path string the UI shows, so
/// `ffi::resource_watch` can observe a remote file through the SAME session the viewers use.
pub(crate) fn watch_target(session_id: i32, raw_path: &str) -> Result<(Arc<dyn AsyncVfs>, EntryPath), String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    Ok((vfs, listing_path(raw_path, network_rooted)))
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
            "isLink": entry.metadata.is_link,
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

/// Replace a file's contents through openbc-engine and the selected VFS.
#[no_mangle]
pub extern "C" fn openbc_engine_write_file(
    session_id: i32,
    path: *const u8,
    path_length: usize,
    bytes: *const u8,
    bytes_length: usize,
) -> *mut c_char {
    let path = unsafe { crate::input_text(path, path_length) };
    if bytes.is_null() && bytes_length > 0 {
        return error_string("file contents pointer was null");
    }
    let contents = if bytes_length == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(bytes, bytes_length) }
    };
    match write_inner(session_id, &path, contents) {
        Ok(()) => ok_string(),
        Err(message) => error_string(message),
    }
}

/// Read file bytes through openbc-engine and the selected VFS.
#[no_mangle]
pub extern "C" fn openbc_engine_read_file(
    session_id: i32,
    path: *const u8,
    path_length: usize,
    out_length: *mut usize,
    error: *mut *mut c_char,
) -> *mut u8 {
    let path = unsafe { crate::input_text(path, path_length) };
    match read_inner_engine(session_id, &path) {
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

fn read_inner_engine(session_id: i32, path: &str) -> Result<Vec<u8>, String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    runtime()
        .block_on(openbc_engine::read_file(
            vfs.as_ref(),
            &listing_path(path, network_rooted),
        ))
        .map_err(vfs_error_message)
}

fn write_inner(session_id: i32, path: &str, bytes: &[u8]) -> Result<(), String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    runtime()
        .block_on(openbc_engine::write_file(
            vfs.as_ref(),
            &listing_path(path, network_rooted),
            bytes,
        ))
        .map_err(vfs_error_message)
}

/// Create one directory through openbc-engine and the selected VFS.
#[no_mangle]
pub extern "C" fn openbc_engine_create_dir(
    session_id: i32,
    path: *const u8,
    path_length: usize,
) -> *mut c_char {
    let path = unsafe { crate::input_text(path, path_length) };
    let (vfs, network_rooted) = match session_handle(session_id) {
        Ok(session) => session,
        Err(message) => return error_string(message),
    };
    match runtime().block_on(openbc_engine::create_directory(
        vfs.as_ref(),
        &listing_path(&path, network_rooted),
    )) {
        Ok(()) => ok_string(),
        Err(error) => error_string(vfs_error_message(error)),
    }
}

/// Rename a file or directory through openbc-engine and the selected VFS.
#[no_mangle]
pub extern "C" fn openbc_engine_rename(
    session_id: i32,
    from: *const u8,
    from_length: usize,
    to: *const u8,
    to_length: usize,
) -> *mut c_char {
    let from = unsafe { crate::input_text(from, from_length) };
    let to = unsafe { crate::input_text(to, to_length) };
    let (vfs, network_rooted) = match session_handle(session_id) {
        Ok(session) => session,
        Err(message) => return error_string(message),
    };
    match runtime().block_on(openbc_engine::rename_entry(
        vfs.as_ref(),
        &listing_path(&from, network_rooted),
        &listing_path(&to, network_rooted),
    )) {
        Ok(()) => ok_string(),
        Err(error) => error_string(vfs_error_message(error)),
    }
}

/// Copy a file or folder through openbc-engine between two VFS sessions.
#[no_mangle]
pub extern "C" fn openbc_engine_copy_entry(
    source_session_id: i32,
    source_path: *const u8,
    source_path_length: usize,
    destination_session_id: i32,
    destination_path: *const u8,
    destination_path_length: usize,
) -> *mut c_char {
    let source_path = unsafe { crate::input_text(source_path, source_path_length) };
    let destination_path = unsafe { crate::input_text(destination_path, destination_path_length) };
    match copy_inner(
        source_session_id,
        &source_path,
        destination_session_id,
        &destination_path,
    ) {
        Ok(()) => ok_string(),
        Err(message) => error_string(message),
    }
}

/// Mirror one source entry to a destination, removing destination-only descendants.
#[no_mangle]
pub extern "C" fn openbc_engine_mirror_entry(
    source_session_id: i32,
    source_path: *const u8,
    source_path_length: usize,
    destination_session_id: i32,
    destination_path: *const u8,
    destination_path_length: usize,
) -> *mut c_char {
    let source_path = unsafe { crate::input_text(source_path, source_path_length) };
    let destination_path = unsafe { crate::input_text(destination_path, destination_path_length) };
    let (source_vfs, source_network_rooted) = match session_handle(source_session_id) {
        Ok(session) => session,
        Err(message) => return error_string(message),
    };
    let (destination_vfs, destination_network_rooted) = match session_handle(destination_session_id)
    {
        Ok(session) => session,
        Err(message) => return error_string(message),
    };
    match runtime().block_on(openbc_engine::mirror_entry(
        source_vfs.as_ref(),
        &listing_path(&source_path, source_network_rooted),
        destination_vfs.as_ref(),
        &listing_path(&destination_path, destination_network_rooted),
    )) {
        Ok(()) => ok_string(),
        Err(error) => error_string(vfs_error_message(error)),
    }
}

fn copy_inner(
    source_session_id: i32,
    source_path: &str,
    destination_session_id: i32,
    destination_path: &str,
) -> Result<(), String> {
    let (source_vfs, source_network_rooted) = session_handle(source_session_id)?;
    let (destination_vfs, destination_network_rooted) = session_handle(destination_session_id)?;
    runtime()
        .block_on(openbc_engine::copy_entry(
            source_vfs.as_ref(),
            &listing_path(source_path, source_network_rooted),
            destination_vfs.as_ref(),
            &listing_path(destination_path, destination_network_rooted),
        ))
        .map_err(vfs_error_message)
}

/// Delete a file or directory tree through openbc-engine and its VFS provider.
#[no_mangle]
pub extern "C" fn openbc_engine_delete_entry(
    session_id: i32,
    path: *const u8,
    path_length: usize,
) -> *mut c_char {
    let path = unsafe { crate::input_text(path, path_length) };
    let (vfs, network_rooted) = match session_handle(session_id) {
        Ok(session) => session,
        Err(message) => return error_string(message),
    };
    match runtime().block_on(openbc_engine::delete_entry(
        vfs.as_ref(),
        &listing_path(&path, network_rooted),
    )) {
        Ok(()) => ok_string(),
        Err(error) => error_string(vfs_error_message(error)),
    }
}

/// Compare one directory level through openbc-engine and openbc-core.
#[no_mangle]
pub extern "C" fn openbc_engine_compare_folder_level(
    left_session_id: i32,
    left_path: *const u8,
    left_path_length: usize,
    right_session_id: i32,
    right_path: *const u8,
    right_path_length: usize,
    left_present: u8,
    right_present: u8,
    check_content: u8,
    ignore_timestamps: u8,
    result: *mut *mut c_char,
) -> *mut c_char {
    let left_path = unsafe { crate::input_text(left_path, left_path_length) };
    let right_path = unsafe { crate::input_text(right_path, right_path_length) };
    match compare_folder_inner(
        left_session_id,
        &left_path,
        right_session_id,
        &right_path,
        left_present != 0,
        right_present != 0,
        check_content != 0,
        ignore_timestamps != 0,
    ) {
        Ok(json) => {
            if !result.is_null() {
                unsafe { *result = error_string(json) };
            }
            ok_string()
        }
        Err(message) => error_string(message),
    }
}

/// Compare two current file contents through openbc-engine and openbc-compute.
#[no_mangle]
pub extern "C" fn openbc_engine_compare_files(
    left_session_id: i32,
    left_path: *const u8,
    left_path_length: usize,
    right_session_id: i32,
    right_path: *const u8,
    right_path_length: usize,
    equal: *mut u8,
) -> *mut c_char {
    let left_path = unsafe { crate::input_text(left_path, left_path_length) };
    let right_path = unsafe { crate::input_text(right_path, right_path_length) };
    match compare_files_inner(left_session_id, &left_path, right_session_id, &right_path) {
        Ok(is_equal) => {
            if !equal.is_null() {
                unsafe { *equal = u8::from(is_equal) };
            }
            ok_string()
        }
        Err(message) => error_string(message),
    }
}

fn compare_files_inner(
    left_session_id: i32,
    left_path: &str,
    right_session_id: i32,
    right_path: &str,
) -> Result<bool, String> {
    let (left_vfs, left_network_rooted) = session_handle(left_session_id)?;
    let (right_vfs, right_network_rooted) = session_handle(right_session_id)?;
    runtime()
        .block_on(openbc_engine::compare_file_content_by_hash(
            left_vfs.as_ref(),
            &listing_path(left_path, left_network_rooted),
            right_vfs.as_ref(),
            &listing_path(right_path, right_network_rooted),
        ))
        .map_err(|error| error.to_string())
}

fn compare_folder_inner(
    left_session_id: i32,
    left_path: &str,
    right_session_id: i32,
    right_path: &str,
    left_present: bool,
    right_present: bool,
    check_content: bool,
    ignore_timestamps: bool,
) -> Result<String, String> {
    let (left_vfs, left_network_rooted) = session_handle(left_session_id)?;
    let (right_vfs, right_network_rooted) = session_handle(right_session_id)?;
    let comparison = runtime()
        .block_on(openbc_engine::compare_folder_sides_with_options(
            left_vfs,
            left_present.then(|| listing_path(left_path, left_network_rooted)),
            right_vfs,
            right_present.then(|| listing_path(right_path, right_network_rooted)),
            check_content,
            ignore_timestamps,
        ))
        .map_err(|error| error.to_string())?;
    let entries: Vec<_> = comparison
        .entries
        .iter()
        .map(|entry| {
            let serialize_side = |side: &Option<openbc_core::DirectoryEntry>| {
                side.as_ref().map(|side| {
                    serde_json::json!({
                        "path": side.path.0.to_string_lossy(),
                        "size": side.metadata.size,
                        "isDir": side.metadata.is_dir,
                        "isLink": side.metadata.is_link,
                        "mtimeMs": mtime_ms(side.metadata.modified),
                    })
                })
            };
            let status = match &entry.status {
                openbc_core::ComparisonStatus::Equal => "equal".to_owned(),
                openbc_core::ComparisonStatus::Different => "different".to_owned(),
                openbc_core::ComparisonStatus::LeftNewer => "left-newer".to_owned(),
                openbc_core::ComparisonStatus::RightNewer => "right-newer".to_owned(),
                openbc_core::ComparisonStatus::Missing => "missing".to_owned(),
                openbc_core::ComparisonStatus::Error(message) => format!("error:{message}"),
            };
            serde_json::json!({
                "name": entry.name,
                "left": serialize_side(&entry.left),
                "right": serialize_side(&entry.right),
                "status": status,
            })
        })
        .collect();
    serde_json::to_string(&entries).map_err(|error| error.to_string())
}

fn read_inner(session_id: i32, path: &str) -> Result<Vec<u8>, String> {
    let (vfs, network_rooted) = session_handle(session_id)?;
    let entry = listing_path(path, network_rooted);
    runtime()
        .block_on(read_small_file(vfs.as_ref(), &entry))
        .map_err(vfs_error_message)
}

// ------------------------------------------------------------------------------------------
// Archives as folders. The extension table lives in `openbc_vfs::archive` (never duplicate it in
// C++). Paths that go *through* an archive (`/a/x.zip/src/f.rs`) already work with every
// `openbc_vfs_*` / `openbc_engine_*` function above, because each session's provider is wrapped in
// `ArchiveAwareVfs`; the functions below only tell the UI which names are worth offering as folders.
// ------------------------------------------------------------------------------------------

/// 1 when the file name should be offered as a folder (zip, jar, 7z, rar, tar.gz, ...), else 0.
/// Name only: no I/O. Zip-based documents (docx, xlsx, epub...) answer 0 on purpose.
#[no_mangle]
pub extern "C" fn openbc_archive_is_archive_name(name: *const u8, name_length: usize) -> i32 {
    let name = unsafe { crate::input_text(name, name_length) };
    i32::from(archive_name_check(&name))
}

/// Number of path components that look like archive files (lexical, no I/O). Greater than 0 means
/// the path is inside an archive or is one, so the UI can avoid auto-expanding nested archives.
#[no_mangle]
pub extern "C" fn openbc_archive_depth(path: *const u8, path_length: usize) -> i32 {
    let path = unsafe { crate::input_text(path, path_length) };
    archive_depth_check(&path)
}

/// JSON array of the extensions offered as folders (lower-case, no dot). Free with
/// `openbc_vfs_string_destroy`. Writes `[]` when this build has no archive support.
#[no_mangle]
pub extern "C" fn openbc_archive_extensions(extensions: *mut *mut c_char) -> *mut c_char {
    if !extensions.is_null() {
        let list = archive_extension_list();
        let json = serde_json::to_string(&list).unwrap_or_else(|_| "[]".into());
        unsafe { *extensions = error_string(json) };
    }
    ok_string()
}

fn archive_name_check(name: &str) -> bool {
    openbc_vfs::archive_is_name(name)
}

fn archive_depth_check(path: &str) -> i32 {
    i32::try_from(openbc_vfs::archive_path_depth(std::path::Path::new(path))).unwrap_or(i32::MAX)
}

fn archive_extension_list() -> Vec<&'static str> {
    openbc_vfs::archive_folder_extensions()
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
    use super::{
        compare_folder_inner, connect_inner, listing_path, openbc_vfs_disconnect, parse_profile,
        runtime,
    };
    use std::fs;
    use std::path::PathBuf;

    #[test]
    fn rejects_implicit_ftps() {
        let err = match parse_profile(r#"{"protocol":"ftps-implicit","host":"example"}"#) {
            Err(error) => error,
            Ok(_) => panic!("implicit FTPS should be rejected"),
        };
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

    fn fixture(name: &str) -> String {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../openbc-vfs/tests/fixtures")
            .join(name)
            .to_string_lossy()
            .into_owned()
    }

    #[test]
    fn archive_paths_work_through_the_session_functions() {
        let session = connect_inner(r#"{"protocol":"local","mountPath":"/"}"#).unwrap();
        let zip = fixture("sample.zip");
        let listing = super::list_inner(session, &zip).unwrap();
        assert!(listing.contains("readme.txt") && listing.contains("src"), "{listing}");
        assert!(!super::stat_inner(session, &zip).unwrap(), "the archive itself stays a file");
        assert!(super::stat_inner(session, &format!("{zip}/src")).unwrap());
        assert_eq!(super::read_inner_engine(session, &format!("{zip}/src/main.rs")).unwrap(), b"fn main() {}\n");
        // Writing inside an archive is refused instead of touching the outer filesystem.
        let err = super::write_inner(session, &format!("{zip}/new.txt"), b"x").unwrap_err();
        assert!(err.contains("read-only"), "{err}");
        openbc_vfs_disconnect(session);
    }

    #[test]
    fn engine_compares_two_different_archive_formats_as_folders() {
        let session = connect_inner(r#"{"protocol":"local","mountPath":"/"}"#).unwrap();
        let (zip, tgz) = (fixture("sample.zip"), fixture("sample.tar.gz"));
        // Same contents, different container: content check must say every file is equal.
        let rows = compare_folder_inner(session, &zip, session, &tgz, true, true, true, false).unwrap();
        let rows: serde_json::Value = serde_json::from_str(&rows).unwrap();
        let by_name = |n: &str| rows.as_array().unwrap().iter().find(|r| r["name"] == n).unwrap().clone();
        assert_eq!(by_name("readme.txt")["status"], "equal");
        assert!(by_name("src")["left"]["isDir"].as_bool().unwrap());
        // One level down, still inside both archives.
        let inner = compare_folder_inner(session, &format!("{zip}/data"), session, &format!("{tgz}/data"), true, true, true, false).unwrap();
        assert!(inner.contains("blob.bin") && inner.contains("\"equal\""), "{inner}");
        // 7z vs rar vs a real folder on the other side.
        assert!(super::compare_files_inner(session, &format!("{}/readme.txt", fixture("sample.7z")), session, &format!("{}/readme.txt", fixture("sample.rar"))).unwrap());
        openbc_vfs_disconnect(session);
    }

    #[test]
    fn archive_name_ffi_helpers() {
        assert!(super::archive_name_check("a.tar.gz") && super::archive_name_check("lib.JAR"));
        assert!(!super::archive_name_check("report.docx") && !super::archive_name_check("libc.so"));
        assert_eq!(super::archive_depth_check("/a/x.zip/y.7z/f.txt"), 2);
        assert!(super::archive_extension_list().contains(&"7z"));
    }

    #[test]
    fn network_listing_path_strips_root() {
        assert_eq!(listing_path("/", true).0, PathBuf::new());
        assert_eq!(listing_path("/foo/bar", true).0, PathBuf::from("foo/bar"));
    }

    #[test]
    fn engine_bridge_compares_and_mutates_local_vfs_paths() {
        let unique = format!(
            "openbc-vfs-engine-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        );
        let root = std::env::temp_dir().join(unique);
        let left = root.join("left");
        let right = root.join("right");
        fs::create_dir_all(&left).unwrap();
        fs::create_dir_all(&right).unwrap();
        let root_json = r#"{"protocol":"local","mountPath":"/"}"#;
        let session = connect_inner(root_json).expect("connect local VFS");
        let left_file = left.join("sample.txt").to_string_lossy().into_owned();
        let right_file = right.join("sample.txt").to_string_lossy().into_owned();
        super::write_inner(session, &left_file, b"same").unwrap();
        super::write_inner(session, &right_file, b"same").unwrap();

        let result = compare_folder_inner(
            session,
            &left.to_string_lossy(),
            session,
            &right.to_string_lossy(),
            true,
            true,
            true,
            false,
        )
        .unwrap();
        let rows: serde_json::Value = serde_json::from_str(&result).unwrap();
        assert_eq!(rows[0]["status"], "equal");

        assert!(super::compare_files_inner(session, &left_file, session, &right_file).unwrap());
        fs::write(&right_file, b"diff").unwrap();
        assert!(!super::compare_files_inner(session, &left_file, session, &right_file).unwrap());
        let one_sided = compare_folder_inner(
            session,
            &left.to_string_lossy(),
            session,
            "",
            true,
            false,
            false,
            false,
        )
        .unwrap();
        let orphan_rows: serde_json::Value = serde_json::from_str(&one_sided).unwrap();
        assert_eq!(orphan_rows[0]["status"], "missing");

        let created = left.join("created").to_string_lossy().into_owned();
        let (vfs, network_rooted) = super::session_handle(session).unwrap();
        runtime()
            .block_on(openbc_engine::create_directory(
                vfs.as_ref(),
                &listing_path(&created, network_rooted),
            ))
            .unwrap();
        assert_eq!(
            super::read_inner_engine(session, &left_file).unwrap(),
            b"same"
        );
        openbc_vfs_disconnect(session);
        fs::remove_dir_all(root).unwrap();
    }
}
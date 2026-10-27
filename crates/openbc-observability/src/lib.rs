//! Centralized application logging and performance instrumentation.

use serde::{Deserialize, Serialize};
use std::cell::RefCell;
use std::collections::{BTreeMap, HashSet};
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, MutexGuard, OnceLock, TryLockError};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tracing_appender::non_blocking::WorkerGuard;

static SESSION: OnceLock<InstrumentationSession> = OnceLock::new();
static LOG_GUARD: OnceLock<WorkerGuard> = OnceLock::new();
static ACTIVE_LOG_PATH: OnceLock<PathBuf> = OnceLock::new();

thread_local! {
    static SESSION_CONTEXT: RefCell<Option<String>> = RefCell::new(None);
}

#[derive(Debug, Serialize)]
struct InstrumentationEvent {
    timestamp: String,
    module: String,
    operation: String,
    duration_ms: u128,
    #[serde(skip_serializing_if = "Option::is_none")]
    left_bytes: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    right_bytes: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    left_lines: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    right_lines: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    folder_bytes: Option<u64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    nested_level: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    resource: Option<String>,
}

struct InstrumentationSession {
    directory: PathBuf,
    state: Mutex<InstrumentationState>,
}

struct InstrumentationState {
    sessions: BTreeMap<String, InstrumentationTabFile>,
    latest_path: Option<PathBuf>,
}

struct InstrumentationTabFile {
    label: String,
    session_started: String,
    path: PathBuf,
    file: File,
    events_offset: u64,
    has_events: bool,
}

#[derive(Debug, Serialize)]
struct InstrumentationReport {
    sessions: BTreeMap<String, InstrumentationTabSession>,
}

#[derive(Debug, Serialize)]
struct InstrumentationTabSession {
    label: String,
    session_started: String,
    events: Vec<InstrumentationEvent>,
}

/// Scoped tab/session identity for synchronous instrumented calls.
pub struct SessionScope {
    previous: Option<String>,
}

impl Drop for SessionScope {
    fn drop(&mut self) {
        SESSION_CONTEXT.with(|context| *context.borrow_mut() = self.previous.take());
    }
}

/// Use a tab ID for instrumentation recorded during the current synchronous call.
pub fn session_scope(session_id: &str) -> SessionScope {
    let previous =
        SESSION_CONTEXT.with(|context| context.borrow_mut().replace(session_id.to_owned()));
    SessionScope { previous }
}

/// Open `path` for appending, creating its parent directory first.
fn open_log_file(path: &Path) -> std::io::Result<File> {
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            fs::create_dir_all(parent)?;
        }
    }
    OpenOptions::new().create(true).append(true).open(path)
}

/// Initialize the process-wide log sink and one YAML instrumentation session.
///
/// Log location is tried in order: the platform's preferred path, the per-user
/// application directory, then the system temp directory. Failing to find a
/// writable location is the only way this returns an error.
pub fn init() -> Result<(), String> {
    let username = username();
    let file_name = format!("open-bc-{username}.log");

    let candidates = [
        preferred_log_path(),
        app_data_dir().join(&file_name),
        env::temp_dir().join("open-bc").join(&file_name),
    ];
    let mut last_error = String::new();
    let mut opened: Option<(File, PathBuf)> = None;
    for candidate in candidates {
        match open_log_file(&candidate) {
            Ok(file) => {
                opened = Some((file, candidate));
                break;
            }
            Err(error) => {
                last_error = format!("cannot open {}: {error}", candidate.display());
            }
        }
    }
    let (log_file, active_log_path) =
        opened.ok_or_else(|| format!("no writable log location ({last_error})"))?;

    let (writer, guard) = tracing_appender::non_blocking(log_file);
    let subscriber = tracing_subscriber::fmt()
        .with_writer(writer)
        .with_ansi(false)
        .with_target(false)
        .with_file(true)
        .with_line_number(true)
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .finish();
    tracing::subscriber::set_global_default(subscriber)
        .map_err(|error| format!("cannot install logging subscriber: {error}"))?;
    let _ = LOG_GUARD.set(guard);
    let _ = ACTIVE_LOG_PATH.set(active_log_path.clone());

    let instrumentation_dir = create_instrumentation_dir()?;
    SESSION
        .set(InstrumentationSession {
            directory: instrumentation_dir,
            state: Mutex::new(InstrumentationState {
                sessions: BTreeMap::new(),
                latest_path: None,
            }),
        })
        .map_err(|_| "instrumentation was initialized more than once".to_string())?;
    tracing::info!(target: "openbc", file = file!(), function = "init", level = "INFO", message = "centralized logging initialized", log_path = %active_log_path.display());
    Ok(())
}

/// Create the instrumentation directory, falling back to the temp dir.
fn create_instrumentation_dir() -> Result<PathBuf, String> {
    let candidates = [
        app_data_dir().join("instrumentation"),
        env::temp_dir().join("open-bc").join("instrumentation"),
    ];
    let mut last_error = String::new();
    for candidate in candidates {
        match fs::create_dir_all(&candidate) {
            Ok(()) => return Ok(candidate),
            Err(error) => {
                last_error = format!("cannot create {}: {error}", candidate.display());
            }
        }
    }
    Err(last_error)
}

/// Return the current user's OpenBC log path.
pub fn log_path() -> PathBuf {
    ACTIVE_LOG_PATH
        .get()
        .cloned()
        .unwrap_or_else(preferred_log_path)
}

/// Return the most recently used tab's YAML report path.
pub fn instrumentation_path() -> Option<PathBuf> {
    SESSION.get()?.state.lock().ok()?.latest_path.clone()
}

/// Register a UI tab so its report exists even before its first instrumented operation.
pub fn register_session(session_id: &str, label: &str) {
    let Some(session) = SESSION.get() else { return };
    let Ok(mut state) = session.state.lock() else {
        return;
    };
    let Some(tab) = get_or_create_tab(&mut state, &session.directory, session_id, label) else {
        return;
    };
    if !label.is_empty() && tab.label != label {
        rewrite_tab_header(tab, session_id, label);
    }
}

/// Return the YAML report containing only the requested tab's event group.
pub fn session_report(session_id: &str) -> Option<String> {
    let session = SESSION.get()?;
    let mut state = session.state.lock().ok()?;
    let tab = state.sessions.get_mut(session_id)?;
    tab.file.flush().ok()?;
    fs::read_to_string(&tab.path).ok()
}

/// Record a timed operation and its input/resource measurements.
pub fn record(
    module: &str,
    operation: &str,
    duration: Duration,
    sizes: Option<(usize, usize)>,
    lines: Option<(usize, usize)>,
    folder_bytes: Option<u64>,
    nested_level: Option<usize>,
    resource: Option<String>,
) {
    let session_id = SESSION_CONTEXT
        .with(|context| context.borrow().clone())
        .filter(|session_id| !session_id.is_empty())
        .unwrap_or_else(|| "application".to_owned());
    usage_note_operation(&session_id, operation, duration);
    let Some(session) = SESSION.get() else { return };
    let event = InstrumentationEvent {
        timestamp: timestamp(),
        module: module.to_owned(),
        operation: operation.to_owned(),
        duration_ms: duration.as_millis(),
        left_bytes: sizes.map(|value| value.0),
        right_bytes: sizes.map(|value| value.1),
        left_lines: lines.map(|value| value.0),
        right_lines: lines.map(|value| value.1),
        folder_bytes,
        nested_level,
        resource,
    };
    let Ok(mut state) = session.state.lock() else {
        return;
    };
    let Some(tab) = get_or_create_tab(&mut state, &session.directory, &session_id, "Application")
    else {
        return;
    };
    append_event(tab, &event);
}

fn get_or_create_tab<'a>(
    state: &'a mut InstrumentationState,
    directory: &PathBuf,
    session_id: &str,
    label: &str,
) -> Option<&'a mut InstrumentationTabFile> {
    if !state.sessions.contains_key(session_id) {
        let tab = create_tab_file(directory, session_id, label)?;
        state.latest_path = Some(tab.path.clone());
        state.sessions.insert(session_id.to_owned(), tab);
    }
    let tab = state.sessions.get_mut(session_id)?;
    state.latest_path = Some(tab.path.clone());
    Some(tab)
}

fn create_tab_file(
    directory: &PathBuf,
    session_id: &str,
    label: &str,
) -> Option<InstrumentationTabFile> {
    let session_started = timestamp();
    let report = InstrumentationReport {
        sessions: BTreeMap::from([(
            session_id.to_owned(),
            InstrumentationTabSession {
                label: label.to_owned(),
                session_started: session_started.clone(),
                events: Vec::new(),
            },
        )]),
    };
    let serialized = serde_yaml::to_string(&report).ok()?;
    let events_offset = serialized.find("events: []")? + "events: ".len();
    let path = directory.join(format!("session-{}.yml", hex_encode(session_id.as_bytes())));
    let mut file = File::create(&path).ok()?;
    file.write_all(serialized.as_bytes()).ok()?;
    file.flush().ok()?;
    Some(InstrumentationTabFile {
        label: label.to_owned(),
        session_started,
        path,
        file,
        events_offset: events_offset as u64,
        has_events: false,
    })
}

fn rewrite_tab_header(tab: &mut InstrumentationTabFile, session_id: &str, label: &str) {
    let Ok(current) = fs::read_to_string(&tab.path) else {
        return;
    };
    let event_data = if tab.has_events {
        let Some(event_data) = current.get(tab.events_offset as usize..) else {
            return;
        };
        event_data.to_owned()
    } else {
        String::new()
    };
    let report = InstrumentationReport {
        sessions: BTreeMap::from([(
            session_id.to_owned(),
            InstrumentationTabSession {
                label: label.to_owned(),
                session_started: tab.session_started.clone(),
                events: Vec::new(),
            },
        )]),
    };
    let Ok(serialized) = serde_yaml::to_string(&report) else {
        return;
    };
    let Some(events_offset) = serialized.find("events: []") else {
        return;
    };
    if tab.file.set_len(0).is_err() || tab.file.seek(SeekFrom::Start(0)).is_err() {
        return;
    }
    let header_end = events_offset + "events: ".len();
    let write_result = if tab.has_events {
        tab.file
            .write_all(serialized[..header_end].as_bytes())
            .and_then(|()| tab.file.write_all(event_data.as_bytes()))
    } else {
        tab.file.write_all(serialized.as_bytes())
    };
    if write_result.is_err() || tab.file.flush().is_err() {
        return;
    }
    tab.label = label.to_owned();
    tab.events_offset = header_end as u64;
}

fn append_event(tab: &mut InstrumentationTabFile, event: &InstrumentationEvent) {
    let Ok(serialized) = serde_yaml::to_string(event) else {
        return;
    };
    let mut lines = serialized.lines();
    let Some(first) = lines.next() else { return };
    let position = if tab.has_events {
        tab.file.seek(SeekFrom::End(0))
    } else {
        let _ = tab.file.set_len(tab.events_offset);
        tab.file.seek(SeekFrom::Start(tab.events_offset))
    };
    if position.is_err() {
        return;
    }
    if !tab.has_events && tab.file.write_all(b"\n").is_err() {
        return;
    }
    if tab.file.write_all(b"      - ").is_err() || writeln!(tab.file, "{first}").is_err() {
        return;
    }
    for line in lines {
        if writeln!(tab.file, "        {line}").is_err() {
            return;
        }
    }
    if tab.file.flush().is_ok() {
        tab.has_events = true;
    }
}

fn hex_encode(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        encoded.push(HEX[(byte >> 4) as usize] as char);
        encoded.push(HEX[(byte & 0x0f) as usize] as char);
    }
    encoded
}

/// Emit a structured log line with a stable file/function/level/message shape.
pub fn log(module: &str, function: &str, level: &str, message: &str) {
    match level {
        "ERROR" => tracing::error!(target: "openbc", module, function, level, message),
        "WARN" => tracing::warn!(target: "openbc", module, function, level, message),
        _ => tracing::info!(target: "openbc", module, function, level, message),
    }
}

/// Current user's name: `USER` on Unix, `USERNAME` on Windows.
fn username() -> String {
    env::var("USER")
        .or_else(|_| env::var("USERNAME"))
        .unwrap_or_else(|_| "unknown".to_string())
}

/// Home directory: `HOME` (Unix, Git Bash), else `USERPROFILE` (Windows).
/// Never falls back to the current directory, which may be read-only
/// (e.g. `C:\Program Files\OpenBC` when launched from a shortcut).
fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .filter(|value| !value.is_empty())
        .or_else(|| env::var_os("USERPROFILE").filter(|value| !value.is_empty()))
        .map(PathBuf::from)
        .unwrap_or_else(env::temp_dir)
}

/// Per-user writable application directory.
/// Windows: `%LOCALAPPDATA%\OpenBC`. Unix: `~/.config/open-bc`.
fn app_data_dir() -> PathBuf {
    #[cfg(windows)]
    {
        env::var_os("LOCALAPPDATA")
            .filter(|value| !value.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| home_dir().join("AppData").join("Local"))
            .join("OpenBC")
    }
    #[cfg(not(windows))]
    {
        home_dir().join(".config/open-bc")
    }
}

/// Preferred log file: `/var/log` on Unix, the per-user app dir on Windows.
fn preferred_log_path() -> PathBuf {
    #[cfg(windows)]
    {
        app_data_dir().join(format!("open-bc-{}.log", username()))
    }
    #[cfg(not(windows))]
    {
        PathBuf::from(format!("/var/log/open-bc-{}.log", username()))
    }
}

// ---------------------------------------------------------------------------
// Usage tracking (anonymous install id + per-launch session summary)
//
// What is collected: install id (random UUID), app version, OS/arch, start/end
// time, how long the app was open, and for each compare tab: its type, how long
// it was open, how long operations ran and how many ran. File names, paths and
// tab labels are NEVER sent. IP address and country are added server-side by
// the ingest Lambda, not by this client.
//
// Offline behaviour: everything is kept in `session.data.yml` in the per-user
// app directory and uploaded in batches the next time the network is reachable.
// Uploads are idempotent (keyed by `app_session_id`) so retries never double count.
// ---------------------------------------------------------------------------

/// Ingest endpoint, e.g. `https://dxxxx.cloudfront.net/track`. Set at build time
/// with `OPENBC_TRACKING_URL=... cargo build`, or override at runtime with the
/// environment variable of the same name. No URL means tracking is fully off.
const TRACKING_URL: Option<&str> = option_env!("OPENBC_TRACKING_URL");
/// Shared ingest key (spam protection only; it ships inside the binary).
const TRACKING_KEY: Option<&str> = option_env!("OPENBC_TRACKING_KEY");
const USAGE_FILE_NAME: &str = "session.data.yml";
const OPT_OUT_MARKER: &str = "telemetry.disabled";
const MAX_PENDING_RECORDS: usize = 500;
const MAX_COMPARE_DETAILS: usize = 100;
const BATCH_SIZE: usize = 25;
const HEARTBEAT_SECS: u64 = 60;
const HEARTBEAT_UPLOAD_EVERY: u64 = 5; // ticks, i.e. every 5 minutes
const HTTP_TIMEOUT: Duration = Duration::from_secs(5);
const SHUTDOWN_UPLOAD_BUDGET: Duration = Duration::from_secs(4);

static USAGE: OnceLock<Mutex<UsageState>> = OnceLock::new();
static USAGE_SHUTDOWN: AtomicBool = AtomicBool::new(false);
static SYNC_LOCK: Mutex<()> = Mutex::new(());

/// One compare tab, as reported to the server.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
struct CompareSessionUsage {
    /// Hash of the UI tab id (never the raw id).
    id: String,
    /// `file`, `folder`, `image`, ... or `unknown`.
    kind: String,
    opened_at: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    closed_at: Option<u64>,
    /// Seconds the tab was open.
    duration_s: u64,
    /// Milliseconds spent inside instrumented operations.
    busy_ms: u64,
    operations: u32,
}

/// One application launch.
#[derive(Debug, Clone, Serialize, Deserialize)]
struct UsageRecord {
    app_session_id: String,
    install_id: String,
    app_version: String,
    os: String,
    arch: String,
    /// `active` (running), `ended` (clean exit) or `recovered` (previous run died).
    status: String,
    started_at: u64,
    last_seen_at: u64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    ended_at: Option<u64>,
    duration_s: u64,
    /// Total compare sessions opened (may exceed `compare_sessions.len()`).
    compare_count: u32,
    compare_sessions: Vec<CompareSessionUsage>,
}

/// Contents of `session.data.yml`.
#[derive(Debug, Default, Serialize, Deserialize)]
struct UsageFile {
    install_id: String,
    #[serde(default)]
    current: Option<UsageRecord>,
    #[serde(default)]
    pending: Vec<UsageRecord>,
}

struct UsageState {
    path: PathBuf,
    url: String,
    file: UsageFile,
    /// Hashed tab ids already counted in `compare_count`.
    seen: HashSet<String>,
}

enum SendOutcome {
    Sent,
    /// Server rejected the payload for good; retrying would never succeed.
    Rejected,
    /// Offline, timeout, 5xx, auth problem: keep the data and retry later.
    Retry,
}

#[derive(Serialize)]
struct UploadBody<'a> {
    records: &'a [UsageRecord],
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn lock_usage(mutex: &Mutex<UsageState>) -> MutexGuard<'_, UsageState> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn short_hash(value: &str) -> String {
    // FNV-1a: only used to avoid sending raw tab ids, not for security.
    let mut hash: u64 = 0xcbf29ce484222325;
    for byte in value.as_bytes() {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{hash:016x}")
}

fn sanitize_kind(kind: &str) -> String {
    let cleaned: String = kind
        .trim()
        .to_ascii_lowercase()
        .chars()
        .filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .take(32)
        .collect();
    if cleaned.is_empty() {
        "unknown".to_owned()
    } else {
        cleaned
    }
}

/// Best-effort session type from an operation name. Prefer passing the type
/// explicitly through `register_compare_session`.
fn infer_kind(operation: &str) -> Option<&'static str> {
    let op = operation.to_ascii_lowercase();
    if op.contains("folder") || op.contains("directory") {
        Some("folder")
    } else if op.contains("image") {
        Some("image")
    } else if op.contains("file") {
        Some("file")
    } else {
        None
    }
}

fn tracking_url() -> Option<String> {
    env::var("OPENBC_TRACKING_URL")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .or_else(|| TRACKING_URL.map(str::to_owned))
}

/// Tracking is on unless the user opted out (env var or marker file).
pub fn usage_tracking_enabled() -> bool {
    if let Ok(value) = env::var("OPENBC_TELEMETRY") {
        if matches!(value.trim().to_ascii_lowercase().as_str(), "0" | "false" | "off" | "no") {
            return false;
        }
    }
    !app_data_dir().join(OPT_OUT_MARKER).exists()
}

/// Persist the user's choice (call from the settings UI). Opting out also
/// deletes any queued, not yet uploaded usage data.
pub fn set_usage_tracking_enabled(enabled: bool) {
    let dir = app_data_dir();
    let marker = dir.join(OPT_OUT_MARKER);
    if enabled {
        let _ = fs::remove_file(marker);
    } else {
        let _ = fs::create_dir_all(&dir);
        let _ = fs::write(marker, b"usage tracking disabled by user\n");
        let _ = fs::remove_file(dir.join(USAGE_FILE_NAME));
        if let Some(state) = USAGE.get() {
            let mut state = lock_usage(state);
            state.file.current = None;
            state.file.pending.clear();
        }
        USAGE_SHUTDOWN.store(true, Ordering::SeqCst);
    }
}

fn load_usage_file(path: &Path) -> UsageFile {
    let mut file = fs::read_to_string(path)
        .ok()
        .and_then(|text| serde_yaml::from_str::<UsageFile>(&text).ok())
        .unwrap_or_default();
    if file.install_id.is_empty() {
        file.install_id = uuid::Uuid::new_v4().to_string();
    }
    file
}

/// Write atomically so a crash mid-write never corrupts the queue.
fn write_usage_file(path: &Path, file: &UsageFile) {
    let Ok(text) = serde_yaml::to_string(file) else { return };
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let tmp = path.with_extension("yml.tmp");
    if fs::write(&tmp, text).is_ok() && fs::rename(&tmp, path).is_err() {
        let _ = fs::remove_file(&tmp);
    }
}

fn new_usage_record(install_id: &str, now: u64) -> UsageRecord {
    UsageRecord {
        app_session_id: uuid::Uuid::new_v4().to_string(),
        install_id: install_id.to_owned(),
        app_version: env!("CARGO_PKG_VERSION").to_owned(),
        os: env::consts::OS.to_owned(),
        arch: env::consts::ARCH.to_owned(),
        status: "active".to_owned(),
        started_at: now,
        last_seen_at: now,
        ended_at: None,
        duration_s: 0,
        compare_count: 0,
        compare_sessions: Vec::new(),
    }
}

/// Close every open compare tab and mark the record complete.
fn finalize_usage_record(record: &mut UsageRecord, end: u64, status: &str) {
    let end = end.max(record.started_at);
    for compare in &mut record.compare_sessions {
        if compare.closed_at.is_none() {
            let closed = end.max(compare.opened_at);
            compare.closed_at = Some(closed);
            compare.duration_s = closed - compare.opened_at;
        }
    }
    record.status = status.to_owned();
    record.last_seen_at = record.last_seen_at.max(end);
    record.ended_at = Some(end);
    record.duration_s = end - record.started_at;
}

/// Find (or create and count) the entry for a tab.
fn open_compare<'a>(
    record: &'a mut UsageRecord,
    seen: &mut HashSet<String>,
    session_id: &str,
    now: u64,
) -> Option<&'a mut CompareSessionUsage> {
    let id = short_hash(session_id);
    if seen.insert(id.clone()) {
        record.compare_count = record.compare_count.saturating_add(1);
        if record.compare_sessions.len() < MAX_COMPARE_DETAILS {
            record.compare_sessions.push(CompareSessionUsage {
                id: id.clone(),
                kind: "unknown".to_owned(),
                opened_at: now,
                ..Default::default()
            });
        }
    }
    record.compare_sessions.iter_mut().find(|c| c.id == id)
}

fn close_compare(record: &mut UsageRecord, session_id: &str, now: u64) {
    let id = short_hash(session_id);
    if let Some(compare) = record.compare_sessions.iter_mut().find(|c| c.id == id) {
        if compare.closed_at.is_none() {
            let closed = now.max(compare.opened_at);
            compare.closed_at = Some(closed);
            compare.duration_s = closed - compare.opened_at;
        }
    }
}

/// Start tracking this launch. Cheap and non-blocking: all network work runs on
/// background threads. Call once right after `init()`.
pub fn usage_start() {
    if USAGE.get().is_some() || !usage_tracking_enabled() {
        return;
    }
    let Some(url) = tracking_url() else { return };
    let path = app_data_dir().join(USAGE_FILE_NAME);
    let mut file = load_usage_file(&path);
    let now = now_secs();

    // The previous run never reached `usage_shutdown` (crash, kill, power loss):
    // close it at the last heartbeat we saw and queue it for upload.
    if let Some(mut previous) = file.current.take() {
        let end = previous.last_seen_at;
        finalize_usage_record(&mut previous, end, "recovered");
        file.pending.push(previous);
    }
    if file.pending.len() > MAX_PENDING_RECORDS {
        let excess = file.pending.len() - MAX_PENDING_RECORDS;
        file.pending.drain(..excess);
    }

    let record = new_usage_record(&file.install_id, now);
    file.current = Some(record.clone());
    write_usage_file(&path, &file);

    let state = UsageState {
        path,
        url: url.clone(),
        file,
        seen: HashSet::new(),
    };
    if USAGE.set(Mutex::new(state)).is_err() {
        return;
    }

    let _ = thread::Builder::new()
        .name("usage-sync".to_owned())
        .spawn(move || {
            // Start ping first (shows live usage + captures IP/country at real time),
            // then drain anything queued while offline.
            let _ = post_records(&url, std::slice::from_ref(&record), HTTP_TIMEOUT);
            flush_pending(None);
        });
    let _ = thread::Builder::new()
        .name("usage-heartbeat".to_owned())
        .spawn(heartbeat_loop);
}

fn heartbeat_loop() {
    let mut tick: u64 = 0;
    loop {
        for _ in 0..HEARTBEAT_SECS {
            thread::sleep(Duration::from_secs(1));
            if USAGE_SHUTDOWN.load(Ordering::SeqCst) {
                return;
            }
        }
        tick += 1;
        let Some(mutex) = USAGE.get() else { return };
        let (url, snapshot) = {
            let mut state = lock_usage(mutex);
            let now = now_secs();
            let Some(current) = state.file.current.as_mut() else { return };
            current.last_seen_at = now;
            current.duration_s = now.saturating_sub(current.started_at);
            let snapshot = current.clone();
            let url = state.url.clone();
            write_usage_file(&state.path, &state.file);
            (url, snapshot)
        };
        if tick % HEARTBEAT_UPLOAD_EVERY == 0 {
            // Best effort; if it fails the final record still carries everything.
            if let SendOutcome::Sent = post_records(&url, std::slice::from_ref(&snapshot), HTTP_TIMEOUT) {
                flush_pending(None);
            }
        }
    }
}

/// Finish this launch and try (for at most a few seconds) to upload it. Call
/// once on application exit. If the upload fails, the record stays in
/// `session.data.yml` and is sent on a later launch.
pub fn usage_shutdown() {
    let Some(mutex) = USAGE.get() else { return };
    if USAGE_SHUTDOWN.swap(true, Ordering::SeqCst) {
        return;
    }
    {
        let mut state = lock_usage(mutex);
        if let Some(mut current) = state.file.current.take() {
            finalize_usage_record(&mut current, now_secs(), "ended");
            state.file.pending.push(current);
        }
        write_usage_file(&state.path, &state.file);
    }
    flush_pending(Some(Instant::now() + SHUTDOWN_UPLOAD_BUDGET));
}

/// Upload queued records in batches, removing each batch only after the server
/// accepted it. Stops at the first failure so an offline machine costs one timeout.
fn flush_pending(deadline: Option<Instant>) {
    let Some(mutex) = USAGE.get() else { return };
    let _guard = loop {
        match SYNC_LOCK.try_lock() {
            Ok(guard) => break guard,
            Err(TryLockError::Poisoned(poisoned)) => break poisoned.into_inner(),
            Err(TryLockError::WouldBlock) => {
                if deadline.is_some_and(|d| Instant::now() >= d) {
                    return;
                }
                thread::sleep(Duration::from_millis(50));
            }
        }
    };
    loop {
        let (url, batch) = {
            let state = lock_usage(mutex);
            let batch: Vec<UsageRecord> =
                state.file.pending.iter().take(BATCH_SIZE).cloned().collect();
            (state.url.clone(), batch)
        };
        if batch.is_empty() {
            break;
        }
        let timeout = match deadline {
            Some(d) => {
                let left = d.saturating_duration_since(Instant::now());
                if left < Duration::from_millis(200) {
                    break;
                }
                left.min(HTTP_TIMEOUT)
            }
            None => HTTP_TIMEOUT,
        };
        match post_records(&url, &batch, timeout) {
            SendOutcome::Sent | SendOutcome::Rejected => {
                let mut state = lock_usage(mutex);
                state
                    .file
                    .pending
                    .retain(|r| !batch.iter().any(|b| b.app_session_id == r.app_session_id));
                write_usage_file(&state.path, &state.file);
            }
            SendOutcome::Retry => break,
        }
    }
}

fn post_records(url: &str, records: &[UsageRecord], timeout: Duration) -> SendOutcome {
    let mut request = ureq::post(url)
        .timeout(timeout)
        .set("content-type", "application/json");
    if let Some(key) = TRACKING_KEY {
        request = request.set("x-api-key", key);
    }
    match request.send_json(UploadBody { records }) {
        Ok(_) => SendOutcome::Sent,
        // 400/413/422: this payload is bad and always will be, so don't keep it forever.
        Err(ureq::Error::Status(400 | 413 | 422, _)) => SendOutcome::Rejected,
        Err(error) => {
            tracing::debug!(target: "openbc", message = "usage upload deferred", error = %error);
            SendOutcome::Retry
        }
    }
}

/// Register a compare tab together with its type (`file`, `folder`, `image`, ...).
pub fn register_compare_session(session_id: &str, label: &str, kind: &str) {
    register_session(session_id, label);
    let Some(mutex) = USAGE.get() else { return };
    let mut state = lock_usage(mutex);
    let now = now_secs();
    let UsageState { file, seen, .. } = &mut *state;
    let Some(current) = file.current.as_mut() else { return };
    if let Some(compare) = open_compare(current, seen, session_id, now) {
        compare.kind = sanitize_kind(kind);
    }
}

/// Mark a compare tab as closed so its duration stops counting.
pub fn close_session(session_id: &str) {
    let Some(mutex) = USAGE.get() else { return };
    let mut state = lock_usage(mutex);
    if let Some(current) = state.file.current.as_mut() {
        close_compare(current, session_id, now_secs());
    }
}

/// Called from `record` for every instrumented operation.
fn usage_note_operation(session_id: &str, operation: &str, duration: Duration) {
    if session_id == "application" {
        return;
    }
    let Some(mutex) = USAGE.get() else { return };
    let mut state = lock_usage(mutex);
    let now = now_secs();
    let UsageState { file, seen, .. } = &mut *state;
    let Some(current) = file.current.as_mut() else { return };
    current.last_seen_at = now;
    if let Some(compare) = open_compare(current, seen, session_id, now) {
        compare.operations = compare.operations.saturating_add(1);
        compare.busy_ms = compare.busy_ms.saturating_add(duration.as_millis() as u64);
        if compare.kind == "unknown" {
            if let Some(kind) = infer_kind(operation) {
                compare.kind = kind.to_owned();
            }
        }
    }
}

fn timestamp() -> String {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
        .to_string()
}

#[cfg(test)]
mod tests {
    use super::{
        append_event, close_compare, create_tab_file, finalize_usage_record, infer_kind,
        load_usage_file, new_usage_record, open_compare, sanitize_kind, session_scope,
        write_usage_file, InstrumentationEvent, MAX_COMPARE_DETAILS, SESSION_CONTEXT,
    };
    use std::collections::HashSet;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn append_only_reports_keep_each_tabs_events_grouped() {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let directory = std::env::temp_dir().join(format!(
            "openbc-observability-{}-{unique}",
            std::process::id()
        ));
        fs::create_dir_all(&directory).unwrap();
        let mut first_tab = create_tab_file(&directory, "tab-a", "First tab").unwrap();
        let mut second_tab = create_tab_file(&directory, "tab-b", "Second tab").unwrap();
        append_event(&mut first_tab, &event("file_compare"));
        append_event(&mut first_tab, &event("syntax_highlighting"));
        append_event(&mut second_tab, &event("inline_diff"));

        let first_yaml = fs::read_to_string(&first_tab.path).unwrap();
        let first_value: serde_yaml::Value = serde_yaml::from_str(&first_yaml).unwrap();
        let first_events = &first_value["sessions"]["tab-a"]["events"];
        assert_eq!(first_events.as_sequence().unwrap().len(), 2);
        assert_eq!(first_events[0]["operation"], "file_compare");
        assert_eq!(first_events[1]["operation"], "syntax_highlighting");

        let second_yaml = fs::read_to_string(&second_tab.path).unwrap();
        let second_value: serde_yaml::Value = serde_yaml::from_str(&second_yaml).unwrap();
        let second_events = &second_value["sessions"]["tab-b"]["events"];
        assert_eq!(second_events.as_sequence().unwrap().len(), 1);
        assert_eq!(second_events[0]["operation"], "inline_diff");

        drop(first_tab);
        drop(second_tab);
        fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn scoped_session_identity_restores_nested_context() {
        assert_eq!(
            SESSION_CONTEXT.with(|context| context.borrow().clone()),
            None
        );
        let _outer = session_scope("tab-a");
        assert_eq!(
            SESSION_CONTEXT.with(|context| context.borrow().clone()),
            Some("tab-a".to_owned())
        );
        {
            let _inner = session_scope("tab-b");
            assert_eq!(
                SESSION_CONTEXT.with(|context| context.borrow().clone()),
                Some("tab-b".to_owned())
            );
        }
        assert_eq!(
            SESSION_CONTEXT.with(|context| context.borrow().clone()),
            Some("tab-a".to_owned())
        );
    }

    #[test]
    fn crashed_run_is_recovered_at_last_heartbeat() {
        let mut record = new_usage_record("install-1", 1_000);
        let mut seen = HashSet::new();
        open_compare(&mut record, &mut seen, "tab-a", 1_010).unwrap().kind = "file".into();
        record.last_seen_at = 1_300;
        finalize_usage_record(&mut record, 1_300, "recovered");
        assert_eq!(record.status, "recovered");
        assert_eq!(record.duration_s, 300);
        assert_eq!(record.compare_sessions[0].closed_at, Some(1_300));
        assert_eq!(record.compare_sessions[0].duration_s, 290);
    }

    #[test]
    fn compare_sessions_are_counted_once_and_capped() {
        let mut record = new_usage_record("install-1", 0);
        let mut seen = HashSet::new();
        for round in 0..2 {
            for index in 0..(MAX_COMPARE_DETAILS + 20) {
                let _ = open_compare(&mut record, &mut seen, &format!("tab-{index}"), round);
            }
        }
        assert_eq!(record.compare_count as usize, MAX_COMPARE_DETAILS + 20);
        assert_eq!(record.compare_sessions.len(), MAX_COMPARE_DETAILS);
    }

    #[test]
    fn closing_a_tab_freezes_its_duration() {
        let mut record = new_usage_record("install-1", 0);
        let mut seen = HashSet::new();
        open_compare(&mut record, &mut seen, "tab-a", 10);
        close_compare(&mut record, "tab-a", 70);
        finalize_usage_record(&mut record, 500, "ended");
        assert_eq!(record.compare_sessions[0].duration_s, 60);
        assert_eq!(record.duration_s, 500);
    }

    #[test]
    fn usage_file_round_trips_and_never_contains_tab_ids() {
        let dir = std::env::temp_dir().join(format!("openbc-usage-{}", std::process::id()));
        let path = dir.join("session.data.yml");
        let mut file = load_usage_file(&path);
        assert!(!file.install_id.is_empty());
        let mut record = new_usage_record(&file.install_id, 5);
        let mut seen = HashSet::new();
        open_compare(&mut record, &mut seen, "/home/me/secret/file.txt", 6);
        finalize_usage_record(&mut record, 20, "ended");
        file.pending.push(record);
        write_usage_file(&path, &file);
        let text = fs::read_to_string(&path).unwrap();
        assert!(!text.contains("secret"));
        let loaded = load_usage_file(&path);
        assert_eq!(loaded.install_id, file.install_id);
        assert_eq!(loaded.pending.len(), 1);
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn kind_is_sanitized_and_inferred() {
        assert_eq!(sanitize_kind(" Folder Compare!! "), "foldercompare");
        assert_eq!(sanitize_kind("***"), "unknown");
        assert_eq!(infer_kind("folder_compare"), Some("folder"));
        assert_eq!(infer_kind("syntax_highlighting"), None);
    }

    fn event(operation: &str) -> InstrumentationEvent {
        InstrumentationEvent {
            timestamp: "event-time".to_owned(),
            module: "openbc-core".to_owned(),
            operation: operation.to_owned(),
            duration_ms: 1,
            left_bytes: None,
            right_bytes: None,
            left_lines: None,
            right_lines: None,
            folder_bytes: None,
            nested_level: None,
            resource: None,
        }
    }
}
//! Centralized application logging and performance instrumentation.

use serde::Serialize;
use std::cell::RefCell;
use std::collections::BTreeMap;
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{Seek, SeekFrom, Write};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
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

/// Initialize the process-wide log sink and one YAML instrumentation session.
pub fn init() -> Result<(), String> {
    let username = env::var("USER").unwrap_or_else(|_| "unknown".to_string());
    let log_path = PathBuf::from(format!("/var/log/open-bc-{username}.log"));
    let (log_file, active_log_path) =
        match OpenOptions::new().create(true).append(true).open(&log_path) {
            Ok(file) => (file, log_path.clone()),
            Err(_) => {
                let fallback = home_dir().join(".config/open-bc");
                fs::create_dir_all(&fallback)
                    .map_err(|error| format!("cannot create {}: {error}", fallback.display()))?;
                let fallback_path = fallback.join(format!("open-bc-{username}.log"));
                let file = OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(&fallback_path)
                    .map_err(|error| {
                        format!(
                            "cannot open {} or its user fallback: {error}",
                            log_path.display()
                        )
                    })?;
                (file, fallback_path)
            }
        };
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

    let instrumentation_dir = home_dir().join(".config/open-bc/instrumentation");
    fs::create_dir_all(&instrumentation_dir)
        .map_err(|error| format!("cannot create {}: {error}", instrumentation_dir.display()))?;
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

/// Return the current user's OpenBC log path.
pub fn log_path() -> PathBuf {
    if let Some(path) = ACTIVE_LOG_PATH.get() {
        return path.clone();
    }
    let username = env::var("USER").unwrap_or_else(|_| "unknown".to_string());
    PathBuf::from(format!("/var/log/open-bc-{username}.log"))
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
    let Some(session) = SESSION.get() else { return };
    let session_id = SESSION_CONTEXT
        .with(|context| context.borrow().clone())
        .filter(|session_id| !session_id.is_empty())
        .unwrap_or_else(|| "application".to_owned());
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

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
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
        append_event, create_tab_file, session_scope, InstrumentationEvent, SESSION_CONTEXT,
    };
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

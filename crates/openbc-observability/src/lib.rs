//! Centralized application logging and performance instrumentation.

use serde::Serialize;
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tracing_appender::non_blocking::WorkerGuard;

static SESSION: OnceLock<InstrumentationSession> = OnceLock::new();
static LOG_GUARD: OnceLock<WorkerGuard> = OnceLock::new();
static ACTIVE_LOG_PATH: OnceLock<PathBuf> = OnceLock::new();

#[derive(Debug, Serialize)]
struct InstrumentationEvent<'a> {
    timestamp: String,
    module: &'a str,
    operation: &'a str,
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
    path: PathBuf,
    file: Mutex<File>,
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
    let session_path = instrumentation_dir.join(format!("{}.yml", timestamp()));
    let mut file = File::create(&session_path)
        .map_err(|error| format!("cannot create {}: {error}", session_path.display()))?;
    writeln!(file, "session_started: {}\nevents:", timestamp())
        .map_err(|error| format!("cannot initialize instrumentation file: {error}"))?;
    SESSION
        .set(InstrumentationSession {
            path: session_path,
            file: Mutex::new(file),
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

/// Return the active session's YAML report path.
pub fn instrumentation_path() -> Option<PathBuf> {
    SESSION.get().map(|session| session.path.clone())
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
    let event = InstrumentationEvent {
        timestamp: timestamp(),
        module,
        operation,
        duration_ms: duration.as_millis(),
        left_bytes: sizes.map(|value| value.0),
        right_bytes: sizes.map(|value| value.1),
        left_lines: lines.map(|value| value.0),
        right_lines: lines.map(|value| value.1),
        folder_bytes,
        nested_level,
        resource,
    };
    if let Ok(serialized) = serde_yaml::to_string(&event) {
        if let Ok(mut file) = session.file.lock() {
            let mut lines = serialized.lines();
            if let Some(first) = lines.next() {
                let _ = writeln!(file, "  - {first}");
            }
            for line in lines {
                let _ = writeln!(file, "    {line}");
            }
            let _ = file.flush();
        }
    }
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

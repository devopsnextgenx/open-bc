//! Media compare actor: header/tag metadata only (never the audio/video payload).

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use openbc_compute::media::read_media_metadata;
use openbc_core::meta_compare::{diff_meta, MetaDiffRow, MetaRow};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

#[derive(Debug)]
pub enum MediaCommand {
    Reload,
}

#[derive(Debug)]
pub struct MediaView {
    pub rows: Vec<MetaDiffRow>,
    pub errors: [Option<String>; 2],
    pub present: [bool; 2],
}

#[derive(Debug)]
pub enum MediaEvent {
    Status(String),
    Result(Arc<MediaView>),
}

#[derive(Clone)]
pub struct MediaCompareHandle {
    tx: mpsc::Sender<MediaCommand>,
}

impl MediaCompareHandle {
    pub fn send(&self, command: MediaCommand) -> bool {
        self.tx.try_send(command).is_ok()
    }
}

pub fn spawn_media_compare(rt: &Handle, source: Arc<dyn ByteSource>, left: PathBuf, right: PathBuf) -> (MediaCompareHandle, mpsc::Receiver<MediaEvent>) {
    let (ctx, mut crx) = mpsc::channel(COMMAND_CHANNEL_CAPACITY);
    let (etx, erx) = mpsc::channel(EVENT_CHANNEL_CAPACITY);
    rt.spawn(async move {
        loop {
            if etx.send(MediaEvent::Status("Reading media headers...".into())).await.is_err() {
                return;
            }
            // Both sides load concurrently; a failure on one side leaves the other intact.
            let (l, r) = tokio::join!(load_side(&source, &left), load_side(&source, &right));
            let diff_rows = rayon_run({
                let (a, b) = (l.0.clone(), r.0.clone());
                move || diff_meta(&a, &b)
            })
            .await;
            let Ok(rows) = diff_rows else { return };
            let view = MediaView {
                rows,
                errors: [l.1, r.1],
                present: [!left.as_os_str().is_empty(), !right.as_os_str().is_empty()],
            };
            if etx.send(MediaEvent::Result(Arc::new(view))).await.is_err() {
                return;
            }
            // One reload at a time; extra queued reloads collapse into one.
            if crx.recv().await.is_none() {
                return;
            }
            while crx.try_recv().is_ok() {}
        }
    });
    (MediaCompareHandle { tx: ctx }, erx)
}

async fn load_side(source: &Arc<dyn ByteSource>, path: &Path) -> (Vec<MetaRow>, Option<String>) {
    if path.as_os_str().is_empty() {
        return (Vec::new(), None);
    }
    let file_row = |key: &str, value: String| MetaRow { group: "File".into(), key: key.into(), value };
    let mut rows = vec![file_row("Name", path.file_name().map_or_else(String::new, |n| n.to_string_lossy().into_owned()))];
    match source.stat(path).await {
        Ok(st) => {
            rows.push(file_row("Size", format!("{} ({} bytes)", human_size(st.size), st.size)));
            if let Some(t) = st.modified {
                rows.push(file_row("Modified", format_utc(t)));
            }
        }
        Err(e) => return (rows, Some(context(path, "stat", e))),
    }
    let (src, p) = (source.clone(), path.to_path_buf());
    let parsed = rayon_run(move || match src.open_reader(&p) {
        Ok(mut reader) => read_media_metadata(&mut *reader).map_err(|e| context(&p, "read metadata", e)),
        Err(e) => Err(context(&p, "open", e)),
    })
    .await;
    match parsed {
        Ok(Ok(mut meta)) => {
            rows.append(&mut meta);
            (rows, None)
        }
        Ok(Err(message)) => (rows, Some(message)),
        Err(e) => (rows, Some(e.to_string())),
    }
}

fn human_size(bytes: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KiB", "MiB", "GiB", "TiB"];
    let (mut v, mut u) = (bytes as f64, 0);
    while v >= 1024.0 && u < UNITS.len() - 1 {
        v /= 1024.0;
        u += 1;
    }
    if u == 0 { format!("{bytes} B") } else { format!("{v:.1} {}", UNITS[u]) }
}

/// `YYYY-MM-DD HH:MM:SS UTC` (civil-from-days, no time-zone database needed).
fn format_utc(t: SystemTime) -> String {
    let secs = t.duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64);
    let (days, rem) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02} {:02}:{:02}:{:02} UTC", rem / 3600, rem / 60 % 60, rem % 60)
}

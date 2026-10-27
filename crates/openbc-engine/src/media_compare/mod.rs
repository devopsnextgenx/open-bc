//! Media compare actor: header/tag metadata plus optional audio waveform preparation.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use async_trait::async_trait;
use openbc_compute::{audio_compare::{AudioComparison, AudioTrack}, media::read_media_metadata};
use openbc_core::meta_compare::{diff_meta, MetaDiffRow, MetaRow};
use tokio::runtime::Handle;
use tokio::sync::mpsc;

use crate::common::*;

mod format;
mod write;

use format::{format_utc, human_size};
pub use write::is_editable;

/// One tag change; `value: None` removes the tag. `group`/`key` are the row's display names.
#[derive(Debug, Clone)]
pub struct TagEdit {
    pub group: String,
    pub key: String,
    pub value: Option<String>,
}

#[derive(Clone)]
pub enum CoverEdit {
    Keep,
    Replace(Vec<u8>),
    Remove,
}

// Manual Debug so logging a command never dumps image bytes.
impl std::fmt::Debug for CoverEdit {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Keep => f.write_str("Keep"),
            Self::Replace(b) => write!(f, "Replace({} bytes)", b.len()),
            Self::Remove => f.write_str("Remove"),
        }
    }
}

/// Edits for one side (0 left, 1 right); the file is rewritten, then both sides are re-read.
#[derive(Debug, Clone)]
pub struct MediaSave {
    pub side: usize,
    pub edits: Vec<TagEdit>,
    pub cover: CoverEdit,
}

#[derive(Debug)]
pub enum MediaCommand {
    Reload,
    Save(MediaSave),
}

pub struct MediaView {
    pub rows: Vec<MetaDiffRow>,
    pub errors: [Option<String>; 2],
    pub present: [bool; 2],
    /// Raw bytes of each side's first embedded picture (cover art), if any.
    pub covers: [Option<Vec<u8>>; 2],
    /// Waveform peaks for audio comparisons, absent for non-audio files.
    pub audio: Option<AudioComparisonView>,
    /// Audio decoding/comparison error, separate from metadata errors.
    pub audio_error: Option<String>,
}

/// A multiresolution waveform suitable for drawing without decoding audio in the UI.
#[derive(Debug, Clone)]
pub struct AudioWaveform {
    pub frame_count: u64,
    pub sample_rate: u32,
    pub levels: Vec<AudioPeakLevel>,
}

#[derive(Debug, Clone)]
pub struct AudioPeakLevel {
    pub frames_per_bin: u64,
    pub bins: Vec<AudioPeak>,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct AudioPeak {
    pub minimum: f32,
    pub maximum: f32,
}

/// The two source waveforms and their shared-prefix absolute-difference waveform.
#[derive(Debug, Clone)]
pub struct AudioComparisonView {
    pub waveforms: [AudioWaveform; 3],
}

// Manual Debug so logging a view never dumps image bytes.
impl std::fmt::Debug for MediaView {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MediaView")
            .field("rows", &self.rows.len())
            .field("errors", &self.errors)
            .field("present", &self.present)
            .field("covers", &self.covers.each_ref().map(|c| c.as_ref().map(Vec::len)))
            .field("audio", &self.audio.is_some())
            .field("audio_error", &self.audio_error)
            .finish()
    }
}

#[derive(Debug)]
pub enum MediaEvent {
    Status(String),
    Result(Arc<MediaView>),
}

/// Non-blocking command sender; `send` returns `false` when the actor is busy or gone.
pub type MediaCompareHandle = CommandHandle<MediaCommand>;

pub fn spawn_media_compare(rt: &Handle, source: Arc<dyn ByteSource>, left: PathBuf, right: PathBuf) -> (MediaCompareHandle, mpsc::Receiver<MediaEvent>) {
    spawn_batched(rt, |etx| Actor { source, paths: [left, right], save_errors: [None, None], etx })
}

/// Everything loaded for one side.
struct LoadedSide {
    rows: Vec<MetaRow>,
    cover: Option<Vec<u8>>,
    error: Option<String>,
}

impl LoadedSide {
    fn empty() -> Self {
        Self { rows: Vec::new(), cover: None, error: None }
    }

    fn failed(rows: Vec<MetaRow>, error: String) -> Self {
        Self { rows, cover: None, error: Some(error) }
    }
}

struct Actor {
    source: Arc<dyn ByteSource>,
    paths: [PathBuf; 2],
    /// "Save failed: ..." per side, shown by the UI in that side's error line after the re-read.
    save_errors: [Option<String>; 2],
    etx: mpsc::Sender<MediaEvent>,
}

// There is no separate load step: every batch of commands (including the very first, empty
// one) ends in `publish`, which re-reads both files. Queued reloads therefore collapse into
// one re-read while saves still run in order.
#[async_trait]
impl BatchedActor for Actor {
    type Command = MediaCommand;

    async fn init(&mut self) -> bool {
        true
    }

    async fn apply(&mut self, command: MediaCommand) -> bool {
        if let MediaCommand::Save(save) = command {
            self.save(save).await;
        }
        true
    }

    async fn publish(&mut self) -> bool {
        let audio_paths = is_audio_path(&self.paths[0]) && is_audio_path(&self.paths[1]);
        let status = if audio_paths {
            "Reading media headers and preparing audio visualization..."
        } else {
            "Reading media headers..."
        };
        if !send_event(&self.etx, MediaEvent::Status(status.into())).await {
            return false;
        }
        // Metadata and audio decoding run concurrently; audio decoding stays off Tokio workers.
        let audio_job = async {
            if !audio_paths { return (None, None); }
            let paths = self.paths.clone();
            match rayon_run(move || load_audio_comparison(&paths[0], &paths[1])).await {
                Ok(Ok(audio)) => (Some(audio), None),
                Ok(Err(error)) => (None, Some(error)),
                Err(error) => (None, Some(error.to_string())),
            }
        };
        let (l, r, (audio, audio_error)) = tokio::join!(
            load_side(&self.source, &self.paths[0]),
            load_side(&self.source, &self.paths[1]),
            audio_job
        );
        let diff = rayon_run({
            let (a, b) = (l.rows.clone(), r.rows.clone());
            move || diff_meta(&a, &b)
        })
        .await;
        let Ok(rows) = diff else { return false };
        let view = MediaView {
            rows,
            errors: [self.save_errors[0].take().or(l.error), self.save_errors[1].take().or(r.error)],
            present: [!is_unset(&self.paths[0]), !is_unset(&self.paths[1])],
            covers: [l.cover, r.cover],
            audio,
            audio_error,
        };
        send_event(&self.etx, MediaEvent::Result(Arc::new(view))).await
    }
}

fn is_audio_path(path: &Path) -> bool {
    matches!(
        path.extension().and_then(|extension| extension.to_str()).map(str::to_ascii_lowercase).as_deref(),
        Some("aac" | "aif" | "aiff" | "alac" | "flac" | "m4a" | "mp3" | "oga" | "ogg" | "opus" | "wav")
    )
}

fn load_audio_comparison(left_path: &Path, right_path: &Path) -> Result<AudioComparisonView, String> {
    let comparison = AudioComparison::from_files(left_path, right_path).map_err(|error| error.to_string())?;
    let left = waveform_for_track(comparison.left()).map_err(|error| error.to_string())?;
    let right = waveform_for_track(comparison.right()).map_err(|error| error.to_string())?;
    let delta = delta_waveform(&comparison);
    Ok(AudioComparisonView { waveforms: [left, right, delta] })
}

fn waveform_for_track(track: &AudioTrack) -> Result<AudioWaveform, openbc_compute::audio_compare::AudioCompareError> {
    const BIN_SIZES: [usize; 3] = [100, 1_000, 10_000];
    let levels = BIN_SIZES.into_iter().map(|frames_per_bin| {
        Ok(AudioPeakLevel {
            frames_per_bin: frames_per_bin as u64,
            bins: track.get_peaks_in_range(0, track.frame_count(), frames_per_bin)?
                .into_iter().map(|(minimum, maximum)| AudioPeak { minimum, maximum }).collect(),
        })
    }).collect::<Result<_, openbc_compute::audio_compare::AudioCompareError>>()?;
    Ok(AudioWaveform { frame_count: track.frame_count() as u64, sample_rate: track.sample_rate(), levels })
}

fn delta_waveform(comparison: &AudioComparison) -> AudioWaveform {
    delta_waveform_from_samples(
        comparison.delta_buffer(), comparison.left().channels(),
        comparison.compared_frame_count(), comparison.left().sample_rate(),
    )
}

fn delta_waveform_from_samples(delta: &[f32], channels: usize, frame_count: usize, sample_rate: u32) -> AudioWaveform {
    const BIN_SIZES: [usize; 3] = [100, 1_000, 10_000];
    let levels = BIN_SIZES.into_iter().map(|frames_per_bin| {
        let bins = (0..frame_count.div_ceil(frames_per_bin)).map(|bin| {
            let first_frame = bin * frames_per_bin;
            let end_frame = (first_frame + frames_per_bin).min(frame_count);
            let samples = &delta[first_frame * channels..end_frame * channels];
            AudioPeak {
                minimum: samples.iter().copied().fold(f32::INFINITY, f32::min),
                maximum: samples.iter().copied().fold(f32::NEG_INFINITY, f32::max),
            }
        }).collect();
        AudioPeakLevel { frames_per_bin: frames_per_bin as u64, bins }
    }).collect();
    AudioWaveform { frame_count: frame_count as u64, sample_rate, levels }
}

#[cfg(test)]
mod audio_visualization_tests {
    use super::{delta_waveform_from_samples, is_audio_path};
    use std::path::Path;

    #[test]
    fn audio_paths_are_matched_case_insensitively_without_classifying_video_as_audio() {
        assert!(is_audio_path(Path::new("take.MP3")));
        assert!(is_audio_path(Path::new("take.flac")));
        assert!(!is_audio_path(Path::new("take.mkv")));
        assert!(!is_audio_path(Path::new("no-extension")));
    }

    #[test]
    fn delta_waveform_aggregates_all_channels_and_keeps_the_shared_frame_count() {
        let waveform = delta_waveform_from_samples(&[0.1, 0.7, 0.3, 0.9], 2, 2, 48_000);
        assert_eq!(waveform.frame_count, 2);
        assert_eq!(waveform.sample_rate, 48_000);
        assert_eq!(waveform.levels.len(), 3);
        assert_eq!(waveform.levels[0].frames_per_bin, 100);
        assert_eq!(waveform.levels[0].bins.len(), 1);
        assert!((waveform.levels[0].bins[0].minimum - 0.1).abs() < f32::EPSILON);
        assert!((waveform.levels[0].bins[0].maximum - 0.9).abs() < f32::EPSILON);
    }
}

impl Actor {
    async fn save(&mut self, save: MediaSave) {
        let side = usize::from(save.side != 0);
        let path = self.paths[side].clone();
        let result = if is_unset(&path) {
            Err("no file selected".to_string())
        } else {
            rayon_run(move || write::apply(&path, &save)).await.map_err(|e| e.to_string()).and_then(|r| r)
        };
        self.save_errors[side] = result.err().map(|e| format!("Save failed: {e}"));
    }
}

async fn load_side(source: &Arc<dyn ByteSource>, path: &Path) -> LoadedSide {
    if is_unset(path) {
        return LoadedSide::empty();
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
        Err(e) => return LoadedSide::failed(rows, context(path, "stat", e)),
    }
    let (src, p) = (source.clone(), path.to_path_buf());
    let parsed = rayon_run(move || match src.open_reader(&p) {
        Ok(mut reader) => read_media_metadata(&mut *reader).map_err(|e| context(&p, "read metadata", e)),
        Err(e) => Err(context(&p, "open", e)),
    })
    .await;
    match parsed {
        Ok(Ok(mut info)) => {
            rows.append(&mut info.rows);
            LoadedSide { rows, cover: info.cover, error: None }
        }
        Ok(Err(message)) => LoadedSide::failed(rows, message),
        Err(e) => LoadedSide::failed(rows, e.to_string()),
    }
}

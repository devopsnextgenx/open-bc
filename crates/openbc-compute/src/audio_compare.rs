//! Audio decoding and waveform data preparation for comparison UIs.
//!
//! Audio files are decoded once to normalized interleaved `f32` PCM. Each track stores
//! three min/max peak levels (100, 1,000 and 10,000 frames per bin), so a viewport query
//! walks only the bins it needs instead of rescanning the full PCM buffer. At most the two
//! boundary bins are rescanned to keep range queries exact.
//!
//! ```no_run
//! use openbc_compute::audio_compare::AudioComparison;
//! # fn example() -> Result<(), Box<dyn std::error::Error>> {
//! let comparison = AudioComparison::from_files("reference.wav", "take.wav")?;
//! let peaks = comparison.left().get_peaks_in_range(0, 48_000, 1_000)?;
//! let channel_deltas = comparison.delta_buffer();
//! # let _ = (peaks, channel_deltas);
//! # Ok(())
//! # }
//! ```

use std::{
    fs::File,
    io,
    path::{Path, PathBuf},
};

use symphonia::{
    core::{
        audio::SampleBuffer,
        codecs::DecoderOptions,
        errors::Error as SymphoniaError,
        formats::FormatOptions,
        io::{MediaSourceStream, MediaSourceStreamOptions},
        meta::MetadataOptions,
        probe::Hint,
    },
    default::{get_codecs, get_probe},
};

const PEAK_BIN_SIZES: [usize; 3] = [100, 1_000, 10_000];

/// Errors returned while decoding or preparing a pair of audio tracks.
#[derive(Debug, thiserror::Error)]
pub enum AudioCompareError {
    /// Opening the requested audio file failed.
    #[error("failed to open audio file '{path}': {source}")]
    OpenFile {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    /// Symphonia could not identify the file's container.
    #[error("failed to identify audio format for '{path}': {message}")]
    Probe { path: PathBuf, message: String },
    /// The file has no decodable audio track.
    #[error("no audio track found in '{0}'")]
    NoAudioTrack(PathBuf),
    /// A packet could not be decoded.
    #[error("failed to decode audio file '{path}': {message}")]
    Decode { path: PathBuf, message: String },
    /// The decoded stream changed channel layout or sample rate mid-file.
    #[error("audio stream parameters changed while decoding '{0}'")]
    StreamParametersChanged(PathBuf),
    /// Decoded PCM cannot be represented by a frame-aligned interleaved buffer.
    #[error("invalid interleaved PCM buffer: {sample_count} samples for {channels} channels")]
    InvalidPcm {
        sample_count: usize,
        channels: usize,
    },
    /// A decoded sample was not finite.
    #[error("non-finite decoded sample at interleaved sample {sample_index} in '{path}'")]
    NonFiniteSample { path: PathBuf, sample_index: usize },
    /// Both files must use the same sample rate to compare corresponding frames.
    #[error("sample rates differ: left is {left} Hz, right is {right} Hz")]
    SampleRateMismatch { left: u32, right: u32 },
    /// Both files must have the same channel count for per-channel delta comparison.
    #[error("channel counts differ: left has {left}, right has {right}")]
    ChannelCountMismatch { left: usize, right: usize },
    /// A zero-sized zoom level does not have a meaningful samples-per-pixel interpretation.
    #[error("zoom level must be greater than zero (samples per pixel)")]
    InvalidZoomLevel,
}

/// Decoded audio and its precomputed min/max waveform pyramid.
///
/// The PCM data is retained because exact partial-bin queries need to inspect only the
/// two edges of a requested range. Peak data is computed over all channels for each frame.
#[derive(Debug)]
pub struct AudioTrack {
    samples: Vec<f32>,
    sample_rate: u32,
    channels: usize,
    frames: usize,
    peaks: PeakPyramid,
}

impl AudioTrack {
    /// Decodes one audio file and precomputes its waveform peak pyramid.
    ///
    /// The complete decoded PCM buffer is retained in memory. For long recordings, callers
    /// should perform this work off the UI thread and account for approximately
    /// `4 * frames * channels` bytes of PCM plus the compact peak levels.
    pub fn load(path: impl AsRef<Path>) -> Result<Self, AudioCompareError> {
        let decoded = decode_audio(path.as_ref())?;
        let frames = decoded.samples.len() / decoded.channels;
        let peaks = PeakPyramid::build(&decoded.samples, decoded.channels, frames);
        Ok(Self {
            samples: decoded.samples,
            sample_rate: decoded.sample_rate,
            channels: decoded.channels,
            frames,
            peaks,
        })
    }

    /// Sample rate of the decoded stream, in samples per second.
    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// Number of interleaved channels in each audio frame.
    pub fn channels(&self) -> usize {
        self.channels
    }

    /// Number of decoded frames (one sample per channel).
    pub fn frame_count(&self) -> usize {
        self.frames
    }

    pub(crate) fn interleaved_samples(&self) -> &[f32] {
        &self.samples
    }

    /// Returns min/max amplitude pairs for the half-open frame range `[start_frame, end_frame)`.
    ///
    /// `zoom_level` is the desired number of source frames per rendered pixel. The closest
    /// available pyramid level no larger than that value is selected (or the finest/coarsest
    /// level when the requested zoom is outside the precomputed range). Bins intersecting
    /// partial range boundaries are calculated from PCM directly; interior bins are constant
    /// time lookups. `end_frame` is clamped to the track length.
    ///
    /// The returned `Vec` contains one `(min, max)` pair per intersected bin, including
    /// exact partial bins at the range boundaries.
    pub fn get_peaks_in_range(
        &self,
        start_frame: usize,
        end_frame: usize,
        zoom_level: usize,
    ) -> Result<Vec<(f32, f32)>, AudioCompareError> {
        self.peaks.get_peaks_in_range(
            &self.samples,
            self.channels,
            self.frames,
            start_frame,
            end_frame,
            zoom_level,
        )
    }
}

/// Two decoded tracks plus their synchronized absolute-difference buffer.
///
/// Pair construction requires matching sample rates and channel counts. Tracks may have
/// different durations; the delta buffer covers their shared prefix only.
#[derive(Debug)]
pub struct AudioComparison {
    left: AudioTrack,
    right: AudioTrack,
    compared_frames: usize,
    delta_buffer: Vec<f32>,
}

impl AudioComparison {
    /// Decodes two audio files and computes `|left - right|` for every shared PCM sample.
    ///
    /// The tracks are compared frame-for-frame from their beginnings. No resampling,
    /// channel remapping, or time alignment is performed.
    pub fn from_files(
        left_path: impl AsRef<Path>,
        right_path: impl AsRef<Path>,
    ) -> Result<Self, AudioCompareError> {
        let left = AudioTrack::load(left_path)?;
        let right = AudioTrack::load(right_path)?;
        Self::from_tracks(left, right)
    }

    fn from_tracks(left: AudioTrack, right: AudioTrack) -> Result<Self, AudioCompareError> {
        if left.sample_rate != right.sample_rate {
            return Err(AudioCompareError::SampleRateMismatch {
                left: left.sample_rate,
                right: right.sample_rate,
            });
        }
        if left.channels != right.channels {
            return Err(AudioCompareError::ChannelCountMismatch {
                left: left.channels,
                right: right.channels,
            });
        }

        let compared_frames = left.frames.min(right.frames);
        let compared_samples = compared_frames * left.channels;
        let delta_buffer = left.samples[..compared_samples]
            .iter()
            .zip(&right.samples[..compared_samples])
            .map(|(a, b)| (a - b).abs())
            .collect();

        Ok(Self {
            left,
            right,
            compared_frames,
            delta_buffer,
        })
    }

    /// The left-hand decoded track.
    pub fn left(&self) -> &AudioTrack {
        &self.left
    }

    /// The right-hand decoded track.
    pub fn right(&self) -> &AudioTrack {
        &self.right
    }

    /// Number of frames present in both tracks.
    pub fn compared_frame_count(&self) -> usize {
        self.compared_frames
    }

    /// Absolute differences in interleaved channel order for the shared frame prefix.
    ///
    /// The returned slice contains `compared_frame_count() * channels()` values.
    pub fn delta_buffer(&self) -> &[f32] {
        &self.delta_buffer
    }
}

#[derive(Debug)]
struct PeakPyramid {
    levels: Vec<PeakLevel>,
}

#[derive(Debug)]
struct PeakLevel {
    frames_per_bin: usize,
    bins: Vec<(f32, f32)>,
}

impl PeakPyramid {
    fn build(samples: &[f32], channels: usize, frames: usize) -> Self {
        let levels = PEAK_BIN_SIZES
            .into_iter()
            .map(|frames_per_bin| PeakLevel {
                frames_per_bin,
                bins: (0..frames.div_ceil(frames_per_bin))
                    .map(|bin| {
                        let start = bin * frames_per_bin;
                        let end = (start + frames_per_bin).min(frames);
                        min_max_for_frames(samples, channels, start, end)
                    })
                    .collect(),
            })
            .collect();
        Self { levels }
    }

    fn get_peaks_in_range(
        &self,
        samples: &[f32],
        channels: usize,
        frames: usize,
        start_frame: usize,
        end_frame: usize,
        zoom_level: usize,
    ) -> Result<Vec<(f32, f32)>, AudioCompareError> {
        if zoom_level == 0 {
            return Err(AudioCompareError::InvalidZoomLevel);
        }

        let start = start_frame.min(frames);
        let end = end_frame.min(frames);
        if start >= end {
            return Ok(Vec::new());
        }

        let level = self
            .levels
            .iter()
            .rev()
            .find(|level| level.frames_per_bin <= zoom_level)
            .unwrap_or(&self.levels[0]);
        let first_bin = start / level.frames_per_bin;
        let last_bin = (end - 1) / level.frames_per_bin;
        let mut peaks = Vec::with_capacity(last_bin - first_bin + 1);

        for bin_index in first_bin..=last_bin {
            let bin_start = bin_index * level.frames_per_bin;
            let bin_end = (bin_start + level.frames_per_bin).min(frames);
            let overlap_start = start.max(bin_start);
            let overlap_end = end.min(bin_end);
            if overlap_start == bin_start && overlap_end == bin_end {
                peaks.push(level.bins[bin_index]);
            } else {
                peaks.push(min_max_for_frames(
                    samples,
                    channels,
                    overlap_start,
                    overlap_end,
                ));
            }
        }

        Ok(peaks)
    }
}

fn min_max_for_frames(
    samples: &[f32],
    channels: usize,
    start_frame: usize,
    end_frame: usize,
) -> (f32, f32) {
    let start = start_frame * channels;
    let end = end_frame * channels;
    let mut min = f32::INFINITY;
    let mut max = f32::NEG_INFINITY;
    for &sample in &samples[start..end] {
        min = min.min(sample);
        max = max.max(sample);
    }
    (min, max)
}

struct DecodedAudio {
    samples: Vec<f32>,
    sample_rate: u32,
    channels: usize,
}

fn decode_audio(path: &Path) -> Result<DecodedAudio, AudioCompareError> {
    let file = File::open(path).map_err(|source| AudioCompareError::OpenFile {
        path: path.to_path_buf(),
        source,
    })?;
    let source = MediaSourceStream::new(Box::new(file), MediaSourceStreamOptions::default());
    let mut hint = Hint::new();
    if let Some(extension) = path.extension().and_then(|extension| extension.to_str()) {
        hint.with_extension(extension);
    }

    let probed = get_probe()
        .format(
            &hint,
            source,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .map_err(|error| AudioCompareError::Probe {
            path: path.to_path_buf(),
            message: error.to_string(),
        })?;
    let mut format = probed.format;
    let (track_id, codec_params) = {
        let track = format
            .tracks()
            .iter()
            .find(|track| track.codec_params.channels.is_some())
            .ok_or_else(|| AudioCompareError::NoAudioTrack(path.to_path_buf()))?;
        (track.id, track.codec_params.clone())
    };
    let mut decoder = get_codecs()
        .make(&codec_params, &DecoderOptions::default())
        .map_err(|error| AudioCompareError::Decode {
            path: path.to_path_buf(),
            message: error.to_string(),
        })?;

    let mut samples = Vec::new();
    let mut stream_parameters = None;
    loop {
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(SymphoniaError::IoError(error)) if error.kind() == io::ErrorKind::UnexpectedEof => {
                break
            }
            Err(error) => {
                return Err(AudioCompareError::Decode {
                    path: path.to_path_buf(),
                    message: error.to_string(),
                });
            }
        };
        if packet.track_id() != track_id {
            continue;
        }

        let decoded = match decoder.decode(&packet) {
            Ok(decoded) => decoded,
            Err(SymphoniaError::ResetRequired) => {
                decoder.reset();
                continue;
            }
            Err(error) => {
                return Err(AudioCompareError::Decode {
                    path: path.to_path_buf(),
                    message: error.to_string(),
                });
            }
        };
        let sample_rate = decoded.spec().rate;
        let channels = decoded.spec().channels.count();
        if channels == 0 {
            return Err(AudioCompareError::InvalidPcm {
                sample_count: 0,
                channels,
            });
        }
        match stream_parameters {
            Some((expected_rate, expected_channels))
                if expected_rate != sample_rate || expected_channels != channels =>
            {
                return Err(AudioCompareError::StreamParametersChanged(
                    path.to_path_buf(),
                ));
            }
            None => stream_parameters = Some((sample_rate, channels)),
            _ => {}
        }

        let mut buffer = SampleBuffer::<f32>::new(decoded.capacity() as u64, *decoded.spec());
        buffer.copy_interleaved_ref(decoded);
        let converted = buffer.samples();
        if converted.len() % channels != 0 {
            return Err(AudioCompareError::InvalidPcm {
                sample_count: converted.len(),
                channels,
            });
        }
        let previous_len = samples.len();
        samples.extend_from_slice(converted);
        for (offset, sample) in samples[previous_len..].iter().enumerate() {
            if !sample.is_finite() {
                return Err(AudioCompareError::NonFiniteSample {
                    path: path.to_path_buf(),
                    sample_index: previous_len + offset,
                });
            }
        }
    }

    let (sample_rate, channels) =
        stream_parameters.ok_or_else(|| AudioCompareError::NoAudioTrack(path.to_path_buf()))?;
    if samples.len() % channels != 0 {
        return Err(AudioCompareError::InvalidPcm {
            sample_count: samples.len(),
            channels,
        });
    }

    Ok(DecodedAudio {
        samples,
        sample_rate,
        channels,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs,
        io::Write,
        sync::atomic::{AtomicUsize, Ordering},
    };

    static TEMP_FILE_ID: AtomicUsize = AtomicUsize::new(0);

    #[test]
    fn range_query_returns_exact_partial_bins_and_selects_resolution() {
        let samples = (0..10_050)
            .map(|index| (index as f32 / 10_050.0).sin())
            .collect::<Vec<_>>();
        let track = AudioTrack {
            frames: samples.len(),
            peaks: PeakPyramid::build(&samples, 1, samples.len()),
            samples,
            sample_rate: 48_000,
            channels: 1,
        };

        let peaks = track.get_peaks_in_range(99, 101, 100).unwrap();
        assert_eq!(peaks.len(), 2);
        assert_eq!(peaks[0], min_max_for_frames(&track.samples, 1, 99, 100));
        assert_eq!(peaks[1], min_max_for_frames(&track.samples, 1, 100, 101));
        assert_eq!(
            track
                .get_peaks_in_range(0, track.frames, 1_000)
                .unwrap()
                .len(),
            11
        );
        assert_eq!(
            track
                .get_peaks_in_range(0, track.frames, 100_000)
                .unwrap()
                .len(),
            2
        );
    }

    #[test]
    fn peak_bins_include_all_channels_and_empty_ranges_are_empty() {
        let samples = vec![-0.2, 0.9, 0.4, -0.8];
        let track = AudioTrack {
            frames: 2,
            peaks: PeakPyramid::build(&samples, 2, 2),
            samples,
            sample_rate: 48_000,
            channels: 2,
        };
        assert_eq!(
            track.get_peaks_in_range(0, 2, 100).unwrap(),
            vec![(-0.8, 0.9)]
        );
        assert!(track.get_peaks_in_range(2, 2, 100).unwrap().is_empty());
        assert!(matches!(
            track.get_peaks_in_range(0, 2, 0),
            Err(AudioCompareError::InvalidZoomLevel)
        ));
    }

    #[test]
    fn computes_per_channel_absolute_delta_for_shared_prefix() {
        let left = test_track(vec![-1.0, 0.25, 0.5, -0.5], 2, 48_000);
        let right = test_track(vec![0.0, -0.25], 2, 48_000);
        let comparison = AudioComparison::from_tracks(left, right).unwrap();
        assert_eq!(comparison.compared_frame_count(), 1);
        assert_eq!(comparison.delta_buffer(), &[1.0, 0.5]);
    }

    #[test]
    fn rejects_mismatched_rate_and_channel_count() {
        let left = test_track(vec![0.0], 1, 1);
        let wrong_rate = AudioTrack {
            sample_rate: 44_100,
            ..test_track(vec![0.0], 1, 1)
        };
        assert!(matches!(
            AudioComparison::from_tracks(left, wrong_rate),
            Err(AudioCompareError::SampleRateMismatch { .. })
        ));

        let left = test_track(vec![0.0], 1, 1);
        let wrong_channels = test_track(vec![0.0, 0.0], 2, 1);
        assert!(matches!(
            AudioComparison::from_tracks(left, wrong_channels),
            Err(AudioCompareError::ChannelCountMismatch { .. })
        ));
    }

    #[test]
    fn decodes_integer_pcm_wav_to_normalized_float_samples() {
        let path = write_test_wav();
        let result = AudioTrack::load(&path);
        let _ = fs::remove_file(&path);
        let track = result.unwrap();

        assert_eq!(track.sample_rate(), 8_000);
        assert_eq!(track.channels(), 1);
        assert_eq!(track.frame_count(), 4);
        assert_eq!(track.samples[0], -1.0);
        assert_eq!(track.samples[1], 0.0);
        assert!((track.samples[2] - 0.5).abs() < f32::EPSILON);
        assert!(track.samples[3] > 0.99);
    }

    #[test]
    fn loads_two_files_and_builds_the_delta_buffer() {
        let left_path = write_test_wav();
        let right_path = write_test_wav();
        let result = AudioComparison::from_files(&left_path, &right_path);
        let _ = fs::remove_file(&left_path);
        let _ = fs::remove_file(&right_path);
        let comparison = result.unwrap();

        assert_eq!(comparison.compared_frame_count(), 4);
        assert_eq!(comparison.delta_buffer(), &[0.0; 4]);
        assert_eq!(comparison.left().frame_count(), 4);
        assert_eq!(comparison.right().sample_rate(), 8_000);
    }

    fn test_track(samples: Vec<f32>, channels: usize, sample_rate: u32) -> AudioTrack {
        assert_eq!(samples.len() % channels, 0);
        let frames = samples.len() / channels;
        let peaks = PeakPyramid::build(&samples, channels, frames);
        AudioTrack {
            samples,
            sample_rate,
            channels,
            frames,
            peaks,
        }
    }

    fn write_test_wav() -> PathBuf {
        let id = TEMP_FILE_ID.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "openbc-audio-compare-{}-{id}.wav",
            std::process::id()
        ));
        let pcm = [-32_768i16, 0, 16_384, 32_767];
        let data_len = (pcm.len() * 2) as u32;
        let mut file = File::create(&path).unwrap();
        file.write_all(b"RIFF").unwrap();
        file.write_all(&(36 + data_len).to_le_bytes()).unwrap();
        file.write_all(b"WAVEfmt ").unwrap();
        file.write_all(&16u32.to_le_bytes()).unwrap();
        file.write_all(&1u16.to_le_bytes()).unwrap();
        file.write_all(&1u16.to_le_bytes()).unwrap();
        file.write_all(&8_000u32.to_le_bytes()).unwrap();
        file.write_all(&16_000u32.to_le_bytes()).unwrap();
        file.write_all(&2u16.to_le_bytes()).unwrap();
        file.write_all(&16u16.to_le_bytes()).unwrap();
        file.write_all(b"data").unwrap();
        file.write_all(&data_len.to_le_bytes()).unwrap();
        for sample in pcm {
            file.write_all(&sample.to_le_bytes()).unwrap();
        }
        path
    }
}

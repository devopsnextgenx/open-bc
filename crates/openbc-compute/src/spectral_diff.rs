//! STFT-based audio comparison and RGB spectral-difference image generation.
//!
//! Time increases from left to right and frequency increases from bottom to top. Matching
//! magnitudes are neutral gray; excess energy in A is magenta and excess energy in B is cyan.
//! The result is an owned RGB888 buffer that can be wrapped by Qt as `QImage::Format_RGB888`.
//!
//! ```
//! use openbc_compute::spectral_diff::{SpectralDiff, SpectralDiffOptions};
//! # fn example() -> Result<(), Box<dyn std::error::Error>> {
//! let diff = SpectralDiff::from_files("reference.wav", "take.wav",
//!                                     SpectralDiffOptions::default())?;
//! let image = diff.render_rgb()?;
//! # let _ = image;
//! # Ok(())
//! # }
//! ```

use std::path::Path;

use rayon::prelude::*;

use crate::audio_compare::{AudioCompareError, AudioTrack};

const MAX_FFT_SIZE: usize = 65_536;

/// STFT and display parameters for a spectral comparison.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpectralDiffOptions {
    /// FFT length. Must be a power of two and at least 2.
    pub fft_size: usize,
    /// Number of source frames between adjacent STFT frames.
    pub hop_size: usize,
    /// Lowest visible magnitude in decibels relative to full-scale PCM.
    pub min_db: f32,
    /// Magnitude mapped to full channel intensity.
    pub max_db: f32,
    /// Caps rendered time columns; each frequency bin keeps its peak over the column's time span.
    pub max_time_columns: usize,
}

impl Default for SpectralDiffOptions {
    fn default() -> Self {
        Self {
            fft_size: 2048,
            hop_size: 512,
            min_db: -80.0,
            max_db: 0.0,
            max_time_columns: 16_384,
        }
    }
}

/// Errors raised while preparing or rendering a spectral comparison.
#[derive(Debug, thiserror::Error)]
pub enum SpectralDiffError {
    /// Decoding either input audio file failed.
    #[error(transparent)]
    Audio(#[from] AudioCompareError),
    /// Inputs need a common sample rate for aligned frequency and time axes.
    #[error("sample rates differ: file A is {file_a} Hz, file B is {file_b} Hz")]
    SampleRateMismatch { file_a: u32, file_b: u32 },
    /// FFT length must be a power of two and at least two.
    #[error("FFT size must be a power of two between 2 and {MAX_FFT_SIZE}")]
    InvalidFftSize,
    /// Hop size must be positive and no larger than the FFT size.
    #[error("hop size must be in the range 1..=FFT size")]
    InvalidHopSize,
    /// Decibel range must be finite, and minimum must be less than maximum.
    #[error("decibel range must be finite with min_db less than max_db")]
    InvalidDecibelRange,
    /// At least one output time column must be allowed.
    #[error("maximum time columns must be greater than zero")]
    InvalidColumnLimit,
    /// A pixel buffer size could not be represented or allocated.
    #[error("could not allocate RGB image buffer ({bytes} bytes)")]
    ImageAllocation { bytes: usize },
    /// A spectrogram's magnitude buffer could not be represented or allocated.
    #[error("could not allocate spectrogram buffer ({bytes} bytes)")]
    SpectrogramAllocation { bytes: usize },
}

/// Contiguous, tightly packed RGB888 heat-map image.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RgbImage {
    width: usize,
    height: usize,
    pixels: Vec<u8>,
}

impl RgbImage {
    /// Number of image columns, corresponding to time.
    pub fn width(&self) -> usize {
        self.width
    }

    /// Number of image rows, corresponding to frequency bins.
    pub fn height(&self) -> usize {
        self.height
    }

    /// RGB888 bytes in row-major order, with `width * 3` bytes per row.
    pub fn pixels(&self) -> &[u8] {
        &self.pixels
    }

    /// Number of bytes between the first byte of adjacent rows.
    pub fn bytes_per_line(&self) -> usize {
        self.width * 3
    }
}

/// A pair of audio tracks transformed into an aligned spectral difference.
#[derive(Debug)]
pub struct SpectralDiff {
    sample_rate: u32,
    hop_size: usize,
    column_stride: usize,
    fft_size: usize,
    duration_frames: usize,
    left: TimeSpectrogram,
    right: TimeSpectrogram,
}

impl SpectralDiff {
    /// Decodes two files and prepares their aligned STFT magnitude spectrograms.
    pub fn from_files(
        file_a: impl AsRef<Path>,
        file_b: impl AsRef<Path>,
        options: SpectralDiffOptions,
    ) -> Result<Self, SpectralDiffError> {
        let left = AudioTrack::load(file_a)?;
        let right = AudioTrack::load(file_b)?;
        Self::from_tracks(&left, &right, options)
    }

    /// Prepares a spectral comparison from tracks decoded by `AudioTrack`.
    ///
    /// Track channel counts and durations may differ. Channel magnitudes are combined by
    /// root-mean-square power, and a shorter track is black (zero magnitude) after it ends.
    pub fn from_tracks(
        file_a: &AudioTrack,
        file_b: &AudioTrack,
        options: SpectralDiffOptions,
    ) -> Result<Self, SpectralDiffError> {
        validate_options(options)?;
        if file_a.sample_rate() != file_b.sample_rate() {
            return Err(SpectralDiffError::SampleRateMismatch {
                file_a: file_a.sample_rate(),
                file_b: file_b.sample_rate(),
            });
        }

        let frame_count_a = file_a.frame_count();
        let frame_count_b = file_b.frame_count();
        let duration_frames = frame_count_a.max(frame_count_b);
        let stft_frames_a = div_ceil(frame_count_a, options.hop_size);
        let stft_frames_b = div_ceil(frame_count_b, options.hop_size);
        let max_frames = stft_frames_a.max(stft_frames_b);
        let column_stride = div_ceil(max_frames, options.max_time_columns).max(1);
        let columns_a = div_ceil(stft_frames_a, column_stride);
        let columns_b = div_ceil(stft_frames_b, column_stride);
        let window = hann_window(options.fft_size);
        let twiddles = fft_twiddles(options.fft_size);
        let bins = options.fft_size / 2 + 1;

        let left = make_time_spectrogram(
            file_a.interleaved_samples(),
            file_a.channels(),
            options,
            column_stride,
            columns_a,
            bins,
            &window,
            &twiddles,
        )?;
        let right = make_time_spectrogram(
            file_b.interleaved_samples(),
            file_b.channels(),
            options,
            column_stride,
            columns_b,
            bins,
            &window,
            &twiddles,
        )?;

        Ok(Self {
            sample_rate: file_a.sample_rate(),
            hop_size: options.hop_size,
            column_stride,
            fft_size: options.fft_size,
            duration_frames,
            left,
            right,
        })
    }

    /// Sample rate shared by both tracks, in samples per second.
    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// FFT length used for the transform.
    pub fn fft_size(&self) -> usize {
        self.fft_size
    }

    /// Number of source frames represented by each rendered image column.
    pub fn frames_per_column(&self) -> usize {
        self.hop_size.saturating_mul(self.column_stride)
    }

    /// Number of source frames in the longer input track.
    pub fn duration_frames(&self) -> usize {
        self.duration_frames
    }

    /// Number of output columns, spanning the longer track.
    pub fn time_columns(&self) -> usize {
        self.left.columns.max(self.right.columns)
    }

    /// Number of one-sided STFT bins, including DC and Nyquist.
    pub fn frequency_bins(&self) -> usize {
        self.fft_size / 2 + 1
    }

    /// Renders the full comparison as a tightly packed RGB888 buffer.
    ///
    /// Draw this image with Qt using `QImage(pixels, width, height, bytes_per_line,
    /// QImage::Format_RGB888)`. Copy the `QImage` before the `RgbImage` is dropped if it
    /// needs to outlive this buffer.
    pub fn render_rgb(&self) -> Result<RgbImage, SpectralDiffError> {
        let width = self.time_columns();
        let height = self.frequency_bins();
        let byte_count = width
            .checked_mul(height)
            .and_then(|pixels| pixels.checked_mul(3))
            .ok_or(SpectralDiffError::ImageAllocation { bytes: usize::MAX })?;
        let mut pixels = Vec::new();
        pixels
            .try_reserve_exact(byte_count)
            .map_err(|_| SpectralDiffError::ImageAllocation { bytes: byte_count })?;
        pixels.resize(byte_count, 0);

        if width == 0 {
            return Ok(RgbImage {
                width,
                height,
                pixels,
            });
        }

        pixels
            .par_chunks_mut(width * 3)
            .enumerate()
            .for_each(|(row, rgb_row)| {
                let bin = height - 1 - row;
                for column in 0..width {
                    let a = self.left.intensity(column, bin);
                    let b = self.right.intensity(column, bin);
                    let offset = column * 3;
                    rgb_row[offset] = a;
                    rgb_row[offset + 1] = b;
                    rgb_row[offset + 2] = a.max(b);
                }
            });

        Ok(RgbImage {
            width,
            height,
            pixels,
        })
    }
}

#[derive(Debug)]
struct TimeSpectrogram {
    columns: usize,
    bins: usize,
    intensity: Vec<u8>,
}

impl TimeSpectrogram {
    fn intensity(&self, column: usize, bin: usize) -> u8 {
        if column >= self.columns {
            0
        } else {
            self.intensity[column * self.bins + bin]
        }
    }
}

fn validate_options(options: SpectralDiffOptions) -> Result<(), SpectralDiffError> {
    if options.fft_size < 2
        || options.fft_size > MAX_FFT_SIZE
        || !options.fft_size.is_power_of_two()
    {
        return Err(SpectralDiffError::InvalidFftSize);
    }
    if options.hop_size == 0 || options.hop_size > options.fft_size {
        return Err(SpectralDiffError::InvalidHopSize);
    }
    if !options.min_db.is_finite()
        || !options.max_db.is_finite()
        || options.min_db >= options.max_db
    {
        return Err(SpectralDiffError::InvalidDecibelRange);
    }
    if options.max_time_columns == 0 {
        return Err(SpectralDiffError::InvalidColumnLimit);
    }
    Ok(())
}

fn div_ceil(value: usize, divisor: usize) -> usize {
    value / divisor + usize::from(value % divisor != 0)
}

fn hann_window(size: usize) -> Vec<f32> {
    (0..size)
        .map(|index| 0.5 - 0.5 * (std::f32::consts::TAU * index as f32 / (size - 1) as f32).cos())
        .collect()
}

fn fft_twiddles(size: usize) -> Vec<(f32, f32)> {
    (0..size / 2)
        .map(|index| {
            let angle = -std::f32::consts::TAU * index as f32 / size as f32;
            angle.sin_cos()
        })
        .map(|(sin, cos)| (cos, sin))
        .collect()
}

fn make_time_spectrogram(
    samples: &[f32],
    channels: usize,
    options: SpectralDiffOptions,
    column_stride: usize,
    columns: usize,
    bins: usize,
    window: &[f32],
    twiddles: &[(f32, f32)],
) -> Result<TimeSpectrogram, SpectralDiffError> {
    let byte_count = columns
        .checked_mul(bins)
        .ok_or(SpectralDiffError::SpectrogramAllocation { bytes: usize::MAX })?;
    let mut intensity = Vec::new();
    intensity
        .try_reserve_exact(byte_count)
        .map_err(|_| SpectralDiffError::SpectrogramAllocation { bytes: byte_count })?;
    intensity.resize(byte_count, 0);
    let frames = div_ceil(samples.len() / channels, options.hop_size);
    let window_sum: f32 = window.iter().sum();

    intensity
        .par_chunks_mut(bins)
        .enumerate()
        .for_each(|(column, output)| {
            let first_frame = column * column_stride;
            let end_frame = first_frame.saturating_add(column_stride).min(frames);
            let mut strongest_magnitude = vec![0.0f32; bins];
            let mut frame_power = vec![0.0f32; bins];
            let mut fft_buffer = vec![(0.0f32, 0.0f32); options.fft_size];

            for frame in first_frame..end_frame {
                let start = frame * options.hop_size;
                frame_power.fill(0.0);
                for channel in 0..channels {
                    for index in 0..options.fft_size {
                        let source_frame = start.saturating_add(index);
                        let sample = if source_frame < samples.len() / channels {
                            samples[source_frame * channels + channel]
                        } else {
                            0.0
                        };
                        fft_buffer[index] = (sample * window[index], 0.0);
                    }
                    fft_in_place(&mut fft_buffer, twiddles);
                    for bin in 0..bins {
                        let (real, imaginary) = fft_buffer[bin];
                        frame_power[bin] += real * real + imaginary * imaginary;
                    }
                }
                for bin in 0..bins {
                    let edge_bin = bin == 0 || bin == bins - 1;
                    let one_sided_scale = if edge_bin { 1.0 } else { 2.0 };
                    let magnitude =
                        (frame_power[bin] / channels as f32).sqrt() * one_sided_scale / window_sum;
                    strongest_magnitude[bin] = strongest_magnitude[bin].max(magnitude);
                }
            }

            for bin in 0..bins {
                output[bin] = magnitude_to_intensity(
                    strongest_magnitude[bin],
                    options.min_db,
                    options.max_db,
                );
            }
        });

    Ok(TimeSpectrogram {
        columns,
        bins,
        intensity,
    })
}

fn magnitude_to_intensity(magnitude: f32, min_db: f32, max_db: f32) -> u8 {
    if magnitude <= 0.0 {
        return 0;
    }
    let db = 20.0 * magnitude.log10();
    let normalized = ((db - min_db) / (max_db - min_db)).clamp(0.0, 1.0);
    (normalized * u8::MAX as f32).round() as u8
}

fn fft_in_place(values: &mut [(f32, f32)], twiddles: &[(f32, f32)]) {
    let size = values.len();
    let mut reversed = 0usize;
    for index in 1..size {
        let mut bit = size >> 1;
        while reversed & bit != 0 {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if index < reversed {
            values.swap(index, reversed);
        }
    }

    let mut block_size = 2;
    while block_size <= size {
        let half_block = block_size / 2;
        let twiddle_stride = size / block_size;
        for block in values.chunks_exact_mut(block_size) {
            for index in 0..half_block {
                let (wr, wi) = twiddles[index * twiddle_stride];
                let (right_r, right_i) = block[index + half_block];
                let product_r = wr * right_r - wi * right_i;
                let product_i = wr * right_i + wi * right_r;
                let (left_r, left_i) = block[index];
                block[index] = (left_r + product_r, left_i + product_i);
                block[index + half_block] = (left_r - product_r, left_i - product_i);
            }
        }
        if block_size == size {
            break;
        }
        block_size *= 2;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs,
        io::Write,
        path::PathBuf,
        sync::atomic::{AtomicUsize, Ordering},
    };

    static TEMP_FILE_ID: AtomicUsize = AtomicUsize::new(0);

    #[test]
    fn fft_places_sine_energy_in_expected_bin() {
        let size = 256;
        let bin = 11;
        let window = hann_window(size);
        let twiddles = fft_twiddles(size);
        let mut values = (0..size)
            .map(|index| {
                let sample =
                    (std::f32::consts::TAU * bin as f32 * index as f32 / size as f32).sin();
                (sample * window[index], 0.0)
            })
            .collect::<Vec<_>>();

        fft_in_place(&mut values, &twiddles);
        let peak = (0..=size / 2)
            .max_by(|&a, &b| {
                let amplitude = |index: usize| {
                    let (re, im) = values[index];
                    re * re + im * im
                };
                amplitude(a).total_cmp(&amplitude(b))
            })
            .unwrap();
        assert_eq!(peak, bin);
    }

    #[test]
    fn matching_magnitudes_are_neutral_and_unique_energy_is_tinted() {
        let diff = SpectralDiff {
            sample_rate: 48_000,
            hop_size: 1,
            column_stride: 1,
            fft_size: 2,
            duration_frames: 2,
            left: TimeSpectrogram {
                columns: 2,
                bins: 2,
                intensity: vec![128, 180, 220, 0],
            },
            right: TimeSpectrogram {
                columns: 2,
                bins: 2,
                intensity: vec![128, 0, 220, 160],
            },
        };
        let image = diff.render_rgb().unwrap();
        assert_eq!(image.width(), 2);
        assert_eq!(image.height(), 2);
        assert_eq!(
            image.pixels(),
            &[180, 0, 180, 0, 160, 160, 128, 128, 128, 220, 220, 220]
        );
        assert_eq!(image.bytes_per_line(), 6);
    }

    #[test]
    fn limited_width_aggregates_transients_without_losing_alignment() {
        let options = SpectralDiffOptions {
            fft_size: 8,
            hop_size: 2,
            max_time_columns: 2,
            ..SpectralDiffOptions::default()
        };
        let samples = (0..32)
            .map(|index| if index == 17 { 1.0 } else { 0.0 })
            .collect::<Vec<_>>();
        let window = hann_window(options.fft_size);
        let twiddles = fft_twiddles(options.fft_size);
        let spectrogram = make_time_spectrogram(
            &samples,
            1,
            options,
            4,
            2,
            options.fft_size / 2 + 1,
            &window,
            &twiddles,
        )
        .unwrap();
        assert_eq!(spectrogram.columns, 2);
        assert!(spectrogram.intensity.iter().any(|&level| level > 0));
    }

    #[test]
    fn rejects_invalid_transform_and_display_options() {
        let mut options = SpectralDiffOptions::default();
        options.fft_size = 1000;
        assert!(matches!(
            validate_options(options),
            Err(SpectralDiffError::InvalidFftSize)
        ));
        options = SpectralDiffOptions::default();
        options.hop_size = 0;
        assert!(matches!(
            validate_options(options),
            Err(SpectralDiffError::InvalidHopSize)
        ));
        options = SpectralDiffOptions::default();
        options.min_db = options.max_db;
        assert!(matches!(
            validate_options(options),
            Err(SpectralDiffError::InvalidDecibelRange)
        ));
    }

    #[test]
    fn fft_transforms_size_two() {
        let mut values = vec![(1.0, 0.0), (-1.0, 0.0)];
        fft_in_place(&mut values, &fft_twiddles(2));
        assert_eq!(values, vec![(0.0, 0.0), (2.0, 0.0)]);
    }

    #[test]
    fn decoded_identical_files_render_neutral_and_silence_comparison_is_magenta() {
        let signal = write_test_wav(&[0, 8_000, 16_000, 8_000, 0, -8_000, -16_000, -8_000]);
        let silence = write_test_wav(&[0; 8]);
        let options = SpectralDiffOptions {
            fft_size: 8,
            hop_size: 4,
            ..SpectralDiffOptions::default()
        };

        let identical = SpectralDiff::from_files(&signal, &signal, options)
            .unwrap()
            .render_rgb()
            .unwrap();
        assert!(identical
            .pixels()
            .chunks_exact(3)
            .all(|rgb| rgb[0] == rgb[1] && rgb[1] == rgb[2]));

        let different = SpectralDiff::from_files(&signal, &silence, options)
            .unwrap()
            .render_rgb()
            .unwrap();
        assert!(different
            .pixels()
            .chunks_exact(3)
            .any(|rgb| rgb[0] > 0 && rgb[1] == 0 && rgb[2] == rgb[0]));

        let _ = fs::remove_file(signal);
        let _ = fs::remove_file(silence);
    }

    fn write_test_wav(pcm: &[i16]) -> PathBuf {
        let id = TEMP_FILE_ID.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "openbc-spectral-diff-{}-{id}.wav",
            std::process::id()
        ));
        let data_len = (pcm.len() * 2) as u32;
        let mut file = std::fs::File::create(&path).unwrap();
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

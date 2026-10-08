//! AVI (RIFF) header walker: main header plus per-stream video/audio format details.

use std::io::{self, Read, Seek, SeekFrom};

use openbc_core::meta_compare::MetaRow;

use super::bytes::{fourcc, le16, le32};
use super::{format_duration, row, track_row, ReadSeek};

/// Longest header chunk read into memory.
const MAX_HEADER_CHUNK_BYTES: u64 = 128;

fn audio_format_name(tag: u16) -> String {
    match tag {
        0x0001 => "PCM".into(),
        0x0055 => "MP3".into(),
        0x00FF | 0x1610 => "AAC".into(),
        0x2000 => "AC-3".into(),
        other => format!("0x{other:04x}"),
    }
}

pub(super) fn parse(r: &mut dyn ReadSeek, rows: &mut Vec<MetaRow>) -> io::Result<()> {
    rows.push(row("Container", "Format", "AVI"));
    let len = r.seek(SeekFrom::End(0))?;
    let (mut video_tracks, mut audio_tracks) = (0u32, 0u32);
    let mut stream_kind = [0u8; 4];
    let mut stack_end = vec![len];
    let mut pos = 12u64;
    // Walk RIFF chunks; descend into LIST hdrl / strl, skip everything else (incl. movi).
    while pos + 8 <= *stack_end.last().unwrap_or(&len) {
        r.seek(SeekFrom::Start(pos))?;
        let mut h = [0u8; 8];
        r.read_exact(&mut h)?;
        let (id, size) = ([h[0], h[1], h[2], h[3]], u64::from(le32(&h, 4)));
        let body = pos + 8;
        if &id == b"LIST" {
            let mut kind = [0u8; 4];
            r.read_exact(&mut kind)?;
            if &kind == b"hdrl" || &kind == b"strl" {
                stack_end.push((body + size).min(len));
                pos = body + 4;
                continue;
            }
            if &kind == b"movi" {
                break;
            }
        } else if matches!(&id, b"avih" | b"strh" | b"strf") {
            let mut chunk = vec![0u8; size.min(MAX_HEADER_CHUNK_BYTES) as usize];
            r.seek(SeekFrom::Start(body))?;
            r.read_exact(&mut chunk)?;
            match &id {
                b"avih" => main_header(&chunk, rows),
                b"strh" if chunk.len() >= 8 => {
                    stream_kind.copy_from_slice(&chunk[0..4]);
                    if &stream_kind == b"vids" {
                        video_tracks += 1;
                        rows.push(track_row("Video", video_tracks, "Handler", fourcc(&chunk[4..8])));
                    }
                }
                _ if &stream_kind == b"vids" => {
                    let codec = fourcc(chunk.get(16..20).unwrap_or(&[]));
                    rows.push(track_row("Video", video_tracks, "Codec", codec));
                    rows.push(track_row("Video", video_tracks, "Bit depth", le16(&chunk, 14).to_string()));
                }
                _ if &stream_kind == b"auds" => {
                    audio_tracks += 1;
                    let n = audio_tracks;
                    rows.push(track_row("Audio", n, "Format", audio_format_name(le16(&chunk, 0))));
                    rows.push(track_row("Audio", n, "Channels", le16(&chunk, 2).to_string()));
                    rows.push(track_row("Audio", n, "Sample rate", format!("{} Hz", le32(&chunk, 4))));
                }
                _ => {}
            }
        }
        pos = body + size + (size & 1);
        while stack_end.len() > 1 && pos >= *stack_end.last().unwrap_or(&len) {
            stack_end.pop();
        }
    }
    Ok(())
}

/// `avih`: microseconds per frame, total frames and the frame size.
fn main_header(b: &[u8], rows: &mut Vec<MetaRow>) {
    let micros_per_frame = le32(b, 0);
    let total_frames = le32(b, 16);
    rows.push(row("Video", "Dimensions", format!("{} x {}", le32(b, 32), le32(b, 36))));
    rows.push(row("Video", "Total frames", total_frames.to_string()));
    if micros_per_frame > 0 {
        let us = f64::from(micros_per_frame);
        rows.push(row("Video", "Frame rate", format!("{:.3} fps", 1e6 / us)));
        rows.push(row("Container", "Duration", format_duration(f64::from(total_frames) * us / 1e6)));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_known_audio_formats() {
        assert_eq!(audio_format_name(0x0001), "PCM");
        assert_eq!(audio_format_name(0x0055), "MP3");
        assert_eq!(audio_format_name(0x1610), "AAC");
        assert_eq!(audio_format_name(0x1234), "0x1234");
    }

    #[test]
    fn main_header_reports_rate_and_duration() {
        let mut b = vec![0u8; 56];
        b[0..4].copy_from_slice(&40_000u32.to_le_bytes()); // 25 fps
        b[16..20].copy_from_slice(&250u32.to_le_bytes()); // 10 s
        b[32..36].copy_from_slice(&640u32.to_le_bytes());
        b[36..40].copy_from_slice(&480u32.to_le_bytes());
        let mut rows = Vec::new();
        main_header(&b, &mut rows);
        let find = |k: &str| rows.iter().find(|r| r.key == k).map(|r| r.value.clone());
        assert_eq!(find("Dimensions").as_deref(), Some("640 x 480"));
        assert_eq!(find("Frame rate").as_deref(), Some("25.000 fps"));
        assert_eq!(find("Duration").as_deref(), Some("0:00:10.000"));
    }
}

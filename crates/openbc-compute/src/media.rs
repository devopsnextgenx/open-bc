//! Media container metadata (headers only; no audio/video decoding).
//!
//! Audio containers and tags go through `lofty` (0.22 API); MP4/MOV and AVI headers are
//! walked here for video track details. Matroska/WebM is recognised but not parsed.

use std::io::{self, BufReader, Read, Seek, SeekFrom};

use lofty::file::{AudioFile, TaggedFileExt};
use lofty::probe::Probe;
use lofty::tag::ItemValue;
use openbc_core::meta_compare::MetaRow;

pub trait ReadSeek: Read + Seek {}
impl<T: Read + Seek> ReadSeek for T {}

#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    #[error("unrecognised media container")]
    Unsupported,
    #[error("read failed: {0}")]
    Io(#[from] io::Error),
}

fn row(group: &str, key: impl Into<String>, value: impl Into<String>) -> MetaRow {
    MetaRow { group: group.into(), key: key.into(), value: value.into() }
}

fn clip(s: &str) -> String {
    let s = s.split_whitespace().collect::<Vec<_>>().join(" ");
    if s.chars().count() > 300 { s.chars().take(300).collect::<String>() + "…" } else { s }
}

/// `H:MM:SS.mmm` from seconds.
pub fn format_duration(seconds: f64) -> String {
    let ms = (seconds * 1000.0).round() as u64;
    format!("{}:{:02}:{:02}.{:03}", ms / 3_600_000, ms / 60_000 % 60, ms / 1000 % 60, ms % 1000)
}

fn fourcc(b: &[u8]) -> String {
    b.iter().map(|&c| if (32..127).contains(&c) { c as char } else { '.' }).collect::<String>().trim().to_string()
}

/// Reads the metadata rows of one media file (groups: Container, Video, Audio, Subtitle, Tags).
pub fn read_media_metadata(src: &mut dyn ReadSeek) -> Result<Vec<MetaRow>, MediaError> {
    let mut head = [0u8; 12];
    let n = src.read(&mut head)?;
    src.seek(SeekFrom::Start(0))?;
    let mut rows = Vec::new();
    let mut recognised = false;
    if n >= 12 && &head[4..8] == b"ftyp" {
        recognised = true;
        parse_mp4(src, &mut rows)?;
    } else if n >= 12 && &head[0..4] == b"RIFF" && &head[8..12] == b"AVI " {
        recognised = true;
        parse_avi(src, &mut rows)?;
    } else if head[..4] == [0x1A, 0x45, 0xDF, 0xA3] {
        recognised = true;
        rows.push(row("Container", "Format", "Matroska/WebM (track details not parsed)"));
    }
    src.seek(SeekFrom::Start(0))?;
    recognised |= read_lofty(src, &mut rows);
    if recognised { Ok(rows) } else { Err(MediaError::Unsupported) }
}

fn read_lofty(src: &mut dyn ReadSeek, rows: &mut Vec<MetaRow>) -> bool {
    let Ok(probe) = Probe::new(BufReader::new(src)).guess_file_type() else { return false };
    let Ok(file) = probe.read() else { return false };
    rows.push(row("Container", "Type", format!("{:?}", file.file_type())));
    let p = file.properties();
    rows.push(row("Audio", "Duration", format_duration(p.duration().as_secs_f64())));
    if let Some(v) = p.overall_bitrate() { rows.push(row("Audio", "Overall bitrate", format!("{v} kbps"))) }
    if let Some(v) = p.audio_bitrate() { rows.push(row("Audio", "Audio bitrate", format!("{v} kbps"))) }
    if let Some(v) = p.sample_rate() { rows.push(row("Audio", "Sample rate", format!("{v} Hz"))) }
    if let Some(v) = p.bit_depth() { rows.push(row("Audio", "Bit depth", format!("{v}"))) }
    if let Some(v) = p.channels() { rows.push(row("Audio", "Channels", format!("{v}"))) }
    for tag in file.tags() {
        for item in tag.items() {
            let text = match item.value() {
                ItemValue::Text(t) | ItemValue::Locator(t) => clip(t),
                _ => continue,
            };
            if !text.is_empty() {
                rows.push(row("Tags", format!("{:?}", item.key()), text));
            }
        }
    }
    true
}

// ----------------------------------------------------------------------------- MP4 / MOV

#[derive(Default)]
struct Mp4Track {
    handler: [u8; 4],
    codec: String,
    width: f64,
    height: f64,
    timescale: u32,
    duration: u64,
}

#[derive(Default)]
struct Mp4State {
    cur: Option<Mp4Track>,
    tracks: Vec<Mp4Track>,
    rows: Vec<MetaRow>,
}

fn be32(b: &[u8], at: usize) -> u32 {
    b.get(at..at + 4).map_or(0, |s| u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
}
fn be64(b: &[u8], at: usize) -> u64 {
    b.get(at..at + 8).map_or(0, |s| u64::from_be_bytes(s.try_into().unwrap_or([0; 8])))
}

/// `(timescale, duration)` of an `mvhd` / `mdhd` body.
fn time_header(b: &[u8]) -> (u32, u64) {
    if b.first() == Some(&1) { (be32(b, 20), be64(b, 24)) } else { (be32(b, 12), u64::from(be32(b, 16))) }
}

fn walk_boxes(r: &mut dyn ReadSeek, start: u64, end: u64, st: &mut Mp4State) -> io::Result<()> {
    let mut pos = start;
    while pos + 8 <= end {
        r.seek(SeekFrom::Start(pos))?;
        let mut h = [0u8; 8];
        r.read_exact(&mut h)?;
        let (mut size, mut hdr) = (u64::from(be32(&h, 0)), 8u64);
        let kind = [h[4], h[5], h[6], h[7]];
        if size == 1 {
            let mut big = [0u8; 8];
            r.read_exact(&mut big)?;
            (size, hdr) = (u64::from_be_bytes(big), 16);
        } else if size == 0 {
            size = end - pos;
        }
        if size < hdr || pos + size > end {
            break; // corrupt or truncated: keep what was read
        }
        let (body, box_end) = (pos + hdr, pos + size);
        match &kind {
            b"moov" | b"mdia" | b"minf" | b"stbl" => walk_boxes(r, body, box_end, st)?,
            b"trak" => {
                st.cur = Some(Mp4Track::default());
                walk_boxes(r, body, box_end, st)?;
                st.tracks.extend(st.cur.take());
            }
            b"ftyp" | b"mvhd" | b"tkhd" | b"mdhd" | b"hdlr" | b"stsd" => {
                let mut buf = vec![0u8; (box_end - body).min(256) as usize];
                r.seek(SeekFrom::Start(body))?;
                r.read_exact(&mut buf)?;
                small_box(&kind, &buf, st);
            }
            _ => {}
        }
        pos = box_end;
    }
    Ok(())
}

fn small_box(kind: &[u8; 4], b: &[u8], st: &mut Mp4State) {
    match kind {
        b"ftyp" if b.len() >= 8 => {
            st.rows.push(row("Container", "Major brand", fourcc(&b[0..4])));
            let compat: Vec<String> = b[8..].chunks_exact(4).map(fourcc).collect();
            st.rows.push(row("Container", "Compatible brands", compat.join(", ")));
        }
        b"mvhd" => {
            let (ts, dur) = time_header(b);
            if ts > 0 {
                st.rows.push(row("Container", "Duration", format_duration(dur as f64 / f64::from(ts))));
            }
        }
        _ => {
            let Some(t) = st.cur.as_mut() else { return };
            match kind {
                b"tkhd" => {
                    let off = if b.first() == Some(&1) { 88 } else { 76 };
                    (t.width, t.height) = (f64::from(be32(b, off)) / 65536.0, f64::from(be32(b, off + 4)) / 65536.0);
                }
                b"mdhd" => (t.timescale, t.duration) = time_header(b),
                b"hdlr" if b.len() >= 12 => t.handler.copy_from_slice(&b[8..12]),
                b"stsd" if b.len() >= 16 => t.codec = fourcc(&b[12..16]),
                _ => {}
            }
        }
    }
}

fn parse_mp4(r: &mut dyn ReadSeek, rows: &mut Vec<MetaRow>) -> io::Result<()> {
    let len = r.seek(SeekFrom::End(0))?;
    let mut st = Mp4State::default();
    walk_boxes(r, 0, len, &mut st)?;
    rows.append(&mut st.rows);
    let (mut v, mut a, mut o) = (0, 0, 0);
    for t in &st.tracks {
        let (group, n) = match &t.handler {
            b"vide" => ("Video", &mut v),
            b"soun" => ("Audio", &mut a),
            _ => ("Subtitle", &mut o),
        };
        *n += 1;
        let p = format!("Track {n}");
        rows.push(row(group, format!("{p}: Codec"), &t.codec));
        if t.width > 0.0 {
            rows.push(row(group, format!("{p}: Dimensions"), format!("{} x {}", t.width as u32, t.height as u32)));
        }
        if t.timescale > 0 {
            rows.push(row(group, format!("{p}: Duration"), format_duration(t.duration as f64 / f64::from(t.timescale))));
        }
    }
    Ok(())
}

// ----------------------------------------------------------------------------------- AVI

fn le32(b: &[u8], at: usize) -> u32 {
    b.get(at..at + 4).map_or(0, |s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}
fn le16(b: &[u8], at: usize) -> u16 {
    b.get(at..at + 2).map_or(0, |s| u16::from_le_bytes([s[0], s[1]]))
}

fn audio_format_name(tag: u16) -> String {
    match tag {
        0x0001 => "PCM".into(),
        0x0055 => "MP3".into(),
        0x00FF | 0x1610 => "AAC".into(),
        0x2000 => "AC-3".into(),
        other => format!("0x{other:04x}"),
    }
}

fn parse_avi(r: &mut dyn ReadSeek, rows: &mut Vec<MetaRow>) -> io::Result<()> {
    rows.push(row("Container", "Format", "AVI"));
    let len = r.seek(SeekFrom::End(0))?;
    let mut counts = (0, 0);
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
            let mut b = vec![0u8; size.min(128) as usize];
            r.seek(SeekFrom::Start(body))?;
            r.read_exact(&mut b)?;
            match &id {
                b"avih" => {
                    let us = le32(&b, 0);
                    rows.push(row("Video", "Dimensions", format!("{} x {}", le32(&b, 32), le32(&b, 36))));
                    rows.push(row("Video", "Total frames", le32(&b, 16).to_string()));
                    if us > 0 {
                        rows.push(row("Video", "Frame rate", format!("{:.3} fps", 1e6 / f64::from(us))));
                        rows.push(row("Container", "Duration", format_duration(f64::from(le32(&b, 16)) * f64::from(us) / 1e6)));
                    }
                }
                b"strh" if b.len() >= 8 => {
                    stream_kind.copy_from_slice(&b[0..4]);
                    if &stream_kind == b"vids" {
                        counts.0 += 1;
                        rows.push(row("Video", format!("Track {}: Handler", counts.0), fourcc(&b[4..8])));
                    }
                }
                _ if &stream_kind == b"vids" => {
                    rows.push(row("Video", format!("Track {}: Codec", counts.0), fourcc(b.get(16..20).unwrap_or(&[]))));
                    rows.push(row("Video", format!("Track {}: Bit depth", counts.0), le16(&b, 14).to_string()));
                }
                _ if &stream_kind == b"auds" => {
                    counts.1 += 1;
                    let p = format!("Track {}", counts.1);
                    rows.push(row("Audio", format!("{p}: Format"), audio_format_name(le16(&b, 0))));
                    rows.push(row("Audio", format!("{p}: Channels"), le16(&b, 2).to_string()));
                    rows.push(row("Audio", format!("{p}: Sample rate"), format!("{} Hz", le32(&b, 4))));
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn reads_minimal_mp4_ftyp_and_duration() {
        let mut f = Vec::new();
        f.extend(20u32.to_be_bytes());
        f.extend(b"ftypisom\0\0\0\0isom");
        let mut mvhd = vec![0u8; 100];
        mvhd[12..16].copy_from_slice(&1000u32.to_be_bytes());
        mvhd[16..20].copy_from_slice(&5000u32.to_be_bytes());
        let mvhd_box = [&(108u32).to_be_bytes()[..], b"mvhd", &mvhd].concat();
        f.extend(((8 + mvhd_box.len()) as u32).to_be_bytes());
        f.extend(b"moov");
        f.extend(mvhd_box);
        let rows = read_media_metadata(&mut Cursor::new(f)).unwrap();
        assert!(rows.iter().any(|r| r.key == "Duration" && r.value == "0:00:05.000"));
        assert!(rows.iter().any(|r| r.key == "Major brand" && r.value == "isom"));
    }

    #[test]
    fn rejects_unknown_bytes() {
        assert!(matches!(read_media_metadata(&mut Cursor::new(vec![0u8; 64])), Err(MediaError::Unsupported)));
    }
}

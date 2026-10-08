//! MP4 / MOV (ISO base media) box walker: brands, duration and per-track codec/size.

use std::io::{self, Read, Seek, SeekFrom};

use openbc_core::meta_compare::MetaRow;

use super::bytes::{be32, be64, fourcc};
use super::{format_duration, row, track_row, ReadSeek};

/// Longest box body read into memory (the headers of interest are far smaller).
const MAX_SMALL_BOX_BYTES: u64 = 256;

#[derive(Default)]
struct Track {
    handler: [u8; 4],
    codec: String,
    width: f64,
    height: f64,
    timescale: u32,
    duration: u64,
}

#[derive(Default)]
struct State {
    cur: Option<Track>,
    tracks: Vec<Track>,
    rows: Vec<MetaRow>,
}

/// `(timescale, duration)` of an `mvhd` / `mdhd` body.
fn time_header(b: &[u8]) -> (u32, u64) {
    if b.first() == Some(&1) {
        (be32(b, 20), be64(b, 24))
    } else {
        (be32(b, 12), u64::from(be32(b, 16)))
    }
}

pub(super) fn parse(r: &mut dyn ReadSeek, rows: &mut Vec<MetaRow>) -> io::Result<()> {
    let len = r.seek(SeekFrom::End(0))?;
    let mut st = State::default();
    walk_boxes(r, 0, len, &mut st)?;
    rows.append(&mut st.rows);

    let (mut video, mut audio, mut other) = (0u32, 0u32, 0u32);
    for t in &st.tracks {
        let (group, n) = match &t.handler {
            b"vide" => ("Video", &mut video),
            b"soun" => ("Audio", &mut audio),
            _ => ("Subtitle", &mut other),
        };
        *n += 1;
        let n = *n;
        rows.push(track_row(group, n, "Codec", &t.codec));
        if t.width > 0.0 {
            rows.push(track_row(group, n, "Dimensions", format!("{} x {}", t.width as u32, t.height as u32)));
        }
        if t.timescale > 0 {
            rows.push(track_row(group, n, "Duration", format_duration(t.duration as f64 / f64::from(t.timescale))));
        }
    }
    Ok(())
}

fn walk_boxes(r: &mut dyn ReadSeek, start: u64, end: u64, st: &mut State) -> io::Result<()> {
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
                st.cur = Some(Track::default());
                walk_boxes(r, body, box_end, st)?;
                st.tracks.extend(st.cur.take());
            }
            b"ftyp" | b"mvhd" | b"tkhd" | b"mdhd" | b"hdlr" | b"stsd" => {
                let mut buf = vec![0u8; (box_end - body).min(MAX_SMALL_BOX_BYTES) as usize];
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

fn small_box(kind: &[u8; 4], b: &[u8], st: &mut State) {
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

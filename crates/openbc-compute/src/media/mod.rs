//! Media container metadata (headers only; no audio/video decoding).
//!
//! Audio containers and tags go through `lofty` (see `audio`); MP4/MOV and AVI headers are
//! walked by hand for video track details (`mp4`, `avi`). Matroska/WebM is recognised but
//! not parsed.
//!
//! Embedded cover art (ID3 APIC, MP4 covr, FLAC/Vorbis pictures) is reported as a "Tags"
//! row and the first picture's bytes are returned in `MediaInfo::cover` for the UI.

mod audio;
mod avi;
mod bytes;
mod mp4;

use std::io::{self, Read, Seek, SeekFrom};

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

/// Result of reading one media file.
#[derive(Debug, Default)]
pub struct MediaInfo {
    pub rows: Vec<MetaRow>,
    /// Raw bytes of the first embedded picture (jpeg/png/...), if any.
    pub cover: Option<Vec<u8>>,
}

/// `H:MM:SS.mmm` from seconds.
pub fn format_duration(seconds: f64) -> String {
    let ms = (seconds * 1000.0).round() as u64;
    format!("{}:{:02}:{:02}.{:03}", ms / 3_600_000, ms / 60_000 % 60, ms / 1000 % 60, ms % 1000)
}

/// Reads the metadata rows (groups: Container, Video, Audio, Subtitle, Tags) and cover art of one media file.
pub fn read_media_metadata(src: &mut dyn ReadSeek) -> Result<MediaInfo, MediaError> {
    let mut head = [0u8; 12];
    let n = src.read(&mut head)?;
    src.seek(SeekFrom::Start(0))?;

    let mut info = MediaInfo::default();
    let container = sniff(&head, n);
    match container {
        Some(Container::Mp4) => mp4::parse(src, &mut info.rows)?,
        Some(Container::Avi) => avi::parse(src, &mut info.rows)?,
        Some(Container::Matroska) => {
            info.rows.push(row("Container", "Format", "Matroska/WebM (track details not parsed)"));
        }
        None => {}
    }
    src.seek(SeekFrom::Start(0))?;
    let tagged = audio::read(src, &mut info);
    if container.is_some() || tagged {
        Ok(info)
    } else {
        Err(MediaError::Unsupported)
    }
}

/// Containers whose headers are walked by this module (everything else is left to `lofty`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Container {
    Mp4,
    Avi,
    Matroska,
}

fn sniff(head: &[u8; 12], n: usize) -> Option<Container> {
    if n >= 12 && &head[4..8] == b"ftyp" {
        Some(Container::Mp4)
    } else if n >= 12 && &head[0..4] == b"RIFF" && &head[8..12] == b"AVI " {
        Some(Container::Avi)
    } else if head[..4] == [0x1A, 0x45, 0xDF, 0xA3] {
        Some(Container::Matroska)
    } else {
        None
    }
}

// ------------------------------------------------------------------------ row builders

fn row(group: &str, key: impl Into<String>, value: impl Into<String>) -> MetaRow {
    MetaRow { group: group.into(), key: key.into(), value: value.into() }
}

/// `Track <n>: <field>` row shared by the MP4 and AVI walkers.
fn track_row(group: &str, n: u32, field: &str, value: impl Into<String>) -> MetaRow {
    row(group, format!("Track {n}: {field}"), value)
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
        let info = read_media_metadata(&mut Cursor::new(f)).unwrap();
        assert!(info.rows.iter().any(|r| r.key == "Duration" && r.value == "0:00:05.000"));
        assert!(info.rows.iter().any(|r| r.key == "Major brand" && r.value == "isom"));
        assert!(info.cover.is_none());
    }

    #[test]
    fn rejects_unknown_bytes() {
        assert!(matches!(read_media_metadata(&mut Cursor::new(vec![0u8; 64])), Err(MediaError::Unsupported)));
    }

    #[test]
    fn sniffs_each_walked_container() {
        let mut mp4 = [0u8; 12];
        mp4[4..8].copy_from_slice(b"ftyp");
        assert_eq!(sniff(&mp4, 12), Some(Container::Mp4));
        let mut avi = [0u8; 12];
        avi[0..4].copy_from_slice(b"RIFF");
        avi[8..12].copy_from_slice(b"AVI ");
        assert_eq!(sniff(&avi, 12), Some(Container::Avi));
        let mut mkv = [0u8; 12];
        mkv[..4].copy_from_slice(&[0x1A, 0x45, 0xDF, 0xA3]);
        assert_eq!(sniff(&mkv, 12), Some(Container::Matroska));
        assert_eq!(sniff(&mp4, 11), None, "a short read cannot be an MP4/AVI header");
    }

    #[test]
    fn formats_durations() {
        assert_eq!(format_duration(0.0), "0:00:00.000");
        assert_eq!(format_duration(3723.456), "1:02:03.456");
    }
}

//! Audio containers, properties and tags via `lofty` (0.22 API), plus embedded cover art.

use std::fmt::Display;
use std::io::BufReader;

use lofty::file::{AudioFile, TaggedFileExt};
use lofty::probe::Probe;
use lofty::tag::ItemValue;
use openbc_core::meta_compare::MetaRow;

use super::{row, MediaInfo, ReadSeek};

/// Largest cover image handed to the UI (bigger pictures are listed but not returned).
const MAX_COVER_BYTES: usize = 8 * 1024 * 1024;
/// Longest tag value shown before it is cut with an ellipsis.
const MAX_TAG_CHARS: usize = 300;

/// Collapses whitespace and truncates long tag values.
fn clip(s: &str) -> String {
    let s = s.split_whitespace().collect::<Vec<_>>().join(" ");
    if s.chars().count() > MAX_TAG_CHARS {
        s.chars().take(MAX_TAG_CHARS).collect::<String>() + "…"
    } else {
        s
    }
}

fn push_property(rows: &mut Vec<MetaRow>, key: &str, value: Option<impl Display>, unit: &str) {
    if let Some(v) = value {
        rows.push(row("Audio", key, format!("{v}{unit}")));
    }
}

/// Reads properties, tags and pictures into `info`. Returns `false` when `lofty` does not
/// recognise the stream.
pub(super) fn read(src: &mut dyn ReadSeek, info: &mut MediaInfo) -> bool {
    let Ok(probe) = Probe::new(BufReader::new(src)).guess_file_type() else { return false };
    let Ok(file) = probe.read() else { return false };
    let rows = &mut info.rows;
    rows.push(row("Container", "Type", format!("{:?}", file.file_type())));

    let p = file.properties();
    rows.push(row("Audio", "Duration", super::format_duration(p.duration().as_secs_f64())));
    push_property(rows, "Overall bitrate", p.overall_bitrate(), " kbps");
    push_property(rows, "Audio bitrate", p.audio_bitrate(), " kbps");
    push_property(rows, "Sample rate", p.sample_rate(), " Hz");
    push_property(rows, "Bit depth", p.bit_depth(), "");
    push_property(rows, "Channels", p.channels(), "");

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
        for (i, pic) in tag.pictures().iter().enumerate() {
            let mime = pic.mime_type().map_or("unknown type", |m| m.as_str());
            let key = if i == 0 {
                format!("Picture ({:?})", pic.pic_type())
            } else {
                format!("Picture {} ({:?})", i + 1, pic.pic_type())
            };
            rows.push(row("Tags", key, format!("{mime}, {} bytes", pic.data().len())));
            if info.cover.is_none() && !pic.data().is_empty() && pic.data().len() <= MAX_COVER_BYTES {
                info.cover = Some(pic.data().to_vec());
            }
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clip_collapses_whitespace_and_truncates() {
        assert_eq!(clip("  a \n b\t c "), "a b c");
        let long = "x".repeat(MAX_TAG_CHARS + 10);
        let clipped = clip(&long);
        assert_eq!(clipped.chars().count(), MAX_TAG_CHARS + 1);
        assert!(clipped.ends_with('…'));
    }
}

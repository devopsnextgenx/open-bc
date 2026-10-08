//! Rewrite tags / cover of one local media file (uses `lofty`; add `lofty = "0.22"` to
//! openbc-engine/Cargo.toml). Written for lofty ~0.22 - adjust imports if your version differs.
//! The match arms in `set_field` / `is_editable` must use the same row keys that
//! `read_media_metadata` emits (e.g. "Title", "Artist", ...); rename them to match.

use std::io::Cursor;
use std::path::Path;

use lofty::config::WriteOptions;
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::picture::{Picture, PictureType};
use lofty::probe::Probe;
use lofty::tag::{Accessor, ItemKey, Tag};

use super::{CoverEdit, MediaSave, TagEdit};

pub fn apply(path: &Path, save: &MediaSave) -> Result<(), String> {
    let mut file = Probe::open(path).map_err(|e| e.to_string())?.read().map_err(|e| e.to_string())?;
    if file.primary_tag().is_none() {
        let kind = file.primary_tag_type();
        file.insert_tag(Tag::new(kind));
    }
    let tag = file.primary_tag_mut().ok_or("no writable tag for this container")?;

    for e in &save.edits {
        set_field(tag, e)?;
    }
    match &save.cover {
        CoverEdit::Keep => {}
        CoverEdit::Remove => {
            tag.remove_picture_type(PictureType::CoverFront);
        }
        CoverEdit::Replace(bytes) => {
            let mut pic = Picture::from_reader(&mut Cursor::new(bytes)).map_err(|e| e.to_string())?;
            pic.set_pic_type(PictureType::CoverFront);
            tag.remove_picture_type(PictureType::CoverFront);
            tag.push_picture(pic);
        }
    }
    file.save_to_path(path, WriteOptions::default()).map_err(|e| e.to_string())
}

fn set_field(tag: &mut Tag, e: &TagEdit) -> Result<(), String> {
    // `e.key` is the display name the backend put in the row; extend this map with your row keys.
    let v = e.value.clone();
    match (e.key.as_str(), v) {
        ("Title", Some(v)) => tag.set_title(v),
        ("Title", None) => tag.remove_title(),
        ("Artist", Some(v)) => tag.set_artist(v),
        ("Artist", None) => tag.remove_artist(),
        ("Album", Some(v)) => tag.set_album(v),
        ("Album", None) => tag.remove_album(),
        ("Genre", Some(v)) => tag.set_genre(v),
        ("Genre", None) => tag.remove_genre(),
        ("Comment", Some(v)) => tag.set_comment(v),
        ("Comment", None) => tag.remove_comment(),
        ("Track", Some(v)) => tag.set_track(v.trim().parse().map_err(|_| format!("Track must be a number: {v}"))?),
        ("Track", None) => tag.remove_track(),
        ("Year", Some(v)) => { tag.insert_text(ItemKey::Year, v); }
        ("Year", None) => tag.remove_key(&ItemKey::Year),
        ("Album Artist", Some(v)) => { tag.insert_text(ItemKey::AlbumArtist, v); }
        ("Album Artist", None) => tag.remove_key(&ItemKey::AlbumArtist),
        ("Composer", Some(v)) => { tag.insert_text(ItemKey::Composer, v); }
        ("Composer", None) => tag.remove_key(&ItemKey::Composer),
        (other, _) => return Err(format!("tag '{other}' is not editable")),
    }
    Ok(())
}
/// True for rows the UI may edit (bit 2 of the row flags in the C ABI).
pub fn is_editable(group: &str, key: &str) -> bool {
    group != "File"
        && matches!(key, "Title" | "Artist" | "Album" | "Album Artist" | "Composer" | "Genre" | "Comment" | "Track" | "Year")
}
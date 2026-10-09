//! Rewrite tags / cover of one local media file (uses `lofty`, written for ~0.22).
//!
//! The row keys here must match the ones `read_media_metadata` emits; [`Field`] is the single
//! place that maps a row key to a tag field, used both for editing and for `is_editable`.

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

    for edit in &save.edits {
        set_field(tag, edit)?;
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

/// The tag fields the UI may edit, keyed by the display name of their metadata row.
#[derive(Clone, Copy)]
enum Field {
    Title,
    Artist,
    Album,
    AlbumArtist,
    Composer,
    Genre,
    Comment,
    Track,
    Year,
}

impl Field {
    fn from_key(key: &str) -> Option<Self> {
        Some(match key {
            "Title" => Self::Title,
            "Artist" => Self::Artist,
            "Album" => Self::Album,
            "Album Artist" => Self::AlbumArtist,
            "Composer" => Self::Composer,
            "Genre" => Self::Genre,
            "Comment" => Self::Comment,
            "Track" => Self::Track,
            "Year" => Self::Year,
            _ => return None,
        })
    }

    /// Sets the field, or removes it for `None`.
    fn write(self, tag: &mut Tag, value: Option<String>) -> Result<(), String> {
        match (self, value) {
            (Self::Title, Some(v)) => tag.set_title(v),
            (Self::Title, None) => tag.remove_title(),
            (Self::Artist, Some(v)) => tag.set_artist(v),
            (Self::Artist, None) => tag.remove_artist(),
            (Self::Album, Some(v)) => tag.set_album(v),
            (Self::Album, None) => tag.remove_album(),
            (Self::Genre, Some(v)) => tag.set_genre(v),
            (Self::Genre, None) => tag.remove_genre(),
            (Self::Comment, Some(v)) => tag.set_comment(v),
            (Self::Comment, None) => tag.remove_comment(),
            (Self::Track, Some(v)) => tag.set_track(v.trim().parse().map_err(|_| format!("Track must be a number: {v}"))?),
            (Self::Track, None) => tag.remove_track(),
            (Self::Year, Some(v)) => {
                tag.insert_text(ItemKey::Year, v);
            }
            (Self::Year, None) => tag.remove_key(&ItemKey::Year),
            (Self::AlbumArtist, Some(v)) => {
                tag.insert_text(ItemKey::AlbumArtist, v);
            }
            (Self::AlbumArtist, None) => tag.remove_key(&ItemKey::AlbumArtist),
            (Self::Composer, Some(v)) => {
                tag.insert_text(ItemKey::Composer, v);
            }
            (Self::Composer, None) => tag.remove_key(&ItemKey::Composer),
        }
        Ok(())
    }
}

fn set_field(tag: &mut Tag, edit: &TagEdit) -> Result<(), String> {
    let field = Field::from_key(&edit.key).ok_or_else(|| format!("tag '{}' is not editable", edit.key))?;
    field.write(tag, edit.value.clone())
}

/// True for rows the UI may edit (bit 2 of the row flags in the C ABI).
pub fn is_editable(group: &str, key: &str) -> bool {
    group != "File" && Field::from_key(key).is_some()
}
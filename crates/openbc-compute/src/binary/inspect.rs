//! Read the bytes of one section or function straight from the file on disk for the hex /
//! disassembly inspector.
//!
//! The parser records where each element's bytes live (`file_offset`) and their SHA-256, so the
//! inspector never re-parses a binary: it seeks, reads a bounded window and checks the hash. A
//! mismatch (the file changed since it was analysed, or the format stores the content somewhere
//! other than the recorded offset, as in a slice of a universal Mach-O) is reported rather than
//! silently showing the wrong bytes.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::digest::hex_digest;

use super::CONTENT_HASH;

/// Bytes read for one element.
#[derive(Debug)]
pub struct Region {
    /// The bytes (at most `limit`).
    pub bytes: Vec<u8>,
    /// `true` when the element is larger than `limit` and only its beginning was read.
    pub truncated: bool,
    /// `true` when the bytes were complete and hash to `expected_sha256`.
    pub verified: bool,
}

/// Read `size` bytes at `offset` (capped at `limit`) and verify them against `expected_sha256`.
pub fn read_region(path: &Path, offset: u64, size: u64, expected_sha256: &str, limit: usize) -> Result<Region, String> {
    let io = |e: std::io::Error| format!("cannot read {}: {e}", path.display());
    let mut file = File::open(path).map_err(io)?;
    file.seek(SeekFrom::Start(offset)).map_err(io)?;
    let wanted = usize::try_from(size).unwrap_or(usize::MAX);
    let take = wanted.min(limit);
    let mut bytes = vec![0u8; take];
    let mut filled = 0;
    while filled < take {
        let n = file.read(&mut bytes[filled..]).map_err(io)?;
        if n == 0 {
            break;
        }
        filled += n;
    }
    bytes.truncate(filled);
    let truncated = wanted > take;
    let verified = !truncated && filled == take && hex_digest(CONTENT_HASH, &bytes) == expected_sha256;
    Ok(Region { bytes, truncated, verified })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_verifies_and_truncates() {
        let dir = std::env::temp_dir().join(format!("openbc-inspect-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("blob.bin");
        let data: Vec<u8> = (0..=255u8).collect();
        std::fs::write(&path, &data).unwrap();

        let sha = hex_digest(CONTENT_HASH, &data[16..48]);
        let ok = read_region(&path, 16, 32, &sha, 1024).unwrap();
        assert_eq!(ok.bytes, &data[16..48]);
        assert!(ok.verified && !ok.truncated);

        let wrong = read_region(&path, 17, 32, &sha, 1024).unwrap();
        assert!(!wrong.verified, "a shifted window must not verify");

        let cut = read_region(&path, 0, 256, &sha, 64).unwrap();
        assert!(cut.truncated && !cut.verified);
        assert_eq!(cut.bytes.len(), 64);

        assert!(read_region(&dir.join("missing.bin"), 0, 1, "", 1).is_err());
        let _ = std::fs::remove_dir_all(&dir);
    }
}

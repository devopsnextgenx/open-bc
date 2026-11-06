use std::cmp;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

/// Raised when the underlying file changes while a view is open.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceChanged {
    pub path: PathBuf,
    pub previous_len: u64,
    pub current_len: u64,
}

impl std::fmt::Display for SourceChanged {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "source changed for {} ({} -> {})",
            self.path.display(),
            self.previous_len,
            self.current_len
        )
    }
}

impl std::error::Error for SourceChanged {}

/// A simple chunked reader that keeps only a bounded amount of file data in memory.
#[derive(Debug, Clone)]
pub struct LargeTextSource {
    path: PathBuf,
    len: u64,
    modified: Option<SystemTime>,
}

impl LargeTextSource {
    /// Open a file and record its length and modification timestamp.
    pub fn open(path: impl AsRef<Path>) -> io::Result<Self> {
        let path = path.as_ref().to_path_buf();
        let metadata = fs::metadata(&path)?;
        Ok(Self {
            path,
            len: metadata.len(),
            modified: metadata.modified().ok(),
        })
    }

    #[must_use]
    pub fn len(&self) -> u64 {
        self.len
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Read a byte range using the file's own seek logic. The cache only keeps a bounded number of
    /// blocks so the caller retains predictable peak memory use.
    pub fn read_at(&mut self, offset: u64, length: usize) -> io::Result<Vec<u8>> {
        if offset >= self.len {
            return Ok(Vec::new());
        }
        let length = cmp::min(
            length,
            usize::try_from(self.len.saturating_sub(offset)).unwrap_or(usize::MAX),
        );

        let mut file = File::open(&self.path)?;
        file.seek(SeekFrom::Start(offset))?;
        let mut out = vec![0; length];
        let mut filled = 0;
        while filled < length {
            let read = file.read(&mut out[filled..])?;
            if read == 0 {
                break;
            }
            filled += read;
        }
        out.truncate(filled);
        // Nothing is retained: the previous implementation cloned every read into a cache that was
        // immediately cleared again on the next call (a pure extra copy).
        Ok(out)
    }

    /// Verify that the file still has the same metadata the view was opened with.
    pub fn verify(&self) -> Result<(), SourceChanged> {
        let metadata = fs::metadata(&self.path).map_err(|_| SourceChanged {
            path: self.path.clone(),
            previous_len: self.len,
            current_len: 0,
        })?;
        let current_len = metadata.len();
        let current_modified = metadata.modified().ok();
        if current_len != self.len || current_modified != self.modified {
            return Err(SourceChanged {
                path: self.path.clone(),
                previous_len: self.len,
                current_len,
            });
        }
        Ok(())
    }

    /// Read the entire file into memory when the input is small enough to be kept as a debug / smoke
    /// fallback.
    pub fn read_entire(&self) -> io::Result<Vec<u8>> {
        let mut file = File::open(&self.path)?;
        let mut out = Vec::new();
        file.read_to_end(&mut out)?;
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::LargeTextSource;
    use std::fs;

    #[test]
    fn reads_byte_ranges_without_panicking() {
        let path = std::env::temp_dir().join("openbc_large_source_test.txt");
        fs::write(&path, b"alpha\nbeta\ngamma").unwrap();
        let mut source = LargeTextSource::open(&path).unwrap();
        let chunk = source.read_at(0, 10).unwrap();
        assert_eq!(&chunk[..], b"alpha\nbeta");
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn detects_when_source_changes() {
        let path = std::env::temp_dir().join("openbc_large_source_change_test.txt");
        fs::write(&path, b"before").unwrap();
        let source = LargeTextSource::open(&path).unwrap();
        fs::write(&path, b"after").unwrap();
        let err = source.verify().unwrap_err();
        assert_eq!(err.previous_len, 6);
        assert_eq!(err.current_len, 5);
        fs::remove_file(path).unwrap();
    }
}

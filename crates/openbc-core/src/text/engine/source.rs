//! Random-access byte storage: memory-mapped with a buffered fallback.
//!
//! The engine never copies a file into application-owned memory. [`MappedTextFile`] maps the file
//! (`memmap2`, which is portable across Linux, macOS and Windows) and hands out borrowed slices.
//! When mapping is refused (unsupported file system, 32-bit address-space limits, or an explicit
//! policy) it falls back to positional reads through a small bounded block cache.
//!
//! **Caveat (documented limitation):** if another process truncates a file while it is mapped,
//! touching the vanished pages can fault (`SIGBUS` on Unix). [`TextSource::verify_unchanged`] lets
//! the owner check size and modification time before trusting the mapping again; the file-watch
//! layer is expected to call it and rebuild the model when it fails.

use std::borrow::Cow;
use std::collections::VecDeque;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use memmap2::Mmap;

use super::error::EngineError;

/// Random access to the bytes of a text file.
pub trait TextSource: Send + Sync {
    /// Total length in bytes.
    fn len(&self) -> u64;

    /// True when the source has no bytes.
    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Bytes in `offset..offset+len`, clamped to the end of the source. Memory-mapped sources
    /// return a borrowed slice; buffered sources return an owned copy of just that range.
    fn read(&self, offset: u64, len: usize) -> io::Result<Cow<'_, [u8]>>;

    /// Whether `read` is zero-copy.
    fn is_mapped(&self) -> bool {
        false
    }

    /// Fails with [`EngineError::SourceChanged`] when the backing file no longer matches what was
    /// opened. In-memory sources always succeed.
    fn verify_unchanged(&self) -> Result<(), EngineError> {
        Ok(())
    }
}

/// How [`MappedTextFile::open_with`] should access the file.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum MapPolicy {
    /// Map when possible, otherwise use buffered reads.
    #[default]
    Auto,
    /// Never map; always use buffered positional reads (useful on network shares).
    Buffered,
}

const BLOCK_BYTES: usize = 1 << 20;
const BLOCK_CACHE_BLOCKS: usize = 8;

struct BufferedReader {
    file: Mutex<File>,
    blocks: Mutex<VecDeque<(u64, Arc<[u8]>)>>,
}

impl BufferedReader {
    fn block(&self, number: u64, file_len: u64) -> io::Result<Arc<[u8]>> {
        {
            let mut blocks = self.blocks.lock().unwrap_or_else(|e| e.into_inner());
            if let Some(pos) = blocks.iter().position(|(n, _)| *n == number) {
                let hit = blocks.remove(pos).expect("position is valid");
                blocks.push_back(hit.clone());
                return Ok(hit.1);
            }
        }
        let start = number * BLOCK_BYTES as u64;
        let want = (file_len.saturating_sub(start)).min(BLOCK_BYTES as u64) as usize;
        let mut data = vec![0u8; want];
        {
            let mut file = self.file.lock().unwrap_or_else(|e| e.into_inner());
            file.seek(SeekFrom::Start(start))?;
            file.read_exact(&mut data)?;
        }
        let data: Arc<[u8]> = data.into();
        let mut blocks = self.blocks.lock().unwrap_or_else(|e| e.into_inner());
        blocks.push_back((number, data.clone()));
        while blocks.len() > BLOCK_CACHE_BLOCKS {
            blocks.pop_front();
        }
        Ok(data)
    }
}

enum Backing {
    Empty,
    Mapped(Mmap),
    Buffered(BufferedReader),
}

/// A file opened for random access without loading it into memory.
pub struct MappedTextFile {
    path: PathBuf,
    len: u64,
    modified: Option<SystemTime>,
    backing: Backing,
}

impl MappedTextFile {
    /// Open with [`MapPolicy::Auto`].
    pub fn open(path: impl AsRef<Path>) -> Result<Self, EngineError> {
        Self::open_with(path, MapPolicy::Auto)
    }

    /// Open a file, choosing the access strategy by `policy`.
    pub fn open_with(path: impl AsRef<Path>, policy: MapPolicy) -> Result<Self, EngineError> {
        let path = path.as_ref().to_path_buf();
        let file = File::open(&path)?;
        let meta = file.metadata()?;
        if !meta.is_file() {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "not a regular file").into());
        }
        let len = meta.len();
        let modified = meta.modified().ok();
        let backing = if len == 0 {
            Backing::Empty
        } else if policy == MapPolicy::Auto && usize::try_from(len).is_ok() {
            // SAFETY: the mapping is read-only. Concurrent truncation by another process is the
            // documented limitation above; `verify_unchanged` is the mitigation.
            match unsafe { Mmap::map(&file) } {
                Ok(map) => Backing::Mapped(map),
                Err(_) => Backing::Buffered(BufferedReader {
                    file: Mutex::new(file),
                    blocks: Mutex::new(VecDeque::new()),
                }),
            }
        } else {
            Backing::Buffered(BufferedReader {
                file: Mutex::new(file),
                blocks: Mutex::new(VecDeque::new()),
            })
        };
        Ok(Self { path, len, modified, backing })
    }

    /// Path this source was opened from.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl TextSource for MappedTextFile {
    fn len(&self) -> u64 {
        self.len
    }

    fn read(&self, offset: u64, len: usize) -> io::Result<Cow<'_, [u8]>> {
        if offset >= self.len || len == 0 {
            return Ok(Cow::Borrowed(&[]));
        }
        let len = len.min((self.len - offset).min(usize::MAX as u64) as usize);
        match &self.backing {
            Backing::Empty => Ok(Cow::Borrowed(&[])),
            Backing::Mapped(map) => {
                let start = offset as usize;
                Ok(Cow::Borrowed(&map[start..start + len]))
            }
            Backing::Buffered(reader) => {
                let first = offset / BLOCK_BYTES as u64;
                let last = (offset + len as u64 - 1) / BLOCK_BYTES as u64;
                if first == last {
                    let block = reader.block(first, self.len)?;
                    let inner = (offset - first * BLOCK_BYTES as u64) as usize;
                    return Ok(Cow::Owned(block[inner..inner + len].to_vec()));
                }
                let mut out = Vec::with_capacity(len);
                for number in first..=last {
                    let block = reader.block(number, self.len)?;
                    let block_start = number * BLOCK_BYTES as u64;
                    let from = offset.saturating_sub(block_start) as usize;
                    let to = ((offset + len as u64).min(block_start + block.len() as u64)
                        - block_start) as usize;
                    out.extend_from_slice(&block[from..to]);
                }
                Ok(Cow::Owned(out))
            }
        }
    }

    fn is_mapped(&self) -> bool {
        matches!(self.backing, Backing::Mapped(_))
    }

    fn verify_unchanged(&self) -> Result<(), EngineError> {
        let meta = std::fs::metadata(&self.path).map_err(|_| EngineError::SourceChanged)?;
        if meta.len() != self.len || meta.modified().ok() != self.modified {
            return Err(EngineError::SourceChanged);
        }
        Ok(())
    }
}

/// An in-memory source, for tests and for already-materialised small inputs.
#[derive(Clone, Debug, Default)]
pub struct MemorySource(Arc<Vec<u8>>);

impl MemorySource {
    /// Wrap owned bytes.
    #[must_use]
    pub fn new(bytes: impl Into<Vec<u8>>) -> Self {
        Self(Arc::new(bytes.into()))
    }
}

impl TextSource for MemorySource {
    fn len(&self) -> u64 {
        self.0.len() as u64
    }

    fn read(&self, offset: u64, len: usize) -> io::Result<Cow<'_, [u8]>> {
        let total = self.0.len() as u64;
        if offset >= total {
            return Ok(Cow::Borrowed(&[]));
        }
        let start = offset as usize;
        let end = start.saturating_add(len).min(self.0.len());
        Ok(Cow::Borrowed(&self.0[start..end]))
    }

    fn is_mapped(&self) -> bool {
        true
    }
}

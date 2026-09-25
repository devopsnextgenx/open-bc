//! Lazy virtual filesystem contracts and the local filesystem adapter.

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath, FolderMetadata};
use std::path::{Path, PathBuf};
use thiserror::Error;
use tokio::io::{AsyncRead, AsyncReadExt};

/// Errors returned by a virtual filesystem provider.
#[derive(Debug, Error)]
pub enum VfsError {
    /// The provider could not complete an operating-system operation.
    #[error("{operation} failed for {path}: {source}")]
    Io {
        operation: &'static str,
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    /// The provider does not implement an optional operation.
    #[error("operation {0} is not supported by this provider")]
    Unsupported(&'static str),
}

/// A readable, lazy virtual filesystem.
#[async_trait]
pub trait AsyncVfs: Send + Sync {
    /// Read exactly one directory level.
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError>;

    /// Retrieve metadata without recursively scanning children.
    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError>;

    /// Open a regular file as an asynchronous byte stream.
    async fn open_file(
        &self,
        path: &EntryPath,
    ) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError>;
}

/// Local filesystem implementation rooted at a configured directory.
#[derive(Clone, Debug)]
pub struct LocalVfs {
    root: PathBuf,
}

impl LocalVfs {
    /// Create a provider rooted at `root`.
    ///
    /// ```
    /// use openbc_vfs::LocalVfs;
    /// let _vfs = LocalVfs::new(".");
    /// ```
    #[must_use]
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    fn resolve(&self, path: &EntryPath) -> PathBuf {
        self.root.join(&path.0)
    }
}

#[async_trait]
impl AsyncVfs for LocalVfs {
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        let started = std::time::Instant::now();
        let base = self.resolve(path);
        let mut entries = tokio::fs::read_dir(&base)
            .await
            .map_err(|source| VfsError::Io {
                operation: "read_dir",
                path: base.clone(),
                source,
            })?;
        let mut result = Vec::new();
        while let Some(entry) = entries.next_entry().await.map_err(|source| VfsError::Io {
            operation: "read_dir",
            path: base.clone(),
            source,
        })? {
            let metadata = entry.metadata().await.map_err(|source| VfsError::Io {
                operation: "metadata",
                path: entry.path(),
                source,
            })?;
            let relative = entry
                .path()
                .strip_prefix(&self.root)
                .unwrap_or(entry.path().as_path())
                .to_path_buf();
            result.push(DirectoryEntry {
                name: entry.file_name().to_string_lossy().into_owned(),
                path: EntryPath(relative),
                metadata: EntryMetadata {
                    size: metadata.len(),
                    modified: metadata.modified().ok(),
                    is_dir: metadata.is_dir(),
                },
            });
        }
        openbc_observability::record(
            "openbc-vfs",
            "read_dir",
            started.elapsed(),
            None,
            None,
            Some(result.iter().map(|entry| entry.metadata.size).sum()),
            Some(path.0.components().count()),
            Some("local filesystem".to_owned()),
        );
        Ok(result)
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        let resolved = self.resolve(path);
        let metadata = tokio::fs::metadata(&resolved)
            .await
            .map_err(|source| VfsError::Io {
                operation: "metadata",
                path: resolved,
                source,
            })?;
        Ok(EntryMetadata {
            size: metadata.len(),
            modified: metadata.modified().ok(),
            is_dir: metadata.is_dir(),
        })
    }

    async fn open_file(
        &self,
        path: &EntryPath,
    ) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        let resolved = self.resolve(path);
        let file = tokio::fs::File::open(&resolved)
            .await
            .map_err(|source| VfsError::Io {
                operation: "open_file",
                path: resolved,
                source,
            })?;
        Ok(Box::new(file))
    }
}

/// Read a provider file into memory for small-file comparisons.
pub async fn read_small_file(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<Vec<u8>, VfsError> {
    let mut reader = vfs.open_file(path).await?;
    let mut bytes = Vec::new();
    reader
        .read_to_end(&mut bytes)
        .await
        .map_err(|source| VfsError::Io {
            operation: "read_file",
            path: Path::new(&path.0).to_path_buf(),
            source,
        })?;
    Ok(bytes)
}

/// Read one folder level and return the metadata needed by a folder view.
pub async fn read_folder_level(
    vfs: &dyn AsyncVfs,
    path: &EntryPath,
) -> Result<(FolderMetadata, Vec<DirectoryEntry>), VfsError> {
    let entries = vfs.read_dir(path).await?;
    let metadata = vfs.stat(path).await?;
    let folder_metadata = FolderMetadata {
        path: path.clone(),
        item_count: entries.len(),
        size: entries.iter().map(|entry| entry.metadata.size).sum(),
        modified: metadata.modified,
        readable: true,
    };
    Ok((folder_metadata, entries))
}

//! Lazy virtual filesystem contracts and the local filesystem adapter.

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath, FolderMetadata};
use std::path::{Path, PathBuf};
use thiserror::Error;
use tokio::io::{AsyncRead, AsyncReadExt};

#[cfg(feature = "sftp")]
pub mod sftp;
#[cfg(feature = "ftp")]
pub mod ftp;
#[cfg(feature = "network")]
pub mod network;

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

    /// Replace a file's contents with the supplied bytes.
    async fn write_file(&self, _path: &EntryPath, _bytes: &[u8]) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("write_file"))
    }

    /// Create one directory at the requested path.
    async fn create_dir(&self, _path: &EntryPath) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("create_dir"))
    }

    /// Rename a file or directory within this provider.
    async fn rename(&self, _from: &EntryPath, _to: &EntryPath) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("rename"))
    }
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
            let link_metadata = tokio::fs::symlink_metadata(entry.path())
                .await
                .map_err(|source| VfsError::Io {
                    operation: "symlink_metadata",
                    path: entry.path(),
                    source,
                })?;
            let metadata = if link_metadata.file_type().is_symlink() {
                tokio::fs::metadata(entry.path())
                    .await
                    .unwrap_or_else(|_| link_metadata.clone())
            } else {
                link_metadata.clone()
            };
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
                    is_link: link_metadata.file_type().is_symlink(),
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
                path: resolved.clone(),
                source,
            })?;
        let link_metadata = tokio::fs::symlink_metadata(&resolved)
            .await
            .map_err(|source| VfsError::Io {
                operation: "symlink_metadata",
                path: resolved.clone(),
                source,
            })?;
        Ok(EntryMetadata {
            size: metadata.len(),
            modified: metadata.modified().ok(),
            is_dir: metadata.is_dir(),
            is_link: link_metadata.file_type().is_symlink(),
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

    async fn write_file(&self, path: &EntryPath, bytes: &[u8]) -> Result<(), VfsError> {
        let resolved = self.resolve(path);
        tokio::fs::write(&resolved, bytes)
            .await
            .map_err(|source| VfsError::Io {
                operation: "write_file",
                path: resolved,
                source,
            })
    }

    async fn create_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        let resolved = self.resolve(path);
        tokio::fs::create_dir(&resolved)
            .await
            .map_err(|source| VfsError::Io {
                operation: "create_dir",
                path: resolved,
                source,
            })
    }

    async fn rename(&self, from: &EntryPath, to: &EntryPath) -> Result<(), VfsError> {
        let source = self.resolve(from);
        let destination = self.resolve(to);
        tokio::fs::rename(&source, &destination)
            .await
            .map_err(|source_error| VfsError::Io {
                operation: "rename",
                path: source,
                source: source_error,
            })
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

/// A connection profile that can be turned into a live [`AsyncVfs`] with
/// [`build_vfs`]. This is the Rust-side counterpart of the UI's saved
/// connection profile (protocol + host + credentials + root path); the two
/// are kept as separate types deliberately - the UI profile also carries
/// display-only fields (a name, a description, "remember this" flags) that
/// have no business in the VFS layer - but they should map onto each other
/// one-to-one wherever this crate is wired up behind the UI.
pub enum ConnectionProfile {
    Local {
        root: PathBuf,
    },
    #[cfg(feature = "sftp")]
    Sftp(sftp::SftpConfig),
    #[cfg(feature = "ftp")]
    Ftp(ftp::FtpConfig),
    #[cfg(feature = "network")]
    Network(network::NetworkShareConfig),
}

/// Build the right provider for a profile. This is the single place that
/// needs to change when a new protocol is added, so the UI layer and any
/// callers only ever depend on [`AsyncVfs`] and never on a concrete
/// provider type.
pub async fn build_vfs(profile: ConnectionProfile) -> Result<Box<dyn AsyncVfs>, VfsError> {
    match profile {
        ConnectionProfile::Local { root } => Ok(Box::new(LocalVfs::new(root))),
        #[cfg(feature = "sftp")]
        ConnectionProfile::Sftp(config) => Ok(Box::new(sftp::SftpVfs::connect(config).await?)),
        #[cfg(feature = "ftp")]
        ConnectionProfile::Ftp(config) => Ok(Box::new(ftp::FtpVfs::connect(config).await?)),
        #[cfg(feature = "network")]
        ConnectionProfile::Network(config) => Ok(Box::new(network::NetworkVfs::open(config).await?)),
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::{AsyncVfs, EntryPath, LocalVfs};

    #[tokio::test]
    async fn local_listing_keeps_valid_and_dangling_links_without_following_them() {
        let root = std::env::temp_dir().join(format!(
            "openbc-vfs-links-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        tokio::fs::create_dir_all(root.join("target")).await.unwrap();
        std::os::unix::fs::symlink(root.join("target"), root.join("directory-link")).unwrap();
        std::os::unix::fs::symlink(root.join("missing"), root.join("dangling-link")).unwrap();

        let entries = LocalVfs::new(&root)
            .read_dir(&EntryPath("".into()))
            .await
            .unwrap();
        let directory_link = entries.iter().find(|entry| entry.name == "directory-link").unwrap();
        assert!(directory_link.metadata.is_dir);
        assert!(directory_link.metadata.is_link);
        let dangling_link = entries.iter().find(|entry| entry.name == "dangling-link").unwrap();
        assert!(!dangling_link.metadata.is_dir);
        assert!(dangling_link.metadata.is_link);

        tokio::fs::remove_dir_all(root).await.unwrap();
    }
}

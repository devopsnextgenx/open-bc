//! Network-share virtual filesystem provider, for Samba/SMB/CIFS shares and
//! plain mapped network drives.
//!
//! There is no mature pure-Rust SMB client, and hand-rolling the SMB2/SMB3
//! protocol is well outside what this crate should own. The pragmatic (and
//! honestly, the most compatible) approach is the same one every desktop
//! file manager takes: let the operating system mount the share - Windows
//! via a drive letter or a `\\host\share` UNC path, macOS/Linux via
//! `mount_smbfs`/`gvfs`/`cifs-utils` onto a local mountpoint - and then
//! treat that mounted path as an ordinary local directory. `NetworkVfs` is
//! therefore a thin, explicitly-named wrapper around [`crate::LocalVfs`]
//! rooted at the mount path, kept as its own type so callers (and the UI
//! layer) can distinguish "this is a network share the user picked" from
//! "this is a plain local folder" for things like connection-status display
//! and reconnect prompts, without changing the read behaviour at all.

use std::path::PathBuf;

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use tokio::io::AsyncRead;

use crate::{AsyncVfs, LocalVfs, VfsError};

/// Where a [`NetworkVfs`] should look for its already-mounted share.
#[derive(Clone, Debug)]
pub struct NetworkShareConfig {
    /// Display host name, e.g. `fileserver` - used only for error messages
    /// and UI labels, since by the time this runs the OS has already
    /// resolved and mounted the share.
    pub host: String,
    pub share_name: String,
    /// Local path the share is mounted at: a drive letter root like
    /// `Z:\` on Windows, or a mountpoint like `/mnt/fileserver-share` on
    /// Linux/macOS.
    pub mount_path: PathBuf,
}

/// SMB/network-share-backed [`AsyncVfs`]. See the module docs for why this
/// delegates to an already-mounted path instead of speaking SMB directly.
pub struct NetworkVfs {
    inner: LocalVfs,
    host: String,
    share_name: String,
}

impl NetworkVfs {
    /// Wrap an already-mounted share. Fails fast with a clear error if the
    /// mount path doesn't exist or isn't a directory, since that almost
    /// always means the share isn't actually mounted yet (stale drive
    /// letter, VPN dropped, credentials expired, etc.) rather than a
    /// permissions problem inside the share.
    pub async fn open(config: NetworkShareConfig) -> Result<Self, VfsError> {
        let metadata = tokio::fs::metadata(&config.mount_path)
            .await
            .map_err(|source| VfsError::Io {
                operation: "open_mount",
                path: config.mount_path.clone(),
                source,
            })?;
        if !metadata.is_dir() {
            return Err(VfsError::Io {
                operation: "open_mount",
                path: config.mount_path.clone(),
                source: std::io::Error::new(
                    std::io::ErrorKind::NotADirectory,
                    "mount path exists but is not a directory - is the share actually mounted?",
                ),
            });
        }
        Ok(NetworkVfs {
            inner: LocalVfs::new(config.mount_path),
            host: config.host,
            share_name: config.share_name,
        })
    }

    pub fn host(&self) -> &str {
        &self.host
    }

    pub fn share_name(&self) -> &str {
        &self.share_name
    }
}

#[async_trait]
impl AsyncVfs for NetworkVfs {
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        self.inner.read_dir(path).await
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        self.inner.stat(path).await
    }

    async fn open_file(&self, path: &EntryPath) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        self.inner.open_file(path).await
    }

    async fn write_file(&self, path: &EntryPath, bytes: &[u8]) -> Result<(), VfsError> {
        self.inner.write_file(path, bytes).await
    }

    async fn create_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        self.inner.create_dir(path).await
    }

    async fn rename(&self, from: &EntryPath, to: &EntryPath) -> Result<(), VfsError> {
        self.inner.rename(from, to).await
    }

    async fn remove_file(&self, path: &EntryPath) -> Result<(), VfsError> {
        self.inner.remove_file(path).await
    }

    async fn remove_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        self.inner.remove_dir(path).await
    }
}

//! FTP / explicit-FTPS virtual filesystem provider, backed by `suppaftp`'s
//! tokio-async client.
//!
//! `suppaftp`'s async stream types (`AsyncFtpStream`, `AsyncNativeTlsConnector`,
//! `AsyncNativeTlsFtpStream`) live under `suppaftp::tokio` rather than the
//! crate root as of the version this workspace resolves to - the crate's
//! `async` feature was split into separate `smol`/`tokio` backends, each
//! with its own stream types, some time after this module was first
//! written. If the workspace's lockfile moves to a version with a
//! differently-shaped `tokio` module (or renames methods like `list`/
//! `list_file`, `retr_as_stream`, `finalize_retr_stream`, `into_secure`),
//! recheck this file's imports and the handful of calls below (`login`,
//! `cwd`, `pwd`, `list`, `retr_as_stream`, `into_secure`) against that
//! version's docs.rs source before relying on this in production.
//!
//! Two API quirks worth flagging explicitly, since they're easy to get
//! wrong against this crate's async API:
//!
//! - `into_secure` does NOT turn a plain `ImplAsyncFtpStream<AsyncNoTlsStream>`
//!   into a TLS one - its bound is `impl AsyncTlsConnector<Stream = T>` for
//!   the *same* `T` the stream already has. To do explicit FTPS you must
//!   connect with `AsyncNativeTlsFtpStream::connect(..)` (i.e. already typed
//!   for TLS) and then call `.into_secure(..)` on that to perform the
//!   handshake.
//! - There is no `retr_as_buffer` on the async stream (that's sync-only);
//!   the async equivalent is `retr_as_stream` + read to end +
//!   `finalize_retr_stream`.

use std::path::PathBuf;
use std::time::Duration;

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use suppaftp::list::File as FtpListEntry;
use suppaftp::tokio::{
    AsyncFtpStream, AsyncNativeTlsConnector, AsyncNativeTlsFtpStream,
};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt};

use crate::{AsyncVfs, VfsError};

/// Whether to use plain FTP or upgrade to explicit FTPS (`AUTH TLS`) right
/// after connecting. Implicit FTPS (TLS from the first byte) is legacy and
/// intentionally not offered here; prefer explicit mode or SFTP.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FtpSecurity {
    Plain,
    ExplicitTls,
}

/// Connection details for an FTP/FTPS provider.
#[derive(Clone, Debug)]
pub struct FtpConfig {
    pub host: String,
    pub port: u16,
    pub security: FtpSecurity,
    pub username: String,
    pub password: String,
    /// Remote directory that `EntryPath("")` resolves to, e.g. `/public_html`.
    pub root: String,
    pub passive_mode: bool,
    pub connect_timeout_secs: u32,
}

impl Default for FtpConfig {
    fn default() -> Self {
        Self {
            host: String::new(),
            port: 21,
            security: FtpSecurity::ExplicitTls,
            username: "anonymous".into(),
            password: String::new(),
            root: "/".into(),
            passive_mode: true,
            connect_timeout_secs: 15,
        }
    }
}

/// The two stream flavours `suppaftp` gives us depending on whether we
/// upgraded to TLS. Kept as an enum (rather than a trait object) so every
/// operation stays a plain, explicit match.
enum FtpConnection {
    Plain(AsyncFtpStream),
    Tls(Box<AsyncNativeTlsFtpStream>),
}

/// FTP/FTPS-backed [`AsyncVfs`]. Roots all paths at `config.root` on the
/// remote host, the same way [`crate::LocalVfs`] roots them at a local
/// directory. Each call takes an exclusive lock on the single underlying
/// control connection, mirroring the one-op-at-a-time trade-off documented
/// on [`crate::sftp::SftpVfs`].
pub struct FtpVfs {
    connection: tokio::sync::Mutex<FtpConnection>,
    root: String,
}

impl FtpVfs {
    /// Connect, optionally upgrade to explicit TLS, log in, and set the
    /// working directory to `config.root`.
    pub async fn connect(config: FtpConfig) -> Result<Self, VfsError> {
        let address = format!("{}:{}", config.host, config.port);
        let connection = match config.security {
            FtpSecurity::Plain => {
                let mut stream = timeout(config.connect_timeout_secs, AsyncFtpStream::connect(address.as_str()))
                    .await
                    .map_err(|err| ftp_err("connect", &config.host, err))?;
                if config.passive_mode {
                    stream.set_mode(suppaftp::Mode::Passive);
                }
                stream
                    .login(&config.username, &config.password)
                    .await
                    .map_err(|err| ftp_err("login", &config.host, err))?;
                FtpConnection::Plain(stream)
            }
            FtpSecurity::ExplicitTls => {
                // Must connect already typed for TLS (`AsyncNativeTlsFtpStream`,
                // not `AsyncFtpStream`) - `into_secure` performs the handshake
                // on a stream of that type, it does not convert one.
                let stream = timeout(
                    config.connect_timeout_secs,
                    AsyncNativeTlsFtpStream::connect(address.as_str()),
                )
                .await
                .map_err(|err| ftp_err("connect", &config.host, err))?;
                let connector = AsyncNativeTlsConnector::from(
                    suppaftp::async_native_tls::TlsConnector::new(),
                );
                let mut stream = stream
                    .into_secure(connector, &config.host)
                    .await
                    .map_err(|err| ftp_err("starttls", &config.host, err))?;
                if config.passive_mode {
                    stream.set_mode(suppaftp::Mode::Passive);
                }
                stream
                    .login(&config.username, &config.password)
                    .await
                    .map_err(|err| ftp_err("login", &config.host, err))?;
                FtpConnection::Tls(Box::new(stream))
            }
        };

        let vfs = FtpVfs { connection: tokio::sync::Mutex::new(connection), root: config.root };
        vfs.cwd(&vfs.root).await?;
        Ok(vfs)
    }

    async fn cwd(&self, path: &str) -> Result<(), VfsError> {
        let mut guard = self.connection.lock().await;
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.cwd(path).await,
            FtpConnection::Tls(stream) => stream.cwd(path).await,
        };
        result.map_err(|err| ftp_err("cwd", path, err))
    }

    fn resolve(&self, path: &EntryPath) -> String {
        join_remote(&self.root, &path.0.to_string_lossy())
    }

    async fn list_raw(&self, remote_path: &str) -> Result<Vec<String>, VfsError> {
        let mut guard = self.connection.lock().await;
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.list(Some(remote_path)).await,
            FtpConnection::Tls(stream) => stream.list(Some(remote_path)).await,
        };
        result.map_err(|err| ftp_err("list", remote_path, err))
    }

    /// Download `remote_path` in full, using the stream-based `retr_as_stream`
    /// / `finalize_retr_stream` pair (the async client has no `retr_as_buffer`
    /// - that method only exists on the sync `ImplFtpStream`).
    async fn retr_to_buffer(&self, remote_path: &str) -> Result<Vec<u8>, VfsError> {
        let mut guard = self.connection.lock().await;
        let mut buffer = Vec::new();
        match &mut *guard {
            FtpConnection::Plain(stream) => {
                let mut data_stream = stream
                    .retr_as_stream(remote_path)
                    .await
                    .map_err(|err| ftp_err("retr", remote_path, err))?;
                data_stream.read_to_end(&mut buffer).await.map_err(|err| VfsError::Io {
                    operation: "retr",
                    path: PathBuf::from(remote_path),
                    source: err,
                })?;
                stream
                    .finalize_retr_stream(data_stream)
                    .await
                    .map_err(|err| ftp_err("retr", remote_path, err))?;
            }
            FtpConnection::Tls(stream) => {
                let mut data_stream = stream
                    .retr_as_stream(remote_path)
                    .await
                    .map_err(|err| ftp_err("retr", remote_path, err))?;
                data_stream.read_to_end(&mut buffer).await.map_err(|err| VfsError::Io {
                    operation: "retr",
                    path: PathBuf::from(remote_path),
                    source: err,
                })?;
                stream
                    .finalize_retr_stream(data_stream)
                    .await
                    .map_err(|err| ftp_err("retr", remote_path, err))?;
            }
        }
        Ok(buffer)
    }
}

#[async_trait]
impl AsyncVfs for FtpVfs {
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        let remote_path = self.resolve(path);
        let lines = self.list_raw(&remote_path).await?;
        let mut result = Vec::with_capacity(lines.len());
        for line in lines {
            let Ok(entry) = FtpListEntry::try_from(line.as_str()) else { continue };
            let name = entry.name().to_string();
            if name == "." || name == ".." {
                continue;
            }
            let relative = PathBuf::from(&path.0).join(&name);
            result.push(DirectoryEntry {
                name,
                path: EntryPath(relative),
                metadata: EntryMetadata {
                    size: entry.size() as u64,
                    modified: Some(entry.modified()),
                    is_dir: entry.is_directory(),
                    is_link: entry.is_symlink(),
                },
            });
        }
        Ok(result)
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        // Plain FTP has no single "stat a path" command; list the parent
        // directory and find the matching entry, same trick most FTP
        // clients use.
        let parent = path.0.parent().map(|p| EntryPath(p.to_path_buf())).unwrap_or_else(|| EntryPath(PathBuf::new()));
        let name = path.0.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let siblings = self.read_dir(&parent).await?;
        siblings
            .into_iter()
            .find(|entry| entry.name == name)
            .map(|entry| entry.metadata)
            .ok_or_else(|| VfsError::Io {
                operation: "stat",
                path: PathBuf::from(&path.0),
                source: std::io::Error::new(std::io::ErrorKind::NotFound, "not found in parent listing"),
            })
    }

    async fn open_file(&self, path: &EntryPath) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        let remote_path = self.resolve(path);
        let bytes = self.retr_to_buffer(&remote_path).await?;

        let (mut writer, reader) = tokio::io::duplex(64 * 1024);
        tokio::spawn(async move {
            let _ = writer.write_all(&bytes).await;
        });
        Ok(Box::new(reader))
    }

    async fn write_file(&self, path: &EntryPath, bytes: &[u8]) -> Result<(), VfsError> {
        let remote_path = self.resolve(path);
        let mut guard = self.connection.lock().await;
        let mut reader = tokio::io::BufReader::new(std::io::Cursor::new(bytes.to_vec()));
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.put_file(&remote_path, &mut reader).await,
            FtpConnection::Tls(stream) => stream.put_file(&remote_path, &mut reader).await,
        };
        result.map(|_| ()).map_err(|err| ftp_err("write_file", &remote_path, err))
    }

    async fn create_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        let remote_path = self.resolve(path);
        let mut guard = self.connection.lock().await;
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.mkdir(&remote_path).await,
            FtpConnection::Tls(stream) => stream.mkdir(&remote_path).await,
        };
        result.map_err(|err| ftp_err("create_dir", &remote_path, err))
    }

    async fn rename(&self, from: &EntryPath, to: &EntryPath) -> Result<(), VfsError> {
        let source = self.resolve(from);
        let destination = self.resolve(to);
        let mut guard = self.connection.lock().await;
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.rename(&source, &destination).await,
            FtpConnection::Tls(stream) => stream.rename(&source, &destination).await,
        };
        result.map_err(|err| ftp_err("rename", &source, err))
    }

    async fn remove_file(&self, path: &EntryPath) -> Result<(), VfsError> {
        let remote_path = self.resolve(path);
        let mut guard = self.connection.lock().await;
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.rm(&remote_path).await,
            FtpConnection::Tls(stream) => stream.rm(&remote_path).await,
        };
        result.map_err(|err| ftp_err("remove_file", &remote_path, err))
    }

    async fn remove_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        let remote_path = self.resolve(path);
        let mut guard = self.connection.lock().await;
        let result = match &mut *guard {
            FtpConnection::Plain(stream) => stream.rmdir(&remote_path).await,
            FtpConnection::Tls(stream) => stream.rmdir(&remote_path).await,
        };
        result.map_err(|err| ftp_err("remove_dir", &remote_path, err))
    }
}

fn join_remote(root: &str, relative: &str) -> String {
    if relative.is_empty() {
        return root.to_string();
    }
    format!("{}/{}", root.trim_end_matches('/'), relative.trim_start_matches('/'))
}

async fn timeout<T>(
    secs: u32,
    future: impl std::future::Future<Output = Result<T, suppaftp::FtpError>>,
) -> Result<T, suppaftp::FtpError> {
    match tokio::time::timeout(Duration::from_secs(u64::from(secs.max(1))), future).await {
        Ok(result) => result,
        Err(_) => Err(suppaftp::FtpError::ConnectionError(std::io::Error::new(
            std::io::ErrorKind::TimedOut,
            "connect timed out",
        ))),
    }
}

fn ftp_err(operation: &'static str, path: &str, err: suppaftp::FtpError) -> VfsError {
    VfsError::Io {
        operation,
        path: PathBuf::from(path),
        source: std::io::Error::new(std::io::ErrorKind::Other, err.to_string()),
    }
}
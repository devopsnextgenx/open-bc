//! FTP / explicit-FTPS virtual filesystem provider, backed by `suppaftp`'s
//! tokio-async client.
//!
//! `suppaftp`'s method names have moved around a little across major
//! versions (`list` vs `list_file`, the exact `File` parsing type, etc.).
//! The calls below match the `tokio` + `tokio-async-native-tls` feature set
//! pinned in `Cargo.toml`; if the workspace's lockfile resolves to a
//! noticeably older or newer `suppaftp`, check that crate's changelog
//! against the handful of calls here (`login`, `cwd`, `pwd`, `list`,
//! `retr_as_buffer`, `into_secure`) before relying on this in production.

use std::path::PathBuf;
use std::time::Duration;

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use suppaftp::list::File as FtpListEntry;
use suppaftp::{AsyncFtpStream, AsyncNativeTlsConnector, AsyncNativeTlsFtpStream};
use tokio::io::{AsyncRead, AsyncWriteExt};

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
                let stream = timeout(config.connect_timeout_secs, AsyncFtpStream::connect(address.as_str()))
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
                },
            });
        }
        Ok(result)
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        // Plain FTP has no single "stat a path" command; list the parent
        // directory and find the matching entry, same trick most FTP
        // clients use.
        let parent = path.0.parent().map(EntryPath::from).unwrap_or_else(|| EntryPath(PathBuf::new()));
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
        let bytes = {
            let mut guard = self.connection.lock().await;
            let result = match &mut *guard {
                FtpConnection::Plain(stream) => stream.retr_as_buffer(&remote_path).await,
                FtpConnection::Tls(stream) => stream.retr_as_buffer(&remote_path).await,
            };
            result
                .map_err(|err| ftp_err("retr", &remote_path, err))?
                .into_inner()
        };

        let (mut writer, reader) = tokio::io::duplex(64 * 1024);
        tokio::spawn(async move {
            let _ = writer.write_all(&bytes).await;
        });
        Ok(Box::new(reader))
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

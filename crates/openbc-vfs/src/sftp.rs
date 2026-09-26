//! SFTP virtual filesystem provider, backed by `ssh2`.
//!
//! `ssh2` exposes a synchronous (blocking) API, so every operation here runs
//! inside `tokio::task::spawn_blocking`. The underlying `ssh2::Session` is
//! not safe to drive concurrently from multiple threads, so it is guarded
//! by a `std::sync::Mutex`: only one SFTP operation is in flight at a time
//! per [`SftpVfs`] instance. That is a fine trade-off for a compare tool
//! (folder scans and whole-file reads) but would need a connection pool for
//! heavy parallel throughput.

use std::io::Read;
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::UNIX_EPOCH;

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use ssh2::Session;
use tokio::io::{AsyncRead, AsyncWriteExt};

use crate::{AsyncVfs, VfsError};

/// How an [`SftpVfs`] should authenticate to the remote host.
#[derive(Clone, Debug)]
pub enum SftpAuth {
    /// Username/password authentication.
    Password { username: String, password: String },
    /// Public-key authentication using a private key file on disk.
    PrivateKey {
        username: String,
        private_key: PathBuf,
        passphrase: Option<String>,
    },
}

/// Connection details for an SFTP provider.
#[derive(Clone, Debug)]
pub struct SftpConfig {
    pub host: String,
    pub port: u16,
    pub auth: SftpAuth,
    /// Remote directory that `EntryPath("")` resolves to, e.g. `/home/me`.
    pub root: PathBuf,
    /// Timeout for the initial TCP connect and SSH handshake, in seconds.
    pub connect_timeout_secs: u32,
}

impl Default for SftpConfig {
    fn default() -> Self {
        Self {
            host: String::new(),
            port: 22,
            auth: SftpAuth::Password { username: String::new(), password: String::new() },
            root: PathBuf::from("/"),
            connect_timeout_secs: 15,
        }
    }
}

/// SFTP-backed [`AsyncVfs`]. Roots all paths at `config.root` on the remote
/// host, the same way [`crate::LocalVfs`] roots them at a local directory.
pub struct SftpVfs {
    session: Arc<Mutex<Session>>,
    root: PathBuf,
}

impl SftpVfs {
    /// Open a TCP connection, negotiate SSH and authenticate. Resolves once
    /// the session is ready to perform SFTP operations.
    pub async fn connect(config: SftpConfig) -> Result<Self, VfsError> {
        tokio::task::spawn_blocking(move || Self::connect_blocking(config))
            .await
            .map_err(join_error)?
    }

    fn connect_blocking(config: SftpConfig) -> Result<Self, VfsError> {
        let tcp = TcpStream::connect((config.host.as_str(), config.port)).map_err(|source| {
            VfsError::Io { operation: "tcp_connect", path: PathBuf::from(&config.host), source }
        })?;
        tcp.set_read_timeout(Some(std::time::Duration::from_secs(u64::from(
            config.connect_timeout_secs.max(1),
        ))))
        .ok();

        let mut session = Session::new().map_err(|err| ssh_err("session_new", &config.root, &err))?;
        session.set_tcp_stream(tcp);
        session.handshake().map_err(|err| ssh_err("handshake", &config.root, &err))?;

        match &config.auth {
            SftpAuth::Password { username, password } => session
                .userauth_password(username, password)
                .map_err(|err| ssh_err("userauth_password", &config.root, &err))?,
            SftpAuth::PrivateKey { username, private_key, passphrase } => session
                .userauth_pubkey_file(username, None, private_key, passphrase.as_deref())
                .map_err(|err| ssh_err("userauth_pubkey_file", &config.root, &err))?,
        }

        if !session.authenticated() {
            return Err(VfsError::Io {
                operation: "userauth",
                path: PathBuf::from(&config.host),
                source: std::io::Error::new(std::io::ErrorKind::PermissionDenied, "authentication was rejected"),
            });
        }

        Ok(SftpVfs { session: Arc::new(Mutex::new(session)), root: config.root })
    }

    fn resolve(&self, path: &EntryPath) -> PathBuf {
        self.root.join(&path.0)
    }
}

fn ssh_err(operation: &'static str, path: &Path, err: &ssh2::Error) -> VfsError {
    VfsError::Io {
        operation,
        path: path.to_path_buf(),
        source: std::io::Error::new(std::io::ErrorKind::Other, err.to_string()),
    }
}

fn join_error(err: tokio::task::JoinError) -> VfsError {
    VfsError::Io {
        operation: "spawn_blocking",
        path: PathBuf::new(),
        source: std::io::Error::new(std::io::ErrorKind::Other, err.to_string()),
    }
}

#[async_trait]
impl AsyncVfs for SftpVfs {
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        let session = self.session.clone();
        let root = self.root.clone();
        let resolved = self.resolve(path);
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            let listing = sftp.readdir(&resolved).map_err(|err| ssh_err("readdir", &resolved, &err))?;
            let mut result = Vec::with_capacity(listing.len());
            for (entry_path, stat) in listing {
                let name = entry_path
                    .file_name()
                    .map(|name| name.to_string_lossy().into_owned())
                    .unwrap_or_default();
                if name == "." || name == ".." {
                    continue;
                }
                let relative = entry_path.strip_prefix(&root).unwrap_or(&entry_path).to_path_buf();
                result.push(DirectoryEntry {
                    name,
                    path: EntryPath(relative),
                    metadata: EntryMetadata {
                        size: stat.size.unwrap_or(0),
                        modified: stat
                            .mtime
                            .map(|secs| UNIX_EPOCH + std::time::Duration::from_secs(secs)),
                        is_dir: stat.is_dir(),
                    },
                });
            }
            Ok(result)
        })
        .await
        .map_err(join_error)?
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        let session = self.session.clone();
        let resolved = self.resolve(path);
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            let stat = sftp.stat(&resolved).map_err(|err| ssh_err("stat", &resolved, &err))?;
            Ok(EntryMetadata {
                size: stat.size.unwrap_or(0),
                modified: stat.mtime.map(|secs| UNIX_EPOCH + std::time::Duration::from_secs(secs)),
                is_dir: stat.is_dir(),
            })
        })
        .await
        .map_err(join_error)?
    }

    async fn open_file(&self, path: &EntryPath) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        let session = self.session.clone();
        let resolved = self.resolve(path);
        // ssh2's file handle is blocking and borrows the session, so it
        // can't be handed back across the spawn_blocking boundary. Instead
        // we read the whole file inside the blocking task (fine for the
        // small-file comparisons this trait is used for - see
        // `read_small_file` in lib.rs) and stream the bytes back to the
        // caller through an in-memory `tokio::io::duplex` pipe, which is a
        // real `AsyncRead` without pulling in an extra crate.
        let bytes = tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            let mut file = sftp.open(&resolved).map_err(|err| ssh_err("open_file", &resolved, &err))?;
            let mut bytes = Vec::new();
            file.read_to_end(&mut bytes).map_err(|source| VfsError::Io {
                operation: "read_file",
                path: resolved.clone(),
                source,
            })?;
            Ok::<Vec<u8>, VfsError>(bytes)
        })
        .await
        .map_err(join_error)??;

        let (mut writer, reader) = tokio::io::duplex(64 * 1024);
        tokio::spawn(async move {
            let _ = writer.write_all(&bytes).await;
        });
        Ok(Box::new(reader))
    }
}

//! SFTP virtual filesystem provider, backed by `ssh2`.
//!
//! `ssh2` exposes a synchronous (blocking) API, so every operation here runs
//! inside `tokio::task::spawn_blocking`. The underlying `ssh2::Session` is
//! not safe to drive concurrently from multiple threads, so it is guarded
//! by a `std::sync::Mutex`: only one SFTP operation is in flight at a time
//! per [`SftpVfs`] instance. That is a fine trade-off for a compare tool
//! (folder scans and whole-file reads) but would need a connection pool for
//! heavy parallel throughput.

use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, UNIX_EPOCH};

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
        let timeout = Duration::from_secs(u64::from(config.connect_timeout_secs.max(1)));
        // `TcpStream::connect` has no timeout of its own and relies on the
        // OS default, which on Windows can be far longer than on
        // Linux/macOS (multiple minutes against a firewalled/unreachable
        // host) - resolve the address(es) ourselves and bound each attempt
        // with `connect_timeout` so a bad host/port fails within
        // `connect_timeout_secs` instead of appearing to hang.
        let addrs: Vec<_> = (config.host.as_str(), config.port)
            .to_socket_addrs()
            .map_err(|source| VfsError::Io {
                operation: "resolve_host",
                path: PathBuf::from(&config.host),
                source,
            })?
            .collect();
        if addrs.is_empty() {
            return Err(VfsError::Io {
                operation: "resolve_host",
                path: PathBuf::from(&config.host),
                source: std::io::Error::new(
                    std::io::ErrorKind::NotFound,
                    "no addresses found for host",
                ),
            });
        }
        let mut last_error = None;
        let mut tcp = None;
        for addr in addrs {
            match TcpStream::connect_timeout(&addr, timeout) {
                Ok(stream) => {
                    tcp = Some(stream);
                    break;
                }
                Err(error) => last_error = Some(error),
            }
        }
        let tcp = tcp.ok_or_else(|| VfsError::Io {
            operation: "tcp_connect",
            path: PathBuf::from(&config.host),
            source: last_error.unwrap_or_else(|| {
                std::io::Error::new(std::io::ErrorKind::TimedOut, "connection timed out")
            }),
        })?;
        tcp.set_read_timeout(Some(timeout)).ok();

        let mut session = Session::new().map_err(|err| ssh_err("session_new", &config.root, &err))?;
        session.set_tcp_stream(tcp);
        session.handshake().map_err(|err| ssh_err("handshake", &config.root, &err))?;

        match &config.auth {
            SftpAuth::Password { username, password } => {
                authenticate_password(&session, username, password, &config.root)?
            }
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

/// Answers every keyboard-interactive challenge with the configured
/// password. Good enough for the common case this exists for - a server
/// with `PasswordAuthentication no` / `KbdInteractiveAuthentication yes`
/// (several managed SFTP hosts and hardened OpenSSH setups default to this)
/// asking a single "Password:" prompt - not a general MFA client.
struct PasswordPrompter<'a> {
    password: &'a str,
}

impl ssh2::KeyboardInteractivePrompt for PasswordPrompter<'_> {
    fn prompt<'a>(
        &mut self,
        _username: &str,
        _instructions: &str,
        prompts: &[ssh2::Prompt<'a>],
    ) -> Vec<String> {
        prompts.iter().map(|_| self.password.to_owned()).collect()
    }
}

/// Tries plain password auth first, then falls back to keyboard-interactive
/// with the same password. Some SFTP servers (including several that
/// Windows clients commonly hit) only advertise "keyboard-interactive" and
/// reject `userauth_password` outright even with correct credentials, which
/// otherwise surfaces as a flat, confusing "authentication was rejected".
fn authenticate_password(
    session: &Session,
    username: &str,
    password: &str,
    root: &Path,
) -> Result<(), VfsError> {
    if session.userauth_password(username, password).is_ok() {
        return Ok(());
    }
    let mut prompter = PasswordPrompter { password };
    session
        .userauth_keyboard_interactive(username, &mut prompter)
        .map_err(|err| ssh_err("userauth_password_or_keyboard_interactive", root, &err))
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
                        is_link: stat.file_type().is_symlink(),
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
                is_link: stat.file_type().is_symlink(),
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

    async fn write_file(&self, path: &EntryPath, bytes: &[u8]) -> Result<(), VfsError> {
        let session = self.session.clone();
        let resolved = self.resolve(path);
        let bytes = bytes.to_vec();
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            let mut file = sftp.create(&resolved).map_err(|err| ssh_err("write_file", &resolved, &err))?;
            file.write_all(&bytes).map_err(|source| VfsError::Io {
                operation: "write_file",
                path: resolved,
                source,
            })
        })
        .await
        .map_err(join_error)?
    }

    async fn create_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        let session = self.session.clone();
        let resolved = self.resolve(path);
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            sftp.mkdir(&resolved, 0o755).map_err(|err| ssh_err("create_dir", &resolved, &err))
        })
        .await
        .map_err(join_error)?
    }

    async fn rename(&self, from: &EntryPath, to: &EntryPath) -> Result<(), VfsError> {
        let session = self.session.clone();
        let source = self.resolve(from);
        let destination = self.resolve(to);
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &source, &err))?;
            sftp.rename(&source, &destination, None)
                .map_err(|err| ssh_err("rename", &source, &err))
        })
        .await
        .map_err(join_error)?
    }

    async fn remove_file(&self, path: &EntryPath) -> Result<(), VfsError> {
        let session = self.session.clone();
        let resolved = self.resolve(path);
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            sftp.unlink(&resolved).map_err(|err| ssh_err("remove_file", &resolved, &err))
        })
        .await
        .map_err(join_error)?
    }

    async fn remove_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        let session = self.session.clone();
        let resolved = self.resolve(path);
        tokio::task::spawn_blocking(move || {
            let session = session.lock().expect("sftp session mutex poisoned");
            let sftp = session.sftp().map_err(|err| ssh_err("sftp_init", &resolved, &err))?;
            sftp.rmdir(&resolved).map_err(|err| ssh_err("remove_dir", &resolved, &err))
        })
        .await
        .map_err(join_error)?
    }
}

//! File operations routed through the selected VFS provider, so the UI never touches the
//! filesystem directly.

use std::collections::HashSet;

use openbc_core::EntryPath;
use openbc_vfs::{read_small_file, AsyncVfs, VfsError};

/// Replace file contents through the selected VFS provider.
pub async fn write_file(vfs: &dyn AsyncVfs, path: &EntryPath, bytes: &[u8]) -> Result<(), VfsError> {
    vfs.write_file(path, bytes).await
}

/// Read file bytes through the selected VFS provider.
pub async fn read_file(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<Vec<u8>, VfsError> {
    read_small_file(vfs, path).await
}

/// Create one directory through the selected VFS provider.
pub async fn create_directory(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<(), VfsError> {
    vfs.create_dir(path).await
}

/// Rename a file or directory through the selected VFS provider.
pub async fn rename_entry(vfs: &dyn AsyncVfs, from: &EntryPath, to: &EntryPath) -> Result<(), VfsError> {
    vfs.rename(from, to).await
}

/// Copy one file or directory tree between VFS providers without UI-side filesystem access.
pub async fn copy_entry(
    source_vfs: &dyn AsyncVfs,
    source_path: &EntryPath,
    destination_vfs: &dyn AsyncVfs,
    destination_path: &EntryPath,
) -> Result<(), VfsError> {
    let mut pending = vec![(source_path.clone(), destination_path.clone())];
    while let Some((source, destination)) = pending.pop() {
        let metadata = source_vfs.stat(&source).await?;
        if metadata.is_link {
            return Err(VfsError::Unsupported("copy_symlink"));
        }
        if !metadata.is_dir {
            let bytes = read_small_file(source_vfs, &source).await?;
            destination_vfs.write_file(&destination, &bytes).await?;
            continue;
        }
        match destination_vfs.stat(&destination).await {
            Ok(existing) if existing.is_dir => {}
            Ok(_) => {
                return Err(VfsError::Io {
                    operation: "copy_entry",
                    path: destination.0,
                    source: std::io::Error::new(
                        std::io::ErrorKind::AlreadyExists,
                        "destination exists and is not a directory",
                    ),
                });
            }
            Err(_) => destination_vfs.create_dir(&destination).await?,
        }
        for entry in source_vfs.read_dir(&source).await? {
            pending.push((entry.path, child_path(&destination, entry.name)));
        }
    }
    Ok(())
}

/// Make one destination entry match a source entry, removing destination-only descendants.
pub async fn mirror_entry(
    source_vfs: &dyn AsyncVfs,
    source_path: &EntryPath,
    destination_vfs: &dyn AsyncVfs,
    destination_path: &EntryPath,
) -> Result<(), VfsError> {
    copy_entry(source_vfs, source_path, destination_vfs, destination_path).await?;
    let source_metadata = source_vfs.stat(source_path).await?;
    if !source_metadata.is_dir || source_metadata.is_link {
        return Ok(());
    }
    let mut folders = vec![(source_path.clone(), destination_path.clone())];
    while let Some((source, destination)) = folders.pop() {
        let source_entries = source_vfs.read_dir(&source).await?;
        let destination_entries = destination_vfs.read_dir(&destination).await?;
        let source_names: HashSet<_> = source_entries.iter().map(|entry| entry.name.as_str()).collect();
        for entry in destination_entries {
            if !source_names.contains(entry.name.as_str()) {
                delete_entry(destination_vfs, &entry.path).await?;
            }
        }
        for entry in source_entries {
            if entry.metadata.is_dir && !entry.metadata.is_link {
                folders.push((entry.path.clone(), child_path(&destination, entry.name)));
            }
        }
    }
    Ok(())
}

/// Delete a file or directory tree through its provider, without following symlinks.
pub async fn delete_entry(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<(), VfsError> {
    let mut pending = vec![(path.clone(), false)];
    while let Some((current, visited)) = pending.pop() {
        let metadata = vfs.stat(&current).await?;
        if !metadata.is_dir || metadata.is_link {
            vfs.remove_file(&current).await?;
        } else if visited {
            vfs.remove_dir(&current).await?;
        } else {
            pending.push((current.clone(), true));
            for entry in vfs.read_dir(&current).await? {
                pending.push((entry.path, false));
            }
        }
    }
    Ok(())
}

/// `parent/name` as an [`EntryPath`].
fn child_path(parent: &EntryPath, name: String) -> EntryPath {
    EntryPath(parent.0.join(name))
}

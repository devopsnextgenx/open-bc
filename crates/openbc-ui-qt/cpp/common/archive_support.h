// archive_support.h
// ---------------------------------------------------------------------------
// "Is this an archive I can open as a folder?" for the UI. The extension table is NOT here: it
// lives in Rust (openbc_vfs::archive) and is queried through the FFI, so C++ and the VFS can never
// disagree (same rule as the syntax-highlighting table, see AGENTS.md section 10).
//
// Paths that go through an archive ("/data/x.zip/src/main.rs") are ordinary paths for every
// openbc_vfs_* / openbc_engine_* call; the Rust provider resolves them. An archive FILE still
// stat()s as a file, so copy / delete / rename act on the archive itself.
// ---------------------------------------------------------------------------
#pragma once

#include <QByteArray>
#include <QFileInfo>
#include <QString>

#include "remote/vfs_bridge_ffi.h"

namespace openbc::app::archives {

// True for file names that should be offered as folders (zip, jar, 7z, rar, tar.gz, ...).
// Zip-based documents (docx, xlsx, epub) are deliberately false. Name only: no I/O.
inline bool isArchiveName(const QString& name) {
    const QByteArray bytes = name.toUtf8();
    return openbc_archive_is_archive_name(reinterpret_cast<const unsigned char*>(bytes.constData()),
                                          static_cast<size_t>(bytes.size())) != 0;
}

// Number of path components that look like archive files. > 0: the path is, or is inside, an
// archive. Lexical, no I/O.
inline int depth(const QString& path) {
    const QByteArray bytes = path.toUtf8();
    return openbc_archive_depth(reinterpret_cast<const unsigned char*>(bytes.constData()),
                                static_cast<size_t>(bytes.size()));
}

// A folder-compare side that is not a directory on disk but can still be listed: an archive
// file, or a path inside one. (Whether it really opens is decided by the Rust side, which reports
// a readable error otherwise.)
inline bool acceptsAsFolder(const QString& path) {
    if (path.isEmpty()) return false;
    return depth(path) > 0 && (isArchiveName(QFileInfo(path).fileName()) || !QFileInfo(path).exists());
}

// A path that goes THROUGH an archive ("/data/x.zip/src/main.rs"): there is no such file on disk,
// so QFile / QFileInfo cannot open it. Read it with sharedVfsBridge().localReadFile(), which
// resolves it through the archive-aware local session.
inline bool isVirtual(const QString& path) {
    return !path.isEmpty() && depth(path) > 0 && !QFileInfo::exists(path);
}

// Both sides (either may be empty, not both) are archives: open them as a folder compare.
inline bool bothArchives(const QString& left, const QString& right) {
    if (left.isEmpty() && right.isEmpty()) return false;
    auto ok = [](const QString& p) { return p.isEmpty() || isArchiveName(QFileInfo(p).fileName()); };
    return ok(left) && ok(right);
}

}  // namespace openbc::app::archives

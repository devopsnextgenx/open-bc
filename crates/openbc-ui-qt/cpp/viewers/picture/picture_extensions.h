// picture_extensions.h
// ---------------------------------------------------------------------------
// Single source of truth for "which files are pictures". Used by:
//   * folder_compare_viewer.h  - double-click routes these to Picture Compare
//   * picture_compare_viewer.h - the "Select image" file dialog filter
// Add an extension here (it must be one the Rust image decoder can open) and
// both places pick it up.
// ---------------------------------------------------------------------------
#pragma once

#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>

namespace openbc::app {

inline const QStringList& pictureExtensions() {
    static const QStringList extensions = {"png", "jpg", "jpeg", "webp", "bmp", "gif", "tif", "tiff"};
    return extensions;
}

// Works for local paths and for remote display paths (sftp://host/dir/a.png):
// only the file-name suffix is looked at.
inline bool isPictureFilePath(const QString& path) {
    if (path.isEmpty()) return false;
    static const QSet<QString> set(pictureExtensions().begin(), pictureExtensions().end());
    return set.contains(QFileInfo(path).suffix().toLower());
}

inline QString pictureFileDialogFilter() {
    QStringList globs;
    for (const QString& extension : pictureExtensions()) globs << "*." + extension;
    return "Images (" + globs.join(' ') + ");;All files (*)";
}

}  // namespace openbc::app

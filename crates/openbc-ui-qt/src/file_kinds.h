// file_kinds.h
// ---------------------------------------------------------------------------
// One place that answers "what kind of viewer does this file belong to?".
// Used by:
//   * folder_compare_view.h  - double-click routing (picture / 3D / text)
//   * home_view.h            - per-kind icons in the Sessions tree
//
// Keep the 3D list in sync with the openbc-compute model3d loaders; formats
// added later only need registerModel3dExtension("xyz") at startup.
// ---------------------------------------------------------------------------
#pragma once

#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QString>

#include "picture_extensions.h"

namespace openbc::app {

enum class ViewerKind { Text, Picture, Model3d, Table, Media, Registry, Version, Hex };

namespace detail {

inline QString lowerSuffix(const QString& path) { return QFileInfo(path).suffix().toLower(); }

inline QSet<QString>& model3dExtensionSet() {
    static QSet<QString> set = {"stl", "step", "stp", "iges", "igs", "brep", "brp",
                                "obj", "ply",  "off", "3mf",  "glb", "gltf"};
    return set;
}

inline const QSet<QString>& tableExtensionSet() {
    // Open / common spreadsheet formats. No dedicated compare view yet, but
    // they already get their own node icon.
    static const QSet<QString> set = {"csv", "tsv", "xls", "xlsx", "xlsm", "xlsb", "ods", "fods"};
    return set;
}

inline const QSet<QString>& mediaExtensionSet() {
    static const QSet<QString> set = {"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma",
                                      "mp4", "mkv", "avi",  "mov", "webm", "wmv", "m4v"};
    return set;
}

inline const QSet<QString>& versionExtensionSet() {
    static const QSet<QString> set = {"exe", "dll", "sys", "ocx", "drv", "scr", "cpl"};
    return set;
}

inline const QSet<QString>& hexExtensionSet() {
    static const QSet<QString> set = {"bin", "img", "iso", "rom", "dmp"};
    return set;
}

}  // namespace detail

inline void registerModel3dExtension(const QString& extension) {
    QString ext = extension.trimmed().toLower();
    if (ext.startsWith('.')) ext.remove(0, 1);
    if (!ext.isEmpty()) detail::model3dExtensionSet().insert(ext);
}

inline bool isModel3dFilePath(const QString& path) {
    return detail::model3dExtensionSet().contains(detail::lowerSuffix(path));
}

// Extension check plus a cheap sniff for the one ambiguous extension: ".obj" is
// also a compiled object file. Wavefront OBJ is text, so a NUL byte in the first
// 512 bytes means it is not a model. Remote / unreadable paths trust the extension.
inline bool looksLikeModel3dFile(const QString& path) {
    if (!isModel3dFilePath(path)) return false;
    if (detail::lowerSuffix(path) != "obj") return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return true;
    return !file.read(512).contains('\0');
}

inline ViewerKind classifyFilePath(const QString& path) {
    if (path.isEmpty()) return ViewerKind::Text;
    if (isPictureFilePath(path)) return ViewerKind::Picture;
    const QString ext = detail::lowerSuffix(path);
    if (detail::model3dExtensionSet().contains(ext)) return ViewerKind::Model3d;
    if (detail::tableExtensionSet().contains(ext)) return ViewerKind::Table;
    if (detail::mediaExtensionSet().contains(ext)) return ViewerKind::Media;
    if (ext == "reg") return ViewerKind::Registry;
    if (detail::versionExtensionSet().contains(ext)) return ViewerKind::Version;
    if (detail::hexExtensionSet().contains(ext)) return ViewerKind::Hex;
    return ViewerKind::Text;
}

// Matches the `kind` strings of sessionTypes() in home_view.h.
inline QString sessionKindForViewer(ViewerKind kind) {
    switch (kind) {
    case ViewerKind::Picture: return "picture-compare";
    case ViewerKind::Model3d: return "model3d-compare";
    case ViewerKind::Table: return "table-compare";
    case ViewerKind::Media: return "media-compare";
    case ViewerKind::Registry: return "registry-compare";
    case ViewerKind::Version: return "version-compare";
    case ViewerKind::Hex: return "hex-compare";
    case ViewerKind::Text: break;
    }
    return "text-compare";
}

// Maps whatever string a session was recorded with ("folder", "picture",
// "model3d", "picture-compare", "image", ...) to a sessionTypes() kind.
// Returns an empty string when the kind is not recognised.
inline QString canonicalSessionKind(const QString& raw) {
    QString k = raw.trimmed().toLower();
    k.remove('-');
    k.remove('_');
    k.remove(' ');
    if (k.endsWith("compare")) k.chop(7);
    if (k == "folder") return "folder-compare";
    if (k == "foldermerge") return "folder-merge";
    if (k == "foldersync") return "folder-sync";
    if (k == "text") return "text-compare";
    if (k == "textmerge") return "text-merge";
    if (k == "textedit") return "text-edit";
    if (k == "hex" || k == "binary") return "hex-compare";
    if (k == "media" || k == "audio" || k == "video") return "media-compare";
    if (k == "picture" || k == "pictures" || k == "image" || k == "images") return "picture-compare";
    if (k == "model3d" || k == "3d" || k == "model") return "model3d-compare";
    if (k == "registry") return "registry-compare";
    if (k == "table" || k == "csv" || k == "excel" || k == "spreadsheet") return "table-compare";
    if (k == "version") return "version-compare";
    return {};
}

}  // namespace openbc::app
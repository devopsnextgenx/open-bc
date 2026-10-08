// compare_extensions.h
// ---------------------------------------------------------------------------
// Which file extensions open in the table / registry / media compare tabs, and
// the session-history kind each one is recorded under (the same strings that
// home_view.h's sessionTypes() uses). Hex has no extension list: any file pair
// can be opened in hex compare from the folder view's context menu.
// Names are deliberately distinct from file_kinds.h / picture_extensions.h.
// ---------------------------------------------------------------------------
#pragma once

#include <QFileInfo>
#include <QSet>
#include <QString>

namespace openbc::app {

// .csv / .tsv open in the table viewer by default. Set to false to keep them in
// the text compare (the table viewer stays reachable through "Compare as Table").
constexpr bool kCsvOpensAsTable = true;

inline const QSet<QString>& tableCompareExtensions() {
    static const QSet<QString> set = [] {
        QSet<QString> s{"xlsx", "xlsm", "xlsb", "xls", "ods"};
        if (kCsvOpensAsTable) s |= QSet<QString>{"csv", "tsv"};
        return s;
    }();
    return set;
}

inline const QSet<QString>& registryCompareExtensions() {
    static const QSet<QString> set{"reg"};
    return set;
}

// Audio/video containers whose headers and tags the backend can read. ".ts" is left
// out on purpose: it is far more often TypeScript than MPEG transport stream.
inline const QSet<QString>& mediaCompareExtensions() {
    static const QSet<QString> set{"mp3", "wav", "flac", "ogg", "oga", "opus", "m4a", "aac", "wma",
                                   "mp4", "m4v", "mov", "avi", "mkv", "webm"};
    return set;
}

inline QString compareExtensionOf(const QString& path) { return QFileInfo(path).suffix().toLower(); }
inline bool isTableComparePath(const QString& path) { return tableCompareExtensions().contains(compareExtensionOf(path)); }
inline bool isRegistryComparePath(const QString& path) { return registryCompareExtensions().contains(compareExtensionOf(path)); }
inline bool isMediaComparePath(const QString& path) { return mediaCompareExtensions().contains(compareExtensionOf(path)); }

// Session-history kind for the new viewers, or an empty string for anything else.
inline QString newViewerSessionKind(const QString& path) {
    if (isTableComparePath(path)) return "table-compare";
    if (isRegistryComparePath(path)) return "registry-compare";
    if (isMediaComparePath(path)) return "media-compare";
    return {};
}

}  // namespace openbc::app
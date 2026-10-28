// drop_payload.h
// ---------------------------------------------------------------------------
// Turns the QMimeData of a drag-and-drop into validated, classified items for
// the central router (drop_router.h). One place owns:
//   * MIME/URL parsing  (file:// URLs incl. percent-encoded and non-ASCII names,
//                        what Nautilus / Dolphin / Thunar / Explorer put in
//                        text/uri-list),
//   * validation        (exists, readable, regular file or directory),
//   * type detection    (classifyFilePath = the existing dispatch, refined by a
//                        header sniff only for unknown/ambiguous extensions),
//   * remote URLs       (accepted only when a saved connection profile matches,
//                        i.e. exactly what the VFS layer can open today).
//
// GUI-thread cost is bounded: at most the 1-2 dropped items are stat()ed, and at
// most 4 KiB is read from a local file whose extension is not recognised. Remote
// items are never touched over the network here (their kind comes from the name).
// ---------------------------------------------------------------------------
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMimeData>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <string>
#include <vector>

#include "common/content_sniff.h"
#include "common/drop_router.h"
#include "common/file_kinds.h"
#include "common/remote_selector.h"

namespace openbc::app {

struct DroppedItem {
    QString path;        // normalised absolute local path, or "scheme://user@host/path"
    bool isDir = false;
    bool remote = false;
    ViewerKind kind = ViewerKind::Text;
};

struct DropParse {
    QList<DroppedItem> items;
    QStringList problems;  // one human-readable line per rejected entry
    bool hadUrls = false;
};

namespace drop_detail {

inline QString native(const QString& path) { return QDir::toNativeSeparators(path); }

// Kind of a local regular file: the existing extension dispatch, plus a header sniff when the
// extension is unknown (falls back to Text) or ambiguous (.obj is also a compiled object file).
inline ViewerKind classifyLocalFile(const QString& path) {
    ViewerKind byExtension = classifyFilePath(path);
    const bool ambiguous = byExtension == ViewerKind::Model3d && !looksLikeModel3dFile(path);
    if (ambiguous) byExtension = ViewerKind::Text;  // not a model after all: let the sniff decide
    if (byExtension != ViewerKind::Text) return byExtension;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return ViewerKind::Text;
    const QByteArray head = file.read(4096);
    return sniff::refineKind(byExtension, sniff::sniffBytes(
        reinterpret_cast<const unsigned char*>(head.constData()), static_cast<std::size_t>(head.size())));
}

}  // namespace drop_detail

// Cheap pre-check used while dragging (no file-system access).
inline int droppedUrlCount(const QMimeData* mime) { return mime && mime->hasUrls() ? mime->urls().size() : 0; }

inline DropParse parseDropMime(const QMimeData* mime) {
    DropParse out;
    if (!mime || !mime->hasUrls()) return out;
    out.hadUrls = true;
    QSet<QString> seen;  // the same URL listed twice (some managers do) is one item
    for (const QUrl& url : mime->urls()) {
        if (!url.isValid()) {
            out.problems << "Ignored an invalid URL in the drop.";
            continue;
        }
        if (url.isLocalFile()) {
            const QString local = url.toLocalFile();
            if (local.isEmpty()) {
                out.problems << "Ignored a file URL with no path.";
                continue;
            }
            const QFileInfo info(local);
            if (!info.exists()) {
                out.problems << "Not found: " + drop_detail::native(local);
                continue;
            }
            DroppedItem item;
            if (info.isDir()) {
                if (!info.isReadable() || !info.isExecutable()) {
                    out.problems << "Cannot open folder (permission denied): " + drop_detail::native(local);
                    continue;
                }
                item.isDir = true;
            } else if (info.isFile()) {
                if (!info.isReadable()) {
                    out.problems << "Cannot read file (permission denied): " + drop_detail::native(local);
                    continue;
                }
            } else {
                out.problems << "Not a regular file or folder: " + drop_detail::native(local);
                continue;
            }
            item.path = QDir::cleanPath(info.absoluteFilePath());
            const QString key = info.canonicalFilePath().isEmpty() ? item.path : info.canonicalFilePath();
            if (seen.contains(key)) continue;
            seen.insert(key);
            if (!item.isDir) item.kind = drop_detail::classifyLocalFile(item.path);
            out.items << item;
            continue;
        }
        // Not a local file: only usable when a saved remote profile covers it.
        QString text = url.toString(QUrl::FullyDecoded | QUrl::RemovePassword | QUrl::RemoveQuery | QUrl::RemoveFragment);
        const bool trailingSlash = text.endsWith('/');
        if (trailingSlash) text.chop(1);
        if (!openbc_remote::isRemotePath(text)) {
            out.problems << "No saved remote connection matches " + text + ". Add it under Remote connections first.";
            continue;
        }
        if (seen.contains(text)) continue;
        seen.insert(text);
        DroppedItem item;
        item.path = text;
        item.remote = true;
        item.isDir = trailingSlash;  // directory-ness of a remote item is only knowable from the URL here
        if (!item.isDir) item.kind = classifyFilePath(text);
        out.items << item;
    }
    return out;
}

inline std::vector<drop::Item> toRouterItems(const QList<DroppedItem>& items) {
    std::vector<drop::Item> out;
    out.reserve(static_cast<std::size_t>(items.size()));
    for (const DroppedItem& it : items) out.push_back({it.path.toUtf8().toStdString(), it.isDir, it.kind});
    return out;
}

inline QString fromRouterPath(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<int>(s.size())); }

}  // namespace openbc::app

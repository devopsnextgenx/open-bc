// folder_model.h
// ---------------------------------------------------------------------------
// Folder-compare data model: byte/size formatting, name filters, per-side
// file-system info, entry pairing/classification, and the QTreeWidgetItem
// helpers that turn an EntryPair into a row. No UI beyond QTreeWidgetItem
// construction lives here - CompareSession (compare_session.h) owns the
// actual panes, toolbars and menus.
// ---------------------------------------------------------------------------
#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QString>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#include "qt_style.h"

namespace openbc::app {

using openbc::ui::PairClass;
using openbc::ui::RowStatus;
using openbc::ui::kClassRole;
using openbc::ui::kIsDirRole;
using openbc::ui::kPathRole;
using openbc::ui::kStatusRole;
using openbc::ui::statusBit;
namespace icons = openbc::ui::icons;
namespace color = openbc::ui::color;

inline QString formatBytes(qint64 bytes) {
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    const int decimals = (unit == 0 || value >= 100.0) ? 0 : 1;
    return QString::number(value, 'f', decimals) + " " + units[unit];
}

// Column text: exact bytes with thousands separators, GB above 1 GiB.
inline QString formatSize(qint64 bytes) {
    constexpr qint64 gib = 1024LL * 1024LL * 1024LL;
    if (bytes >= gib) {
        return QString::number(static_cast<double>(bytes) / static_cast<double>(gib), 'f', 2) + " GB";
    }
    return QLocale().toString(bytes);
}

inline QString freeSpaceText(const QString& path) {
    const QStorageInfo storage(path);
    if (!storage.isValid() || !storage.isReady()) {
        return "-";
    }
    return formatBytes(storage.bytesAvailable()) + " free on " +
           QDir::toNativeSeparators(storage.rootPath());
}

// ---------------------------------------------------------------------------
// File-name filter: "*.cpp;*.h", "*.txt", or a plain substring like "foo".
// Applies to files only; folders are always listed so their contents stay
// reachable.
// ---------------------------------------------------------------------------

class NameFilter {
public:
    static NameFilter parse(const QString& text) {
        NameFilter filter;
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty() || trimmed == "*" || trimmed == "*.*") {
            return filter;
        }
        const QStringList tokens =
            trimmed.split(QRegularExpression("[;,\\s]+"), Qt::SkipEmptyParts);
        for (QString token : tokens) {
            if (token == "*" || token == "*.*") {
                return NameFilter();
            }
            if (!token.contains('*') && !token.contains('?')) {
                token = "*" + token + "*";
            }
            filter.patterns_.emplace_back(QRegularExpression::wildcardToRegularExpression(token),
                                          QRegularExpression::CaseInsensitiveOption);
        }
        filter.matchAll_ = filter.patterns_.empty();
        return filter;
    }

    bool matches(const QString& name) const {
        if (matchAll_) {
            return true;
        }
        for (const auto& pattern : patterns_) {
            if (pattern.match(name).hasMatch()) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<QRegularExpression> patterns_;
    bool matchAll_ = true;
};

struct CompareOptions {
    bool checkContent = false;
    bool ignoreTimestamps = false;
    NameFilter filter;
};

// ---------------------------------------------------------------------------
// Plain-data description of one side of a row. Everything that needs file I/O
// is gathered on the worker thread so the GUI thread never touches the disk.
// ---------------------------------------------------------------------------

struct SideInfo {
    bool exists = false;
    bool isDir = false;
    bool isLink = false;
    QString name;
    QString path;
    QString ext;
    QString attrs;
    qint64 size = 0;
    QDateTime modified;
};

inline SideInfo makeSide(const QFileInfo& info) {
    SideInfo side;
    side.exists = true;
    side.isDir = info.isDir();
    side.isLink = info.isSymLink();
    side.name = info.fileName();
    side.path = info.absoluteFilePath();
    side.ext = side.isDir ? QString() : info.suffix();
    side.size = side.isDir ? 0 : info.size();
    side.modified = info.lastModified();
    QString attrs;
    if (side.isDir) attrs += 'D';
    if (side.isLink) attrs += 'L';
    if (info.isHidden()) attrs += 'H';
    if (!info.isWritable()) attrs += 'R';
    if (!side.isDir && info.isExecutable()) attrs += 'X';
    side.attrs = attrs;
    return side;
}

inline std::vector<SideInfo> listFolder(const QString& path, const NameFilter& filter) {
    std::vector<SideInfo> result;
    if (path.isEmpty()) {
        return result;
    }
    const QDir directory(path);
    const auto infos = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& info : infos) {
        if (!info.isDir() && !filter.matches(info.fileName())) {
            continue;
        }
        result.push_back(makeSide(info));
    }
    return result;
}

inline bool contentEqual(const QString& leftPath, const QString& rightPath,
                  const std::atomic_bool& cancelled) {
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)) {
        return false;
    }
    constexpr qint64 chunkSize = 1024 * 1024;
    while (!left.atEnd() && !right.atEnd()) {
        if (cancelled.load()) {
            return false;
        }
        if (left.read(chunkSize) != right.read(chunkSize)) {
            return false;  // stop at the first difference
        }
    }
    return left.atEnd() && right.atEnd();
}

struct EntryPair {
    SideInfo left;
    SideInfo right;
    RowStatus leftStatus = RowStatus::Pending;
    RowStatus rightStatus = RowStatus::Pending;
    PairClass pairClass = PairClass::Pending;
    bool recurse = false;
};

// Same scheme as Beyond Compare: green/orange = differing timestamps
// (newer / older), red = same timestamp but different content, purple =
// only on one side.
inline void classifyEntry(EntryPair& e, const CompareOptions& options, const std::atomic_bool& cancelled) {
    const SideInfo& l = e.left;
    const SideInfo& r = e.right;
    const bool leftRecurse = l.exists && l.isDir && !l.isLink;
    const bool rightRecurse = r.exists && r.isDir && !r.isLink;
    e.recurse = leftRecurse || rightRecurse;

    if (!l.exists || !r.exists) {
        e.leftStatus = e.rightStatus = RowStatus::Orphan;
        e.pairClass = l.exists ? PairClass::OrphanLeft : PairClass::OrphanRight;
        return;
    }
    if (l.isDir != r.isDir) {  // file vs folder
        e.leftStatus = e.rightStatus = RowStatus::Different;
        e.pairClass = PairClass::Different;
        return;
    }
    if (l.isDir) {
        if (e.recurse) {  // resolved once the children are known
            e.leftStatus = e.rightStatus = RowStatus::Pending;
            e.pairClass = PairClass::Pending;
        } else {
            const bool same = l.isLink == r.isLink;
            e.leftStatus = e.rightStatus = same ? RowStatus::Equal : RowStatus::Different;
            e.pairClass = same ? PairClass::Same : PairClass::Different;
        }
        return;
    }

    const bool sameSize = l.size == r.size;
    const bool sameTime = options.ignoreTimestamps ||
                          qAbs(l.modified.toMSecsSinceEpoch() - r.modified.toMSecsSinceEpoch()) <= 2000;
    bool same = false;
    if (sameSize) {
        same = options.checkContent ? contentEqual(l.path, r.path, cancelled) : sameTime;
    }
    if (same) {
        e.leftStatus = e.rightStatus = RowStatus::Equal;
        e.pairClass = PairClass::Same;
    } else if (!sameTime) {
        const bool leftNewer = l.modified > r.modified;
        e.leftStatus = leftNewer ? RowStatus::Newer : RowStatus::Older;
        e.rightStatus = leftNewer ? RowStatus::Older : RowStatus::Newer;
        e.pairClass = leftNewer ? PairClass::LeftNewer : PairClass::RightNewer;
    } else {
        e.leftStatus = e.rightStatus = RowStatus::Different;
        e.pairClass = PairClass::Different;
    }
}

inline QString pairKey(const QString& name) {
#ifdef Q_OS_WIN
    return name.toLower();
#else
    return name;
#endif
}

inline std::vector<EntryPair> collectEntries(const QString& leftPath, const QString& rightPath,
                                      const CompareOptions& options,
                                      const std::atomic_bool& cancelled) {
    std::vector<EntryPair> entries;
    std::vector<std::pair<QString, size_t>> index;  // key -> position in `entries`
    QHash<QString, size_t> positions;

    for (auto& side : listFolder(leftPath, options.filter)) {
        positions.insert(pairKey(side.name), entries.size());
        EntryPair e;
        e.left = std::move(side);
        entries.push_back(std::move(e));
    }
    for (auto& side : listFolder(rightPath, options.filter)) {
        const auto found = positions.constFind(pairKey(side.name));
        if (found != positions.constEnd()) {
            entries[*found].right = std::move(side);
        } else {
            EntryPair e;
            e.right = std::move(side);
            entries.push_back(std::move(e));
        }
    }

    std::sort(entries.begin(), entries.end(), [](const EntryPair& a, const EntryPair& b) {
        const bool aDir = (a.left.exists && a.left.isDir) || (a.right.exists && a.right.isDir);
        const bool bDir = (b.left.exists && b.left.isDir) || (b.right.exists && b.right.isDir);
        if (aDir != bDir) {
            return aDir;  // folders first
        }
        const QString& an = a.left.exists ? a.left.name : a.right.name;
        const QString& bn = b.left.exists ? b.left.name : b.right.name;
        const int cmp = QString::compare(an, bn, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : an < bn;
    });

    for (auto& e : entries) {
        if (cancelled.load()) {
            break;
        }
        classifyEntry(e, options, cancelled);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

inline void applyRowStyle(QTreeWidgetItem* item, RowStatus status, bool isDir) {
    // Folder names keep the normal text colour (the folder icon carries the
    // status); their size/date/attribute columns are dimmed.
    const QColor name = isDir ? color::text() : openbc::ui::statusTextColor(status);
    const QColor rest = isDir ? color::dimText() : name;
    item->setForeground(0, name);
    for (int column = 1; column < item->columnCount(); ++column) {
        item->setForeground(column, rest);
    }
}

inline QTreeWidgetItem* addRow(QTreeWidget* tree, const SideInfo& side, RowStatus status,
                        PairClass pairClass, QTreeWidgetItem* parent) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setData(0, kStatusRole, static_cast<int>(status));
    item->setData(0, kClassRole, static_cast<int>(pairClass));
    item->setData(0, kPathRole, side.exists ? side.path : QString());
    item->setData(0, kIsDirRole, side.exists && side.isDir);
    if (side.exists) {
        item->setText(0, side.name);
        item->setText(1, side.ext);
        item->setText(2, side.isDir ? QString() : formatSize(side.size));
        item->setText(3, side.modified.toString("yyyy-MM-dd HH:mm:ss"));
        item->setText(4, side.attrs);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setIcon(0, side.isDir ? icons::folderIconForMask(0) : icons::markerIcon(status));
        applyRowStyle(item, status, side.isDir);
    }
    return item;
}

inline QTreeWidgetItem* pairedItem(QTreeWidgetItem* item, QTreeWidget* otherTree) {
    if (!item) {
        return nullptr;
    }
    QList<int> rows;
    for (auto* current = item; current; current = current->parent()) {
        rows.prepend(current->parent() ? current->parent()->indexOfChild(current)
                                       : item->treeWidget()->indexOfTopLevelItem(current));
    }
    QTreeWidgetItem* counterpart = otherTree->topLevelItem(rows.takeFirst());
    for (const int row : rows) {
        if (!counterpart || row >= counterpart->childCount()) {
            return nullptr;
        }
        counterpart = counterpart->child(row);
    }
    return counterpart;
}

}  // namespace openbc::app

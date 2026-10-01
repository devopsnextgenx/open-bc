// folder_model.h
// ---------------------------------------------------------------------------
// Folder-compare data model: byte/size formatting, name filters, per-side
// file-system info, entry pairing/classification, and the QTreeWidgetItem
// helpers that turn an EntryPair into a row. No UI beyond QTreeWidgetItem
// construction lives here - CompareSession (folder_compare_view.h) owns the
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
using openbc::ui::kChildrenLoadedRole;
using openbc::ui::kCompareRevisionRole;
using openbc::ui::kIsDirRole;
using openbc::ui::kFolderStatusMaskRole;
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

struct EntryPair {
    SideInfo left;
    SideInfo right;
    RowStatus leftStatus = RowStatus::Pending;
    RowStatus rightStatus = RowStatus::Pending;
    PairClass pairClass = PairClass::Pending;
    bool recurse = false;
};

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
    item->setData(0, kFolderStatusMaskRole, side.isDir ? 0 : statusBit(status));
    if (!side.exists) {
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    } else {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
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

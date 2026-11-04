// folder_model.h
// ---------------------------------------------------------------------------
// Folder-compare data model: byte/size formatting, name filters, per-side
// file-system info, entry pairing/classification, and the QTreeWidgetItem
// helpers that turn an EntryPair into a row. No UI beyond QTreeWidgetItem
// construction lives here - FolderCompareViewer (folder_compare_viewer.h) owns the
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

#include "style/qt_style.h"

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
// Name masks support wildcards, substrings, and base-folder-relative paths.
// File and folder inclusion/exclusion are configured separately.
// ---------------------------------------------------------------------------

class NameFilter {
public:
    static NameFilter parse(const QString& text, bool emptyMatchesAll = true) {
        NameFilter filter;
        filter.matchAll_ = emptyMatchesAll;
        const QStringList tokens =
            text.split(QRegularExpression("[;\\r\\n]+"), Qt::SkipEmptyParts);
        for (QString token : tokens) {
            token = token.trimmed();
            if (token.isEmpty())
                continue;
            if (token == "*" || token == "*.*")
                return NameFilter();
            const bool pathPattern = token.contains('/') || token.contains('\\');
            token.replace('\\', '/');
            while (token.startsWith('/'))
                token.remove(0, 1);
            if (!pathPattern && !token.contains('*') && !token.contains('?'))
                token = "*" + token + "*";
            if (token == "*.*")
                token = "*";
            filter.patterns_.emplace_back(
                QRegularExpression::wildcardToRegularExpression(token),
                QRegularExpression::CaseInsensitiveOption);
            filter.pathPatterns_.push_back(pathPattern);
            filter.patternTexts_.append(token);
        }
        if (filter.patterns_.empty()) {
            filter.matchAll_ = emptyMatchesAll;
            return filter;
        }
        filter.matchAll_ = false;
        return filter;
    }

    bool matches(const QString& relativePath) const {
        if (matchAll_) {
            return true;
        }
        const QString normalized = QString(relativePath).replace('\\', '/');
        const QString name = normalized.section('/', -1);
        for (size_t i = 0; i < patterns_.size(); ++i) {
            const QString candidate = pathPatterns_[i] ? normalized : name;
            if (patterns_[i].match(candidate).hasMatch()) {
                return true;
            }
        }
        return false;
    }

    bool matchesOrHasMatchingDescendant(const QString& relativePath) const {
        if (matches(relativePath))
            return true;
        QString prefix = QString(relativePath).replace('\\', '/');
        if (!prefix.isEmpty() && !prefix.endsWith('/'))
            prefix += '/';
        for (size_t i = 0; i < patterns_.size(); ++i) {
            if (pathPatterns_[i] &&
                globCanMatchPrefix(patternTexts_[static_cast<int>(i)], prefix))
                return true;
        }
        return false;
    }

private:
    static bool globCanMatchPrefix(const QString& pattern,
                                   const QString& prefix) {
        std::vector<bool> states(static_cast<size_t>(pattern.size() + 1), false);
        states[0] = true;
        const auto closeStars = [&pattern](std::vector<bool>& current) {
            for (int i = 0; i < pattern.size(); ++i) {
                if (current[static_cast<size_t>(i)] && pattern[i] == '*')
                    current[static_cast<size_t>(i + 1)] = true;
            }
        };
        closeStars(states);
        for (const QChar character : prefix) {
            std::vector<bool> next(states.size(), false);
            for (int i = 0; i < pattern.size(); ++i) {
                if (!states[static_cast<size_t>(i)])
                    continue;
                if (pattern[i] == '*') {
                    next[static_cast<size_t>(i)] = true;
                } else if (pattern[i] == '?' ||
                           pattern[i].toCaseFolded() == character.toCaseFolded()) {
                    next[static_cast<size_t>(i + 1)] = true;
                }
            }
            states = std::move(next);
            closeStars(states);
        }
        return std::any_of(states.cbegin(), states.cend(),
                           [](bool active) { return active; });
    }

    std::vector<QRegularExpression> patterns_;
    std::vector<bool> pathPatterns_;
    QStringList patternTexts_;
    bool matchAll_ = true;
};

struct CompareOptions {
    bool checkContent = false;
    bool ignoreTimestamps = false;
    // Show differing archive pairs (zip, 7z, tar.gz, ...) as expandable folders.
    bool archivesAsFolders = true;
    NameFilter includeFiles = NameFilter::parse("*.*");
    NameFilter excludeFiles = NameFilter::parse({}, false);
    NameFilter includeFolders = NameFilter::parse("*");
    NameFilter excludeFolders = NameFilter::parse({}, false);
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
    const QColor name = isDir ? color::folderText() : openbc::ui::statusTextColor(status);
    const QColor rest = isDir ? color::folderInfo() : name;
    item->setForeground(0, name);
    for (int column = 1; column < item->columnCount(); ++column) {
        item->setForeground(column, rest);
    }
}

inline QString statusLabel(RowStatus status) {
    switch (status) {
    case RowStatus::Equal: return "Equal";
    case RowStatus::Newer: return "Newer";
    case RowStatus::Older: return "Older";
    case RowStatus::Different: return "Different";
    case RowStatus::Orphan: return "Missing on other side";
    case RowStatus::Pending: return "Comparing";
    }
    return "Unknown";
}

inline void applyStatusMarker(QTreeWidgetItem* item, RowStatus status) {
    const QString label = statusLabel(status);
    item->setIcon(1, icons::markerIcon(status));
    item->setData(1, Qt::AccessibleTextRole, label);
    item->setToolTip(1, label);
}

inline QTreeWidgetItem* addRow(QTreeWidget* tree, const SideInfo& side, RowStatus status,
                        PairClass pairClass, QTreeWidgetItem* parent) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setData(0, kStatusRole, static_cast<int>(status));
    item->setData(0, kClassRole, static_cast<int>(pairClass));
    item->setData(0, kPathRole, side.exists ? side.path : QString());
    item->setData(0, kIsDirRole, side.exists && side.isDir);
    item->setData(0, kFolderStatusMaskRole, side.isDir ? 0 : statusBit(status));
    applyStatusMarker(item, status);
    if (!side.exists) {
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    } else {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    if (side.exists) {
        item->setText(0, side.name);
        item->setText(2, side.ext);
        item->setText(3, side.isDir ? QString() : formatSize(side.size));
        item->setText(4, side.modified.toString("yyyy-MM-dd HH:mm:ss"));
        item->setText(5, side.attrs);
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        item->setIcon(0, side.isDir ? icons::folderIconForMask(0) : icons::markerIcon(status));
        applyRowStyle(item, status, side.isDir);
    }
    return item;
}

// Turns an archive FILE row (zip, 7z, tar.gz, ...) into an expandable folder row. Its children are
// not loaded yet (kChildrenLoadedRole = false): the viewer's normal lazy path lists them when the
// row is expanded, exactly like a folder that was not recursed into.
inline void convertToFolderRow(QTreeWidgetItem* item) {
    item->setData(0, kIsDirRole, true);
    item->setData(0, kChildrenLoadedRole, false);
    item->setData(0, kFolderStatusMaskRole, 0);
    item->setText(3, QString());
    item->setIcon(0, icons::folderIconForMask(0));
    item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    applyRowStyle(item, static_cast<RowStatus>(item->data(0, kStatusRole).toInt()), true);
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

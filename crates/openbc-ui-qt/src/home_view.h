#pragma once

#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QToolButton>
#include <QDropEvent>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

#include "session_history.h"
#include "qt_style.h"

namespace openbc::app {

class HistoryTree : public QTreeWidget {
public:
    std::function<void(QTreeWidgetItem*, QTreeWidgetItem*)> onSessionDropped;

    explicit HistoryTree(QWidget* parent = nullptr) : QTreeWidget(parent) {}

protected:
    void dropEvent(QDropEvent* event) override {
        auto* source = currentItem();
        auto* target = itemAt(event->position().toPoint());
        if (source && target && onSessionDropped) {
            onSessionDropped(source, target);
            event->acceptProposedAction();
            return;
        }
        QTreeWidget::dropEvent(event);
    }
};

inline QIcon historyFileIcon(const QString& path) {
    const QString extension = QFileInfo(path).suffix().toLower();
    QColor color(0x8f, 0x9b, 0xb3);
    if (extension == "rs") color = QColor(0xe5, 0x7a, 0x44);
    else if (extension == "cpp" || extension == "h") color = QColor(0x4f, 0x9d, 0xde);
    else if (extension == "py") color = QColor(0xe5, 0xc0, 0x7b);
    else if (extension == "js" || extension == "ts") color = QColor(0xe8, 0xc5, 0x47);
    else if (extension == "json" || extension == "toml" || extension == "yaml") color = QColor(0x68, 0xc0, 0x9a);
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(QPen(color, 1.2));
    painter.setBrush(color.darker(185));
    painter.drawRoundedRect(QRectF(3, 1.5, 10, 13), 1, 1);
    painter.setPen(QPen(color.lighter(145), 1));
    painter.drawLine(QPointF(5, 6), QPointF(11, 6));
    painter.drawLine(QPointF(5, 9), QPointF(11, 9));
    painter.drawLine(QPointF(5, 12), QPointF(9, 12));
    return QIcon(pixmap);
}

class HomeView : public QWidget {
public:
    std::function<void()> onNewFolderCompare;
    std::function<void()> onNewTextCompare;
    std::function<void(const SessionHistoryEntry&)> onOpenHistory;

    explicit HomeView(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("homeView");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(28, 22, 28, 22);
        auto* heading = new QLabel("OpenBC", this);
        heading->setObjectName("homeHeading");
        root->addWidget(heading);
        auto* subtitle = new QLabel("Compare folders and files, then keep the sessions you return to.", this);
        subtitle->setObjectName("homeSubtitle");
        root->addWidget(subtitle);

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->setObjectName("homeSplit");
        split->setChildrenCollapsible(false);
        root->addWidget(split, 1);

        history_ = new HistoryTree(split);
        history_->setObjectName("homeHistory");
        history_->setHeaderHidden(true);
        history_->setRootIsDecorated(true);
        history_->setUniformRowHeights(true);
        history_->setIndentation(18);
        history_->setSelectionMode(QAbstractItemView::SingleSelection);
        history_->setEditTriggers(QAbstractItemView::EditKeyPressed);
        history_->setDragEnabled(true);
        history_->setAcceptDrops(true);
        history_->setDropIndicatorShown(true);
        history_->setDragDropMode(QAbstractItemView::InternalMove);
        history_->setContextMenuPolicy(Qt::CustomContextMenu);

        auto* details = new QWidget(split);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(26, 4, 4, 4);
        auto* optionsTitle = new QLabel("Start a comparison", details);
        optionsTitle->setObjectName("homeSectionTitle");
        detailsLayout->addWidget(optionsTitle);
        auto* actions = new QHBoxLayout;
        auto* folder = new QToolButton(details);
        folder->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::FolderOpen));
        folder->setText("Folder compare");
        folder->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        folder->setObjectName("homeAction");
        auto* text = new QToolButton(details);
        text->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Compare));
        text->setText("Text compare");
        text->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        text->setObjectName("homeAction");
        actions->addWidget(folder);
        actions->addWidget(text);
        actions->addStretch();
        detailsLayout->addLayout(actions);
        connect(folder, &QToolButton::clicked, this, [this]() { if (onNewFolderCompare) onNewFolderCompare(); });
        connect(text, &QToolButton::clicked, this, [this]() { if (onNewTextCompare) onNewTextCompare(); });

        auto* selectedTitle = new QLabel("Selected session", details);
        selectedTitle->setObjectName("homeSectionTitle");
        detailsLayout->addWidget(selectedTitle);
        label_ = new QLineEdit(details);
        label_->setPlaceholderText("Session name");
        left_ = new QLineEdit(details);
        right_ = new QLineEdit(details);
        auto* form = new QFormLayout;
        form->addRow("Name", label_);
        form->addRow("Left", left_);
        form->addRow("Right", right_);
        detailsLayout->addLayout(form);
        auto* buttons = new QHBoxLayout;
        open_ = new QPushButton("Open", details);
        edit_ = new QPushButton("Edit paths", details);
        save_ = new QPushButton("Save session", details);
        buttons->addWidget(open_);
        buttons->addWidget(edit_);
        buttons->addWidget(save_);
        buttons->addStretch();
        detailsLayout->addLayout(buttons);
        detailsLayout->addStretch();
        clearDetails();

        connect(history_, &QTreeWidget::itemSelectionChanged, this, [this]() { showSelected(); });
        connect(history_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item || !item->data(0, Qt::UserRole + 3).toBool() || !onOpenHistory) return;
                history_->setCurrentItem(item);
                // Reopening from any node other than "Today" (Yesterday/This
                // week/Last week/Older, or a saved node) records a fresh
                // entry for today so each day's sessions are tracked; if it's
                // already in Today, record() is a no-op.
                SessionHistory::record(selected_.kind, selected_.left, selected_.right);
                refreshHistory();
                onOpenHistory(selected_);
            });
        connect(history_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int column) { handleItemRenamed(item, column); });
        history_->onSessionDropped = [this](QTreeWidgetItem* source, QTreeWidgetItem* target) {
            moveSession(source, target);
        };
        connect(open_, &QPushButton::clicked, this, [this]() {
            if (selected_.kind.isEmpty() || !onOpenHistory) return;
            selected_.label = label_->text().trimmed();
            selected_.left = left_->text();
            selected_.right = right_->text();
            // Same as the double-click path: reopening from a non-Today node
            // records a fresh entry for today; already-Today items are a no-op.
            SessionHistory::record(selected_.kind, selected_.left, selected_.right);
            refreshHistory();
            onOpenHistory(selected_);
        });
        connect(edit_, &QPushButton::clicked, this, [this]() {
            const bool enabled = !left_->isEnabled();
            left_->setEnabled(enabled);
            right_->setEnabled(enabled);
            edit_->setText(enabled ? "Done editing" : "Edit paths");
        });
        connect(save_, &QPushButton::clicked, this, [this]() { saveSelected(); });
        connect(history_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
            contextMenu(position);
        });
        refreshHistory();
        split->setSizes({300, 700});
    }

    void refreshHistory() {
        const QSignalBlocker blocker(history_);
        history_->clear();
        auto* recent = new QTreeWidgetItem(history_, {"Recent sessions"});
        recent->setIcon(0, openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Refresh));
        recent->setFlags(Qt::ItemIsEnabled);

        // Group the flat recent-history list into Today / Yesterday / This
        // week / Last week / Older sub-nodes; only buckets that actually
        // hold a session are shown, each with its own colour and icon.
        const auto entries = SessionHistory::load();
        QList<QList<int>> buckets(kRecentBucketCount);
        for (int index = 0; index < entries.size(); ++index) {
            buckets[static_cast<int>(SessionHistory::bucketFor(entries[index].openedAt))].append(index);
        }
        for (int ordinal = 0; ordinal < kRecentBucketCount; ++ordinal) {
            if (buckets[ordinal].isEmpty()) continue;
            const auto bucket = static_cast<RecentBucket>(ordinal);
            auto* bucketItem = new QTreeWidgetItem(recent, {SessionHistory::bucketLabel(bucket)});
            bucketItem->setIcon(0, openbc::ui::icons::groupNodeIcon(bucketColor(bucket)));
            bucketItem->setFlags(Qt::ItemIsEnabled);
            bucketItem->setData(0, Qt::UserRole + 5, true);
            for (int index : buckets[ordinal]) addSessionItem(bucketItem, entries[index], false, {}, index);
            bucketItem->setExpanded(true);
        }
        recent->setExpanded(true);
        for (const auto& node : SessionHistory::loadSavedNodes()) addNodeItem(nullptr, node);
        history_->expandAll();
    }

private:
    static QColor bucketColor(RecentBucket bucket) {
        switch (bucket) {
        case RecentBucket::Today: return openbc::ui::color::groupToday();
        case RecentBucket::Yesterday: return openbc::ui::color::groupYesterday();
        case RecentBucket::ThisWeek: return openbc::ui::color::groupThisWeek();
        case RecentBucket::LastWeek: return openbc::ui::color::groupLastWeek();
        case RecentBucket::Older:
        default: return openbc::ui::color::groupOlder();
        }
    }

    // Colour for a named saved-session node: "Personal" gets its own fixed
    // colour, nodes nested under it get a persisted random colour each, and
    // everything else falls back to a shared neutral colour.
    static QColor nodeGroupColor(QTreeWidgetItem* parent, const SavedSessionNode& node) {
        if (!parent && node.name == "Personal") return openbc::ui::color::groupPersonal();
        const QColor persisted = SessionHistory::colorForNode(node.path);
        return persisted.isValid() ? persisted : openbc::ui::color::groupDefault();
    }

    void addSessionItem(QTreeWidgetItem* parent, const SessionHistoryEntry& entry, bool saved,
                        const QString& node, int index) {
        const QString label = entry.label.isEmpty() ? SessionHistory::defaultLabel(entry.kind, entry.left, entry.right) : entry.label;
        auto* item = new QTreeWidgetItem(parent, {label});
        item->setIcon(0, entry.kind == "folder"
                              ? openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow())
                              : historyFileIcon(entry.left));
        item->setToolTip(0, entry.left + "\n<->\n" + entry.right);
        item->setData(0, Qt::UserRole, saved);
        item->setData(0, Qt::UserRole + 1, index);
        item->setData(0, Qt::UserRole + 2, node);
        item->setData(0, Qt::UserRole + 3, true);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled);
    }

    void addNodeItem(QTreeWidgetItem* parent, const SavedSessionNode& node) {
        auto* item = parent ? new QTreeWidgetItem(parent, {node.name}) : new QTreeWidgetItem(history_, {node.name});
        item->setIcon(0, openbc::ui::icons::groupNodeIcon(nodeGroupColor(parent, node)));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDropEnabled);
        item->setData(0, Qt::UserRole + 2, node.path);
        item->setData(0, Qt::UserRole + 4, true);
        for (int index = 0; index < node.sessions.size(); ++index) {
            addSessionItem(item, node.sessions[index], true, node.path, index);
        }
        for (const auto& child : node.children) addNodeItem(item, child);
        item->setExpanded(true);
    }

    bool hasSelection() const { return selectedItem_ && selectedItem_->data(0, Qt::UserRole + 3).toBool(); }

    void showSelected() {
        selectedItem_ = history_->currentItem();
        if (!hasSelection()) {
            clearDetails();
            return;
        }
        const bool saved = selectedItem_->data(0, Qt::UserRole).toBool();
        const int index = selectedItem_->data(0, Qt::UserRole + 1).toInt();
        const QString node = selectedItem_->data(0, Qt::UserRole + 2).toString();
        selected_ = saved ? SessionHistory::loadSaved(node).value(index) : SessionHistory::load().value(index);
        selected_.savedNode = node;
        selected_.savedIndex = index;
        selectedSaved_ = saved;
        label_->setText(selectedItem_->text(0));
        left_->setText(selected_.left);
        right_->setText(selected_.right);
        left_->setEnabled(false);
        right_->setEnabled(false);
        edit_->setText("Edit paths");
        open_->setEnabled(true);
        edit_->setEnabled(true);
        save_->setEnabled(true);
        save_->setText(saved ? "Save changes" : "Save session");
    }

    void clearDetails() {
        selected_ = {};
        selectedItem_ = nullptr;
        label_->clear();
        left_->clear();
        right_->clear();
        left_->setEnabled(false);
        right_->setEnabled(false);
        open_->setEnabled(false);
        edit_->setEnabled(false);
        save_->setEnabled(false);
    }

    void saveSelected() {
        if (!hasSelection()) return;
        selected_.label = label_->text().trimmed();
        selected_.left = left_->text();
        selected_.right = right_->text();
        if (selectedSaved_) {
            SessionHistory::updateSaved(selected_.savedNode, selected_.savedIndex, selected_, selected_.label);
        } else {
            bool ok = false;
            const QString node = QInputDialog::getText(this, "Save session", "Named node", QLineEdit::Normal,
                                                       "Personal", &ok);
            if (ok && !node.trimmed().isEmpty()) SessionHistory::save(selected_, node, selected_.label);
        }
        refreshHistory();
    }

    void contextMenu(const QPoint& position) {
        auto* item = history_->itemAt(position);
        if (!item) return;
        history_->setCurrentItem(item);
        if (item->data(0, Qt::UserRole + 5).toBool()) return;  // date-bucket header: no menu
        const bool isNode = item->data(0, Qt::UserRole + 4).toBool();
        const bool saved = item->data(0, Qt::UserRole).toBool();
        const QString node = item->data(0, Qt::UserRole + 2).toString();
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (!item->parent()) {
            if (isNode) {
                showNodeMenu(item, node, position);
            }
            return;
        }
        if (isNode) {
            showNodeMenu(item, node, position);
            return;
        }
        if (!item->data(0, Qt::UserRole + 3).toBool()) return;  // only real session rows below
        QMenu menu(history_);
        QAction* rename = menu.addAction("Rename session");
        QAction* save = menu.addAction("Save to named node");
        QAction* remove = menu.addAction("Remove from history");
        QAction* chosen = menu.exec(history_->viewport()->mapToGlobal(position));
        if (chosen == rename) {
            history_->editItem(item, 0);
        } else if (chosen == save) {
            history_->setCurrentItem(item);
            saveSelected();
        } else if (chosen == remove) {
            if (saved) SessionHistory::removeSaved(node, index); else SessionHistory::removeAt(index);
            refreshHistory();
        }
    }

    void showNodeMenu(QTreeWidgetItem* item, const QString& node, const QPoint& position) {
        QMenu menu(history_);
        QAction* create = menu.addAction("New saved session folder");
        QAction* rename = menu.addAction("Rename");
        QAction* remove = menu.addAction("Remove named node");
        QAction* chosen = menu.exec(history_->viewport()->mapToGlobal(position));
        if (chosen == create) {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "New saved session folder", "Name",
                                                       QLineEdit::Normal, QString(), &ok);
            if (ok && SessionHistory::createNode(node, name)) refreshHistory();
        } else if (chosen == rename) {
            history_->editItem(item, 0);
        } else if (chosen == remove) {
            SessionHistory::removeNode(node);
            refreshHistory();
        }
    }

    void handleItemRenamed(QTreeWidgetItem* item, int column) {
        if (!item || column != 0) return;
        const QString value = item->text(0).trimmed();
        if (item->data(0, Qt::UserRole + 4).toBool()) {
            const QString oldNode = item->data(0, Qt::UserRole + 2).toString();
            if (value.isEmpty() || !SessionHistory::renameNode(oldNode, value)) {
                const QSignalBlocker blocker(history_);
                item->setText(0, QFileInfo(oldNode).fileName());
                return;
            }
            refreshHistory();
            return;
        }
        if (!item->data(0, Qt::UserRole + 3).toBool() || value.isEmpty()) return;
        const bool saved = item->data(0, Qt::UserRole).toBool();
        const QString node = item->data(0, Qt::UserRole + 2).toString();
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (saved) SessionHistory::renameSaved(node, index, value);
        else SessionHistory::renameRecent(index, value);
        refreshHistory();
    }

    void moveSession(QTreeWidgetItem* source, QTreeWidgetItem* target) {
        if (!source || !target || !source->data(0, Qt::UserRole + 3).toBool() ||
            !target->data(0, Qt::UserRole + 4).toBool()) return;
        const bool saved = source->data(0, Qt::UserRole).toBool();
        const QString sourceNode = source->data(0, Qt::UserRole + 2).toString();
        const int index = source->data(0, Qt::UserRole + 1).toInt();
        const QString targetNode = target->data(0, Qt::UserRole + 2).toString();
        if (SessionHistory::moveSession(saved, sourceNode, index, targetNode)) refreshHistory();
    }

    HistoryTree* history_ = nullptr;
    QTreeWidgetItem* selectedItem_ = nullptr;
    QLineEdit* label_ = nullptr;
    QLineEdit* left_ = nullptr;
    QLineEdit* right_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* save_ = nullptr;
    SessionHistoryEntry selected_;
    bool selectedSaved_ = false;
};

}  // namespace openbc::app
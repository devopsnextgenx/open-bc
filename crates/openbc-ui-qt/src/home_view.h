#pragma once

#include <QFileInfo>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

#include "session_history.h"
#include "qt_style.h"

namespace openbc::app {

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

        history_ = new QTreeWidget(split);
        history_->setObjectName("homeHistory");
        history_->setHeaderHidden(true);
        history_->setRootIsDecorated(true);
        history_->setUniformRowHeights(true);
        history_->setIndentation(18);
        history_->setSelectionMode(QAbstractItemView::SingleSelection);
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
        connect(open_, &QPushButton::clicked, this, [this]() {
            if (selected_.kind.isEmpty() || !onOpenHistory) return;
            selected_.label = label_->text().trimmed();
            selected_.left = left_->text();
            selected_.right = right_->text();
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
        history_->clear();
        auto* recent = new QTreeWidgetItem(history_, {"Recent sessions"});
        recent->setIcon(0, openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Refresh));
        recent->setFlags(Qt::ItemIsEnabled);
        const auto entries = SessionHistory::load();
        for (int index = 0; index < entries.size(); ++index) addSessionItem(recent, entries[index], false, {}, index);
        recent->setExpanded(true);
        for (const auto& node : SessionHistory::loadSavedNodes()) {
            auto* nodeItem = new QTreeWidgetItem(history_, {node.name});
            nodeItem->setIcon(0, openbc::ui::icons::glyph(openbc::ui::icons::Glyph::FolderOpen));
            nodeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            nodeItem->setData(0, Qt::UserRole + 2, node.name);
            for (int index = 0; index < node.sessions.size(); ++index) {
                addSessionItem(nodeItem, node.sessions[index], true, node.name, index);
            }
            nodeItem->setExpanded(true);
        }
        history_->expandAll();
    }

private:
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
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
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
        const bool saved = item->data(0, Qt::UserRole).toBool();
        const QString node = item->data(0, Qt::UserRole + 2).toString();
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (!item->parent()) {
            if (node.isEmpty()) return;
            QMenu menu(history_);
            QAction* remove = menu.addAction("Remove named node");
            if (menu.exec(history_->viewport()->mapToGlobal(position)) == remove) {
                SessionHistory::removeNode(node);
                refreshHistory();
            }
            return;
        }
        if (!saved && item->parent()->text(0) != "Recent sessions") return;
        QMenu menu(history_);
        QAction* rename = menu.addAction("Rename session");
        QAction* save = menu.addAction("Save to named node");
        QAction* remove = menu.addAction("Remove from history");
        QAction* chosen = menu.exec(history_->viewport()->mapToGlobal(position));
        if (chosen == rename) {
            const QString value = QInputDialog::getText(this, "Rename session", "Name", QLineEdit::Normal, item->text(0));
            if (!value.trimmed().isEmpty()) {
                if (saved) SessionHistory::renameSaved(node, index, value);
                else SessionHistory::renameRecent(index, value);
            }
            refreshHistory();
        } else if (chosen == save) {
            history_->setCurrentItem(item);
            saveSelected();
        } else if (chosen == remove) {
            if (saved) SessionHistory::removeSaved(node, index); else SessionHistory::removeAt(index);
            refreshHistory();
        }
    }

    QTreeWidget* history_ = nullptr;
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

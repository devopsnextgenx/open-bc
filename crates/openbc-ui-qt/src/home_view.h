#pragma once

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

#include "session_history.h"
#include "qt_style.h"

namespace openbc::app {

inline QIcon timelineIcon(const QColor& color) {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(QPen(color.lighter(135), 1.3));
    painter.drawLine(QPointF(8, 1), QPointF(8, 15));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(8, 8), 4, 4);
    return QIcon(pixmap);
}

inline QIcon historyFileIcon(const QString& path) {
    const QString extension = QFileInfo(path).suffix().toLower();
    QColor color(0x8f, 0x9b, 0xb3);
    if (extension == "rs") color = QColor(0xe5, 0x7a, 0x44);
    else if (extension == "cpp" || extension == "h") color = QColor(0x4f, 0x9d, 0xde);
    else if (extension == "py") color = QColor(0xe5, 0xc0, 0x7b);
    else if (extension == "js" || extension == "ts") color = QColor(0xe8, 0xc5, 0x47);
    else if (extension == "json" || extension == "toml" || extension == "yaml") {
        color = QColor(0x68, 0xc0, 0x9a);
    }
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
        root->setContentsMargins(36, 28, 36, 28);
        auto* heading = new QLabel("OpenBC", this);
        heading->setObjectName("homeHeading");
        root->addWidget(heading);
        auto* subtitle = new QLabel(
            "Compare folders and files with a clear history of recent work.", this);
        subtitle->setObjectName("homeSubtitle");
        root->addWidget(subtitle);

        auto* actions = new QHBoxLayout;
        auto* folder = new QPushButton("New Folder Compare", this);
        auto* text = new QPushButton("New Text Compare", this);
        actions->addWidget(folder);
        actions->addWidget(text);
        actions->addStretch();
        root->addLayout(actions);
        connect(folder, &QPushButton::clicked, this,
                [this]() { if (onNewFolderCompare) onNewFolderCompare(); });
        connect(text, &QPushButton::clicked, this,
                [this]() { if (onNewTextCompare) onNewTextCompare(); });

        auto* historyTitle = new QLabel("Recent sessions", this);
        historyTitle->setObjectName("homeSectionTitle");
        root->addWidget(historyTitle);
        history_ = new QTreeWidget(this);
        history_->setObjectName("homeHistory");
        history_->setHeaderHidden(true);
        history_->setRootIsDecorated(true);
        history_->setUniformRowHeights(true);
        history_->setIndentation(20);
        history_->setSelectionMode(QAbstractItemView::SingleSelection);
        history_->setContextMenuPolicy(Qt::CustomContextMenu);
        root->addWidget(history_, 1);
        connect(history_, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem* item, int) {
                    const int index = item->data(0, Qt::UserRole).toInt();
                    if (index >= 0 && index < historyEntries_.size() && onOpenHistory) {
                        onOpenHistory(historyEntries_[index]);
                    }
                });
        connect(history_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
            auto* item = history_->itemAt(position);
            if (!item || !item->parent()) return;
            const int index = item->data(0, Qt::UserRole).toInt();
            if (index < 0 || index >= historyEntries_.size()) return;
            QMenu menu(history_);
            QAction* remove = menu.addAction("Remove from history");
            if (menu.exec(history_->viewport()->mapToGlobal(position)) == remove) {
                SessionHistory::removeAt(index);
                refreshHistory();
            }
        });
        refreshHistory();
    }

    void refreshHistory() {
        history_->clear();
        historyEntries_ = SessionHistory::load();
        QHash<QString, QTreeWidgetItem*> groups;
        for (int index = 0; index < historyEntries_.size(); ++index) {
            const auto& entry = historyEntries_[index];
            const int age = entry.openedAt.date().daysTo(QDate::currentDate());
            const QString groupName = age <= 0 ? "Today"
                                      : age == 1 ? "Yesterday"
                                      : age <= 7 ? "This week"
                                      : age <= 14 ? "Last week"
                                      : age <= 31 ? "Last month"
                                                   : "Older";
            auto* group = groups.value(groupName);
            if (!group) {
                group = new QTreeWidgetItem(history_, {groupName});
                const QColor groupColor = groupName == "Today"       ? QColor(0x55, 0xd6, 0xa7)
                                        : groupName == "Yesterday"  ? QColor(0x5b, 0xb5, 0xf5)
                                        : groupName == "This week"  ? QColor(0xc0, 0x9a, 0xff)
                                        : groupName == "Last week"  ? QColor(0xf0, 0xb3, 0x5a)
                                        : groupName == "Last month" ? QColor(0xf0, 0x7a, 0x8a)
                                                                     : QColor(0x9a, 0xa3, 0xb8);
                group->setIcon(0, timelineIcon(groupColor));
                group->setExpanded(true);
                group->setFlags(Qt::ItemIsEnabled);
                groups.insert(groupName, group);
            }
            const QString left = entry.left.isEmpty() ? "(missing)" : entry.left;
            const QString right = entry.right.isEmpty() ? "(missing)" : entry.right;
            auto* leaf = new QTreeWidgetItem(group);
            const bool folder = entry.kind == "folder";
            leaf->setIcon(0, folder ? openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow())
                                    : historyFileIcon(left));
            leaf->setText(0, (folder ? "Folder  " : "Files   ") + QFileInfo(left).fileName() +
                              "  <->  " + QFileInfo(right).fileName());
            leaf->setToolTip(0, left + "\n<->\n" + right);
            leaf->setData(0, Qt::UserRole, index);
            leaf->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
        history_->expandAll();
    }

private:
    QTreeWidget* history_ = nullptr;
    QList<SessionHistoryEntry> historyEntries_;
};

}  // namespace openbc::app
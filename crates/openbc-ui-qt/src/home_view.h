#pragma once

#include <QDateTime>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

#include "session_history.h"

namespace openbc::app {

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
        root->addWidget(history_, 1);
        connect(history_, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem* item, int) {
                    const int index = item->data(0, Qt::UserRole).toInt();
                    if (index >= 0 && index < historyEntries_.size() && onOpenHistory) {
                        onOpenHistory(historyEntries_[index]);
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
                group->setExpanded(true);
                group->setFlags(Qt::ItemIsEnabled);
                groups.insert(groupName, group);
            }
            const QString left = entry.left.isEmpty() ? "(missing)" : entry.left;
            const QString right = entry.right.isEmpty() ? "(missing)" : entry.right;
            auto* leaf = new QTreeWidgetItem(group);
            leaf->setText(0, (entry.kind == "folder" ? "Folder  " : "Text    ") + left +
                              "  <->  " + right);
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
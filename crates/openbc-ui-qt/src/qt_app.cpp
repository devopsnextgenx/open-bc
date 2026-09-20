#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCryptographicHash>
#include <QMetaObject>
#include <QPointer>
#include <QScrollBar>
#include <QThreadPool>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStorageInfo>
#include <QStyle>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace {

enum class RowStatus { Pending, Equal, Different, Missing };

constexpr int pathRole = Qt::UserRole;
constexpr int statusRole = Qt::UserRole + 1;

QString formatBytes(qint64 bytes) {
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', unit == 0 ? 0 : 1) + " " + units[unit];
}

QString folderStatusText(const QString& label, const QString& path) {
    QFileInfo info(path);
    QStorageInfo storage(path);
    const QString access = info.isReadable() ? "Read access" : "Access denied";
    return QString("%1  |  %2  |  %3  |  %4  |  %5 free")
                            .arg(label)
                            .arg(info.exists() ? "Folder" : "Unavailable")
                            .arg(info.exists() ? formatBytes(storage.bytesAvailable()) : "-")
                            .arg(access)
                            .arg(storage.rootPath());
}

QTreeWidget* createTree() {
    auto* tree = new QTreeWidget;
    tree->setColumnCount(3);
    tree->setHeaderLabels({"Name", "Size", "Modified"});
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setRootIsDecorated(true);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    return tree;
}

void setRowHeight(QTreeWidget* tree, QTreeWidgetItem* item) {
    const QSize sizeHint(0, tree->fontMetrics().lineSpacing() + 8);
    for (int column = 0; column < tree->columnCount(); ++column) {
        item->setSizeHint(column, sizeHint);
    }
}

QColor statusColor(RowStatus status) {
    switch (status) {
    case RowStatus::Pending:
        return QColor("#3f4b5b");
    case RowStatus::Equal:
        return QColor("#214d35");
    case RowStatus::Different:
        return QColor("#5c4b1f");
    case RowStatus::Missing:
        return QColor("#5a2b2b");
    }
    return QColor();
}

QMap<QString, QFileInfo> folderEntries(const QString& path, const QString& filter) {
    QMap<QString, QFileInfo> entries;
    if (path.isEmpty()) {
        return entries;
    }
    QDir directory(path);
    const auto infos = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::Name);
    for (const QFileInfo& info : infos) {
        if (!filter.isEmpty() && !info.fileName().contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        entries.insert(info.fileName(), info);
    }
    return entries;
}

RowStatus compareEntries(const QFileInfo* left, const QFileInfo* right) {
    if (!left || !right) {
        return RowStatus::Missing;
    }
    if (left->isDir() && right->isDir()) {
        return RowStatus::Equal;
    }
    return !left->isDir() && !right->isDir() && left->size() == right->size()
                   && left->lastModified() == right->lastModified()
               ? RowStatus::Equal
               : RowStatus::Different;
}

RowStatus compareFiles(const QFileInfo* left, const QFileInfo* right, bool checkContent) {
    if (!left || !right || left->isDir() || right->isDir()) {
        return compareEntries(left, right);
    }
    if (left->size() != right->size()) {
        return RowStatus::Different;
    }
    if (!checkContent) {
        return left->lastModified() == right->lastModified() ? RowStatus::Equal
                                                               : RowStatus::Different;
    }

    QFile leftFile(left->absoluteFilePath());
    QFile rightFile(right->absoluteFilePath());
    if (!leftFile.open(QIODevice::ReadOnly) || !rightFile.open(QIODevice::ReadOnly)) {
        return RowStatus::Different;
    }
    QCryptographicHash leftHasher(QCryptographicHash::Sha256);
    QCryptographicHash rightHasher(QCryptographicHash::Sha256);
    constexpr qint64 chunkSize = 1024 * 1024;
    while (!leftFile.atEnd() && !rightFile.atEnd()) {
        leftHasher.addData(leftFile.read(chunkSize));
        rightHasher.addData(rightFile.read(chunkSize));
    }
    if (!leftFile.atEnd() || !rightFile.atEnd()) {
        return RowStatus::Different;
    }
    return leftHasher.result() == rightHasher.result() ? RowStatus::Equal : RowStatus::Different;
}

void styleRow(QTreeWidgetItem* item, RowStatus status) {
    const QColor backgroundColor = statusColor(status);
    const QBrush background(backgroundColor);
    const QBrush foreground(backgroundColor.lightnessF() < 0.55 ? QColor("#f8fafc")
                                                                  : QColor("#17202a"));
    for (int column = 0; column < 3; ++column) {
        item->setBackground(column, background);
        item->setForeground(column, foreground);
    }
    item->setData(0, statusRole, static_cast<int>(status));
}

QTreeWidgetItem* addRow(QTreeWidget* tree, const QString& name, const QFileInfo* info,
                        RowStatus status, QTreeWidgetItem* parent = nullptr) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setText(0, name);
    if (info) {
        item->setText(1, info->isDir() ? "Folder" : formatBytes(info->size()));
        item->setText(2, info->lastModified().toString("M/d/yyyy h:mm AP"));
    } else {
        item->setText(1, {});
        item->setText(2, {});
    }
    item->setData(0, pathRole, info ? info->absoluteFilePath() : QString());
    setRowHeight(tree, item);
    styleRow(item, status);
    if (info && info->isDir()) {
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    return item;
}

struct ComparisonRun : std::enable_shared_from_this<ComparisonRun> {
    struct NodeState {
        QTreeWidgetItem* leftItem = nullptr;
        QTreeWidgetItem* rightItem = nullptr;
        std::shared_ptr<NodeState> parent;
        int pendingChildren = 0;
        bool different = false;
    };

    struct EntryPair {
        QString name;
        QFileInfo left;
        QFileInfo right;
        bool hasLeft = false;
        bool hasRight = false;
        RowStatus status = RowStatus::Pending;
    };

    QPointer<QTreeWidget> leftTree;
    QPointer<QTreeWidget> rightTree;
    QThreadPool pool;
    std::atomic_bool cancelled = false;
    bool checkContent = true;
    QString filter;
    std::function<void()> onComplete;

    ComparisonRun(QTreeWidget* leftTree, QTreeWidget* rightTree, bool checkContent,
                  QString filter)
        : leftTree(leftTree), rightTree(rightTree), checkContent(checkContent),
          filter(std::move(filter)) {
        pool.setMaxThreadCount(5);
    }

    ~ComparisonRun() {
        cancelled.store(true);
        pool.waitForDone();
    }

    void start(const QString& leftPath, const QString& rightPath) {
        scheduleFolder(leftPath, rightPath, nullptr, nullptr, nullptr);
    }

    static std::vector<EntryPair> collectEntries(const QString& leftPath, const QString& rightPath,
                                                  const QString& filter, bool checkContent) {
        const auto leftEntries = folderEntries(leftPath, filter);
        const auto rightEntries = folderEntries(rightPath, filter);
        QMap<QString, EntryPair> entries;
        for (auto it = leftEntries.cbegin(); it != leftEntries.cend(); ++it) {
            entries[it.key()].name = it.key();
            entries[it.key()].left = it.value();
            entries[it.key()].hasLeft = true;
        }
        for (auto it = rightEntries.cbegin(); it != rightEntries.cend(); ++it) {
            entries[it.key()].name = it.key();
            entries[it.key()].right = it.value();
            entries[it.key()].hasRight = true;
        }
        std::vector<EntryPair> result;
        result.reserve(entries.size());
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            auto entry = it.value();
            const QFileInfo* left = entry.hasLeft ? &entry.left : nullptr;
            const QFileInfo* right = entry.hasRight ? &entry.right : nullptr;
            entry.status = left && right && left->isDir() && right->isDir()
                               ? RowStatus::Pending
                               : compareFiles(left, right, checkContent);
            result.push_back(std::move(entry));
        }
        return result;
    }

    void scheduleFolder(QString leftPath, QString rightPath, QTreeWidgetItem* leftParent,
                        QTreeWidgetItem* rightParent, std::shared_ptr<NodeState> parent) {
        auto run = shared_from_this();
        auto* task = QRunnable::create([run, leftPath = std::move(leftPath), rightPath = std::move(rightPath),
                                         leftParent, rightParent, parent = std::move(parent)]() mutable {
            if (run->cancelled.load()) {
                return;
            }
            const auto entries = collectEntries(leftPath, rightPath, run->filter, run->checkContent);
            QMetaObject::invokeMethod(run->leftTree, [run, entries, leftParent, rightParent,
                                                       parent]() {
                if (run->cancelled.load() || !run->leftTree || !run->rightTree) {
                    return;
                }
                auto state = std::make_shared<NodeState>();
                state->leftItem = leftParent;
                state->rightItem = rightParent;
                state->parent = parent;
                for (const auto& entry : entries) {
                    const QFileInfo* left = entry.hasLeft ? &entry.left : nullptr;
                    const QFileInfo* right = entry.hasRight ? &entry.right : nullptr;
                    const bool expandableFolder = (left && left->isDir()) || (right && right->isDir());
                    const RowStatus status = entry.status;
                    auto* leftItem = addRow(run->leftTree, left ? entry.name : QString(), left,
                                            status, leftParent);
                    auto* rightItem = addRow(run->rightTree, right ? entry.name : QString(), right,
                                             status, rightParent);
                    if (expandableFolder) {
                        ++state->pendingChildren;
                        run->scheduleFolder(left && left->isDir() ? left->absoluteFilePath() : QString(),
                                            right && right->isDir() ? right->absoluteFilePath() : QString(),
                                            leftItem, rightItem, state);
                    } else if (status != RowStatus::Equal) {
                        state->different = true;
                    }
                }
                run->finishNode(state);
            }, Qt::QueuedConnection);
        });
        task->setAutoDelete(true);
        pool.start(task);
    }

    void finishNode(const std::shared_ptr<NodeState>& state) {
        const RowStatus status = state->different ? RowStatus::Different : RowStatus::Equal;
        if (state->leftItem) {
            styleRow(state->leftItem, status);
        }
        if (state->rightItem) {
            styleRow(state->rightItem, status);
        }
        if (state->parent) {
            auto parent = state->parent;
            parent->different = parent->different || status != RowStatus::Equal;
            --parent->pendingChildren;
            if (parent->pendingChildren == 0) {
                finishNode(parent);
            }
        } else if (onComplete && !cancelled.load()) {
            onComplete();
        }
    }
};

QTreeWidgetItem* pairedItem(QTreeWidgetItem* item, QTreeWidget* otherTree) {
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

}

extern "C" int openbc_run_gui() {
    int argc = 1;
    char application_name[] = "openbc-qt";
    char* argv[] = {application_name, nullptr};
    QApplication application(argc, argv);

    QMainWindow window;
    window.setWindowTitle("OpenBC - Folder Compare");
    window.resize(1180, 760);

    auto* fileMenu = window.menuBar()->addMenu("Session");
    auto* actionsMenu = window.menuBar()->addMenu("Actions");
    auto* editMenu = window.menuBar()->addMenu("Edit");
    auto* searchMenu = window.menuBar()->addMenu("Search");
    auto* viewMenu = window.menuBar()->addMenu("View");
    auto* toolsMenu = window.menuBar()->addMenu("Tools");
    window.menuBar()->addMenu("Help");

    auto* toolbar = new QToolBar("Commands", &window);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    window.addToolBar(toolbar);

    auto* central = new QWidget(&window);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(6, 4, 6, 4);
    mainLayout->setSpacing(4);

    auto* commandRow = new QWidget(central);
    auto* commandLayout = new QHBoxLayout(commandRow);
    commandLayout->setContentsMargins(0, 0, 0, 0);
    auto* commandLabel = new QLabel("Compare:", commandRow);
    auto* command = new QComboBox(commandRow);
    command->addItems({"Folder structure", "Folder contents", "Text compare", "Binary compare"});
    command->setMinimumWidth(190);
    commandLayout->addWidget(commandLabel);
    commandLayout->addWidget(command);
    commandLayout->addStretch();
    mainLayout->addWidget(commandRow);

    auto* panes = new QSplitter(Qt::Horizontal, central);
    auto* leftPanel = new QWidget(panes);
    auto* rightPanel = new QWidget(panes);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    leftLayout->setContentsMargins(0, 0, 2, 0);
    rightLayout->setContentsMargins(2, 0, 0, 0);

    auto* leftPath = new QLineEdit(leftPanel);
    auto* rightPath = new QLineEdit(rightPanel);
    auto* leftPathRow = new QWidget(leftPanel);
    auto* rightPathRow = new QWidget(rightPanel);
    auto* leftPathLayout = new QHBoxLayout(leftPathRow);
    auto* rightPathLayout = new QHBoxLayout(rightPathRow);
    leftPathLayout->setContentsMargins(0, 0, 0, 0);
    rightPathLayout->setContentsMargins(0, 0, 0, 0);
    auto* leftBrowse = new QPushButton("...", leftPathRow);
    auto* rightBrowse = new QPushButton("...", rightPathRow);
    leftBrowse->setToolTip("Select left folder");
    rightBrowse->setToolTip("Select right folder");
    leftPathLayout->addWidget(leftPath);
    leftPathLayout->addWidget(leftBrowse);
    rightPathLayout->addWidget(rightPath);
    rightPathLayout->addWidget(rightBrowse);
    leftPath->setPlaceholderText("Left folder path or resource");
    rightPath->setPlaceholderText("Right folder path or resource");
    leftPath->setText("/home/kira/tmp/source");
    rightPath->setText("/home/kira/tmp/target");
    auto* leftTree = createTree();
    auto* rightTree = createTree();
    leftLayout->addWidget(leftPathRow);
    leftLayout->addWidget(leftTree);
    rightLayout->addWidget(rightPathRow);
    rightLayout->addWidget(rightTree);
    panes->addWidget(leftPanel);
    panes->addWidget(rightPanel);
    panes->setSizes({550, 550});
    mainLayout->addWidget(panes, 1);

    auto* console = new QPlainTextEdit(central);
    console->setReadOnly(true);
    console->setMaximumBlockCount(500);
    console->setPlaceholderText("Activity log");
    console->setMinimumHeight(110);
    mainLayout->addWidget(console);
    window.setCentralWidget(central);
    auto* leftMeta = new QLabel(&window);
    auto* rightMeta = new QLabel(&window);
    window.statusBar()->addPermanentWidget(leftMeta, 1);
    window.statusBar()->addPermanentWidget(rightMeta, 1);

    auto log = [console](const QString& message) {
        console->appendPlainText(QTime::currentTime().toString("HH:mm:ss") + "  " + message);
    };
    QString currentFilter;
    std::shared_ptr<ComparisonRun> activeRun;
    auto refresh = [&]() {
        if (activeRun) {
            activeRun->cancelled.store(true);
        }
        leftTree->clear();
        rightTree->clear();
        activeRun = std::make_shared<ComparisonRun>(
            leftTree, rightTree, command->currentText() != "Folder structure", currentFilter);
        ComparisonRun* expectedRun = activeRun.get();
        activeRun->onComplete = [&, expectedRun]() {
            if (activeRun.get() != expectedRun) {
                return;
            }
            window.statusBar()->showMessage("Folder comparison completed");
            log("Folder comparison completed");
        };
        activeRun->start(leftPath->text(), rightPath->text());
        leftMeta->setText(folderStatusText("Left: " + leftPath->text(), leftPath->text()));
        rightMeta->setText(folderStatusText("Right: " + rightPath->text(), rightPath->text()));
        window.statusBar()->showMessage("Comparing folder contents...");
        log("Started five-worker folder comparison");
    };
    auto chooseFolder = [&](QLineEdit* path, const QString& side) {
        const QString selected = QFileDialog::getExistingDirectory(&window, "Select " + side + " folder", path->text());
        if (!selected.isEmpty()) {
            path->setText(selected);
            refresh();
            log(side + " resource selected: " + selected);
        }
    };

    auto* compare = new QAction(window.style()->standardIcon(QStyle::SP_DialogApplyButton), "Compare", &window);
    auto* swap = new QAction(window.style()->standardIcon(QStyle::SP_ArrowRight), "Swap", &window);
    auto* filter = new QAction(window.style()->standardIcon(QStyle::SP_FileDialogContentsView), "Filters", &window);
    auto* select = new QAction(window.style()->standardIcon(QStyle::SP_DialogYesButton), "Select", &window);
    auto* selectRight = new QAction("Select right folder", &window);
    auto* refreshAction = new QAction(window.style()->standardIcon(QStyle::SP_BrowserReload), "Refresh", &window);
    toolbar->addAction(compare);
    toolbar->addAction(swap);
    toolbar->addAction(filter);
    toolbar->addSeparator();
    toolbar->addAction(select);
    toolbar->addAction(refreshAction);
    fileMenu->addAction("New comparison", [&]() { log("New comparison requested"); });
    fileMenu->addAction("Open comparison", [&]() { log("Open comparison requested"); });
    fileMenu->addSeparator();
    fileMenu->addAction("Close", &window, &QWidget::close);
    actionsMenu->addAction(compare);
    actionsMenu->addAction(swap);
    actionsMenu->addAction(filter);
    actionsMenu->addAction(selectRight);
    auto* compareContents = actionsMenu->addAction("Compare contents");
    compareContents->setCheckable(true);
    compareContents->setChecked(true);
    auto* ignoreDates = actionsMenu->addAction("Ignore timestamps");
    ignoreDates->setCheckable(true);
    editMenu->addAction("Edit selection", [&]() { log("Edit selection requested"); });
    searchMenu->addAction("Find", [&]() { log("Find requested in current folder level"); });
    viewMenu->addAction("Toggle console", [&]() { console->setVisible(!console->isVisible()); });
    toolsMenu->addAction("Comparison options", [&]() { log("Comparison options opened"); });

    QObject::connect(compare, &QAction::triggered, [&]() { log("Comparing folder contents recursively"); refresh(); });
    QObject::connect(swap, &QAction::triggered, [&]() {
        const QString oldLeft = leftPath->text();
        leftPath->setText(rightPath->text());
        rightPath->setText(oldLeft);
        refresh();
        log("Left and right resources swapped");
    });
    QObject::connect(filter, &QAction::triggered, [&]() {
        bool accepted = false;
        const QString pattern = QInputDialog::getText(&window, "Filters", "Name contains:", QLineEdit::Normal, "", &accepted);
        if (accepted) {
            currentFilter = pattern;
            refresh();
            log("Applied filter: " + (pattern.isEmpty() ? "all entries" : pattern));
        }
    });
    QObject::connect(select, &QAction::triggered, [&]() { chooseFolder(leftPath, "left"); });
    QObject::connect(selectRight, &QAction::triggered, [&]() { chooseFolder(rightPath, "right"); });
    QObject::connect(leftBrowse, &QPushButton::clicked, [&]() { chooseFolder(leftPath, "left"); });
    QObject::connect(rightBrowse, &QPushButton::clicked, [&]() { chooseFolder(rightPath, "right"); });
    QObject::connect(refreshAction, &QAction::triggered, refresh);
    QObject::connect(leftPath, &QLineEdit::returnPressed, refresh);
    QObject::connect(rightPath, &QLineEdit::returnPressed, refresh);
    QObject::connect(leftTree, &QTreeWidget::itemExpanded, [&](QTreeWidgetItem* item) {
        auto* counterpart = pairedItem(item, rightTree);
        if (counterpart) {
            const QSignalBlocker blocker(rightTree);
            counterpart->setExpanded(true);
        }
        const QString path = item->data(0, pathRole).toString();
        log("Expanded left folder: " + path);
    });
    QObject::connect(rightTree, &QTreeWidget::itemExpanded, [&](QTreeWidgetItem* item) {
        auto* counterpart = pairedItem(item, leftTree);
        if (counterpart) {
            const QSignalBlocker blocker(leftTree);
            counterpart->setExpanded(true);
        }
        const QString path = item->data(0, pathRole).toString();
        log("Expanded right folder: " + path);
    });
    QObject::connect(leftTree, &QTreeWidget::itemCollapsed, [&](QTreeWidgetItem* item) {
        if (auto* counterpart = pairedItem(item, rightTree)) {
            const QSignalBlocker blocker(rightTree);
            counterpart->setExpanded(false);
        }
    });
    QObject::connect(rightTree, &QTreeWidget::itemCollapsed, [&](QTreeWidgetItem* item) {
        if (auto* counterpart = pairedItem(item, leftTree)) {
            const QSignalBlocker blocker(leftTree);
            counterpart->setExpanded(false);
        }
    });
    QObject::connect(leftTree, &QTreeWidget::currentItemChanged,
                     [&](QTreeWidgetItem* current, QTreeWidgetItem*) {
                         if (auto* counterpart = pairedItem(current, rightTree)) {
                             const QSignalBlocker blocker(rightTree);
                             rightTree->setCurrentItem(counterpart);
                         }
                     });
    QObject::connect(rightTree, &QTreeWidget::currentItemChanged,
                     [&](QTreeWidgetItem* current, QTreeWidgetItem*) {
                         if (auto* counterpart = pairedItem(current, leftTree)) {
                             const QSignalBlocker blocker(leftTree);
                             leftTree->setCurrentItem(counterpart);
                         }
                     });

    bool synchronizingScroll = false;
    auto synchronizeHorizontalScrollBars = [&](QScrollBar* source, QScrollBar* target) {
        if (synchronizingScroll || !source || !target) {
            return;
        }
        synchronizingScroll = true;
        const int sourceRange = source->maximum() - source->minimum();
        const int targetRange = target->maximum() - target->minimum();
        const int sourceOffset = source->value() - source->minimum();
        const int targetValue = sourceRange > 0
                                     ? target->minimum() +
                                           (sourceOffset * targetRange + sourceRange / 2) / sourceRange
                                     : target->minimum();
        target->setValue(targetValue);
        synchronizingScroll = false;
    };
    auto synchronizeVerticalTrees = [&](QTreeWidget* source, QTreeWidget* target) {
        if (synchronizingScroll || !source || !target) {
            return;
        }
        auto* sourceItem = source->itemAt(1, 1);
        auto* targetItem = pairedItem(sourceItem, target);
        if (!targetItem) {
            return;
        }
        synchronizingScroll = true;
        const int sourceTop = source->visualItemRect(sourceItem).top();
        target->scrollToItem(targetItem, QAbstractItemView::PositionAtTop);
        const int targetTop = target->visualItemRect(targetItem).top();
        target->verticalScrollBar()->setValue(target->verticalScrollBar()->value() +
                                              targetTop - sourceTop);
        synchronizingScroll = false;
    };
    QObject::connect(leftTree->verticalScrollBar(), &QScrollBar::valueChanged,
                     [&](int) { synchronizeVerticalTrees(leftTree, rightTree); });
    QObject::connect(rightTree->verticalScrollBar(), &QScrollBar::valueChanged,
                     [&](int) { synchronizeVerticalTrees(rightTree, leftTree); });
    QObject::connect(leftTree->verticalScrollBar(), &QScrollBar::rangeChanged,
                     [&](int, int) { synchronizeVerticalTrees(leftTree, rightTree); });
    QObject::connect(rightTree->verticalScrollBar(), &QScrollBar::rangeChanged,
                     [&](int, int) { synchronizeVerticalTrees(rightTree, leftTree); });
    QObject::connect(leftTree->horizontalScrollBar(), &QScrollBar::valueChanged,
                     [&](int) { synchronizeHorizontalScrollBars(leftTree->horizontalScrollBar(),
                                                                 rightTree->horizontalScrollBar()); });
    QObject::connect(rightTree->horizontalScrollBar(), &QScrollBar::valueChanged,
                     [&](int) { synchronizeHorizontalScrollBars(rightTree->horizontalScrollBar(),
                                                                 leftTree->horizontalScrollBar()); });
    QObject::connect(leftTree->horizontalScrollBar(), &QScrollBar::rangeChanged,
                     [&](int, int) { synchronizeHorizontalScrollBars(leftTree->horizontalScrollBar(),
                                                                      rightTree->horizontalScrollBar()); });
    QObject::connect(rightTree->horizontalScrollBar(), &QScrollBar::rangeChanged,
                     [&](int, int) { synchronizeHorizontalScrollBars(rightTree->horizontalScrollBar(),
                                                                      leftTree->horizontalScrollBar()); });

    refresh();
    leftMeta->setText(folderStatusText("Left: " + leftPath->text(), leftPath->text()));
    rightMeta->setText(folderStatusText("Right: " + rightPath->text(), rightPath->text()));
    window.statusBar()->showMessage("Ready");
    log("Ready: select two folders and compare");
    window.show();

    return application.exec();
}
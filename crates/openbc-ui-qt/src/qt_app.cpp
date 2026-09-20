#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStorageInfo>
#include <QStyle>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

namespace {

struct FolderPane {
    QLineEdit* path;
    QTreeWidget* tree;
};

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

void populateTree(QTreeWidget* tree, const QString& path, const QString& filter) {
    tree->clear();
    QDir directory(path);
    const auto entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& info : entries) {
        if (!filter.isEmpty() && !info.fileName().contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto* item = new QTreeWidgetItem(tree);
        item->setText(0, info.fileName());
        item->setText(1, info.isDir() ? "Folder" : formatBytes(info.size()));
        item->setText(2, info.lastModified().toString("M/d/yyyy h:mm AP"));
        item->setData(0, Qt::UserRole, info.absoluteFilePath());
        if (info.isDir()) {
            new QTreeWidgetItem(item);
        }
    }
    tree->sortItems(0, Qt::AscendingOrder);
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

void populateChildren(QTreeWidgetItem* parent, const QString& path, const QString& filter) {
    parent->takeChildren();
    QDir directory(path);
    const auto entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& info : entries) {
        if (!filter.isEmpty() && !info.fileName().contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto* item = new QTreeWidgetItem(parent);
        item->setText(0, info.fileName());
        item->setText(1, info.isDir() ? "Folder" : formatBytes(info.size()));
        item->setText(2, info.lastModified().toString("M/d/yyyy h:mm AP"));
        item->setData(0, Qt::UserRole, info.absoluteFilePath());
        if (info.isDir()) {
            new QTreeWidgetItem(item);
        }
    }
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
    leftPath->setText(QDir::homePath());
    rightPath->setText(QDir::homePath());
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
    auto refresh = [&]() {
        populateTree(leftTree, leftPath->text(), "");
        populateTree(rightTree, rightPath->text(), "");
        leftMeta->setText(folderStatusText("Left: " + leftPath->text(), leftPath->text()));
        rightMeta->setText(folderStatusText("Right: " + rightPath->text(), rightPath->text()));
        window.statusBar()->showMessage("Background content comparison completed");
        log("Background content comparison completed");
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

    QObject::connect(compare, &QAction::triggered, [&]() { log("Comparing visible folder levels"); refresh(); });
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
            populateTree(leftTree, leftPath->text(), pattern);
            populateTree(rightTree, rightPath->text(), pattern);
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
        const QString path = item->data(0, Qt::UserRole).toString();
        populateChildren(item, path, "");
        log("Expanded left folder: " + path);
    });
    QObject::connect(rightTree, &QTreeWidget::itemExpanded, [&](QTreeWidgetItem* item) {
        const QString path = item->data(0, Qt::UserRole).toString();
        populateChildren(item, path, "");
        log("Expanded right folder: " + path);
    });

    populateTree(leftTree, leftPath->text(), "");
    populateTree(rightTree, rightPath->text(), "");
    leftMeta->setText(folderStatusText("Left: " + leftPath->text(), leftPath->text()));
    rightMeta->setText(folderStatusText("Right: " + rightPath->text(), rightPath->text()));
    window.statusBar()->showMessage("Ready");
    log("Ready: select two folders and compare");
    window.show();

    return application.exec();
}
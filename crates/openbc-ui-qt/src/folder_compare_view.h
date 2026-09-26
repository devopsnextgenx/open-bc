// folder_compare_view.h
// ---------------------------------------------------------------------------
// Folder-compare engine and UI: ComparisonRun walks both trees on a
// background thread pool and reports progress/completion; CompareSession is
// the tab widget (two QTreeWidget panes, toolbar, filters, context menus)
// built on top of it. Extracted unchanged from the original monolithic
// qt_app.cpp - this area wasn't part of the requested text-editor feature
// work, so it is deliberately left as-is rather than risk introducing bugs
// in code that already worked.
//
// Talks to main_window.h only through onTextCompareRequested/onHomeRequested
// style callbacks, so it has no dependency on TextCompareView.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSettings>
#include <QRunnable>
#include <QSplitter>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>

#include "folder_model.h"
#include "path_selector.h"
#include "qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;
using openbc::ui::CompareTree;
using openbc::ui::kPairClassCount;

// ---------------------------------------------------------------------------
// Comparison engine (one instance per refresh of one session)
// ---------------------------------------------------------------------------

struct ComparisonRun : std::enable_shared_from_this<ComparisonRun> {
    // Bookkeeping for one folder level while its children are being compared.
    struct NodeState {
        QTreeWidgetItem* leftItem = nullptr;
        QTreeWidgetItem* rightItem = nullptr;
        std::shared_ptr<NodeState> parent;
        int pendingChildren = 0;
        bool different = false;
        bool leftMissing = false;
        bool rightMissing = false;
        bool mismatched = false;  // file on one side, folder on the other
        quint32 leftMask = 0;     // which kinds of status occur below (folder icon)
        quint32 rightMask = 0;
        qint64 leftBytes = 0;
        qint64 rightBytes = 0;
    };

    QPointer<QTreeWidget> leftTree;
    QPointer<QTreeWidget> rightTree;
    QThreadPool pool;
    std::atomic_bool cancelled{false};
    CompareOptions options;
    std::function<void()> onProgress;
    std::function<void()> onComplete;

    // Totals shown in the pane footers (GUI thread only).
    int leftFiles = 0;
    int rightFiles = 0;
    qint64 leftBytes = 0;
    qint64 rightBytes = 0;

    ComparisonRun(QTreeWidget* left, QTreeWidget* right, CompareOptions opts)
        : leftTree(left), rightTree(right), options(std::move(opts)) {
        pool.setMaxThreadCount(5);
    }

    void start(const QString& leftPath, const QString& rightPath) {
        scheduleFolder(leftPath, rightPath, nullptr, nullptr, nullptr, false);
    }

    void scheduleFolder(QString leftPath, QString rightPath, QTreeWidgetItem* leftParent,
                        QTreeWidgetItem* rightParent, std::shared_ptr<NodeState> parent,
                        bool mismatched) {
        auto run = shared_from_this();
        auto* task = QRunnable::create([run, leftPath = std::move(leftPath),
                                        rightPath = std::move(rightPath), leftParent, rightParent,
                                        parent = std::move(parent), mismatched]() {
            if (run->cancelled.load()) {
                return;
            }
            auto entries = collectEntries(leftPath, rightPath, run->options, run->cancelled);
            if (run->cancelled.load()) {
                return;
            }
            const bool leftMissing = leftPath.isEmpty();
            const bool rightMissing = rightPath.isEmpty();
            // qApp is always a valid context, and the lambda re-checks
            // `cancelled` and the QPointers on the GUI thread, so a closed tab
            // can never be touched.
            QMetaObject::invokeMethod(
                qApp,
                [run, entries = std::move(entries), leftParent, rightParent, parent, leftMissing,
                 rightMissing, mismatched]() {
                    run->populate(entries, leftParent, rightParent, parent, leftMissing,
                                  rightMissing, mismatched);
                },
                Qt::QueuedConnection);
        });
        task->setAutoDelete(true);
        pool.start(task);
    }

    void populate(const std::vector<EntryPair>& entries, QTreeWidgetItem* leftParent,
                  QTreeWidgetItem* rightParent, const std::shared_ptr<NodeState>& parent,
                  bool leftMissing, bool rightMissing, bool mismatched) {
        if (cancelled.load() || !leftTree || !rightTree) {
            return;
        }
        auto state = std::make_shared<NodeState>();
        state->leftItem = leftParent;
        state->rightItem = rightParent;
        state->parent = parent;
        state->leftMissing = leftMissing;
        state->rightMissing = rightMissing;
        state->mismatched = mismatched;

        for (const auto& entry : entries) {
            auto* leftItem = addRow(leftTree, entry.left, entry.leftStatus, entry.pairClass, leftParent);
            auto* rightItem = addRow(rightTree, entry.right, entry.rightStatus, entry.pairClass, rightParent);

            if (entry.left.exists && !entry.left.isDir) {
                ++leftFiles;
                leftBytes += entry.left.size;
                state->leftBytes += entry.left.size;
            }
            if (entry.right.exists && !entry.right.isDir) {
                ++rightFiles;
                rightBytes += entry.right.size;
                state->rightBytes += entry.right.size;
            }

            if (entry.recurse) {
                ++state->pendingChildren;
                const bool leftReal = entry.left.exists && entry.left.isDir && !entry.left.isLink;
                const bool rightReal = entry.right.exists && entry.right.isDir && !entry.right.isLink;
                scheduleFolder(leftReal ? entry.left.path : QString(),
                               rightReal ? entry.right.path : QString(), leftItem, rightItem, state,
                               entry.left.exists && entry.right.exists &&
                                   entry.left.isDir != entry.right.isDir);
            } else {
                if (entry.pairClass != PairClass::Same) {
                    state->different = true;
                }
                if (entry.left.exists) state->leftMask |= statusBit(entry.leftStatus);
                if (entry.right.exists) state->rightMask |= statusBit(entry.rightStatus);
            }
        }
        if (onProgress) {
            onProgress();
        }
        // Only complete once every sub-folder below has completed too.
        if (state->pendingChildren == 0) {
            finishNode(state);
        }
    }

    static void finishFolderItem(QTreeWidgetItem* item, quint32 mask, qint64 bytes) {
        if (!item || !item->data(0, kIsDirRole).toBool()) {
            return;
        }
        item->setIcon(0, icons::folderIconForMask(mask));
        item->setText(2, formatSize(bytes));
    }

    void finishNode(const std::shared_ptr<NodeState>& state) {
        if (cancelled.load()) {
            return;
        }
        const bool orphan = !state->mismatched && (state->leftMissing || state->rightMissing);
        if (state->leftMissing && !state->mismatched) state->rightMask |= statusBit(RowStatus::Orphan);
        if (state->rightMissing && !state->mismatched) state->leftMask |= statusBit(RowStatus::Orphan);
        const bool different = state->different || orphan || state->mismatched;

        if (state->leftItem || state->rightItem) {
            if (!state->mismatched) {
                RowStatus status = RowStatus::Equal;
                PairClass pairClass = PairClass::Same;
                if (orphan) {
                    status = RowStatus::Orphan;
                    pairClass = state->leftMissing ? PairClass::OrphanRight : PairClass::OrphanLeft;
                } else if (state->different) {
                    status = RowStatus::Different;
                    pairClass = PairClass::Different;
                }
                for (auto* item : {state->leftItem, state->rightItem}) {
                    if (item) {
                        item->setData(0, kStatusRole, static_cast<int>(status));
                        item->setData(0, kClassRole, static_cast<int>(pairClass));
                    }
                }
            }
            finishFolderItem(state->leftItem, state->leftMask, state->leftBytes);
            finishFolderItem(state->rightItem, state->rightMask, state->rightBytes);
        }

        if (state->parent) {
            auto parent = state->parent;
            parent->different = parent->different || different;
            parent->leftMask |= state->leftMask;
            parent->rightMask |= state->rightMask;
            parent->leftBytes += state->leftBytes;
            parent->rightBytes += state->rightBytes;
            if (--parent->pendingChildren == 0) {
                finishNode(parent);
            }
        } else if (onComplete) {
            onComplete();
        }
    }
};

// A run may be released by a worker thread (its tasks hold references). Its
// QThreadPool must never be destroyed from one of its own threads, so the
// final delete is always bounced to the GUI thread.
std::shared_ptr<ComparisonRun> makeRun(QTreeWidget* left, QTreeWidget* right, CompareOptions options) {
    return std::shared_ptr<ComparisonRun>(
        new ComparisonRun(left, right, std::move(options)), [](ComparisonRun* run) {
            if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
                delete run;
            } else {
                QMetaObject::invokeMethod(qApp, [run]() { delete run; }, Qt::QueuedConnection);
            }
        });
}

// ---------------------------------------------------------------------------
// One tab = one independent folder comparison. All of its commands, options,
// filters and log live inside the tab, so nothing leaks across sessions.
// ---------------------------------------------------------------------------

class CompareSession : public QWidget {
public:
    std::function<void()> onTitleChanged;
    std::function<void(const QString&, const QString&, const QString&, const QString&)>
        onTextCompare;

    // What a right-click landed on. Copied by value: the context menu runs a
    // nested event loop and the rows may be rebuilt while it is open, so it
    // must never hold on to QTreeWidgetItem pointers.
    enum class Side { Left, Right };
    struct NodeContext {
        Side side = Side::Left;
        bool isDir = false;
        bool existsHere = false;  // false on the placeholder row of an orphan
        QString path;             // node on the clicked side (empty if missing)
        QString otherPath;        // its counterpart on the other side (may be empty)
        QString parentPath;       // folder in which create actions add entries

        QString displayPath() const { return existsHere ? path : otherPath; }
    };

    // Extension points for the "Open With" and "Explorer" submenus. Their real
    // content comes from separate libraries: assign a provider that adds
    // actions to the given submenu. While a provider is unset (or adds nothing)
    // the submenu shows a single "Dummy Item".
    std::function<void(QMenu*, const NodeContext&)> populateOpenWithMenu;
    std::function<void(QMenu*, const NodeContext&)> populateExplorerMenu;

    explicit CompareSession(QSettings* preferences = nullptr, QWidget* parent = nullptr)
        : QWidget(parent), preferences_(preferences) {
        createActions();
        buildUi();
        connectSignals();
        for (auto* action : findChildren<QAction*>()) {
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            addAction(action);
        }
    }

    ~CompareSession() override { cancelRun(); }

    // ---- menu integration (the main window shows the *current* tab's actions)
    QList<QAction*> actionsMenuItems() const {
        return {compareAction_, refreshAction_, swapAction_, nullptr, selectLeftAction_,
                selectRightAction_, nullptr, contentsAction_, timestampsAction_, nullptr,
                filterApplyAction_, filterClearAction_};
    }
    QList<QAction*> editMenuItems() const { return {editSelectionAction_}; }
    QList<QAction*> searchMenuItems() const { return {findAction_}; }
    QList<QAction*> viewMenuItems() const {
        QList<QAction*> items;
        for (auto* action : showActions_) items << action;
        items << nullptr << expandAllAction_ << collapseAllAction_ << nullptr << consoleAction_;
        return items;
    }
    QList<QAction*> toolsMenuItems() const { return {optionsAction_}; }

    // ---- state
    QString leftText() const { return pathText(leftPath_); }
    QString rightText() const { return pathText(rightPath_); }

    void setPaths(const QString& left, const QString& right) {
        setPath(leftPath_, left);
        setPath(rightPath_, right);
        updateTitle();
    }

    QString title() const {
        const QString l = folderLabel(leftText());
        const QString r = folderLabel(rightText());
        if (l.isEmpty() && r.isEmpty()) {
            return "New Folder Compare";
        }
        return (l.isEmpty() ? QString("...") : l) + " \u2194 " + (r.isEmpty() ? QString("...") : r);
    }

    QString detailedTitle() const {
        return QDir::toNativeSeparators(leftText()) + "  <->  " + QDir::toNativeSeparators(rightText());
    }

    QString instrumentationSessionId() const { return instrumentationSessionId_; }

    void copySettingsFrom(const CompareSession& other) {
        modeCombo_->setCurrentIndex(other.modeCombo_->currentIndex());
        contentsAction_->setChecked(other.contentsAction_->isChecked());
        timestampsAction_->setChecked(other.timestampsAction_->isChecked());
        filterCombo_->setEditText(other.filterCombo_->currentText());
        for (int i = 0; i < kPairClassCount; ++i) {
            showActions_[i]->setChecked(other.showActions_[i]->isChecked());
        }
    }

    void log(const QString& message) {
        console_->appendPlainText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") +
                                  "  " + message);
    }

    void cancelRun() {
        if (run_) {
            run_->cancelled.store(true);
            run_.reset();
        }
    }

    void refresh() {
        cancelRun();
        leftTree_->clear();
        rightTree_->clear();

        const QString left = leftText();
        const QString right = rightText();
        remember(leftPath_, left);
        remember(rightPath_, right);
        remember(filterCombo_, filterCombo_->currentText().trimmed());
        updateTitle();
        updateFooter();

        if (left.isEmpty() || right.isEmpty()) {
            log("Select two folders to compare");
            return;
        }
        if (!QFileInfo(left).isDir()) {
            log("Left folder not found: " + QDir::toNativeSeparators(left));
            return;
        }
        if (!QFileInfo(right).isDir()) {
            log("Right folder not found: " + QDir::toNativeSeparators(right));
            return;
        }

        leftFree_->setText(freeSpaceText(left));
        rightFree_->setText(freeSpaceText(right));

        CompareOptions options;
        options.checkContent = contentsAction_->isChecked();
        options.ignoreTimestamps = timestampsAction_->isChecked();
        options.filter = NameFilter::parse(filterCombo_->currentText());

        run_ = makeRun(leftTree_, rightTree_, std::move(options));
        const ComparisonRun* expected = run_.get();
        run_->onProgress = [this, expected]() {
            if (run_.get() == expected) updateFooter();
        };
        run_->onComplete = [this, expected]() {
            if (run_.get() != expected) return;
            updateFooter();
            applyViewFilter();
            focusPendingEdit();
            log("Folder comparison completed");
        };
        log("Load comparison: " + QDir::toNativeSeparators(left) + " <-> " +
            QDir::toNativeSeparators(right));
        log(contentsAction_->isChecked() ? "Comparing folder contents (byte-for-byte)..."
                                         : "Comparing folder structure (size and timestamp)...");
        run_->start(left, right);
    }

private:
    // ---------------------------------------------------------------- actions
    void createActions() {
        using icons::Glyph;
        compareAction_ = new QAction(icons::glyph(Glyph::Compare), "Compare", this);
        compareAction_->setToolTip("Compare the two folders again (recursive)");
        refreshAction_ = new QAction(icons::glyph(Glyph::Refresh), "Refresh", this);
        refreshAction_->setShortcut(QKeySequence(Qt::Key_F5));
        refreshAction_->setToolTip("Refresh (F5)");
        swapAction_ = new QAction(icons::glyph(Glyph::Swap), "Swap sides", this);
        swapAction_->setToolTip("Swap left and right folders");
        selectLeftAction_ = new QAction(icons::glyph(Glyph::FolderOpen), "Select left folder...", this);
        selectRightAction_ = new QAction(icons::glyph(Glyph::FolderOpen), "Select right folder...", this);

        struct Toggle {
            const char* text;
            const char* tip;
            QColor left;
            QColor right;
        };
        const Toggle toggles[kPairClassCount] = {
            {"Show identical", "Show items that are the same on both sides", color::same(), color::same()},
            {"Show left orphans", "Show items that exist only on the left", color::orphan(), QColor()},
            {"Show right orphans", "Show items that exist only on the right", QColor(), color::orphan()},
            {"Show left newer", "Show items that are newer on the left", color::newer(), color::older()},
            {"Show right newer", "Show items that are newer on the right", color::older(), color::newer()},
            {"Show different", "Show items with the same timestamp but different content", color::different(), color::different()},
        };
        for (int i = 0; i < kPairClassCount; ++i) {
            auto* action = new QAction(icons::dashIcon(toggles[i].left, toggles[i].right),
                                       toggles[i].text, this);
            action->setToolTip(toggles[i].tip);
            action->setCheckable(true);
            action->setChecked(true);
            showActions_[i] = action;
        }

        expandAllAction_ = new QAction(icons::glyph(Glyph::ExpandAll), "Expand all", this);
        expandAllAction_->setToolTip("Expand all folders");
        collapseAllAction_ = new QAction(icons::glyph(Glyph::CollapseAll), "Collapse all", this);
        collapseAllAction_->setToolTip("Collapse all folders");

        contentsAction_ = new QAction(icons::glyph(Glyph::Contents), "Compare contents", this);
        contentsAction_->setCheckable(true);
        contentsAction_->setToolTip("Compare file contents byte-for-byte instead of size + timestamp");
        timestampsAction_ = new QAction(icons::glyph(Glyph::Timestamps), "Ignore timestamps", this);
        timestampsAction_->setCheckable(true);
        timestampsAction_->setToolTip("Ignore modification times when comparing");

        filterApplyAction_ = new QAction(icons::glyph(Glyph::Filter), "Apply filter", this);
        filterApplyAction_->setToolTip("Apply the file filter");
        filterClearAction_ = new QAction(icons::glyph(Glyph::FilterClear), "Clear filter", this);
        filterClearAction_->setToolTip("Show all files");

        consoleAction_ = new QAction("Toggle console", this);
        consoleAction_->setCheckable(true);
        consoleAction_->setChecked(true);
        editSelectionAction_ = new QAction("Edit selection", this);
        findAction_ = new QAction("Find", this);
        optionsAction_ = new QAction("Comparison options", this);
    }

    // --------------------------------------------------------------------- UI
    struct Pane {
        QWidget* panel = nullptr;
        QWidget* pathBar = nullptr;
        QWidget* footer = nullptr;
        PathSelector* selector = nullptr;
        QComboBox* path = nullptr;
        QToolButton* browse = nullptr;
        QToolButton* up = nullptr;
        CompareTree* tree = nullptr;
        QLabel* count = nullptr;
        QLabel* free = nullptr;
    };

    Pane makePane(const QString& side, QWidget* parent) {
        Pane pane;
        pane.panel = new QWidget(parent);
        auto* layout = new QVBoxLayout(pane.panel);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto* pathBar = new QFrame(pane.panel);
        pathBar->setObjectName("pathBar");
        auto* pathLayout = new QHBoxLayout(pathBar);
        pathLayout->setContentsMargins(0, 1, 2, 1);
        pathLayout->setSpacing(0);
        pane.selector = new PathSelector(PathSelector::Mode::Folder, side, pathBar, /*showUp=*/true);
        pane.path = pane.selector->combo();
        pane.browse = pane.selector->browseButton();
        pane.up = pane.selector->upButton();
        pathLayout->addWidget(pane.selector, 1);

        pane.tree = new CompareTree(pane.panel);

        auto* footer = new QFrame(pane.panel);
        footer->setObjectName("footer");
        auto* footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(0, 0, 0, 0);
        footerLayout->setSpacing(0);
        pane.count = new QLabel(footer);
        pane.free = new QLabel(footer);
        auto* divider = new QFrame(footer);
        divider->setObjectName("footerDivider");
        divider->setFixedWidth(1);
        footerLayout->addWidget(pane.count, 1);
        footerLayout->addWidget(divider);
        footerLayout->addWidget(pane.free, 1);

        layout->addWidget(pathBar);
        layout->addWidget(pane.tree, 1);
        layout->addWidget(footer);
        pane.pathBar = pathBar;
        pane.footer = footer;
        return pane;
    }

    // Blank column between the two panes (like Beyond Compare's centre strip).
    // Its top row lines up with the path bars and holds the "both folders up"
    // button; the rest is empty but matches the header / body / footer of the
    // trees so the three columns read as one continuous view.
    QWidget* makeGutter(const Pane& left, QWidget* parent) {
        using icons::Glyph;
        constexpr int kGutterWidth = 40;

        auto* gutter = new QWidget(parent);
        gutter->setFixedWidth(kGutterWidth);
        auto* layout = new QVBoxLayout(gutter);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto* top = new QFrame(gutter);
        top->setObjectName("pathBar");  // same look as the path bars
        top->setFixedHeight(left.pathBar->sizeHint().height());
        auto* topLayout = new QHBoxLayout(top);
        topLayout->setContentsMargins(0, 1, 0, 1);
        topLayout->setSpacing(0);
        bothUp_ = new QToolButton(top);
        bothUp_->setIcon(icons::glyph(Glyph::FolderUp));
        bothUp_->setToolTip("Go to parent folder on both sides");
        topLayout->addStretch(1);
        topLayout->addWidget(bothUp_);
        topLayout->addStretch(1);

        auto* header = new QFrame(gutter);
        header->setObjectName("gutterHeader");
        header->setFixedHeight(left.tree->header()->sizeHint().height());

        auto* body = new QFrame(gutter);
        body->setObjectName("gutterBody");

        auto* footer = new QFrame(gutter);
        footer->setObjectName("footer");
        footer->setFixedHeight(left.footer->sizeHint().height());

        layout->addWidget(top);
        layout->addWidget(header);
        layout->addWidget(body, 1);
        layout->addWidget(footer);
        return gutter;
    }

    void buildUi() {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        // Toolbar directly under the tab strip: everything here acts on THIS tab.
        toolbar_ = new QToolBar(this);
        toolbar_->setMovable(false);
        toolbar_->setFloatable(false);
        toolbar_->setIconSize(QSize(20, 20));
        toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolbar_->addAction(compareAction_);
        toolbar_->addAction(refreshAction_);
        toolbar_->addAction(swapAction_);
        toolbar_->addSeparator();
        for (auto* action : showActions_) toolbar_->addAction(action);
        toolbar_->addSeparator();
        toolbar_->addAction(expandAllAction_);
        toolbar_->addAction(collapseAllAction_);
        toolbar_->addSeparator();
        toolbar_->addAction(contentsAction_);
        toolbar_->addAction(timestampsAction_);
        toolbar_->addSeparator();
        toolbar_->addWidget(new QLabel("Compare:", toolbar_));
        modeCombo_ = new QComboBox(toolbar_);
        modeCombo_->addItems({"Folder structure", "Folder contents", "Text compare", "Binary compare"});
        modeCombo_->setMinimumWidth(150);
        toolbar_->addWidget(modeCombo_);
        toolbar_->addSeparator();
        toolbar_->addWidget(new QLabel("Filters:", toolbar_));
        filterCombo_ = new QComboBox(toolbar_);
        filterCombo_->setEditable(true);
        filterCombo_->setInsertPolicy(QComboBox::NoInsert);
        filterCombo_->setMinimumWidth(190);
        filterCombo_->addItem("*.*");
        filterCombo_->setToolTip("File filter, e.g. *.cpp;*.h  or just part of a name.\n"
                                 "Folders are always shown.");
        toolbar_->addWidget(filterCombo_);
        toolbar_->addAction(filterApplyAction_);
        toolbar_->addAction(filterClearAction_);
        root->addWidget(toolbar_);

        Pane left = makePane("Left", nullptr);
        Pane right = makePane("Right", nullptr);
        leftSelector_ = left.selector;
        rightSelector_ = right.selector;
        leftPath_ = left.path;
        rightPath_ = right.path;
        leftBrowse_ = left.browse;
        rightBrowse_ = right.browse;
        leftUp_ = left.up;
        rightUp_ = right.up;

        // Browsing or going up inside the path row itself should behave
        // exactly like it always did: refresh the compare, and (browse
        // only, matching the old chooseFolder()) log which folder was
        // picked.
        leftSelector_->onBrowsed = [this](const QString& path) {
            refresh();
            log("left resource selected: " + path);
        };
        rightSelector_->onBrowsed = [this](const QString& path) {
            refresh();
            log("right resource selected: " + path);
        };
        leftSelector_->onWentUp = [this](const QString&) { refresh(); };
        rightSelector_->onWentUp = [this](const QString&) { refresh(); };
        leftTree_ = left.tree;
        rightTree_ = right.tree;
        leftCount_ = left.count;
        rightCount_ = right.count;
        leftFree_ = left.free;
        rightFree_ = right.free;
        configureColumnVisibility();

        auto* panes = new QSplitter(Qt::Horizontal, this);
        panes->setChildrenCollapsible(false);
        panes->addWidget(left.panel);
        panes->addWidget(makeGutter(left, panes));
        panes->addWidget(right.panel);
        panes->setStretchFactor(0, 1);
        panes->setStretchFactor(1, 0);
        panes->setStretchFactor(2, 1);
        panes->setSizes({550, 40, 550});
        panes->handle(2)->setEnabled(false);  // one draggable divider is enough

        console_ = new QPlainTextEdit(this);
        console_->setObjectName("console");
        console_->setReadOnly(true);
        console_->setMaximumBlockCount(500);
        console_->setPlaceholderText("Activity log");
        console_->setMinimumHeight(60);

        splitter_ = new QSplitter(Qt::Vertical, this);
        splitter_->setChildrenCollapsible(false);
        splitter_->addWidget(panes);
        splitter_->addWidget(console_);
        splitter_->setStretchFactor(0, 1);
        splitter_->setStretchFactor(1, 0);
        splitter_->setSizes({640, 130});
        root->addWidget(splitter_, 1);
    }

    // ---------------------------------------------------------------- signals
    void connectSignals() {
        using QAct = QAction;
        connect(compareAction_, &QAct::triggered, this, [this]() {
            log("Comparing folder contents recursively");
            refresh();
        });
        connect(refreshAction_, &QAct::triggered, this, [this]() { refresh(); });
        connect(swapAction_, &QAct::triggered, this, [this]() {
            const QString oldLeft = leftText();
            setPath(leftPath_, rightText());
            setPath(rightPath_, oldLeft);
            refresh();
            log("Left and right resources swapped");
        });
        connect(selectLeftAction_, &QAct::triggered, this,
                [this]() { chooseFolder(leftSelector_, "left"); });
        connect(selectRightAction_, &QAct::triggered, this,
                [this]() { chooseFolder(rightSelector_, "right"); });
        connect(bothUp_, &QToolButton::clicked, this, [this]() { goUpBoth(); });
        connect(leftPath_->lineEdit(), &QLineEdit::returnPressed, this, [this]() { refresh(); });
        connect(rightPath_->lineEdit(), &QLineEdit::returnPressed, this, [this]() { refresh(); });
        connect(leftPath_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { refresh(); });
        connect(rightPath_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { refresh(); });

        // Compare mode <-> "compare contents" toggle stay in sync (per tab).
        connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            const QSignalBlocker blocker(contentsAction_);
            contentsAction_->setChecked(index != 0);
            log("Compare mode: " + modeCombo_->itemText(index));
            refresh();
        });
        connect(contentsAction_, &QAct::toggled, this, [this](bool checked) {
            const QSignalBlocker blocker(modeCombo_);
            if (checked && modeCombo_->currentIndex() == 0) {
                modeCombo_->setCurrentIndex(1);
            } else if (!checked) {
                modeCombo_->setCurrentIndex(0);
            }
            refresh();
        });
        connect(timestampsAction_, &QAct::toggled, this, [this](bool checked) {
            log(checked ? "Ignoring timestamps" : "Timestamps are compared");
            refresh();
        });

        connect(filterApplyAction_, &QAct::triggered, this, [this]() {
            log("Applied filter: " + filterCombo_->currentText());
            refresh();
        });
        connect(filterClearAction_, &QAct::triggered, this, [this]() {
            filterCombo_->setEditText("*.*");
            log("Filter cleared");
            refresh();
        });
        connect(filterCombo_->lineEdit(), &QLineEdit::returnPressed, this, [this]() { refresh(); });
        connect(filterCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int) { refresh(); });

        for (auto* action : showActions_) {
            connect(action, &QAct::toggled, this, [this](bool) { applyViewFilter(); });
        }
        connect(expandAllAction_, &QAct::triggered, this, [this]() {
            leftTree_->expandAll();
            rightTree_->expandAll();
        });
        connect(collapseAllAction_, &QAct::triggered, this, [this]() {
            leftTree_->collapseAll();
            rightTree_->collapseAll();
        });
        connect(consoleAction_, &QAct::toggled, this, [this](bool visible) { console_->setVisible(visible); });
        connect(editSelectionAction_, &QAct::triggered, this, [this]() {
            if (auto* tree = activeTree()) {
                renameSelected(tree);
            }
        });
        connect(findAction_, &QAct::triggered, this, [this]() { log("Find requested in current folder level"); });
        connect(optionsAction_, &QAct::triggered, this, [this]() { log("Comparison options opened"); });

        connectTreePair(leftTree_, rightTree_, "left");
        connectTreePair(rightTree_, leftTree_, "right");
        connectScrollSync();

        for (CompareTree* tree : {leftTree_, rightTree_}) {
            tree->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(tree, &QWidget::customContextMenuRequested, this,
                    [this, tree](const QPoint& pos) { showNodeMenu(tree, pos); });
            connect(tree, &QTreeWidget::itemChanged, this,
                [this, tree](QTreeWidgetItem* item, int column) {
                handleItemRenamed(tree, item, column);
                });
            connect(tree, &QTreeWidget::itemDoubleClicked, this,
                    [this, tree](QTreeWidgetItem* item, int) { openTextCompare(tree, item); });
        }
    }

    void openTextCompare(QTreeWidget* tree, QTreeWidgetItem* item) {
        if (!item || item->data(0, kIsDirRole).toBool()) {
            return;
        }
        const bool isLeft = tree == leftTree_;
        auto* otherTree = isLeft ? rightTree_ : leftTree_;
        auto* counterpart = pairedItem(item, otherTree);
        const QString leftPath = isLeft ? item->data(0, kPathRole).toString()
                                        : counterpart ? counterpart->data(0, kPathRole).toString() : QString();
        const QString rightPath = isLeft ? counterpart ? counterpart->data(0, kPathRole).toString() : QString()
                                         : item->data(0, kPathRole).toString();
        if (leftPath.isEmpty() && rightPath.isEmpty()) {
            return;
        }

        QPointer<CompareSession> self(this);
        auto* task = QRunnable::create([self, leftPath, rightPath]() {
            QFile leftFile(leftPath);
            QFile rightFile(rightPath);
            QString leftText;
            QString rightText;
            QString error;
            if (!leftPath.isEmpty() && !leftFile.open(QIODevice::ReadOnly)) {
                error = "Could not open left file: " + leftPath;
            } else if (!rightPath.isEmpty() && !rightFile.open(QIODevice::ReadOnly)) {
                error = "Could not open right file: " + rightPath;
            } else {
                if (!leftPath.isEmpty()) leftText = QString::fromUtf8(leftFile.readAll());
                if (!rightPath.isEmpty()) rightText = QString::fromUtf8(rightFile.readAll());
            }
            QMetaObject::invokeMethod(
                qApp,
                [self, leftPath, rightPath, leftText = std::move(leftText),
                 rightText = std::move(rightText), error = std::move(error)]() {
                    if (!self) {
                        return;
                    }
                    if (!error.isEmpty()) {
                        self->log(error);
                        return;
                    }
                    if (self->onTextCompare) {
                        self->onTextCompare(leftPath, rightPath, leftText, rightText);
                    }
                },
                Qt::QueuedConnection);
        });
        task->setAutoDelete(true);
        QThreadPool::globalInstance()->start(task);
        log("Opening text comparison: " + QDir::toNativeSeparators(leftPath) + " <-> " +
            QDir::toNativeSeparators(rightPath));
    }

    // Expanding / collapsing / selecting a row on one side mirrors on the other.
    void connectTreePair(QTreeWidget* tree, QTreeWidget* other, const QString& side) {
        connect(tree, &QTreeWidget::itemExpanded, this, [this, other, side](QTreeWidgetItem* item) {
            if (auto* counterpart = pairedItem(item, other)) {
                const QSignalBlocker blocker(other);
                counterpart->setExpanded(true);
            }
            if (item->data(0, kIsDirRole).toBool()) {
                log("Expanded " + side + " folder: " + item->data(0, kPathRole).toString());
            }
        });
        connect(tree, &QTreeWidget::itemCollapsed, this, [other](QTreeWidgetItem* item) {
            if (auto* counterpart = pairedItem(item, other)) {
                const QSignalBlocker blocker(other);
                counterpart->setExpanded(false);
            }
        });
        connect(tree, &QTreeWidget::currentItemChanged, this,
                [other](QTreeWidgetItem* current, QTreeWidgetItem*) {
                    if (auto* counterpart = pairedItem(current, other)) {
                        const QSignalBlocker blocker(other);
                        other->setCurrentItem(counterpart);
                    }
                });
    }

    static QStringList folderColumnNames() {
        return {"name", "extension", "size", "modified", "attributes"};
    }

    static QStringList folderColumnLabels() {
        return {"Name", "Ext", "Size", "Modified", "Attributes"};
    }

    void configureColumnVisibility() {
        for (CompareTree* tree : {leftTree_, rightTree_}) {
            tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(tree->header(), &QWidget::customContextMenuRequested, this,
                    [this, tree](const QPoint& pos) { showColumnMenu(tree, pos); });
        }

        const QStringList names = folderColumnNames();
        for (int column = 0; column < names.size(); ++column) {
            const bool visible = !preferences_ ||
                                 preferences_->value("folder/columns/" + names[column], true).toBool();
            setColumnVisible(column, visible, false);
        }
    }

    void setColumnVisible(int column, bool visible, bool persist) {
        const int visibleCount = leftTree_->header()->count() - leftTree_->header()->hiddenSectionCount();
        if (!visible && visibleCount == 1) {
            return;
        }
        leftTree_->setColumnHidden(column, !visible);
        rightTree_->setColumnHidden(column, !visible);
        if (persist && preferences_) {
            const QStringList names = folderColumnNames();
            preferences_->setValue("folder/columns/" + names[column], visible);
        }
    }

    void showColumnMenu(CompareTree* tree, const QPoint& pos) {
        QMenu menu(tree->header());
        const QStringList labels = folderColumnLabels();
        const int visibleCount = tree->header()->count() - tree->header()->hiddenSectionCount();
        for (int column = 0; column < labels.size(); ++column) {
            auto* action = menu.addAction(labels[column]);
            action->setCheckable(true);
            action->setChecked(!tree->isColumnHidden(column));
            action->setEnabled(action->isChecked() || visibleCount > 1);
            connect(action, &QAction::triggered, this,
                    [this, column](bool visible) { setColumnVisible(column, visible, true); });
        }
        menu.exec(tree->header()->viewport()->mapToGlobal(pos));
    }

    // ------------------------------------------------- node context menu
    // Right-click on a file or folder row. Every entry only logs for now; the
    // layout follows Beyond Compare (folders get a few extra entries, files get
    // "File Compare Report...").
    void showNodeMenu(CompareTree* tree, const QPoint& pos) {
        QTreeWidgetItem* item = tree->itemAt(pos);
        const bool isLeft = tree == leftTree_;
        QTreeWidgetItem* counterpart = item ? pairedItem(item, isLeft ? rightTree_ : leftTree_) : nullptr;

        NodeContext ctx;
        ctx.side = isLeft ? Side::Left : Side::Right;
        if (item) {
            ctx.path = item->data(0, kPathRole).toString();
            ctx.existsHere = !ctx.path.isEmpty();
            ctx.otherPath = counterpart ? counterpart->data(0, kPathRole).toString() : QString();
            // A placeholder row has no data of its own; its counterpart knows the kind.
            ctx.isDir = item->data(0, kIsDirRole).toBool() ||
                        (counterpart && counterpart->data(0, kIsDirRole).toBool());
            ctx.parentPath = item->parent()
                                 ? item->parent()->data(0, kPathRole).toString()
                                 : pathText(isLeft ? leftPath_ : rightPath_);
            tree->setCurrentItem(item);  // right-click selects the row, like a left-click
        } else {
            ctx.isDir = true;
            ctx.parentPath = pathText(isLeft ? leftPath_ : rightPath_);
        }

        QMenu menu(tree);
        buildNodeMenu(menu, ctx);
        menu.exec(tree->viewport()->mapToGlobal(pos));
    }

    CompareTree* treeFor(Side side) const { return side == Side::Left ? leftTree_ : rightTree_; }

    CompareTree* activeTree() const {
        if (leftTree_->hasFocus()) return leftTree_;
        if (rightTree_->hasFocus()) return rightTree_;
        return leftTree_->currentItem() ? leftTree_ : rightTree_;
    }

    void renameSelected(CompareTree* tree) {
        if (!tree) return;
        auto* item = tree->currentItem();
        if (!item || item->data(0, kPathRole).toString().isEmpty()) return;
        tree->scrollToItem(item);
        tree->editItem(item, 0);
    }

    static QTreeWidgetItem* findPath(QTreeWidget* tree, const QString& path) {
        if (!tree || path.isEmpty()) return nullptr;
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
            if (item->data(0, kPathRole).toString() == path) return item;
            for (int i = 0; i < item->childCount(); ++i) {
                if (auto* found = visit(item->child(i))) return found;
            }
            return static_cast<QTreeWidgetItem*>(nullptr);
        };
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (auto* found = visit(tree->topLevelItem(i))) return found;
        }
        return nullptr;
    }

    void focusPendingEdit() {
        if (pendingEditPath_.isEmpty() || !pendingEditTree_) return;
        auto* item = findPath(pendingEditTree_, pendingEditPath_);
        const QString path = pendingEditPath_;
        pendingEditPath_.clear();
        pendingEditTree_->clearFocus();
        if (!item) return;
        pendingEditTree_->setCurrentItem(item);
        pendingEditTree_->scrollToItem(item);
        QTimer::singleShot(0, this, [this, item]() {
            if (item->treeWidget()) item->treeWidget()->editItem(item, 0);
        });
        log("Created " + QDir::toNativeSeparators(path));
    }

    void handleItemRenamed(CompareTree* tree, QTreeWidgetItem* item, int column) {
        if (column != 0 || !item) return;
        const QString oldPath = item->data(0, kPathRole).toString();
        const QString newName = item->text(0).trimmed();
        if (oldPath.isEmpty() || newName.isEmpty() || QFileInfo(oldPath).fileName() == newName) {
            if (!oldPath.isEmpty() && item->text(0) != QFileInfo(oldPath).fileName()) {
                const QSignalBlocker blocker(tree);
                item->setText(0, QFileInfo(oldPath).fileName());
            }
            return;
        }
        const QString newPath = QFileInfo(oldPath).absolutePath() + QDir::separator() + newName;
        if (!QFileInfo::exists(newPath) && QFile::rename(oldPath, newPath)) {
            refresh();
            log("Renamed " + QDir::toNativeSeparators(oldPath) + " to " +
                QDir::toNativeSeparators(newPath));
            return;
        }
        const QSignalBlocker blocker(tree);
        item->setText(0, QFileInfo(oldPath).fileName());
        QMessageBox::warning(this, "Rename failed", "Could not rename " + oldPath);
    }

    void createEntry(const NodeContext& ctx, bool directory) {
        const QString parent = QDir::cleanPath(ctx.parentPath);
        if (parent.isEmpty() || !QFileInfo(parent).isDir()) return;
        const QString base = directory ? "New Folder" : "New File";
        QString name = base;
        int suffix = 2;
        while (QFileInfo::exists(QDir(parent).filePath(name))) {
            name = base + " " + QString::number(suffix++);
        }
        const QString path = QDir(parent).filePath(name);
        const bool created = directory ? QDir().mkpath(path) : [&]() {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            file.close();
            return true;
        }();
        if (!created) {
            QMessageBox::warning(this, "Create failed", "Could not create " + path);
            return;
        }
        pendingEditTree_ = treeFor(ctx.side);
        pendingEditPath_ = path;
        refresh();
    }

    void buildNodeMenu(QMenu& menu, const NodeContext& ctx) {
        namespace mi = icons::menuicons;
        const bool toRight = ctx.side == Side::Left;  // "the other side"
        const QString other = toRight ? "Right" : "Left";

        // ---- open / navigate
        QAction* open = addNodeAction(&menu, ctx, ctx.isDir ? "Open Folder" : "Open");
        QFont defaultFont = open->font();
        defaultFont.setBold(true);  // the default (double-click) action
        open->setFont(defaultFont);
        if (ctx.isDir) {
            addNodeAction(&menu, ctx, "Open Subfolders");
            addNodeAction(&menu, ctx, "Close Subfolders");
            addNodeAction(&menu, ctx, "Set as Base Folder");
            addNodeAction(&menu, ctx, "Set as Base on Other Side");
            addNodeAction(&menu, ctx, "Open in New View");
        }
        QMenu* openWith = menu.addMenu("Open With");
        openWith->menuAction()->setEnabled(ctx.existsHere);
        fillProviderMenu(openWith, ctx, populateOpenWithMenu);
        addNodeAction(&menu, ctx, "Compare To...", {}, "F7");
        addNodeAction(&menu, ctx, "Align With...", {}, "F6");
        menu.addSeparator();

        // ---- operate on the node
        addNodeAction(&menu, ctx, "Compare Contents...", mi::compareContents());
        addNodeAction(&menu, ctx, "Copy to " + other + "...", mi::copyArrow(toRight),
                      toRight ? "Ctrl+R" : "Ctrl+L");
        addNodeAction(&menu, ctx, "Move to " + other + "...", mi::moveArrow(toRight));
        addNodeAction(&menu, ctx, "Copy to Folder...", mi::copyToFolder());
        addNodeAction(&menu, ctx, "Move to Folder...", mi::moveToFolder());
        addNodeAction(&menu, ctx, "Delete...", mi::remove());
        QAction* rename = addNodeAction(&menu, ctx, "Rename", mi::rename(), "F2");
        connect(rename, &QAction::triggered, this, [this, ctx]() {
            if (auto* tree = treeFor(ctx.side)) {
                renameSelected(tree);
            }
        });
        addNodeAction(&menu, ctx, "Attributes...");
        addNodeAction(&menu, ctx, "Touch...", mi::touch());
        addNodeAction(&menu, ctx, ctx.isDir ? "Exclude" : "Exclude...");
        addNodeAction(&menu, ctx, "Copy Filename");

        QMenu* create = menu.addMenu("Create");
        QAction* newFolder = addNodeAction(create, ctx, "New Folder", mi::newFolder(), {}, false);
        QAction* newFile = addNodeAction(create, ctx, "New File", {}, {}, false);
        create->menuAction()->setEnabled(!ctx.parentPath.isEmpty());
        connect(newFolder, &QAction::triggered, this,
            [this, ctx]() { createEntry(ctx, true); });
        connect(newFile, &QAction::triggered, this,
            [this, ctx]() { createEntry(ctx, false); });

        // "Ignored" shows a green tick while the node is ignored. The state is
        // only remembered for the session so far; it does not affect the compare.
        const bool ignored = ignoredPaths_.contains(ctx.path);
        QAction* ignoredAction =
            addNodeAction(&menu, ctx, "Ignored", ignored ? mi::check() : QIcon());
        connect(ignoredAction, &QAction::triggered, this, [this, ctx]() {
            const bool nowIgnored = !ignoredPaths_.remove(ctx.path);
            if (nowIgnored) {
                ignoredPaths_.insert(ctx.path);
            }
            log(QString("Ignored is now %1: %2")
                    .arg(nowIgnored ? "on" : "off", QDir::toNativeSeparators(ctx.path)));
        });

        addNodeAction(&menu, ctx, "Refresh Selection", {}, "Shift+F5", /*needsNode=*/false);
        if (!ctx.isDir) {
            addNodeAction(&menu, ctx, "File Compare Report...", mi::report());
        }
        menu.addSeparator();

        // ---- synchronize (acts on the whole comparison, not on this node)
        QMenu* sync = menu.addMenu("Synchronize");
        addNodeAction(sync, ctx, "Update Right...", mi::update(mi::SyncDir::Right), {}, false);
        addNodeAction(sync, ctx, "Update Left...", mi::update(mi::SyncDir::Left), {}, false);
        addNodeAction(sync, ctx, "Update Both...", mi::update(mi::SyncDir::Both), {}, false);
        addNodeAction(sync, ctx, "Mirror to Right...", mi::mirror(mi::SyncDir::Right),
                      "Shift+Ctrl+R", false);
        addNodeAction(sync, ctx, "Mirror to Left...", mi::mirror(mi::SyncDir::Left),
                      "Shift+Ctrl+L", false);
        menu.addSeparator();

        QMenu* explorer = menu.addMenu("Explorer");
        explorer->menuAction()->setEnabled(ctx.existsHere);
        fillProviderMenu(explorer, ctx, populateExplorerMenu);
    }

    // Adds one entry that logs when activated. Entries that work on the node
    // itself (`needsNode`) are greyed out on the empty side of an orphan row.
    // Inside a submenu the log line is prefixed with the submenu's title.
    QAction* addNodeAction(QMenu* menu, const NodeContext& ctx, const QString& text,
                           const QIcon& icon = QIcon(), const QString& shortcut = QString(),
                           bool needsNode = true) {
        QAction* action = menu->addAction(icon, text);
        if (!shortcut.isEmpty()) {
            // Shown right-aligned; the action is not attached to a widget, so it
            // does not become a global shortcut.
            action->setShortcut(QKeySequence(shortcut));
            action->setShortcutVisibleInContextMenu(true);
        }
        action->setEnabled(ctx.existsHere || !needsNode);
        const QString label = menu->title().isEmpty() ? text : menu->title() + " > " + text;
        connect(action, &QAction::triggered, this, [this, ctx, label]() { logMenuAction(ctx, label); });
        return action;
    }

    // Lets a provider fill a submenu, or falls back to a single dummy entry.
    void fillProviderMenu(QMenu* submenu, const NodeContext& ctx,
                          const std::function<void(QMenu*, const NodeContext&)>& provider) {
        if (provider) {
            provider(submenu, ctx);
        }
        if (submenu->isEmpty()) {
            addNodeAction(submenu, ctx, "Dummy Item", QIcon(), QString(), /*needsNode=*/false);
        }
    }

    void logMenuAction(const NodeContext& ctx, const QString& label) {
        const QString side = ctx.side == Side::Left ? "left" : "right";
        const QString kind = ctx.isDir ? "folder" : "file";
        log(QString("Context menu \"%1\" on %2 %3: %4")
                .arg(label, side, kind, QDir::toNativeSeparators(ctx.displayPath())));
    }

    void syncHorizontal(QScrollBar* source, QScrollBar* target) {
        if (syncingScroll_ || !source || !target) {
            return;
        }
        syncingScroll_ = true;
        const int sourceRange = source->maximum() - source->minimum();
        const int targetRange = target->maximum() - target->minimum();
        const int sourceOffset = source->value() - source->minimum();
        const int targetValue =
            sourceRange > 0
                ? target->minimum() + (sourceOffset * targetRange + sourceRange / 2) / sourceRange
                : target->minimum();
        target->setValue(targetValue);
        syncingScroll_ = false;
    }

    void syncVertical(QTreeWidget* source, QTreeWidget* target) {
        if (syncingScroll_ || !source || !target) {
            return;
        }
        auto* sourceItem = source->itemAt(1, 1);
        auto* targetItem = pairedItem(sourceItem, target);
        if (!targetItem) {
            return;
        }
        syncingScroll_ = true;
        const int sourceTop = source->visualItemRect(sourceItem).top();
        target->scrollToItem(targetItem, QAbstractItemView::PositionAtTop);
        const int targetTop = target->visualItemRect(targetItem).top();
        target->verticalScrollBar()->setValue(target->verticalScrollBar()->value() + targetTop - sourceTop);
        syncingScroll_ = false;
    }

    void connectScrollSync() {
        auto* lv = leftTree_->verticalScrollBar();
        auto* rv = rightTree_->verticalScrollBar();
        auto* lh = leftTree_->horizontalScrollBar();
        auto* rh = rightTree_->horizontalScrollBar();
        connect(lv, &QScrollBar::valueChanged, this, [this](int) { syncVertical(leftTree_, rightTree_); });
        connect(rv, &QScrollBar::valueChanged, this, [this](int) { syncVertical(rightTree_, leftTree_); });
        connect(lv, &QScrollBar::rangeChanged, this, [this](int, int) { syncVertical(leftTree_, rightTree_); });
        connect(rv, &QScrollBar::rangeChanged, this, [this](int, int) { syncVertical(rightTree_, leftTree_); });
        connect(lh, &QScrollBar::valueChanged, this, [this, lh, rh](int) { syncHorizontal(lh, rh); });
        connect(rh, &QScrollBar::valueChanged, this, [this, lh, rh](int) { syncHorizontal(rh, lh); });
        connect(lh, &QScrollBar::rangeChanged, this, [this, lh, rh](int, int) { syncHorizontal(lh, rh); });
        connect(rh, &QScrollBar::rangeChanged, this, [this, lh, rh](int, int) { syncHorizontal(rh, lh); });
    }

    // ---------------------------------------------------------------- helpers
    static QString pathText(const QComboBox* combo) { return combo->currentText().trimmed(); }

    static QString folderLabel(const QString& path) {
        if (path.isEmpty()) {
            return {};
        }
        const QString name = QFileInfo(QDir::cleanPath(path)).fileName();
        return name.isEmpty() ? QDir::toNativeSeparators(path) : name;
    }

    static void remember(QComboBox* combo, const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        const QSignalBlocker blocker(combo);
        const int existing = combo->findText(text);
        if (existing >= 0) {
            combo->removeItem(existing);
        }
        combo->insertItem(0, text);
        combo->setCurrentIndex(0);
    }

    static void setPath(QComboBox* combo, const QString& text) { combo->setEditText(text); }

    // Used by the "Select Left/Right folder..." menu actions, which need
    // the same picking dialog as the path row's own Browse button but are
    // triggered from the menu bar rather than a click on that button.
    void chooseFolder(PathSelector* selector, const QString& side) {
        const QString selected = selector->pickPath();
        if (!selected.isEmpty()) {
            selector->setText(selected);
            refresh();
            log(side + " resource selected: " + selected);
        }
    }

    // Move BOTH sides to their parent folder with a single refresh.
    void goUpBoth() {
        bool moved = false;
        for (QComboBox* combo : {leftPath_, rightPath_}) {
            QDir dir(pathText(combo));
            if (!pathText(combo).isEmpty() && dir.cdUp()) {
                setPath(combo, dir.absolutePath());
                moved = true;
            }
        }
        if (moved) {
            refresh();
            log("Both folders moved up to their parent folders");
        }
    }

    void updateTitle() {
        setToolTip(detailedTitle());
        if (onTitleChanged) {
            onTitleChanged();
        }
    }

    void updateFooter() {
        const int lf = run_ ? run_->leftFiles : 0;
        const int rf = run_ ? run_->rightFiles : 0;
        const qint64 lb = run_ ? run_->leftBytes : 0;
        const qint64 rb = run_ ? run_->rightBytes : 0;
        leftCount_->setText(QString("%1 file(s), %2").arg(lf).arg(formatBytes(lb)));
        rightCount_->setText(QString("%1 file(s), %2").arg(rf).arg(formatBytes(rb)));
        if (!run_) {
            leftFree_->clear();
            rightFree_->clear();
        }
    }

    // Hide rows whose kind is switched off in the toolbar. A folder stays
    // visible while any row below it is visible.
    bool filterRows(QTreeWidgetItem* left, QTreeWidgetItem* right) {
        bool visible = false;
        if (left->childCount() > 0) {
            for (int i = 0; i < left->childCount() && i < right->childCount(); ++i) {
                visible = filterRows(left->child(i), right->child(i)) || visible;
            }
        } else {
            const int cls = left->data(0, kClassRole).toInt();
            visible = cls < 0 || cls >= kPairClassCount || showActions_[cls]->isChecked();
        }
        left->setHidden(!visible);
        right->setHidden(!visible);
        return visible;
    }

    void applyViewFilter() {
        const int count = std::min(leftTree_->topLevelItemCount(), rightTree_->topLevelItemCount());
        for (int i = 0; i < count; ++i) {
            filterRows(leftTree_->topLevelItem(i), rightTree_->topLevelItem(i));
        }
    }

    // ---------------------------------------------------------------- members
    QToolBar* toolbar_ = nullptr;
    PathSelector* leftSelector_ = nullptr;
    PathSelector* rightSelector_ = nullptr;
    QComboBox* leftPath_ = nullptr;
    QComboBox* rightPath_ = nullptr;
    QToolButton* leftBrowse_ = nullptr;
    QToolButton* rightBrowse_ = nullptr;
    QToolButton* leftUp_ = nullptr;
    QToolButton* rightUp_ = nullptr;
    QToolButton* bothUp_ = nullptr;
    CompareTree* leftTree_ = nullptr;
    CompareTree* rightTree_ = nullptr;
    QLabel* leftCount_ = nullptr;
    QLabel* rightCount_ = nullptr;
    QLabel* leftFree_ = nullptr;
    QLabel* rightFree_ = nullptr;
    QComboBox* modeCombo_ = nullptr;
    QComboBox* filterCombo_ = nullptr;
    QPlainTextEdit* console_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QSettings* preferences_ = nullptr;
    QString instrumentationSessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QAction* compareAction_ = nullptr;
    QAction* refreshAction_ = nullptr;
    QAction* swapAction_ = nullptr;
    QAction* selectLeftAction_ = nullptr;
    QAction* selectRightAction_ = nullptr;
    QAction* showActions_[kPairClassCount] = {};
    QAction* expandAllAction_ = nullptr;
    QAction* collapseAllAction_ = nullptr;
    QAction* contentsAction_ = nullptr;
    QAction* timestampsAction_ = nullptr;
    QAction* filterApplyAction_ = nullptr;
    QAction* filterClearAction_ = nullptr;
    QAction* consoleAction_ = nullptr;
    QAction* editSelectionAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* optionsAction_ = nullptr;

    std::shared_ptr<ComparisonRun> run_;
    bool syncingScroll_ = false;
    QSet<QString> ignoredPaths_;  // paths marked "Ignored" in the context menu
    QPointer<CompareTree> pendingEditTree_;
    QString pendingEditPath_;
};

}  // namespace openbc::app
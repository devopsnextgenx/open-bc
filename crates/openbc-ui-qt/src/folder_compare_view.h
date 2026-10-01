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
#include <QSet>
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
#include "remote_profile_store.h"
#include "remote_vfs_bridge.h"

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
        bool pendingFolders = false;
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
    std::function<void(const QString&)> onError;
    RemoteProfile leftProfile;
    RemoteProfile rightProfile;
    bool leftRemote = false;
    bool rightRemote = false;
    QSet<QString> ignoredPaths;

    // Totals shown in the pane footers (GUI thread only).
    int leftFiles = 0;
    int rightFiles = 0;
    qint64 leftBytes = 0;
    qint64 rightBytes = 0;

    ComparisonRun(QTreeWidget* left, QTreeWidget* right, CompareOptions opts)
        : leftTree(left), rightTree(right), options(std::move(opts)) {
        pool.setMaxThreadCount(5);
    }

    void start(const QString& leftPath, const QString& rightPath,
               RemoteProfile leftConnection, bool hasLeftConnection,
               RemoteProfile rightConnection, bool hasRightConnection) {
        leftProfile = std::move(leftConnection);
        rightProfile = std::move(rightConnection);
        leftRemote = hasLeftConnection;
        rightRemote = hasRightConnection;
        scheduleFolder(leftPath, rightPath, nullptr, nullptr, nullptr, false);
    }

    void scheduleFolder(QString leftPath, QString rightPath, QTreeWidgetItem* leftParent,
                        QTreeWidgetItem* rightParent, std::shared_ptr<NodeState> parent,
                        bool mismatched) {
        auto run = shared_from_this();
        auto* task = QRunnable::create([run, leftPath = std::move(leftPath),
                                        rightPath = std::move(rightPath), leftParent, rightParent,
                                        parent = std::move(parent), mismatched,
                                        leftProfile = run->leftProfile, leftRemote = run->leftRemote,
                                        rightProfile = run->rightProfile, rightRemote = run->rightRemote]() {
            if (run->cancelled.load()) {
                return;
            }
            QString error;
            auto entries = collectEntriesFromEngine(leftPath, leftProfile, leftRemote, rightPath,
                                                    rightProfile, rightRemote, run->options,
                                                    run->cancelled, &error);
            if (run->cancelled.load()) {
                return;
            }
            if (!error.isEmpty()) {
                QMetaObject::invokeMethod(
                    qApp, [run, error, leftParent, rightParent, parent, mismatched,
                           leftMissing = leftPath.isEmpty(), rightMissing = rightPath.isEmpty()]() {
                        if (run->cancelled.load()) return;
                        run->populate({}, leftParent, rightParent, parent, leftMissing,
                                      rightMissing, mismatched, error);
                        if (run->onError) run->onError(error);
                    }, Qt::QueuedConnection);
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

    static QString childPath(const QString& parent, const QString& name) {
        if (parent.contains("://")) {
            return parent.endsWith('/') ? parent + name : parent + '/' + name;
        }
        return QDir(parent).filePath(name);
    }

    static SideInfo makeEngineSide(const VfsCompareSide& source, const QString& name,
                                   const QString& parent) {
        SideInfo side;
        if (!source.exists) return side;
        side.exists = true;
        side.isDir = source.isDir;
        side.isLink = source.isLink;
        side.name = name;
        side.path = childPath(parent, name);
        side.ext = side.isDir ? QString() : QFileInfo(name).suffix();
        side.size = source.size;
        side.modified = source.modified;
        if (side.isDir) side.attrs = "D";
        return side;
    }

    static std::vector<EntryPair> collectEntriesFromEngine(
        const QString& leftPath, const RemoteProfile& leftProfile, bool leftRemote,
        const QString& rightPath, const RemoteProfile& rightProfile, bool rightRemote,
        const CompareOptions& options, const std::atomic_bool& cancelled, QString* error) {
        QList<VfsCompareEntry> rows;
        if (!sharedVfsBridge().compareFolderLevel(
            leftPath, leftProfile, leftRemote, rightPath, rightProfile, rightRemote,
            false, options.ignoreTimestamps, &rows, error)) {
            return {};
        }
        std::vector<EntryPair> entries;
        entries.reserve(static_cast<size_t>(rows.size()));
        for (const VfsCompareEntry& row : rows) {
            if (cancelled.load()) break;
            EntryPair entry;
            entry.left = makeEngineSide(row.left, row.name, leftPath);
            entry.right = makeEngineSide(row.right, row.name, rightPath);
            if ((!entry.left.isDir && entry.left.exists && !options.filter.matches(row.name)) ||
                (!entry.right.isDir && entry.right.exists && !options.filter.matches(row.name))) {
                continue;
            }
                entry.recurse = (entry.left.exists && entry.left.isDir && !entry.left.isLink) ||
                        (entry.right.exists && entry.right.isDir && !entry.right.isLink);
            const bool mismatched = entry.left.exists && entry.right.exists &&
                            (entry.left.isDir != entry.right.isDir ||
                             entry.left.isLink != entry.right.isLink);
            if (mismatched) {
                entry.leftStatus = entry.rightStatus = RowStatus::Different;
                entry.pairClass = PairClass::Different;
            } else if (entry.recurse) {
                entry.leftStatus = entry.rightStatus = RowStatus::Pending;
                entry.pairClass = PairClass::Pending;
            } else if (row.status == "equal") {
                entry.leftStatus = entry.rightStatus = RowStatus::Equal;
                entry.pairClass = PairClass::Same;
            } else if (row.status == "missing") {
                entry.leftStatus = entry.rightStatus = RowStatus::Orphan;
                entry.pairClass = entry.left.exists ? PairClass::OrphanLeft : PairClass::OrphanRight;
            } else if (row.status == "left-newer") {
                entry.leftStatus = RowStatus::Newer;
                entry.rightStatus = RowStatus::Older;
                entry.pairClass = PairClass::LeftNewer;
            } else if (row.status == "right-newer") {
                entry.leftStatus = RowStatus::Older;
                entry.rightStatus = RowStatus::Newer;
                entry.pairClass = PairClass::RightNewer;
            } else {
                entry.leftStatus = entry.rightStatus = RowStatus::Different;
                entry.pairClass = PairClass::Different;
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const EntryPair& left, const EntryPair& right) {
            const bool leftDir = (left.left.exists && left.left.isDir) ||
                                 (left.right.exists && left.right.isDir);
            const bool rightDir = (right.left.exists && right.left.isDir) ||
                                  (right.right.exists && right.right.isDir);
            if (leftDir != rightDir) return leftDir;
            const QString& leftName = left.left.exists ? left.left.name : left.right.name;
            const QString& rightName = right.left.exists ? right.left.name : right.right.name;
            return QString::compare(leftName, rightName, Qt::CaseInsensitive) < 0;
        });
        return entries;
    }

    void scheduleFileComparison(const EntryPair& entry, QTreeWidgetItem* leftItem,
                                QTreeWidgetItem* rightItem,
                                const std::shared_ptr<NodeState>& state) {
        auto run = shared_from_this();
        const int leftRevision = leftItem ? leftItem->data(0, kCompareRevisionRole).toInt() : 0;
        const int rightRevision = rightItem ? rightItem->data(0, kCompareRevisionRole).toInt() : 0;
        auto* task = QRunnable::create(
            [run, entry, leftItem, rightItem, state, leftRevision, rightRevision]() {
            if (run->cancelled.load()) return;
            RemoteProfile leftProfile = run->leftProfile;
            RemoteProfile rightProfile = run->rightProfile;
            QString error;
            const bool leftRemote = run->leftRemote ||
                RemoteProfileStore::findProfileForPath(entry.left.path, &leftProfile);
            const bool rightRemote = run->rightRemote ||
                RemoteProfileStore::findProfileForPath(entry.right.path, &rightProfile);
            bool equal = false;
            const bool compared = sharedVfsBridge().compareFiles(
                entry.left.path, leftProfile, leftRemote, entry.right.path, rightProfile,
                rightRemote, &equal, &error);
            QMetaObject::invokeMethod(
                qApp,
                [run, leftItem, rightItem, state, leftRevision, rightRevision,
                 compared, equal, error]() {
                    if (run->cancelled.load()) return;
                    if ((leftItem && leftItem->data(0, kCompareRevisionRole).toInt() != leftRevision) ||
                        (rightItem && rightItem->data(0, kCompareRevisionRole).toInt() != rightRevision)) {
                        if (leftItem) {
                            const auto status = static_cast<RowStatus>(
                                leftItem->data(0, kStatusRole).toInt());
                            state->leftMask |= statusBit(status);
                            state->different = state->different ||
                                leftItem->data(0, kClassRole).toInt() != static_cast<int>(PairClass::Same);
                        }
                        if (rightItem) {
                            const auto status = static_cast<RowStatus>(
                                rightItem->data(0, kStatusRole).toInt());
                            state->rightMask |= statusBit(status);
                        }
                        if (--state->pendingChildren == 0) run->finishNode(state);
                        return;
                    }
                    const RowStatus leftStatus = compared
                        ? equal ? RowStatus::Equal : RowStatus::Different
                        : RowStatus::Pending;
                    const RowStatus rightStatus = leftStatus;
                    const PairClass pairClass = compared
                        ? equal ? PairClass::Same : PairClass::Different
                        : PairClass::Pending;
                    for (auto* item : {leftItem, rightItem}) {
                        if (!item) continue;
                        const RowStatus status = item == leftItem ? leftStatus : rightStatus;
                        item->setData(0, kStatusRole, static_cast<int>(status));
                        item->setData(0, kClassRole, static_cast<int>(pairClass));
                        item->setData(0, kFolderStatusMaskRole, statusBit(status));
                        item->setIcon(0, icons::markerIcon(status));
                        applyRowStyle(item, status, false);
                    }
                    state->different = state->different || pairClass != PairClass::Same;
                    state->pendingFolders = state->pendingFolders || !compared;
                    if (leftItem) state->leftMask |= statusBit(leftStatus);
                    if (rightItem) state->rightMask |= statusBit(rightStatus);
                    if (!compared && run->onError) run->onError(error);
                    if (--state->pendingChildren == 0) run->finishNode(state);
                }, Qt::QueuedConnection);
            });
        task->setAutoDelete(true);
        pool.start(task);
    }

    void populate(const std::vector<EntryPair>& entries, QTreeWidgetItem* leftParent,
                  QTreeWidgetItem* rightParent, const std::shared_ptr<NodeState>& parent,
                  bool leftMissing, bool rightMissing, bool mismatched,
                  const QString& error = {}) {
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
        state->different = !error.isEmpty();

        for (const auto& entry : entries) {
            if ((!entry.left.path.isEmpty() && ignoredPaths.contains(entry.left.path)) ||
                (!entry.right.path.isEmpty() && ignoredPaths.contains(entry.right.path))) {
                continue;
            }
            const bool hashPending = options.checkContent && entry.left.exists && entry.right.exists &&
                !entry.left.isDir && !entry.right.isDir && !entry.left.isLink && !entry.right.isLink &&
                entry.left.size == entry.right.size;
            const RowStatus leftStatus = hashPending ? RowStatus::Pending : entry.leftStatus;
            const RowStatus rightStatus = hashPending ? RowStatus::Pending : entry.rightStatus;
            const PairClass pairClass = hashPending ? PairClass::Pending : entry.pairClass;
            auto* leftItem = addRow(leftTree, entry.left, leftStatus, pairClass, leftParent);
            auto* rightItem = addRow(rightTree, entry.right, rightStatus, pairClass, rightParent);

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
                state->pendingFolders = state->pendingFolders || entry.pairClass == PairClass::Pending;
                if (entry.pairClass == PairClass::Different) {
                    state->different = true;
                    if (entry.left.exists) state->leftMask |= statusBit(RowStatus::Different);
                    if (entry.right.exists) state->rightMask |= statusBit(RowStatus::Different);
                }
                if (leftItem && entry.left.isDir) {
                    leftItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                }
                if (rightItem && entry.right.isDir) {
                    rightItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                }
            } else if (hashPending) {
                ++state->pendingChildren;
                scheduleFileComparison(entry, leftItem, rightItem, state);
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
        item->setData(0, kFolderStatusMaskRole, static_cast<quint32>(mask));
        item->setIcon(0, icons::folderIconForMask(mask));
        item->setText(2, formatSize(bytes));
    }

    static void refreshAncestorFolders(QTreeWidgetItem* leftItem, QTreeWidgetItem* rightItem) {
        auto* leftParent = leftItem ? leftItem->parent() : nullptr;
        auto* rightParent = rightItem ? rightItem->parent() : nullptr;
        while (leftParent && rightParent) {
            bool different = false;
            bool pending = false;
            quint32 leftMask = 0;
            quint32 rightMask = 0;
            const int count = std::min(leftParent->childCount(), rightParent->childCount());
            for (int index = 0; index < count; ++index) {
                auto* leftChild = leftParent->child(index);
                auto* rightChild = rightParent->child(index);
                const auto leftStatus = static_cast<RowStatus>(
                    leftChild->data(0, kStatusRole).toInt());
                const auto rightStatus = static_cast<RowStatus>(
                    rightChild->data(0, kStatusRole).toInt());
                leftMask |= leftChild->data(0, kFolderStatusMaskRole).toUInt();
                rightMask |= rightChild->data(0, kFolderStatusMaskRole).toUInt();
                const int pairClass = leftChild->data(0, kClassRole).toInt();
                different = different || pairClass != static_cast<int>(PairClass::Same) &&
                                         pairClass != static_cast<int>(PairClass::Pending);
                pending = pending || leftStatus == RowStatus::Pending ||
                                      rightStatus == RowStatus::Pending;
            }
            const RowStatus status = different ? RowStatus::Different
                                               : pending ? RowStatus::Pending : RowStatus::Equal;
            const PairClass pairClass = different ? PairClass::Different
                                                  : pending ? PairClass::Pending : PairClass::Same;
            leftParent->setData(0, kStatusRole, static_cast<int>(status));
            rightParent->setData(0, kStatusRole, static_cast<int>(status));
            leftParent->setData(0, kClassRole, static_cast<int>(pairClass));
            rightParent->setData(0, kClassRole, static_cast<int>(pairClass));
            leftParent->setData(0, kFolderStatusMaskRole, leftMask);
            rightParent->setData(0, kFolderStatusMaskRole, rightMask);
            leftParent->setIcon(0, icons::folderIconForMask(leftMask));
            rightParent->setIcon(0, icons::folderIconForMask(rightMask));
            leftItem = leftParent;
            rightItem = rightParent;
            leftParent = leftItem->parent();
            rightParent = rightItem->parent();
        }
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
                } else if (state->pendingFolders) {
                    status = RowStatus::Pending;
                    pairClass = PairClass::Pending;
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
            refreshAncestorFolders(state->leftItem, state->rightItem);
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
    std::function<void(const QString&, const QString&)> onNewFolderCompare;

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
        QString leftParentPath;
        QString rightParentPath;

        QString displayPath() const { return existsHere ? path : otherPath; }
    };

    // Extension points for the "Open With" and "Explorer" submenus. Their real
    // content can be supplied by application integrations; local fallbacks are
    // provided when no provider is installed.
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

        RemoteProfile leftProfile, rightProfile;
        const bool leftRemote = RemoteProfileStore::findProfileForPath(left, &leftProfile);
        const bool rightRemote = RemoteProfileStore::findProfileForPath(right, &rightProfile);
        if (!leftRemote && !QFileInfo(left).isDir()) {
            log("Left folder not found: " + QDir::toNativeSeparators(left));
            return;
        }
        if (!rightRemote && !QFileInfo(right).isDir()) {
            log("Right folder not found: " + QDir::toNativeSeparators(right));
            return;
        }

        leftFree_->setText(leftRemote ? "-" : freeSpaceText(left));
        rightFree_->setText(rightRemote ? "-" : freeSpaceText(right));

        CompareOptions options;
        options.checkContent = contentsAction_->isChecked();
        options.ignoreTimestamps = timestampsAction_->isChecked();
        options.filter = NameFilter::parse(filterCombo_->currentText());

        run_ = makeRun(leftTree_, rightTree_, std::move(options));
        run_->ignoredPaths = ignoredPaths_;
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
        run_->onError = [this, expected](const QString& error) {
            if (run_.get() != expected) return;
            log("Folder comparison failed: " + error);
        };
        log("Load comparison: " + QDir::toNativeSeparators(left) + " <-> " +
            QDir::toNativeSeparators(right));
        log(contentsAction_->isChecked() ? "Comparing folder contents (byte-for-byte)..."
                                         : "Comparing folder structure (size and timestamp)...");
        run_->start(left, right, leftProfile, leftRemote, rightProfile, rightRemote);
    }

    // Confirms (via the real openbc_vfs_stat FFI call, off the GUI thread)
    // whether each remote side resolves to a folder, and reports that plus
    // the current recursive-compare limitation. Local sides are reported
    // as "local" without a redundant re-check, since refresh() already
    // validated them with QFileInfo before either side could reach here.
    void reportRemoteFolderCheck(QString left, RemoteProfile leftProfile, bool leftRemote, QString right,
                                 RemoteProfile rightProfile, bool rightRemote) {
        log("Recursive comparison against a remote connection isn't wired up yet - checking whether "
            "the picked remote path(s) resolve to a folder...");
        QPointer<CompareSession> self(this);
        QThreadPool::globalInstance()->start([self, left, leftProfile, leftRemote, right, rightProfile,
                                              rightRemote]() {
            auto describe = [](const QString& path, const RemoteProfile& profile, bool remote) -> QString {
                if (!remote) return "local";
                QString error;
                const bool isDir =
                    sharedVfsBridge().remoteIsDir(profile, remoteRelativePath(path, profile), &error);
                if (isDir) return "remote folder confirmed on " + profile.name;
                return "remote check failed on " + profile.name +
                       (error.isEmpty() ? QString(" (not a folder)") : (": " + error));
            };
            const QString leftStatus = describe(left, leftProfile, leftRemote);
            const QString rightStatus = describe(right, rightProfile, rightRemote);
            QMetaObject::invokeMethod(
                qApp,
                [self, leftStatus, rightStatus]() {
                    if (!self) return;
                    self->log("Left: " + leftStatus + " / Right: " + rightStatus);
                },
                Qt::QueuedConnection);
        });
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
        pane.selector = new PathSelector(PathSelector::Mode::Folder, side, pathBar, /*showUp=*/true,
                                         "All files (*.*)", /*enableRemote=*/true, &sharedVfsBridge());
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
        openTextComparePaths(leftPath, rightPath);
    }

    void openTextComparePaths(const QString& leftPath, const QString& rightPath) {
        RemoteProfile leftProfile;
        RemoteProfile rightProfile;
        const bool leftRemote = !leftPath.isEmpty() &&
                                RemoteProfileStore::findProfileForPath(leftPath, &leftProfile);
        const bool rightRemote = !rightPath.isEmpty() &&
                                 RemoteProfileStore::findProfileForPath(rightPath, &rightProfile);

        QPointer<CompareSession> self(this);
        auto* task = QRunnable::create([self, leftPath, rightPath, leftRemote, leftProfile,
                                        rightRemote, rightProfile]() {
            QString leftText;
            QString rightText;
            QString error;
            auto readSide = [](const QString& path, bool remote, const RemoteProfile& profile,
                               QString* text, QString* readError) {
                if (path.isEmpty()) return true;
                if (remote) {
                    QByteArray bytes;
                    if (!sharedVfsBridge().remoteReadFile(
                            profile, remoteRelativePath(path, profile), &bytes, readError)) {
                        return false;
                    }
                    *text = QString::fromUtf8(bytes);
                    return true;
                }
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    *readError = file.errorString();
                    return false;
                }
                *text = QString::fromUtf8(file.readAll());
                return true;
            };
            QString readError;
            if (!readSide(leftPath, leftRemote, leftProfile, &leftText, &readError)) {
                error = "Could not open left file: " + leftPath + " (" + readError + ")";
            } else if (!readSide(rightPath, rightRemote, rightProfile, &rightText, &readError)) {
                error = "Could not open right file: " + rightPath + " (" + readError + ")";
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

    void loadFolderLevel(QTreeWidget* tree, QTreeWidget* other, QTreeWidgetItem* item) {
        if (!run_ || run_->cancelled.load() || !item ||
            item->data(0, kChildrenLoadedRole).toBool()) return;
        auto* counterpart = pairedItem(item, other);
        const bool isLeft = tree == leftTree_;
        auto* leftItem = isLeft ? item : counterpart;
        auto* rightItem = isLeft ? counterpart : item;
        const bool leftFolder = leftItem && leftItem->data(0, kIsDirRole).toBool();
        const bool rightFolder = rightItem && rightItem->data(0, kIsDirRole).toBool();
        if (!leftFolder && !rightFolder) return;
        item->setData(0, kChildrenLoadedRole, true);
        if (counterpart) counterpart->setData(0, kChildrenLoadedRole, true);
        const QString leftPath = leftFolder ? leftItem->data(0, kPathRole).toString() : QString();
        const QString rightPath = rightFolder ? rightItem->data(0, kPathRole).toString() : QString();
        const bool mismatched = leftItem && rightItem &&
            leftItem->data(0, kPathRole).toString().size() &&
            rightItem->data(0, kPathRole).toString().size() &&
            leftFolder != rightFolder;
        run_->scheduleFolder(leftPath, rightPath, leftItem, rightItem, nullptr, mismatched);
    }

    // Expanding / collapsing / selecting a row on one side mirrors on the other.
    void connectTreePair(QTreeWidget* tree, QTreeWidget* other, const QString& side) {
        connect(tree->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [tree, other](const QItemSelection& selected, const QItemSelection& deselected) {
                    const QSignalBlocker blocker(other);
                    auto mirrorSelection = [tree, other](const QItemSelection& selection, bool select) {
                        QSet<QTreeWidgetItem*> changedItems;
                        for (const QModelIndex& index : selection.indexes()) {
                            if (index.column() == 0) {
                                if (auto* item = tree->itemFromIndex(index)) changedItems.insert(item);
                            }
                        }
                        for (auto* item : changedItems) {
                            if (auto* counterpart = pairedItem(item, other))
                                counterpart->setSelected(select);
                        }
                    };
                    mirrorSelection(deselected, false);
                    mirrorSelection(selected, true);
                });
        connect(tree, &QTreeWidget::itemExpanded, this,
                [this, tree, other, side](QTreeWidgetItem* item) {
            if (auto* counterpart = pairedItem(item, other)) {
                const QSignalBlocker blocker(other);
                counterpart->setExpanded(true);
            }
            loadFolderLevel(tree, other, item);
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
            ctx.leftParentPath = isLeft
                ? ctx.parentPath
                : counterpart && counterpart->parent()
                    ? counterpart->parent()->data(0, kPathRole).toString()
                    : pathText(leftPath_);
            ctx.rightParentPath = isLeft
                ? counterpart && counterpart->parent()
                    ? counterpart->parent()->data(0, kPathRole).toString()
                    : pathText(rightPath_)
                : ctx.parentPath;
            if (!item->isSelected()) {
                tree->clearSelection();
                item->setSelected(true);
            }
            tree->setCurrentItem(item);
        } else {
            ctx.isDir = true;
            ctx.parentPath = pathText(isLeft ? leftPath_ : rightPath_);
            ctx.leftParentPath = pathText(leftPath_);
            ctx.rightParentPath = pathText(rightPath_);
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

    static QString parentPathFor(const QString& path) {
        RemoteProfile profile;
        if (RemoteProfileStore::findProfileForPath(path, &profile)) {
            const QString root = profile.displayAddress() + "/";
            const QString relative = path.mid(root.size());
            const int separator = relative.lastIndexOf('/');
            return separator < 0 ? root : root + relative.left(separator);
        }
        return QFileInfo(path).absolutePath();
    }

    void focusPendingEdit() {
        if (pendingEditPath_.isEmpty() || !pendingEditTree_) return;
        auto* item = findPath(pendingEditTree_, pendingEditPath_);
        if (!item) {
            QString ancestorPath = parentPathFor(pendingEditPath_);
            while (!ancestorPath.isEmpty() && !item) {
                item = findPath(pendingEditTree_, ancestorPath);
                if (!item) ancestorPath = parentPathFor(ancestorPath);
            }
            if (item && item->data(0, kIsDirRole).toBool()) {
                auto* other = pendingEditTree_ == leftTree_ ? rightTree_ : leftTree_;
                item->setExpanded(true);
                loadFolderLevel(pendingEditTree_, other, item);
            }
            return;
        }
        const QString path = pendingEditPath_;
        pendingEditPath_.clear();
        pendingEditTree_->clearFocus();
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
        const int separator = oldPath.lastIndexOf('/');
        const QString oldName = oldPath.mid(separator + 1);
        if (oldPath.isEmpty() || newName.isEmpty() || newName.contains('/') ||
            newName.contains('\\') || oldName == newName) {
            if (!oldPath.isEmpty() && item->text(0) != oldName) {
                const QSignalBlocker blocker(tree);
                item->setText(0, oldName);
            }
            return;
        }
        RemoteProfile remoteProfile;
        const bool remote = RemoteProfileStore::findProfileForPath(oldPath, &remoteProfile);
        const QString newPath = ComparisonRun::childPath(parentPathFor(oldPath), newName);
        const QPointer<CompareSession> self(this);
        const QPointer<CompareTree> guardedTree(tree);
        QThreadPool::globalInstance()->start([self, guardedTree, oldPath, newPath,
                                              remoteProfile, remote]() {
            QString error;
            const bool renamed = sharedVfsBridge().renameEntry(
                oldPath, newPath, remoteProfile, remote, &error);
            QMetaObject::invokeMethod(qApp, [self, guardedTree, oldPath, newPath, renamed, error]() {
                if (!self) return;
                if (!renamed) {
                    if (guardedTree) {
                        if (auto* current = findPath(guardedTree, oldPath)) {
                            const QSignalBlocker blocker(guardedTree);
                            current->setText(0, oldPath.mid(oldPath.lastIndexOf('/') + 1));
                        }
                    }
                    QMessageBox::warning(self.data(), "Rename failed",
                                         error.isEmpty() ? QStringLiteral("Could not rename the entry.")
                                                         : error);
                    return;
                }
                self->refresh();
                self->log("Renamed " + oldPath + " to " + newPath);
            }, Qt::QueuedConnection);
        });
    }

    void createEntry(const NodeContext& ctx, bool directory) {
        const QString targetParent = ctx.existsHere && ctx.isDir ? ctx.path : ctx.parentPath;
        RemoteProfile remoteProfile;
        const bool isRemote = RemoteProfileStore::findProfileForPath(targetParent, &remoteProfile);
        const QString parent = isRemote ? targetParent : QDir::cleanPath(targetParent);
        if (parent.isEmpty()) return;
        if (isRemote) {
            const QString remoteParent = remoteRelativePath(parent, remoteProfile);
            const QString base = directory ? "New Folder" : "New File";
            const QPointer<CompareSession> self(this);
            QThreadPool::globalInstance()->start(
                [self, ctx, parent, remoteParent, remoteProfile, directory, base]() {
                    QList<RemoteEntry> entries;
                    QString error;
                    bool created = sharedVfsBridge().remoteListDirectory(
                        remoteProfile, remoteParent, &entries, &error);
                    QSet<QString> names;
                    for (const RemoteEntry& entry : entries) names.insert(entry.name);
                    QString name = base;
                    int suffix = 2;
                    while (created && names.contains(name)) {
                        name = base + " " + QString::number(suffix++);
                    }
                    const QString remotePath = remoteParent.isEmpty()
                                                   ? name
                                                   : remoteParent.endsWith('/')
                                                         ? remoteParent + name
                                                         : remoteParent + '/' + name;
                    if (created) {
                        created = directory
                                      ? sharedVfsBridge().remoteCreateDirectory(
                                            remoteProfile, remotePath, &error)
                                      : sharedVfsBridge().remoteWriteFile(
                                            remoteProfile, remotePath, QByteArray(), &error);
                    }
                    const QString displayPath = ComparisonRun::childPath(parent, name);
                    QMetaObject::invokeMethod(
                        qApp, [self, ctx, directory, displayPath, created, error]() {
                            if (!self) return;
                            if (!created) {
                                QMessageBox::warning(self.data(), "Create failed",
                                                     error.isEmpty() ? QStringLiteral("Could not create the entry.")
                                                                     : error);
                                return;
                            }
                            self->pendingEditTree_ = self->treeFor(ctx.side);
                            self->pendingEditPath_ = displayPath;
                            self->refresh();
                        },
                        Qt::QueuedConnection);
                });
            return;
        }
        const QString base = directory ? "New Folder" : "New File";
        const QPointer<CompareSession> self(this);
        QThreadPool::globalInstance()->start([self, ctx, parent, directory, base]() {
            QString name = base;
            int suffix = 2;
            while (QFileInfo::exists(QDir(parent).filePath(name)))
                name = base + " " + QString::number(suffix++);
            const QString path = QDir(parent).filePath(name);
            QString error;
            const bool created = directory
                ? sharedVfsBridge().createDirectory(path, RemoteProfile(), false, &error)
                : sharedVfsBridge().writeFile(path, RemoteProfile(), false, QByteArray(), &error);
            QMetaObject::invokeMethod(qApp, [self, ctx, path, created, error]() {
                if (!self) return;
                if (!created) {
                    QMessageBox::warning(self, "Create failed",
                                         error.isEmpty() ? QStringLiteral("Could not create the entry.")
                                                         : error);
                    return;
                }
                self->pendingEditTree_ = self->treeFor(ctx.side);
                self->pendingEditPath_ = path;
                self->refresh();
            }, Qt::QueuedConnection);
        });
    }

    void compareNodeFiles(const NodeContext& ctx, bool showReport,
                          const QString& comparisonPath = {}) {
        const bool updateRows = comparisonPath.isEmpty();
        const QString otherPath = comparisonPath.isEmpty() ? ctx.otherPath : comparisonPath;
        const QString leftPath = ctx.side == Side::Left ? ctx.path : otherPath;
        const QString rightPath = ctx.side == Side::Right ? ctx.path : otherPath;
        if (leftPath.isEmpty() || rightPath.isEmpty()) {
            QMessageBox::information(this, "File comparison", "A file is missing on one side.");
            return;
        }
        auto comparisonRun = run_;
        if (!comparisonRun) {
            QMessageBox::warning(this, "File comparison", "Refresh the folder comparison first.");
            return;
        }
        RemoteProfile leftProfile, rightProfile;
        const bool leftRemote = RemoteProfileStore::findProfileForPath(leftPath, &leftProfile);
        const bool rightRemote = RemoteProfileStore::findProfileForPath(rightPath, &rightProfile);
        int leftRevision = 0;
        int rightRevision = 0;
        if (updateRows) {
            if (auto* item = findPath(leftTree_, leftPath)) {
                leftRevision = item->data(0, kCompareRevisionRole).toInt() + 1;
                item->setData(0, kCompareRevisionRole, leftRevision);
            }
            if (auto* item = findPath(rightTree_, rightPath)) {
                rightRevision = item->data(0, kCompareRevisionRole).toInt() + 1;
                item->setData(0, kCompareRevisionRole, rightRevision);
            }
        }
        const QPointer<CompareSession> self(this);
        comparisonRun->pool.start(QRunnable::create(
            [self, comparisonRun, leftPath, rightPath, leftProfile, rightProfile, leftRemote,
             rightRemote, showReport, updateRows, leftRevision, rightRevision]() {
                bool equal = false;
                QString error;
                const bool compared = sharedVfsBridge().compareFiles(
                    leftPath, leftProfile, leftRemote, rightPath, rightProfile, rightRemote,
                    &equal, &error);
                QMetaObject::invokeMethod(qApp, [self, comparisonRun, leftPath, rightPath,
                                                  compared, equal, error, showReport, updateRows,
                                                  leftRevision, rightRevision]() {
                    if (!self || comparisonRun->cancelled.load()) return;
                    if (!compared) {
                        QMessageBox::warning(self, "File comparison",
                                             error.isEmpty() ? QStringLiteral("Could not compare the files.")
                                                             : error);
                        return;
                    }
                    const RowStatus status = equal ? RowStatus::Equal : RowStatus::Different;
                    const PairClass pairClass = equal ? PairClass::Same : PairClass::Different;
                    if (updateRows) {
                        for (auto* tree : {self->leftTree_, self->rightTree_}) {
                            auto* item = findPath(tree, tree == self->leftTree_ ? leftPath : rightPath);
                            const int revision = tree == self->leftTree_ ? leftRevision : rightRevision;
                            if (item && item->data(0, kCompareRevisionRole).toInt() == revision) {
                                item->setData(0, kStatusRole, static_cast<int>(status));
                                item->setData(0, kClassRole, static_cast<int>(pairClass));
                                item->setData(0, kFolderStatusMaskRole, statusBit(status));
                                item->setIcon(0, icons::markerIcon(status));
                                applyRowStyle(item, status, false);
                            }
                        }
                        ComparisonRun::refreshAncestorFolders(findPath(self->leftTree_, leftPath),
                                                              findPath(self->rightTree_, rightPath));
                    }
                    const QString result = equal ? "Files are identical." : "Files are different.";
                    self->log(result + " " + QDir::toNativeSeparators(leftPath) + " <-> " +
                              QDir::toNativeSeparators(rightPath));
                    if (showReport) QMessageBox::information(self, "File Compare Report", result);
                }, Qt::QueuedConnection);
            }));
    }

    void transferFile(const QString& sourcePath, const QString& destinationPath, bool move,
                      bool mirror = false) {
        if (sourcePath.isEmpty() || destinationPath.isEmpty()) return;
        if (sourcePath == destinationPath) {
            QMessageBox::information(this, "Transfer", "Source and destination are the same path.");
            return;
        }
        RemoteProfile sourceProfile, destinationProfile;
        const bool sourceRemote = RemoteProfileStore::findProfileForPath(sourcePath, &sourceProfile);
        const bool destinationRemote =
            RemoteProfileStore::findProfileForPath(destinationPath, &destinationProfile);
        if (!destinationRemote && QFileInfo::exists(destinationPath) &&
            QMessageBox::question(this, "Replace file", "Replace the existing destination file?") !=
                QMessageBox::Yes) {
            return;
        }
        if (sourceRemote && destinationRemote && sourceProfile.name == destinationProfile.name &&
            destinationPath.startsWith(sourcePath + "/")) {
            QMessageBox::warning(this, "Transfer", "A folder cannot be moved inside itself.");
            return;
        }
        if (!sourceRemote && !destinationRemote && QFileInfo(sourcePath).isDir()) {
            const QString sourceAbsolute = QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath());
            const QString destinationAbsolute =
                QDir::cleanPath(QFileInfo(destinationPath).absoluteFilePath());
            if (destinationAbsolute.startsWith(sourceAbsolute + QDir::separator())) {
                QMessageBox::warning(this, "Transfer", "A folder cannot be copied inside itself.");
                return;
            }
        }
        auto comparisonRun = run_;
        if (!comparisonRun) return;
        const QPointer<CompareSession> self(this);
        comparisonRun->pool.start(QRunnable::create(
            [self, sourcePath, destinationPath, sourceProfile, destinationProfile,
             sourceRemote, destinationRemote, move, mirror]() {
                QString error;
                bool ok = sharedVfsBridge().copyEntry(
                    sourcePath, sourceProfile, sourceRemote, destinationPath, destinationProfile,
                    destinationRemote, &error, mirror);
                if (ok && move)
                    ok = sharedVfsBridge().deleteEntry(sourcePath, sourceProfile, sourceRemote, &error);
                QMetaObject::invokeMethod(qApp, [self, sourcePath, destinationPath, ok, error]() {
                    if (!self) return;
                    if (!ok) {
                        QMessageBox::warning(self, "Transfer failed",
                                             error.isEmpty() ? QStringLiteral("Could not transfer the file.")
                                                             : error);
                        return;
                    }
                    self->log("Transferred " + sourcePath + " to " + destinationPath);
                    self->refresh();
                }, Qt::QueuedConnection);
            }));
    }

    void transferNode(const NodeContext& ctx, Side sourceSide, bool move, bool mirror) {
        const QString sourcePath = sourceSide == ctx.side ? ctx.path : ctx.otherPath;
        if (sourcePath.isEmpty()) {
            QMessageBox::information(this, "Synchronize", "The selected source side is missing.");
            return;
        }
        const Side destinationSide = sourceSide == Side::Left ? Side::Right : Side::Left;
        const QString destinationParent = destinationSide == Side::Left
            ? ctx.leftParentPath : ctx.rightParentPath;
        const QString destinationPath = ComparisonRun::childPath(
            destinationParent, QFileInfo(sourcePath).fileName());
        transferFile(sourcePath, destinationPath, move, mirror);
    }

    void activateNodeAction(const NodeContext& ctx, const QString& action) {
        if (action == "Open Folder") {
            setPath(ctx.side == Side::Left ? leftPath_ : rightPath_, ctx.path);
            refresh();
        } else if (action == "Open") {
            auto* tree = treeFor(ctx.side);
            if (auto* item = findPath(tree, ctx.path)) openTextCompare(tree, item);
        } else if (action == "Open Subfolders" || action == "Close Subfolders") {
            auto* tree = treeFor(ctx.side);
            if (auto* item = findPath(tree, ctx.path)) {
                if (action == "Open Subfolders") tree->expandItem(item);
                else tree->collapseItem(item);
            }
        } else if (action == "Set as Base Folder") {
            setPath(ctx.side == Side::Left ? leftPath_ : rightPath_, ctx.path);
            refresh();
        } else if (action == "Set as Base on Other Side") {
            setPath(ctx.side == Side::Left ? rightPath_ : leftPath_, ctx.path);
            refresh();
        } else if (action == "Open in New View") {
            if (onNewFolderCompare) {
                const QString other = ctx.otherPath.isEmpty()
                    ? pathText(ctx.side == Side::Left ? rightPath_ : leftPath_)
                    : ctx.otherPath;
                onNewFolderCompare(ctx.side == Side::Left ? ctx.path : other,
                                   ctx.side == Side::Right ? ctx.path : other);
            }
        } else if (action == "Compare To...") {
            const QString chosen = QFileDialog::getOpenFileName(this, "Choose file to compare");
            if (!chosen.isEmpty()) compareNodeFiles(ctx, false, chosen);
        } else if (action == "Align With...") {
            const QString chosen = QFileDialog::getOpenFileName(this, "Choose file to align with");
            if (!chosen.isEmpty()) {
                const QString leftPath = ctx.side == Side::Left ? ctx.path : chosen;
                const QString rightPath = ctx.side == Side::Right ? ctx.path : chosen;
                openTextComparePaths(leftPath, rightPath);
            }
        } else if (action == "Compare Contents...") {
            compareNodeFiles(ctx, false);
        } else if (action == "File Compare Report...") {
            compareNodeFiles(ctx, true);
        } else if (action == "Copy Filename") {
            QApplication::clipboard()->setText(QFileInfo(ctx.displayPath()).fileName());
        } else if (action == "Refresh Selection") {
            refresh();
        } else if (action == "Ignored" || action == "Exclude" || action == "Exclude...") {
            if (ignoredPaths_.contains(ctx.path)) ignoredPaths_.remove(ctx.path);
            else ignoredPaths_.insert(ctx.path);
            refresh();
        } else if (action == "Rename") {
            renameSelected(treeFor(ctx.side));
        } else if (action == "New Folder" || action == "New File") {
            createEntry(ctx, action == "New Folder");
        } else if (action == "Copy to Right..." || action == "Copy to Left..." ||
                   action == "Move to Right..." || action == "Move to Left...") {
            const bool move = action.startsWith("Move");
            transferNode(ctx, ctx.side, move, false);
        } else if (action == "Copy to Folder..." || action == "Move to Folder...") {
            const QString folder = QFileDialog::getExistingDirectory(this, "Choose destination folder");
            if (!folder.isEmpty()) {
                transferFile(ctx.path, QDir(folder).filePath(QFileInfo(ctx.path).fileName()),
                             action.startsWith("Move"));
            }
        } else if (action == "Delete...") {
            if (QMessageBox::question(this, "Delete", "Delete this entry permanently?") != QMessageBox::Yes)
                return;
            RemoteProfile profile;
            const bool remote = RemoteProfileStore::findProfileForPath(ctx.path, &profile);
            const QString path = ctx.path;
            const QPointer<CompareSession> self(this);
            QThreadPool::globalInstance()->start([self, path, profile, remote]() {
                QString error;
                const bool removed = sharedVfsBridge().deleteEntry(path, profile, remote, &error);
                QMetaObject::invokeMethod(qApp, [self, path, removed, error]() {
                    if (!self) return;
                    if (!removed) QMessageBox::warning(self, "Delete failed", error);
                    else self->refresh();
                }, Qt::QueuedConnection);
            });
        } else if (action == "Attributes...") {
            RemoteProfile profile;
            if (RemoteProfileStore::findProfileForPath(ctx.path, &profile)) {
                QMessageBox::information(this, "Attributes", "Attributes are unavailable for this provider.");
            } else {
                const QString path = ctx.path;
                const QPointer<CompareSession> self(this);
                QThreadPool::globalInstance()->start([self, path]() {
                    const QFileInfo info(path);
                    const QString details = QString("Size: %1 bytes\nModified: %2\nPermissions: %3")
                        .arg(info.size()).arg(info.lastModified().toString())
                        .arg(static_cast<int>(info.permissions()), 0, 16);
                    QMetaObject::invokeMethod(qApp, [self, details]() {
                        if (self) QMessageBox::information(self, "Attributes", details);
                    }, Qt::QueuedConnection);
                });
            }
        } else if (action == "Touch...") {
            RemoteProfile profile;
            if (RemoteProfileStore::findProfileForPath(ctx.path, &profile)) {
                if (ctx.isDir) {
                    QMessageBox::warning(this, "Touch", "Directory timestamps are not supported by this provider.");
                    return;
                }
                const QString path = ctx.path;
                const QPointer<CompareSession> self(this);
                QThreadPool::globalInstance()->start([self, path, profile]() {
                    QByteArray bytes;
                    QString error;
                    bool touched = sharedVfsBridge().remoteReadFile(
                        profile, remoteRelativePath(path, profile), &bytes, &error);
                    if (touched) {
                        touched = sharedVfsBridge().remoteWriteFile(
                            profile, remoteRelativePath(path, profile), bytes, &error);
                    }
                    QMetaObject::invokeMethod(qApp, [self, touched, error]() {
                        if (!self) return;
                        if (!touched)
                            QMessageBox::warning(self, "Touch failed", error);
                        else self->refresh();
                    }, Qt::QueuedConnection);
                });
            } else {
                const QString path = ctx.path;
                const QPointer<CompareSession> self(this);
                QThreadPool::globalInstance()->start([self, path]() {
                    QFile file(path);
                    const bool touched = file.open(QIODevice::ReadWrite) &&
                        file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
                    QMetaObject::invokeMethod(qApp, [self, touched]() {
                        if (!self) return;
                        if (!touched)
                            QMessageBox::warning(self, "Touch failed", "Could not update the modification time.");
                        else self->refresh();
                    }, Qt::QueuedConnection);
                });
            }
        } else if (action == "Update Right..." || action == "Update Left...") {
            const Side destination = action == "Update Right..." ? Side::Right : Side::Left;
            transferNode(ctx, destination == Side::Right ? Side::Left : Side::Right, false, false);
        } else if (action == "Update Both...") {
            QMessageBox choice(this);
            choice.setWindowTitle("Update both sides");
            choice.setText("Choose which side is the source of truth.");
            auto* useLeft = choice.addButton("Use Left", QMessageBox::AcceptRole);
            auto* useRight = choice.addButton("Use Right", QMessageBox::AcceptRole);
            choice.addButton(QMessageBox::Cancel);
            choice.exec();
            if (choice.clickedButton() == useLeft) transferNode(ctx, Side::Left, false, false);
            else if (choice.clickedButton() == useRight) transferNode(ctx, Side::Right, false, false);
        } else if (action == "Mirror to Right..." || action == "Mirror to Left...") {
            const Side source = action == "Mirror to Right..." ? Side::Left : Side::Right;
            if (QMessageBox::question(this, "Mirror", "Remove destination-only entries and mirror the source?") ==
                QMessageBox::Yes) {
                transferNode(ctx, source, false, true);
            }
        } else if (action == "Open Containing Folder") {
            const QString directory = ctx.isDir ? ctx.path : QFileInfo(ctx.path).absolutePath();
            if (RemoteProfileStore::findProfileForPath(directory, nullptr)) {
                QMessageBox::information(this, "Explorer",
                                         "Remote paths are not available in the local file manager.");
            } else {
                QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
            }
        }
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
        addNodeAction(&menu, ctx, "Rename", mi::rename(), "F2");
        addNodeAction(&menu, ctx, "Attributes...");
        addNodeAction(&menu, ctx, "Touch...", mi::touch());
        addNodeAction(&menu, ctx, ctx.isDir ? "Exclude" : "Exclude...");
        addNodeAction(&menu, ctx, "Copy Filename");

        QMenu* create = menu.addMenu("Create");
        addNodeAction(create, ctx, "New Folder", mi::newFolder(), {}, false);
        addNodeAction(create, ctx, "New File", {}, {}, false);
        create->menuAction()->setEnabled(!ctx.parentPath.isEmpty());

        // Ignored entries are omitted from subsequent session comparisons.
        const bool ignored = ignoredPaths_.contains(ctx.path);
        addNodeAction(&menu, ctx, "Ignored", ignored ? mi::check() : QIcon());

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
        connect(action, &QAction::triggered, this,
            [this, ctx, text]() { activateNodeAction(ctx, text); });
        return action;
    }

    // Lets a provider fill a submenu, or falls back to a single dummy entry.
    void fillProviderMenu(QMenu* submenu, const NodeContext& ctx,
                          const std::function<void(QMenu*, const NodeContext&)>& provider) {
        if (provider) {
            provider(submenu, ctx);
            return;
        }
        if (submenu->title() == "Open With") {
            QAction* defaultApplication = submenu->addAction("Default Application");
            QAction* chooseProgram = submenu->addAction("Choose Program...");
            connect(defaultApplication, &QAction::triggered, this, [this, ctx]() {
                if (RemoteProfileStore::findProfileForPath(ctx.path, nullptr)) {
                    QMessageBox::information(this, "Open With",
                                             "No local application can open a remote path directly.");
                } else {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(ctx.path));
                }
            });
            connect(chooseProgram, &QAction::triggered, this, [this, ctx]() {
                if (RemoteProfileStore::findProfileForPath(ctx.path, nullptr)) {
                    QMessageBox::information(this, "Open With",
                                             "Download the remote file before opening it with a local program.");
                    return;
                }
                const QString program = QFileDialog::getOpenFileName(this, "Choose application");
                if (!program.isEmpty()) QProcess::startDetached(program, {ctx.path});
            });
        } else {
            addNodeAction(submenu, ctx, "Open Containing Folder", QIcon(), {}, true);
        }
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
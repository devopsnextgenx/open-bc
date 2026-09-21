#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QRunnable>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStorageInfo>
#include <QStringList>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include "qt_style.h"

namespace {

using openbc::ui::CompareTree;
using openbc::ui::PairClass;
using openbc::ui::RowStatus;
using openbc::ui::kClassRole;
using openbc::ui::kIsDirRole;
using openbc::ui::kPairClassCount;
using openbc::ui::kPathRole;
using openbc::ui::kStatusRole;
using openbc::ui::statusBit;
namespace icons = openbc::ui::icons;
namespace color = openbc::ui::color;

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

QString formatBytes(qint64 bytes) {
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
QString formatSize(qint64 bytes) {
    constexpr qint64 gib = 1024LL * 1024LL * 1024LL;
    if (bytes >= gib) {
        return QString::number(static_cast<double>(bytes) / static_cast<double>(gib), 'f', 2) + " GB";
    }
    return QLocale().toString(bytes);
}

QString freeSpaceText(const QString& path) {
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

SideInfo makeSide(const QFileInfo& info) {
    SideInfo side;
    side.exists = true;
    side.isDir = info.isDir();
    side.isLink = info.isSymLink();
    side.name = info.fileName();
    side.path = info.absoluteFilePath();
    side.ext = side.isDir ? QString() : info.suffix();
    side.size = side.isDir ? 0 : info.size();
    side.modified = info.lastModified();
    QString attrs;
    if (side.isDir) attrs += 'D';
    if (side.isLink) attrs += 'L';
    if (info.isHidden()) attrs += 'H';
    if (!info.isWritable()) attrs += 'R';
    if (!side.isDir && info.isExecutable()) attrs += 'X';
    side.attrs = attrs;
    return side;
}

std::vector<SideInfo> listFolder(const QString& path, const NameFilter& filter) {
    std::vector<SideInfo> result;
    if (path.isEmpty()) {
        return result;
    }
    const QDir directory(path);
    const auto infos = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& info : infos) {
        if (!info.isDir() && !filter.matches(info.fileName())) {
            continue;
        }
        result.push_back(makeSide(info));
    }
    return result;
}

bool contentEqual(const QString& leftPath, const QString& rightPath,
                  const std::atomic_bool& cancelled) {
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly)) {
        return false;
    }
    constexpr qint64 chunkSize = 1024 * 1024;
    while (!left.atEnd() && !right.atEnd()) {
        if (cancelled.load()) {
            return false;
        }
        if (left.read(chunkSize) != right.read(chunkSize)) {
            return false;  // stop at the first difference
        }
    }
    return left.atEnd() && right.atEnd();
}

struct EntryPair {
    SideInfo left;
    SideInfo right;
    RowStatus leftStatus = RowStatus::Pending;
    RowStatus rightStatus = RowStatus::Pending;
    PairClass pairClass = PairClass::Pending;
    bool recurse = false;
};

// Same scheme as Beyond Compare: green/orange = differing timestamps
// (newer / older), red = same timestamp but different content, purple =
// only on one side.
void classifyEntry(EntryPair& e, const CompareOptions& options, const std::atomic_bool& cancelled) {
    const SideInfo& l = e.left;
    const SideInfo& r = e.right;
    const bool leftRecurse = l.exists && l.isDir && !l.isLink;
    const bool rightRecurse = r.exists && r.isDir && !r.isLink;
    e.recurse = leftRecurse || rightRecurse;

    if (!l.exists || !r.exists) {
        e.leftStatus = e.rightStatus = RowStatus::Orphan;
        e.pairClass = l.exists ? PairClass::OrphanLeft : PairClass::OrphanRight;
        return;
    }
    if (l.isDir != r.isDir) {  // file vs folder
        e.leftStatus = e.rightStatus = RowStatus::Different;
        e.pairClass = PairClass::Different;
        return;
    }
    if (l.isDir) {
        if (e.recurse) {  // resolved once the children are known
            e.leftStatus = e.rightStatus = RowStatus::Pending;
            e.pairClass = PairClass::Pending;
        } else {
            const bool same = l.isLink == r.isLink;
            e.leftStatus = e.rightStatus = same ? RowStatus::Equal : RowStatus::Different;
            e.pairClass = same ? PairClass::Same : PairClass::Different;
        }
        return;
    }

    const bool sameSize = l.size == r.size;
    const bool sameTime = options.ignoreTimestamps ||
                          qAbs(l.modified.toMSecsSinceEpoch() - r.modified.toMSecsSinceEpoch()) <= 2000;
    bool same = false;
    if (sameSize) {
        same = options.checkContent ? contentEqual(l.path, r.path, cancelled) : sameTime;
    }
    if (same) {
        e.leftStatus = e.rightStatus = RowStatus::Equal;
        e.pairClass = PairClass::Same;
    } else if (!sameTime) {
        const bool leftNewer = l.modified > r.modified;
        e.leftStatus = leftNewer ? RowStatus::Newer : RowStatus::Older;
        e.rightStatus = leftNewer ? RowStatus::Older : RowStatus::Newer;
        e.pairClass = leftNewer ? PairClass::LeftNewer : PairClass::RightNewer;
    } else {
        e.leftStatus = e.rightStatus = RowStatus::Different;
        e.pairClass = PairClass::Different;
    }
}

QString pairKey(const QString& name) {
#ifdef Q_OS_WIN
    return name.toLower();
#else
    return name;
#endif
}

std::vector<EntryPair> collectEntries(const QString& leftPath, const QString& rightPath,
                                      const CompareOptions& options,
                                      const std::atomic_bool& cancelled) {
    std::vector<EntryPair> entries;
    std::vector<std::pair<QString, size_t>> index;  // key -> position in `entries`
    QHash<QString, size_t> positions;

    for (auto& side : listFolder(leftPath, options.filter)) {
        positions.insert(pairKey(side.name), entries.size());
        EntryPair e;
        e.left = std::move(side);
        entries.push_back(std::move(e));
    }
    for (auto& side : listFolder(rightPath, options.filter)) {
        const auto found = positions.constFind(pairKey(side.name));
        if (found != positions.constEnd()) {
            entries[*found].right = std::move(side);
        } else {
            EntryPair e;
            e.right = std::move(side);
            entries.push_back(std::move(e));
        }
    }

    std::sort(entries.begin(), entries.end(), [](const EntryPair& a, const EntryPair& b) {
        const bool aDir = (a.left.exists && a.left.isDir) || (a.right.exists && a.right.isDir);
        const bool bDir = (b.left.exists && b.left.isDir) || (b.right.exists && b.right.isDir);
        if (aDir != bDir) {
            return aDir;  // folders first
        }
        const QString& an = a.left.exists ? a.left.name : a.right.name;
        const QString& bn = b.left.exists ? b.left.name : b.right.name;
        const int cmp = QString::compare(an, bn, Qt::CaseInsensitive);
        return cmp != 0 ? cmp < 0 : an < bn;
    });

    for (auto& e : entries) {
        if (cancelled.load()) {
            break;
        }
        classifyEntry(e, options, cancelled);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

void applyRowStyle(QTreeWidgetItem* item, RowStatus status, bool isDir) {
    // Folder names keep the normal text colour (the folder icon carries the
    // status); their size/date/attribute columns are dimmed.
    const QColor name = isDir ? color::text() : openbc::ui::statusTextColor(status);
    const QColor rest = isDir ? color::dimText() : name;
    item->setForeground(0, name);
    for (int column = 1; column < item->columnCount(); ++column) {
        item->setForeground(column, rest);
    }
}

QTreeWidgetItem* addRow(QTreeWidget* tree, const SideInfo& side, RowStatus status,
                        PairClass pairClass, QTreeWidgetItem* parent) {
    auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
    item->setData(0, kStatusRole, static_cast<int>(status));
    item->setData(0, kClassRole, static_cast<int>(pairClass));
    item->setData(0, kPathRole, side.exists ? side.path : QString());
    item->setData(0, kIsDirRole, side.exists && side.isDir);
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

    explicit CompareSession(QWidget* parent = nullptr) : QWidget(parent) {
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
        QComboBox* path = nullptr;
        QToolButton* browse = nullptr;
        QToolButton* up = nullptr;
        CompareTree* tree = nullptr;
        QLabel* count = nullptr;
        QLabel* free = nullptr;
    };

    Pane makePane(const QString& side, QWidget* parent) {
        using icons::Glyph;
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
        pane.path = new QComboBox(pathBar);
        pane.path->setEditable(true);
        pane.path->setInsertPolicy(QComboBox::NoInsert);
        pane.path->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        pane.path->setMinimumContentsLength(10);
        pane.path->lineEdit()->setPlaceholderText(side + " folder path");
        pane.browse = new QToolButton(pathBar);
        pane.browse->setIcon(icons::glyph(Glyph::FolderOpen));
        pane.browse->setToolTip("Select " + side.toLower() + " folder");
        pane.up = new QToolButton(pathBar);
        pane.up->setIcon(icons::glyph(Glyph::FolderUp));
        pane.up->setToolTip("Go to parent folder");
        pathLayout->addWidget(pane.path, 1);
        pathLayout->addWidget(pane.browse);
        pathLayout->addWidget(pane.up);

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
        return pane;
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
        leftPath_ = left.path;
        rightPath_ = right.path;
        leftBrowse_ = left.browse;
        rightBrowse_ = right.browse;
        leftUp_ = left.up;
        rightUp_ = right.up;
        leftTree_ = left.tree;
        rightTree_ = right.tree;
        leftCount_ = left.count;
        rightCount_ = right.count;
        leftFree_ = left.free;
        rightFree_ = right.free;

        auto* panes = new QSplitter(Qt::Horizontal, this);
        panes->setChildrenCollapsible(false);
        panes->addWidget(left.panel);
        panes->addWidget(right.panel);
        panes->setSizes({550, 550});

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
        connect(selectLeftAction_, &QAct::triggered, this, [this]() { chooseFolder(leftPath_, "left"); });
        connect(selectRightAction_, &QAct::triggered, this, [this]() { chooseFolder(rightPath_, "right"); });
        connect(leftBrowse_, &QToolButton::clicked, this, [this]() { chooseFolder(leftPath_, "left"); });
        connect(rightBrowse_, &QToolButton::clicked, this, [this]() { chooseFolder(rightPath_, "right"); });
        connect(leftUp_, &QToolButton::clicked, this, [this]() { goUp(leftPath_); });
        connect(rightUp_, &QToolButton::clicked, this, [this]() { goUp(rightPath_); });
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
        connect(editSelectionAction_, &QAct::triggered, this, [this]() { log("Edit selection requested"); });
        connect(findAction_, &QAct::triggered, this, [this]() { log("Find requested in current folder level"); });
        connect(optionsAction_, &QAct::triggered, this, [this]() { log("Comparison options opened"); });

        connectTreePair(leftTree_, rightTree_, "left");
        connectTreePair(rightTree_, leftTree_, "right");
        connectScrollSync();
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

    void chooseFolder(QComboBox* combo, const QString& side) {
        const QString selected =
            QFileDialog::getExistingDirectory(this, "Select " + side + " folder", pathText(combo));
        if (!selected.isEmpty()) {
            setPath(combo, selected);
            refresh();
            log(side + " resource selected: " + selected);
        }
    }

    void goUp(QComboBox* combo) {
        QDir dir(pathText(combo));
        if (!pathText(combo).isEmpty() && dir.cdUp()) {
            setPath(combo, dir.absolutePath());
            refresh();
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
    QComboBox* leftPath_ = nullptr;
    QComboBox* rightPath_ = nullptr;
    QToolButton* leftBrowse_ = nullptr;
    QToolButton* rightBrowse_ = nullptr;
    QToolButton* leftUp_ = nullptr;
    QToolButton* rightUp_ = nullptr;
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
};

}  // namespace

extern "C" int openbc_run_gui() {
    int argc = 1;
    char application_name[] = "openbc-qt";
    char* argv[] = {application_name, nullptr};
    QApplication application(argc, argv);

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    application.setPalette(openbc::ui::darkPalette());
    application.setStyleSheet(openbc::ui::applicationStyleSheet());

    QMainWindow window;
    window.resize(1280, 820);

    auto* sessionMenu = window.menuBar()->addMenu("&Session");
    auto* actionsMenu = window.menuBar()->addMenu("&Actions");
    auto* editMenu = window.menuBar()->addMenu("&Edit");
    auto* searchMenu = window.menuBar()->addMenu("&Search");
    auto* viewMenu = window.menuBar()->addMenu("&View");
    auto* toolsMenu = window.menuBar()->addMenu("&Tools");
    window.menuBar()->addMenu("&Help");

    auto* tabs = new QTabWidget(&window);
    tabs->setDocumentMode(true);
    tabs->setMovable(true);
    tabs->setElideMode(Qt::ElideRight);
    tabs->setUsesScrollButtons(true);
    tabs->tabBar()->setDrawBase(false);
    window.setCentralWidget(tabs);

    auto currentSession = [&]() { return static_cast<CompareSession*>(tabs->currentWidget()); };

    // The Actions / Edit / Search / View / Tools menus always show the actions
    // of the tab that is currently selected, so a command can only ever affect
    // the tab you are looking at.
    auto rebuildMenus = [&]() {
        auto fill = [](QMenu* menu, const QList<QAction*>& items) {
            menu->clear();
            for (QAction* action : items) {
                if (action) {
                    menu->addAction(action);
                } else {
                    menu->addSeparator();
                }
            }
        };
        CompareSession* session = currentSession();
        fill(actionsMenu, session ? session->actionsMenuItems() : QList<QAction*>());
        fill(editMenu, session ? session->editMenuItems() : QList<QAction*>());
        fill(searchMenu, session ? session->searchMenuItems() : QList<QAction*>());
        fill(viewMenu, session ? session->viewMenuItems() : QList<QAction*>());
        fill(toolsMenu, session ? session->toolsMenuItems() : QList<QAction*>());
    };
    auto updateWindowTitle = [&]() {
        CompareSession* session = currentSession();
        window.setWindowTitle(session ? session->title() + " - Folder Compare - OpenBC"
                                      : QString("OpenBC - Folder Compare"));
    };

    std::function<CompareSession*(const QString&, const QString&, bool)> addSession;
    std::function<void(int)> closeTab;

    addSession = [&](const QString& left, const QString& right, bool run) -> CompareSession* {
        auto* session = new CompareSession;
        session->setPaths(left, right);
        const int index =
            tabs->addTab(session, openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow()),
                         session->title());
        tabs->setTabToolTip(index, session->detailedTitle());

        auto* close = new QToolButton;
        close->setObjectName("tabClose");
        close->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Close));
        close->setToolTip("Close tab (Ctrl+W)");
        close->setFocusPolicy(Qt::NoFocus);
        close->setAutoRaise(true);
        tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
        QObject::connect(close, &QToolButton::clicked, session,
                         [&, session]() { closeTab(tabs->indexOf(session)); });

        session->onTitleChanged = [&, session]() {
            const int i = tabs->indexOf(session);
            if (i < 0) {
                return;
            }
            tabs->setTabText(i, session->title());
            tabs->setTabToolTip(i, session->detailedTitle());
            if (tabs->currentIndex() == i) {
                updateWindowTitle();
            }
        };
        tabs->setCurrentIndex(index);
        if (run) {
            session->refresh();
        }
        return session;
    };

    closeTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        auto* session = static_cast<CompareSession*>(tabs->widget(index));
        session->cancelRun();
        tabs->removeTab(index);
        session->deleteLater();
        if (tabs->count() == 0) {
            addSession(QString(), QString(), false);  // never leave the window empty
        }
    };

    auto duplicateTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        auto* source = static_cast<CompareSession*>(tabs->widget(index));
        auto* copy = addSession(source->leftText(), source->rightText(), false);
        copy->copySettingsFrom(*source);
        copy->refresh();
    };

    auto closeOthers = [&](int keep) {
        auto* keepWidget = tabs->widget(keep);
        for (int i = tabs->count() - 1; i >= 0; --i) {
            if (tabs->widget(i) != keepWidget) {
                closeTab(i);
            }
        }
    };

    // Session menu: these manage the tabs themselves.
    auto* newTab = sessionMenu->addAction("New Folder Compare");
    newTab->setShortcut(QKeySequence("Ctrl+T"));
    auto* duplicate = sessionMenu->addAction("Duplicate Tab");
    auto* openComparison = sessionMenu->addAction("Open comparison");
    sessionMenu->addSeparator();
    auto* closeCurrent = sessionMenu->addAction("Close Tab");
    closeCurrent->setShortcut(QKeySequence("Ctrl+W"));
    auto* closeOthersAction = sessionMenu->addAction("Close Other Tabs");
    sessionMenu->addSeparator();
    auto* exitAction = sessionMenu->addAction("Exit");
    exitAction->setShortcut(QKeySequence("Ctrl+Q"));

    QObject::connect(newTab, &QAction::triggered, [&]() { addSession(QString(), QString(), false); });
    QObject::connect(duplicate, &QAction::triggered, [&]() { duplicateTab(tabs->currentIndex()); });
    QObject::connect(openComparison, &QAction::triggered, [&]() {
        if (auto* session = currentSession()) session->log("Open comparison requested");
    });
    QObject::connect(closeCurrent, &QAction::triggered, [&]() { closeTab(tabs->currentIndex()); });
    QObject::connect(closeOthersAction, &QAction::triggered, [&]() { closeOthers(tabs->currentIndex()); });
    QObject::connect(exitAction, &QAction::triggered, &window, &QWidget::close);

    auto* plus = new QToolButton(tabs);
    plus->setObjectName("newTab");
    plus->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Plus));
    plus->setToolTip("New folder compare (Ctrl+T)");
    plus->setAutoRaise(true);
    tabs->setCornerWidget(plus, Qt::TopRightCorner);
    QObject::connect(plus, &QToolButton::clicked, [&]() { addSession(QString(), QString(), false); });

    tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(tabs->tabBar(), &QWidget::customContextMenuRequested, [&](const QPoint& pos) {
        const int index = tabs->tabBar()->tabAt(pos);
        if (index < 0) {
            return;
        }
        QMenu menu;
        QAction* dup = menu.addAction("Duplicate Tab");
        menu.addSeparator();
        QAction* close = menu.addAction("Close Tab");
        QAction* others = menu.addAction("Close Other Tabs");
        others->setEnabled(tabs->count() > 1);
        QAction* chosen = menu.exec(tabs->tabBar()->mapToGlobal(pos));
        if (chosen == dup) {
            duplicateTab(index);
        } else if (chosen == close) {
            closeTab(index);
        } else if (chosen == others) {
            closeOthers(index);
        }
    });

    QObject::connect(tabs, &QTabWidget::currentChanged, [&](int) {
        rebuildMenus();
        updateWindowTitle();
    });

    addSession("/home/kira/tmp/source", "/home/kira/tmp/target", true);
    rebuildMenus();
    updateWindowTitle();
    window.show();

    return application.exec();
}
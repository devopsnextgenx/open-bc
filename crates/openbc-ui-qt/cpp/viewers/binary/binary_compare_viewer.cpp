#include "viewers/binary/binary_compare_viewer.h"

#include <QBrush>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QVBoxLayout>
#include <algorithm>

#include "common/compare_font.h"
#include "common/remote_selector.h"
#include "i18n/i18n.h"
#include "style/qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

namespace {

// Matches openbc_core::binary_compare::ChangeState::glyph().
QString glyphFor(BinaryState state) {
    switch (state) {
    case BinaryState::Unchanged: return QStringLiteral("\U0001F7E2");  // green circle
    case BinaryState::Modified: return QStringLiteral("\U0001F538");   // orange diamond
    case BinaryState::Removed: return QStringLiteral("\u274C");        // cross mark
    case BinaryState::Added: return QStringLiteral("\u2705");          // check mark
    case BinaryState::None: break;
    }
    return {};
}

QString statusName(BinaryState state) {
    switch (state) {
    case BinaryState::Unchanged: return ::openbc::i18n::t("binary.state_unchanged");
    case BinaryState::Modified: return ::openbc::i18n::t("binary.state_modified");
    case BinaryState::Removed: return ::openbc::i18n::t("binary.state_removed");
    case BinaryState::Added: return ::openbc::i18n::t("binary.state_added");
    case BinaryState::None: break;
    }
    return {};
}

QColor colorFor(BinaryState state) {
    namespace color = openbc::ui::color;
    switch (state) {
    case BinaryState::Modified: return color::older();
    case BinaryState::Removed: return color::different();
    case BinaryState::Added: return color::newer();
    case BinaryState::Unchanged: return color::equalMarker();
    case BinaryState::None: break;
    }
    return color::folderText();
}

}  // namespace

// --------------------------------------------------------------------------- model

BinaryTreeModel::BinaryTreeModel(QObject* parent) : QAbstractItemModel(parent) {}

void BinaryTreeModel::setSummary(BinarySummaryPtr summary) {
    beginResetModel();
    summary_ = std::move(summary);
    parent_.clear();
    position_.clear();
    children_.clear();
    topLevel_.clear();
    if (summary_) {
        const std::size_t count = openbc_binary_summary_row_count(summary_.get());
        parent_.resize(static_cast<int>(count));
        position_.resize(static_cast<int>(count));
        children_.resize(static_cast<int>(count));
        QVector<int> stack;  // stack[d] = latest row seen at depth d
        for (std::size_t i = 0; i < count; ++i) {
            const int depth = static_cast<int>(openbc_binary_summary_row_depth(summary_.get(), i));
            stack.resize(std::min<int>(depth, stack.size()));
            const int row = static_cast<int>(i);
            if (stack.isEmpty()) {
                parent_[row] = -1;
                position_[row] = topLevel_.size();
                topLevel_.push_back(row);
            } else {
                const int p = stack.last();
                parent_[row] = p;
                position_[row] = children_[p].size();
                children_[p].push_back(row);
            }
            stack.push_back(row);
        }
    }
    endResetModel();
}

int BinaryTreeModel::backendRow(const QModelIndex& index) const {
    return index.isValid() ? static_cast<int>(index.internalId()) : -1;
}

BinaryState BinaryTreeModel::stateOf(const QModelIndex& index) const {
    const int row = backendRow(index);
    if (row < 0 || !summary_) return BinaryState::None;
    return static_cast<BinaryState>(openbc_binary_summary_row_state(summary_.get(), static_cast<std::size_t>(row)));
}

bool BinaryTreeModel::isLeaf(const QModelIndex& index) const {
    const int row = backendRow(index);
    return row >= 0 && row < children_.size() && children_[row].isEmpty();
}

QModelIndex BinaryTreeModel::indexForBackendRow(int row, int column) const {
    if (row < 0 || row >= parent_.size()) return {};
    return createIndex(position_[row], column, static_cast<quintptr>(row));
}

QModelIndex BinaryTreeModel::index(int row, int column, const QModelIndex& parentIndex) const {
    if (row < 0 || column < 0 || column >= ColCount) return {};
    const QVector<int>& siblings = parentIndex.isValid() ? children_[backendRow(parentIndex)] : topLevel_;
    if (row >= siblings.size()) return {};
    return createIndex(row, column, static_cast<quintptr>(siblings[row]));
}

QModelIndex BinaryTreeModel::parent(const QModelIndex& child) const {
    const int row = backendRow(child);
    if (row < 0 || row >= parent_.size()) return {};
    const int p = parent_[row];
    return p < 0 ? QModelIndex() : createIndex(position_[p], 0, static_cast<quintptr>(p));
}

int BinaryTreeModel::rowCount(const QModelIndex& parentIndex) const {
    if (!parentIndex.isValid()) return topLevel_.size();
    if (parentIndex.column() != 0) return 0;
    return children_[backendRow(parentIndex)].size();
}

QString BinaryTreeModel::text(int row, bool detail) const {
    std::size_t length = 0;
    const std::size_t r = static_cast<std::size_t>(row);
    const std::uint8_t* data = detail ? openbc_binary_summary_row_detail(summary_.get(), r, &length)
                                      : openbc_binary_summary_row_label(summary_.get(), r, &length);
    return backendString(data, length);
}

QVariant BinaryTreeModel::data(const QModelIndex& index, int role) const {
    const int row = backendRow(index);
    if (row < 0 || !summary_) return {};
    const BinaryState state = stateOf(index);
    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == ColElement) {
            const QString glyph = glyphFor(state);
            const QString label = text(row, false);
            // Groups and the root already show their aggregate state in the Status column; the
            // marker is drawn for every row so the tree reads at a glance.
            return glyph.isEmpty() ? label : glyph + QLatin1Char(' ') + label;
        }
        if (index.column() == ColStatus) return statusName(state);
        return text(row, true);
    case Qt::ToolTipRole: {
        const QString detail = text(row, true);
        return detail.isEmpty() ? text(row, false) : text(row, false) + QLatin1Char('\n') + detail;
    }
    case Qt::ForegroundRole:
        if (state == BinaryState::None) return {};
        return QBrush(index.column() == ColDetail ? openbc::ui::color::folderInfo() : colorFor(state));
    default:
        return {};
    }
}

QVariant BinaryTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case ColElement: return ::openbc::i18n::t("binary.col_element");
    case ColStatus: return ::openbc::i18n::t("binary.col_status");
    case ColDetail: return ::openbc::i18n::t("binary.col_detail");
    default: return {};
    }
}

// --------------------------------------------------------------------------- viewer

BinaryCompareViewer::BinaryCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent)
    : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
      channel_(this, &BinaryCompareViewer::destroyHandle,
               [this](const BackendChannel::Event& event) { onEvent(event); }) {
    setObjectName("binaryCompareView");
    setFont(defaultCompareFont());
    buildUi();
    changes_ = std::make_unique<FileChangeController>(this, [this](std::uint8_t) { reloadLatest(); },
                                                      /*wholeTab=*/true);
    startSession();
}

BinaryCompareViewer::~BinaryCompareViewer() { stopSession(); }

QString BinaryCompareViewer::title() const {
    const auto name = [](const QString& p) { return p.isEmpty() ? QStringLiteral("(missing)") : QFileInfo(p).fileName(); };
    return name(leftPath_) + QStringLiteral(" <-> ") + name(rightPath_);
}

QString BinaryCompareViewer::detailedTitle() const { return leftPath_ + QStringLiteral("  <->  ") + rightPath_; }

void BinaryCompareViewer::destroyHandle(int kind, void* handle) {
    if (kind == OPENBC_EV_RESULT) openbc_binary_summary_destroy(static_cast<OpenBcBinarySummary*>(handle));
}

QString BinaryCompareViewer::cleanPathInput(QString raw) {
    raw = raw.trimmed();
    if (raw.size() >= 2 && raw.startsWith('"') && raw.endsWith('"')) raw = raw.mid(1, raw.size() - 2);
    return raw.trimmed();
}

void BinaryCompareViewer::applyDroppedPaths(const QString& left, const QString& right) {
    const QString wanted[2] = {cleanPathInput(left), cleanPathInput(right)};
    for (std::uint8_t side = 0; side < 2; ++side) {
        if (wanted[side].isEmpty()) continue;
        commitPath(side, wanted[side]);
        if ((side == 0 ? leftPath_ : rightPath_) != wanted[side]) return;  // refused: stop
    }
}

void BinaryCompareViewer::startSession() {
    stopSession();
    if (changes_) {
        changes_->track(0, leftPath_);
        changes_->track(1, rightPath_);
    }
    summary_.reset();
    model_->setSummary(nullptr);
    summaryLabel_->clear();
    // Remote files are fetched to a local copy: LIEF parses from a local path.
    const QByteArray left = openbc_remote::localCopy(leftPath_).toUtf8();
    const QByteArray right = openbc_remote::localCopy(rightPath_).toUtf8();
    session_ = openbc_binary_session_create(left.constData(), right.constData(), BackendChannel::callback(),
                                            channel_.user());
    if (!showUnchangedAction_->isChecked()) openbc_binary_show_unchanged(session_, 0);
    status_->setText(::openbc::i18n::t("binary.parsing"));
    progress_->setRange(0, 0);  // busy indicator: parsing has no meaningful percentage
    progress_->show();
    if (onTitleChanged) onTitleChanged();
}

void BinaryCompareViewer::stopSession() {
    if (!session_) return;
    openbc_binary_session_destroy(session_);
    session_ = nullptr;
}

void BinaryCompareViewer::commitPath(std::uint8_t side, const QString& raw) {
    QString& current = side == 0 ? leftPath_ : rightPath_;
    PathSelector* selector = selectors_[side];
    const QString path = cleanPathInput(raw);
    if (path == current) return;
    QString localPath, resolveError;
    if (!path.isEmpty() && !openbc_remote::resolveLocalPath(path, &localPath, &resolveError)) {
        status_->setText(resolveError);
        selector->setText(current);
        return;
    }
    current = path;
    if (!path.isEmpty()) selector->remember(path);
    startSession();
}

void BinaryCompareViewer::reloadLatest() {
    QString local, error;
    for (const QString& path : {leftPath_, rightPath_}) {
        if (path.isEmpty()) continue;
        if (!openbc_remote::resolveLocalPath(path, &local, &error, /*refresh=*/true)) {
            status_->setText(error);
            return;
        }
    }
    startSession();
}

void BinaryCompareViewer::onEvent(const BackendChannel::Event& event) {
    switch (event.kind) {
    case OPENBC_EV_PROGRESS:
        status_->setText(::openbc::i18n::t("binary.parsing"));
        break;
    case OPENBC_EV_RESULT:
        applySummary(std::static_pointer_cast<OpenBcBinarySummary>(event.handle));
        break;
    case OPENBC_EV_ERROR:
        progress_->hide();
        status_->setText(event.message);
        break;
    default:
        break;
    }
}

void BinaryCompareViewer::applySummary(const BinarySummaryPtr& summary) {
    summary_ = summary;
    const OpenBcBinarySummary* s = summary.get();
    progress_->hide();

    QStringList errors;
    for (std::uint8_t side = 0; side < 2; ++side) {
        std::size_t length = 0;
        const QString error = backendString(openbc_binary_summary_error(s, side, &length), length);
        if (!error.isEmpty()) {
            errors << (side == 0 ? ::openbc::i18n::t("binary.version1") : ::openbc::i18n::t("binary.version2")) +
                          QStringLiteral(": ") + error;
        }
    }
    model_->setSummary(errors.isEmpty() ? summary : nullptr);
    if (!errors.isEmpty()) {
        summaryLabel_->clear();
        status_->setText(errors.join(QStringLiteral("  |  ")));
        if (onTitleChanged) onTitleChanged();
        return;
    }

    tree_->expandToDepth(1);  // root + its four groups; deeper levels stay collapsed (can be huge)
    const auto total = [s](std::uint8_t state) { return static_cast<qulonglong>(openbc_binary_summary_total(s, state)); };
    const std::uint8_t fileState = openbc_binary_summary_file_state(s);
    summaryLabel_->setText(::openbc::i18n::t("binary.summary", total(1), total(2), total(3), total(0)));
    status_->setText(fileState == 0 ? ::openbc::i18n::t("binary.files_identical")
                                    : ::openbc::i18n::t("binary.files_differ"));
    if (onTitleChanged) onTitleChanged();
}

void BinaryCompareViewer::jumpToChange(bool forward) {
    if (!model_->rowCountTotal()) return;
    const int total = model_->rowCountTotal();
    const QModelIndex current = tree_->currentIndex();
    int start = current.isValid() ? model_->backendRow(current) : (forward ? -1 : total);
    // Leaves with a change, in pre-order; wraps around at either end.
    for (int step = 1; step <= total; ++step) {
        const int row = forward ? (start + step) % total : ((start - step) % total + total) % total;
        const QModelIndex index = model_->indexForBackendRow(row);
        if (model_->isLeaf(index) && model_->stateOf(index) != BinaryState::Unchanged &&
            model_->stateOf(index) != BinaryState::None) {
            tree_->setCurrentIndex(index);
            tree_->scrollTo(index, QAbstractItemView::PositionAtCenter);
            return;
        }
    }
    status_->setText(::openbc::i18n::t("binary.no_changes"));
}

void BinaryCompareViewer::buildUi() {
    using icons::Glyph;
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    toolbar_ = new QToolBar(this);
    toolbar_->setObjectName("textCompareToolbar");
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(20, 20));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    homeAction_ = toolbar_->addAction(icons::glyph(Glyph::Home), ::openbc::i18n::t("tab.home"));
    sessionsAction_ = toolbar_->addAction(icons::glyph(Glyph::Sessions), ::openbc::i18n::t("common.sessions"));
    toolbar_->addSeparator();
    nextAction_ = toolbar_->addAction(icons::glyph(Glyph::NextSection), ::openbc::i18n::t("hex.next_diff"));
    prevAction_ = toolbar_->addAction(icons::glyph(Glyph::PrevSection), ::openbc::i18n::t("hex.prev_diff"));
    nextAction_->setShortcut(QKeySequence(Qt::Key_F3));
    prevAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    reloadAction_ = toolbar_->addAction(icons::glyph(Glyph::Refresh), ::openbc::i18n::t("common.reload"));
    toolbar_->addSeparator();
    showUnchangedAction_ = toolbar_->addAction(icons::glyph(Glyph::ShowAll), ::openbc::i18n::t("binary.show_unchanged"));
    showUnchangedAction_->setCheckable(true);
    showUnchangedAction_->setChecked(true);
    expandAction_ = toolbar_->addAction(icons::glyph(Glyph::ExpandAll), ::openbc::i18n::t("binary.expand_groups"));
    collapseAction_ = toolbar_->addAction(icons::glyph(Glyph::CollapseAll), ::openbc::i18n::t("binary.collapse_all"));
    root->addWidget(toolbar_);

    // Version 1 (baseline) | Version 2 (target)
    auto* pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->setSpacing(2);
    for (std::uint8_t side = 0; side < 2; ++side) {
        selectors_[side] = openbc_remote::makeRemoteFileSelector(
            this, side == 0 ? ::openbc::i18n::t("binary.version1") : ::openbc::i18n::t("binary.version2"),
            side == 0 ? leftPath_ : rightPath_,
            QStringLiteral("Executables and libraries (*.exe *.dll *.sys *.ocx *.so *.dylib *.elf);;All files (*.*)"),
            [this, side](QString p) { commitPath(side, p); });
        pathRow->addWidget(selectors_[side], 1);
    }
    root->addLayout(pathRow);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setContentsMargins(6, 3, 6, 3);
    root->addWidget(summaryLabel_);

    model_ = new BinaryTreeModel(this);
    tree_ = new QTreeView(this);
    tree_->setObjectName("binaryCompareTree");
    tree_->setModel(model_);
    tree_->setUniformRowHeights(true);  // required for fast layout of very large trees
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);  // read-only by design
    tree_->setAllColumnsShowFocus(true);
    tree_->header()->setStretchLastSection(true);
    tree_->header()->setSectionResizeMode(BinaryTreeModel::ColElement, QHeaderView::Interactive);
    tree_->header()->setSectionResizeMode(BinaryTreeModel::ColStatus, QHeaderView::ResizeToContents);
    tree_->setColumnWidth(BinaryTreeModel::ColElement, 460);
    root->addWidget(tree_, 1);

    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(6, 2, 6, 2);
    status_ = new QLabel(this);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->setMaximumWidth(160);
    progress_->hide();
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(progress_);
    root->addLayout(statusRow);

    QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
    QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
    QObject::connect(nextAction_, &QAction::triggered, this, [this]() { jumpToChange(true); });
    QObject::connect(prevAction_, &QAction::triggered, this, [this]() { jumpToChange(false); });
    QObject::connect(reloadAction_, &QAction::triggered, this, [this]() { startSession(); });
    QObject::connect(showUnchangedAction_, &QAction::toggled, this, [this](bool show) {
        if (session_) openbc_binary_show_unchanged(session_, show ? 1 : 0);  // no re-parse
    });
    QObject::connect(expandAction_, &QAction::triggered, this, [this]() { tree_->expandToDepth(2); });
    QObject::connect(collapseAction_, &QAction::triggered, this, [this]() {
        tree_->collapseAll();
        tree_->expandToDepth(0);
    });
}

}  // namespace openbc::app

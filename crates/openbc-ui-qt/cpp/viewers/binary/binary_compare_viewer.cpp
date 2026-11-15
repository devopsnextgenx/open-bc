#include "viewers/binary/binary_compare_viewer.h"

#include <QBrush>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QList>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
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

// Column of byte `index` in a hex row; mirrors openbc_core::binary_compare::inspect::hex_byte_column.
int hexByteColumn(int index) { return 10 + index * 3 + (index >= 8 ? 1 : 0); }

// Backend row state (0 same, 1 modified, 2 left only, 3 right only) as a translucent tint.
QColor tintFor(std::uint8_t state, int alpha) {
    QColor c = colorFor(static_cast<BinaryState>(state));
    c.setAlpha(alpha);
    return c;
}

QString textOf(const std::uint8_t* data, std::size_t length) { return backendString(data, length); }

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

QString BinaryTreeModel::keyOf(const QModelIndex& index) const {
    const int row = backendRow(index);
    if (row < 0 || !summary_) return {};
    std::size_t length = 0;
    const std::uint8_t* data = openbc_binary_summary_row_key(summary_.get(), static_cast<std::size_t>(row), &length);
    return backendString(data, length);
}

QString BinaryTreeModel::sideText(const QModelIndex& index, int side) const {
    const int row = backendRow(index);
    if (row < 0 || !summary_) return {};
    std::size_t length = 0;
    const std::size_t r = static_cast<std::size_t>(row);
    const std::uint8_t* data = side == 0 ? openbc_binary_summary_row_left(summary_.get(), r, &length)
                                         : openbc_binary_summary_row_right(summary_.get(), r, &length);
    return backendString(data, length);
}

QString BinaryTreeModel::elementText(const QModelIndex& index) const {
    const int row = backendRow(index);
    if (row < 0 || !summary_) return {};
    const QString glyph = glyphFor(stateOf(index));
    const QString label = text(row, false);
    return glyph.isEmpty() ? label : glyph + QLatin1Char(' ') + label;
}

bool BinaryTreeModel::isInspectable(const QModelIndex& index) const {
    const int row = backendRow(index);
    return row >= 0 && summary_ &&
           openbc_binary_summary_row_inspectable(summary_.get(), static_cast<std::size_t>(row)) != 0;
}

bool BinaryTreeModel::presentOn(const QModelIndex& index, int side) const {
    const int row = backendRow(index);
    if (row < 0 || !summary_) return true;
    return openbc_binary_summary_row_present(summary_.get(), static_cast<std::size_t>(row),
                                             static_cast<std::uint8_t>(side)) != 0;
}

QString BinaryTreeModel::sideLabel(int side) const {
    if (!summary_) return {};
    std::size_t length = 0;
    return backendString(openbc_binary_summary_label(summary_.get(), static_cast<std::uint8_t>(side), &length), length);
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

// --------------------------------------------------------------------------- split-view model

BinarySideModel::BinarySideModel(BinaryTreeModel* source, int side, QObject* parent)
    : QIdentityProxyModel(parent), tree_(source), side_(side) {
    setSourceModel(source);
}

QVariant BinarySideModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const QModelIndex src = mapToSource(index);
    const BinaryState state = tree_->stateOf(src);
    // An element that exists only in the other version is a placeholder here (like the empty
    // row the folder view shows for a file that is missing on one side): no text, hatched.
    if (!tree_->presentOn(src, side_)) {
        if (role == Qt::BackgroundRole)
            return QBrush(tintFor(static_cast<std::uint8_t>(side_ == 0 ? 3 : 2), 90), Qt::BDiagPattern);
        if (role == Qt::ToolTipRole)
            return tree_->keyOf(src) + QLatin1Char('\n') +
                   (side_ == 0 ? ::openbc::i18n::t("binary.only_in_version2") : ::openbc::i18n::t("binary.only_in_version1"));
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
        if (index.column() == 0) return tree_->elementText(src);
        return tree_->sideText(src, side_);
    case Qt::ToolTipRole: {
        const QString key = tree_->keyOf(src);
        const QString value = tree_->sideText(src, side_);
        return key.isEmpty() ? value : (value.isEmpty() ? key : key + QLatin1Char('\n') + value);
    }
    case Qt::ForegroundRole:
        if (state == BinaryState::None) return {};
        return QBrush(index.column() == 1 ? openbc::ui::color::folderInfo() : colorFor(state));
    case Qt::BackgroundRole:
        // Tint the half that holds the change: removed on Version 1, added on Version 2.
        if (index.column() == 1 && state == BinaryState::Modified)
            return QBrush(tintFor(1, 40));
        if (index.column() == 1 && ((side_ == 0 && state == BinaryState::Removed) ||
                                    (side_ == 1 && state == BinaryState::Added)))
            return QBrush(tintFor(static_cast<std::uint8_t>(state), 40));
        return {};
    default:
        return {};
    }
}

QVariant BinarySideModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    if (section == 0) return ::openbc::i18n::t("binary.col_element");
    const QString name = side_ == 0 ? ::openbc::i18n::t("binary.version1") : ::openbc::i18n::t("binary.version2");
    const QString file = tree_->sideLabel(side_);
    return file.isEmpty() ? name : name + QStringLiteral(": ") + file;
}

// --------------------------------------------------------------------------- viewer

BinaryCompareViewer::BinaryCompareViewer(const QString& leftPath, const QString& rightPath,
                                         QSettings* preferences, QWidget* parent)
    : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath), preferences_(preferences),
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
    else if (kind == OPENBC_EV_WINDOW) openbc_binary_inspect_destroy(static_cast<OpenBcBinaryInspect*>(handle));
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
    clearInspector(::openbc::i18n::t("binary.inspect_hint"));
    // Remote files are fetched to a local copy: LIEF parses from a local path.
    const QByteArray left = openbc_remote::localCopy(leftPath_).toUtf8();
    const QByteArray right = openbc_remote::localCopy(rightPath_).toUtf8();
    session_ = openbc_binary_session_create(left.constData(), right.constData(), BackendChannel::callback(),
                                            channel_.user());
    if (!showUnchangedAction_->isChecked()) openbc_binary_show_unchanged(session_, 0);
    pushViewSettings();
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
    case OPENBC_EV_WINDOW:
        applyInspect(std::static_pointer_cast<OpenBcBinaryInspect>(event.handle));
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
    const QModelIndex keep = tree_->currentIndex();
    const QString keepKey = keep.isValid() ? model_->keyOf(keep) : QString();
    model_->setSummary(errors.isEmpty() ? summary : nullptr);
    if (!errors.isEmpty()) {
        clearInspector(::openbc::i18n::t("binary.inspect_hint"));
        summaryLabel_->clear();
        status_->setText(errors.join(QStringLiteral("  |  ")));
        if (onTitleChanged) onTitleChanged();
        return;
    }

    for (QTreeView* view : allTrees()) view->expandToDepth(1);  // root + its groups; deeper levels stay collapsed
    Q_UNUSED(keepKey);
    const auto total = [s](std::uint8_t state) { return static_cast<qulonglong>(openbc_binary_summary_total(s, state)); };
    const std::uint8_t fileState = openbc_binary_summary_file_state(s);
    QString line = ::openbc::i18n::t("binary.summary", total(1), total(2), total(3), total(0));
    const qulonglong hidden = static_cast<qulonglong>(openbc_binary_summary_hidden(s));
    if (hidden > 0) line += QStringLiteral("   |   ") + ::openbc::i18n::t("binary.hidden", hidden);
    summaryLabel_->setText(line);
    status_->setText(fileState == 0 ? ::openbc::i18n::t("binary.files_identical")
                                    : ::openbc::i18n::t("binary.files_differ"));
    if (onTitleChanged) onTitleChanged();
}

void BinaryCompareViewer::jumpToChange(bool forward) {
    if (!model_->rowCountTotal()) return;
    const int total = model_->rowCountTotal();
    const QModelIndex current = activeTree()->currentIndex();
    Q_UNUSED(current);
    int start = currentRow_ >= 0 ? currentRow_ : (forward ? -1 : total);
    // Leaves with a change, in pre-order; wraps around at either end.
    for (int step = 1; step <= total; ++step) {
        const int row = forward ? (start + step) % total : ((start - step) % total + total) % total;
        const QModelIndex index = model_->indexForBackendRow(row);
        if (model_->isLeaf(index) && model_->stateOf(index) != BinaryState::Unchanged &&
            model_->stateOf(index) != BinaryState::None) {
            setCurrentBackendRow(row);
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
    viewModeAction_ = toolbar_->addAction(icons::glyph(Glyph::Contents), ::openbc::i18n::t("binary.view_split"));
    viewModeAction_->setCheckable(true);
    viewModeAction_->setToolTip(::openbc::i18n::t("binary.view_mode"));
    demangleAction_ = toolbar_->addAction(icons::glyph(Glyph::Format), ::openbc::i18n::t("binary.demangle"));
    demangleAction_->setCheckable(true);
    demangleAction_->setChecked(binaryopts::demangle(preferences_));
    filtersAction_ = toolbar_->addAction(icons::glyph(Glyph::Filter), ::openbc::i18n::t("binary.filters"));
    filtersMenu_ = new QMenu(this);
    const char* const filterKeys[4] = {"binary.filter_symtab", "binary.filter_debug", "binary.filter_padding",
                                       "binary.filter_local"};
    for (int i = 0; i < 4; ++i) {
        filterActions_[i] = filtersMenu_->addAction(::openbc::i18n::t(filterKeys[i]));
        filterActions_[i]->setCheckable(true);
        filterActions_[i]->setChecked((binaryopts::filters(preferences_) >> i) & 1u);
    }
    filtersAction_->setMenu(filtersMenu_);
    inspectAction_ = toolbar_->addAction(icons::glyph(Glyph::Find), ::openbc::i18n::t("binary.inspect"));
    inspectAction_->setCheckable(true);
    inspectAction_->setChecked(true);
    toolbar_->addSeparator();
    expandAction_ = toolbar_->addAction(icons::glyph(Glyph::ExpandAll), ::openbc::i18n::t("binary.expand_groups"));
    collapseAction_ = toolbar_->addAction(icons::glyph(Glyph::CollapseAll), ::openbc::i18n::t("binary.collapse_all"));
    root->addWidget(toolbar_);
    if (auto* button = qobject_cast<QToolButton*>(toolbar_->widgetForAction(filtersAction_)))
        button->setPopupMode(QToolButton::InstantPopup);

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

    buildSplitView();
    buildInspector();
    views_ = new QStackedWidget(this);
    views_->addWidget(tree_);
    views_->addWidget(splitTrees_[0]->parentWidget());
    mainSplit_ = new QSplitter(Qt::Vertical, this);
    mainSplit_->addWidget(views_);
    mainSplit_->addWidget(inspector_);
    mainSplit_->setStretchFactor(0, 3);
    mainSplit_->setStretchFactor(1, 2);
    mainSplit_->setChildrenCollapsible(false);
    root->addWidget(mainSplit_, 1);

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
    QObject::connect(expandAction_, &QAction::triggered, this, [this]() {
        for (QTreeView* view : allTrees()) view->expandToDepth(2);
    });
    QObject::connect(collapseAction_, &QAction::triggered, this, [this]() {
        for (QTreeView* view : allTrees()) {
            view->collapseAll();
            view->expandToDepth(0);
        }
    });
    QObject::connect(viewModeAction_, &QAction::toggled, this, [this](bool split) {
        setViewMode(split ? binaryopts::ViewMode::Split : binaryopts::ViewMode::Summary);
        binaryopts::setViewMode(preferences_, split ? binaryopts::ViewMode::Split : binaryopts::ViewMode::Summary);
    });
    QObject::connect(demangleAction_, &QAction::toggled, this, [this](bool on) {
        binaryopts::setDemangle(preferences_, on);
        if (session_) openbc_binary_set_demangle(session_, on ? 1 : 0);  // no re-parse
    });
    for (QAction* action : filterActions_) {
        QObject::connect(action, &QAction::toggled, this, [this](bool) {
            binaryopts::setFilters(preferences_, filterMask());
            if (session_) openbc_binary_set_filters(session_, filterMask());  // no re-parse
        });
    }
    QObject::connect(inspectAction_, &QAction::toggled, this, [this](bool on) {
        inspector_->setVisible(on);
        if (on && currentRow_ >= 0) requestInspect(currentRow_);
    });
    QObject::connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
                     [this](const QModelIndex& current, const QModelIndex&) {
                         if (!syncing_) onCurrentRowChanged(current);
                     });

    // Restore the persisted view mode (applied last: it needs every widget to exist).
    const bool split = binaryopts::viewMode(preferences_) == binaryopts::ViewMode::Split;
    const QSignalBlocker blocker(viewModeAction_);
    viewModeAction_->setChecked(split);
    setViewMode(split ? binaryopts::ViewMode::Split : binaryopts::ViewMode::Summary);
}

unsigned BinaryCompareViewer::filterMask() const {
    unsigned mask = 0;
    for (int i = 0; i < 4; ++i)
        if (filterActions_[i] && filterActions_[i]->isChecked()) mask |= (1u << i);
    return mask;
}

void BinaryCompareViewer::pushViewSettings() {
    if (!session_) return;
    openbc_binary_set_demangle(session_, demangleAction_->isChecked() ? 1 : 0);
    openbc_binary_set_filters(session_, filterMask());
}

void BinaryCompareViewer::setViewMode(binaryopts::ViewMode mode) {
    views_->setCurrentIndex(mode == binaryopts::ViewMode::Split ? 1 : 0);
    viewModeAction_->setText(mode == binaryopts::ViewMode::Split ? ::openbc::i18n::t("binary.view_summary")
                                                                 : ::openbc::i18n::t("binary.view_split"));
    if (currentRow_ >= 0) setCurrentBackendRow(currentRow_);  // keep the selection in the visible view
}

QTreeView* BinaryCompareViewer::activeTree() const {
    return views_ && views_->currentIndex() == 1 ? splitTrees_[0] : tree_;
}

// Selects `row` (a backend row) in every view without feedback loops, scrolls the visible one to it.
void BinaryCompareViewer::setCurrentBackendRow(int row) {
    const QModelIndex source = model_->indexForBackendRow(row);
    if (!source.isValid()) return;
    syncing_ = true;
    tree_->setCurrentIndex(source);
    for (int side = 0; side < 2; ++side) splitTrees_[side]->setCurrentIndex(sideModels_[side]->mapFromSource(source));
    QTreeView* visible = activeTree();
    visible->scrollTo(visible->currentIndex(), QAbstractItemView::PositionAtCenter);
    syncing_ = false;
    onCurrentRowChanged(source);
}

void BinaryCompareViewer::onCurrentRowChanged(const QModelIndex& sourceIndex) {
    currentRow_ = model_->backendRow(sourceIndex);
    if (!inspectAction_->isChecked()) return;
    if (currentRow_ >= 0 && model_->isInspectable(sourceIndex))
        requestInspect(currentRow_);
    else
        clearInspector(::openbc::i18n::t("binary.inspect_hint"));
}

void BinaryCompareViewer::syncSplitExpansion(QTreeView* from, QTreeView* to, const QModelIndex& index, bool expanded) {
    if (syncing_) return;
    const int fromSide = from == splitTrees_[0] ? 0 : 1;
    const int toSide = 1 - fromSide;
    const QModelIndex source = sideModels_[fromSide]->mapToSource(index);
    syncing_ = true;
    to->setExpanded(sideModels_[toSide]->mapFromSource(source), expanded);
    syncing_ = false;
}

// Version 1 | Version 2 trees over the same rows; expansion, selection and scrolling mirror each other.
void BinaryCompareViewer::buildSplitView() {
    auto* container = new QSplitter(Qt::Horizontal, this);
    container->setChildrenCollapsible(false);
    for (int side = 0; side < 2; ++side) {
        sideModels_[side] = new BinarySideModel(model_, side, this);
        auto* view = new QTreeView(container);
        view->setObjectName(side == 0 ? "binarySplitLeft" : "binarySplitRight");
        view->setModel(sideModels_[side]);
        view->setUniformRowHeights(true);
        view->setAlternatingRowColors(true);
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        view->setAllColumnsShowFocus(true);
        view->header()->setStretchLastSection(true);
        view->header()->setSectionResizeMode(0, QHeaderView::Interactive);
        view->setColumnWidth(0, 300);
        container->addWidget(view);
        splitTrees_[side] = view;
    }
    for (int side = 0; side < 2; ++side) {
        QTreeView* self = splitTrees_[side];
        QTreeView* other = splitTrees_[1 - side];
        QObject::connect(self, &QTreeView::expanded, this,
                         [this, self, other](const QModelIndex& i) { syncSplitExpansion(self, other, i, true); });
        QObject::connect(self, &QTreeView::collapsed, this,
                         [this, self, other](const QModelIndex& i) { syncSplitExpansion(self, other, i, false); });
        QObject::connect(self->selectionModel(), &QItemSelectionModel::currentChanged, this,
                         [this, side](const QModelIndex& current, const QModelIndex&) {
                             if (syncing_ || !current.isValid()) return;
                             const QModelIndex source = sideModels_[side]->mapToSource(current);
                             syncing_ = true;
                             tree_->setCurrentIndex(source);
                             splitTrees_[1 - side]->setCurrentIndex(sideModels_[1 - side]->mapFromSource(source));
                             syncing_ = false;
                             onCurrentRowChanged(source);
                         });
        QScrollBar* mine = self->verticalScrollBar();
        QScrollBar* theirs = other->verticalScrollBar();
        QObject::connect(mine, &QScrollBar::valueChanged, this, [this, theirs](int value) {
            if (syncing_) return;
            syncing_ = true;
            theirs->setValue(value);
            syncing_ = false;
        });
    }
}

// Hex / disassembly panes under the views.
void BinaryCompareViewer::buildInspector() {
    inspector_ = new QWidget(this);
    auto* layout = new QVBoxLayout(inspector_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    inspectInfo_ = new QLabel(inspector_);
    inspectInfo_->setContentsMargins(6, 2, 6, 0);
    inspectInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspectNotes_ = new QLabel(inspector_);
    inspectNotes_->setContentsMargins(6, 0, 6, 0);
    inspectNotes_->setWordWrap(true);
    inspectNotes_->hide();
    inspectTabs_ = new QTabWidget(inspector_);
    const auto makePair = [this](QPlainTextEdit** edits) {
        auto* pair = new QSplitter(Qt::Horizontal, inspectTabs_);
        for (int side = 0; side < 2; ++side) {
            edits[side] = new QPlainTextEdit(pair);
            edits[side]->setReadOnly(true);
            edits[side]->setLineWrapMode(QPlainTextEdit::NoWrap);
            edits[side]->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            pair->addWidget(edits[side]);
        }
        for (int side = 0; side < 2; ++side) {
            QScrollBar* mine = edits[side]->verticalScrollBar();
            QScrollBar* theirs = edits[1 - side]->verticalScrollBar();
            QObject::connect(mine, &QScrollBar::valueChanged, this, [this, theirs](int value) {
                if (syncing_) return;
                syncing_ = true;
                theirs->setValue(value);
                syncing_ = false;
            });
        }
        return pair;
    };
    inspectTabs_->addTab(makePair(hexEdits_), ::openbc::i18n::t("binary.inspect_hex"));
    inspectTabs_->addTab(makePair(asmEdits_), ::openbc::i18n::t("binary.inspect_asm"));
    layout->addWidget(inspectInfo_);
    layout->addWidget(inspectNotes_);
    layout->addWidget(inspectTabs_, 1);
    clearInspector(::openbc::i18n::t("binary.inspect_hint"));
}

void BinaryCompareViewer::clearInspector(const QString& message) {
    if (!inspector_) return;
    ++inspectSeq_;  // anything still in flight is now stale
    inspectInfo_->setText(message);
    inspectNotes_->hide();
    for (QPlainTextEdit* edit : {hexEdits_[0], hexEdits_[1], asmEdits_[0], asmEdits_[1]}) {
        edit->clear();
        edit->setExtraSelections({});
    }
    inspectTabs_->setTabEnabled(1, false);
}

void BinaryCompareViewer::requestInspect(int row) {
    if (!session_ || !summary_ || row < 0) return;
    const std::uint64_t seq = ++inspectSeq_;
    inspectInfo_->setText(::openbc::i18n::t("binary.inspect_reading"));
    inspectNotes_->hide();
    if (!openbc_binary_inspect(session_, summary_.get(), static_cast<std::size_t>(row), seq))
        clearInspector(::openbc::i18n::t("binary.inspect_unavailable"));
}

void BinaryCompareViewer::applyInspect(const BinaryInspectPtr& inspect) {
    const OpenBcBinaryInspect* s = inspect.get();
    if (!s || !inspector_) return;
    if (openbc_binary_inspect_seq(s) != inspectSeq_) return;  // answer to a row that is no longer selected
    std::size_t length = 0;
    const QString error = textOf(openbc_binary_inspect_error(s, &length), length);
    if (!error.isEmpty()) {
        clearInspector(error);
        return;
    }
    const QString title = textOf(openbc_binary_inspect_title(s, &length), length);

    // Notes (truncation, missing side, hash mismatch, no disassembler).
    QStringList notes;
    for (std::size_t i = 0, n = openbc_binary_inspect_note_count(s); i < n; ++i)
        notes << textOf(openbc_binary_inspect_note(s, i, &length), length);
    inspectNotes_->setText(notes.join(QLatin1Char('\n')));
    inspectNotes_->setVisible(!notes.isEmpty());
    inspectInfo_->setText(::openbc::i18n::t(
        "binary.inspect_summary", title, static_cast<qulonglong>(openbc_binary_inspect_differing_bytes(s)),
        static_cast<qulonglong>(openbc_binary_inspect_length(s, 0)),
        static_cast<qulonglong>(openbc_binary_inspect_length(s, 1))));

    // One side of a pane: text, a tint for every changed line, a stronger one for changed bytes.
    const auto fill = [&](QPlainTextEdit* edit, int side, std::size_t count, bool hex) {
        QStringList lines;
        lines.reserve(static_cast<int>(count));
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* data = hex ? openbc_binary_inspect_hex_text(s, i, static_cast<std::uint8_t>(side), &length)
                                           : openbc_binary_inspect_asm_text(s, i, static_cast<std::uint8_t>(side), &length);
            lines << textOf(data, length);
        }
        edit->setPlainText(lines.join(QLatin1Char('\n')));
        QList<QTextEdit::ExtraSelection> marks;
        const QTextDocument* doc = edit->document();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t state = hex ? openbc_binary_inspect_hex_state(s, i) : openbc_binary_inspect_asm_state(s, i);
            if (state == 0 || state == 255) continue;
            // 2 = only on the left, 3 = only on the right: the other side's line is blank, leave it.
            if ((state == 2 && side == 1) || (state == 3 && side == 0)) continue;
            const QTextBlock block = doc->findBlockByNumber(static_cast<int>(i));
            if (!block.isValid()) continue;
            QTextEdit::ExtraSelection line;
            line.cursor = QTextCursor(block);
            line.format.setBackground(tintFor(state, 55));
            line.format.setProperty(QTextFormat::FullWidthSelection, true);
            marks.append(line);
            if (hex && state == 1) {
                const std::uint32_t mask = openbc_binary_inspect_hex_mask(s, i);
                for (int b = 0; b < 16; ++b) {
                    if (!((mask >> b) & 1u)) continue;
                    QTextEdit::ExtraSelection byte;
                    byte.cursor = QTextCursor(block);
                    byte.cursor.setPosition(block.position() + hexByteColumn(b));
                    byte.cursor.setPosition(block.position() + hexByteColumn(b) + 2, QTextCursor::KeepAnchor);
                    byte.format.setBackground(tintFor(1, 150));
                    marks.append(byte);
                }
            }
        }
        edit->setExtraSelections(marks);
    };
    syncing_ = true;  // loading text moves the scroll bars; do not echo that to the other pane
    const std::size_t hexRows = openbc_binary_inspect_hex_count(s);
    for (int side = 0; side < 2; ++side) fill(hexEdits_[side], side, hexRows, true);
    const bool asmAvailable = openbc_binary_inspect_asm_available(s) != 0;
    const std::size_t asmRows = asmAvailable ? openbc_binary_inspect_asm_count(s) : 0;
    for (int side = 0; side < 2; ++side) {
        if (asmAvailable) fill(asmEdits_[side], side, asmRows, false);
        else { asmEdits_[side]->clear(); asmEdits_[side]->setExtraSelections({}); }
    }
    syncing_ = false;
    inspectTabs_->setTabEnabled(1, asmAvailable);
    inspectTabs_->setCurrentIndex(asmAvailable ? 1 : 0);  // code reads best as instructions
}

}  // namespace openbc::app

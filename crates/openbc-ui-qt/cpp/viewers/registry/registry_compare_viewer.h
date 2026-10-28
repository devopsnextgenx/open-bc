// registry_compare_viewer.h
// ---------------------------------------------------------------------------
// RegistryCompareViewer: compares two Windows registry exports (.reg).
//
// Presentation only: parsing, key/value alignment, copy edits and writing the
// .reg file are done by openbc-engine / openbc-core through the
// openbc_registry_* C ABI. One unified grid (Key/Value | Left | copy | Right):
// the copy column shows a pair of arrows on the first row of each difference
// group, mirroring the copy arrows of the text compare view.
//
// Editing: double-click (or F2 on) a value's Left/Right data cell to change it.
// The edit is sent to the backend (openbc_registry_set_value), which re-diffs and
// answers with a fresh Result. Edited rows are tinted green on the edited side,
// exactly like edited lines in text compare, the side's Save button turns blue,
// and the minimap marks the rows. Ctrl+S saves the active side (the side of the
// current cell). Closing the tab / Reload / Swap / changing a path with unsaved
// edits shows the shared two-sided UnsavedChangesDialog.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QKeySequence>
#include <QSet>
#include <QTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <functional>
#include <memory>

#include "backend/backend_channel.h"
#include "backend/compare_backend_api.h"
#include "common/diff_algorithms.h"
#include "common/diff_grid_common.h"
#include "common/minimap.h"
#include "style/qt_style.h"
#include "common/path_selector.h"
#include "common/unsaved_changes_dialog.h"
#include "common/file_change_controller.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

using RegistryViewPtr = std::shared_ptr<OpenBcRegistryView>;

class RegistryDiffModel : public QAbstractTableModel {
public:
    enum Column { Name = 0, LeftType, LeftData, Copy, RightType, RightData, ColumnCount };
    static constexpr int GroupRole = Qt::UserRole + 1;  // backend group index on a group's first row, else -1

    using QAbstractTableModel::QAbstractTableModel;

    // Called after the user commits a cell edit: (backend view row, side 0/1, new data text).
    std::function<void(int, int, const QString&)> onEdit;

    void setEditable(int side, bool editable) { editable_[side] = editable; }

    // Same tint text compare paints on edited lines.
    static QColor editedColor() { return QColor(40, 170, 90, 170); }

    void setView(const RegistryViewPtr& view, const QVector<int>& visible) {
        beginResetModel();
        view_ = view;
        visible_ = visible;
        pending_.clear();  // the backend result already contains every accepted edit
        firstRowOfGroup_.clear();
        if (view_) {
            for (std::size_t g = 0; g < openbc_registry_group_count(view_.get()); ++g) {
                const int start = static_cast<int>(openbc_registry_group_start(view_.get(), g));
                const int end = static_cast<int>(openbc_registry_group_end(view_.get(), g));
                const int first = diffgrid::firstVisibleAtOrAfter(visible_, start);
                if (first >= 0 && visible_[first] <= end) firstRowOfGroup_.insert(first, static_cast<int>(g));
            }
        }
        endResetModel();
    }

    int viewRow(int modelRow) const { return modelRow >= 0 && modelRow < visible_.size() ? visible_[modelRow] : -1; }
    const QVector<int>& visible() const { return visible_; }
    const QHash<int, int>& groupRows() const { return firstRowOfGroup_; }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : visible_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : ColumnCount; }

    // Drops optimistic values (the backend rejected an edit) and repaints from the backend view.
    void discardPending() {
        if (pending_.isEmpty()) return;
        pending_.clear();
        if (!visible_.isEmpty()) emit dataChanged(index(0, 0), index(visible_.size() - 1, ColumnCount - 1));
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        if (isEditable(index)) f |= Qt::ItemIsEditable;
        return f;
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !onEdit || !isEditable(index)) return false;
        const QString text = value.toString();
        if (text == index.data(Qt::DisplayRole).toString()) return false;  // nothing changed
        const int side = index.column() <= LeftData ? 0 : 1;
        pending_.insert(pendingKey(index.row(), side), text);
        emit dataChanged(index, index);
        onEdit(visible_[index.row()], side, text);
        return true;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static const char* names[ColumnCount] = {"Key / Value", "Left type", "Left data", "", "Right type", "Right data"};
        return QString(names[section]);
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!view_ || !index.isValid()) return {};
        const OpenBcRegistryView* v = view_.get();
        const std::size_t row = static_cast<std::size_t>(visible_[index.row()]);
        std::size_t length = 0;
        const bool keyRow = openbc_registry_row_name(v, row, &length) == nullptr;
        const int kind = openbc_registry_row_kind(v, row);
        const int column = index.column();

        if (role == GroupRole) return firstRowOfGroup_.value(index.row(), -1);
        if (role == Qt::FontRole && column == Name && keyRow) {
            QFont font = QApplication::font();
            font.setBold(true);
            return font;
        }
        if (column == Copy) return {};
        const std::uint8_t side = column <= LeftData ? 0 : 1;
        const bool isType = column == LeftType || column == RightType;

        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            if (column == LeftData || column == RightData) {
                const auto it = pending_.constFind(pendingKey(index.row(), column == LeftData ? 0 : 1));
                if (it != pending_.constEnd()) return *it;
            }
            if (column == Name) {
                if (keyRow) return backendString(openbc_registry_row_key(v, row, &length), length);
                const QString name = backendString(openbc_registry_row_name(v, row, &length), length);
                return "    " + (name == "@" ? QString("(Default)") : name);
            }
            const auto* text = isType ? openbc_registry_row_type(v, row, side, &length) : openbc_registry_row_data(v, row, side, &length);
            return backendString(text, length);
        }
        if (role == Qt::BackgroundRole) {
            if (column == Name) return diffgrid::cellBrush(kind == diffgrid::Changed ? diffgrid::Changed : diffgrid::Equal, true, false);
            const bool present = keyRow ? (side == 0 ? kind != diffgrid::RightOnly : kind != diffgrid::LeftOnly)
                                        : openbc_registry_row_type(v, row, side, &length) != nullptr;
            // Edited on this side (typed here or copied in): same green as edited text lines.
            if (present && (openbc_registry_row_edited(v, row, side) != 0 || pending_.contains(pendingKey(index.row(), side))))
                return QBrush(editedColor());
            return diffgrid::cellBrush(kind, present, !isType);
        }
        return {};
    }

private:
    static quint64 pendingKey(int modelRow, int side) { return (quint64(modelRow) << 1) | quint64(side); }

    // Only the data cell of a value that exists on that side is editable (a missing side is filled with copy).
    bool isEditable(const QModelIndex& index) const {
        if (!view_ || !index.isValid() || (index.column() != LeftData && index.column() != RightData)) return false;
        const int side = index.column() == LeftData ? 0 : 1;
        if (!editable_[side]) return false;
        const std::size_t row = static_cast<std::size_t>(visible_[index.row()]);
        std::size_t length = 0;
        if (openbc_registry_row_name(view_.get(), row, &length) == nullptr) return false;  // key rows have no data
        return openbc_registry_row_type(view_.get(), row, static_cast<std::uint8_t>(side), &length) != nullptr;
    }

    RegistryViewPtr view_;
    QVector<int> visible_;
    QHash<int, int> firstRowOfGroup_;
    QHash<quint64, QString> pending_;  // optimistic cell text until the backend result arrives
    bool editable_[2] = {false, false};
};

// Paints "<- ->" in the copy column on group rows.
class RegistryCopyDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyledItemDelegate::paint(p, option, index);
        if (index.data(RegistryDiffModel::GroupRole).toInt() < 0) return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect.adjusted(3, 2, -3, -2);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0x33, 0x33, 0x3d));
        p->drawRoundedRect(r, 3, 3);
        p->setBrush(QColor(0xe8, 0xea, 0xf4));
        const QPointF c = r.center();
        const qreal q = r.width() / 4.0, s = 4.0;
        p->drawPolygon(QPolygonF({QPointF(c.x() - q + s, c.y() - s), QPointF(c.x() - q - s, c.y()), QPointF(c.x() - q + s, c.y() + s)}));
        p->drawPolygon(QPolygonF({QPointF(c.x() + q - s, c.y() - s), QPointF(c.x() + q + s, c.y()), QPointF(c.x() + q - s, c.y() + s)}));
        p->restore();
    }
};

class RegistryCompareViewer : public QWidget {
public:
    RegistryCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &RegistryCompareViewer::destroyHandle,
                   [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("registryCompareView");
        buildUi();
        changes_ = std::make_unique<FileChangeController>(this, [this](std::uint8_t) { startSession(); }, /*wholeTab=*/true,
                                                          [this]() { return confirmDiscard(); });
        startSession();
    }

    ~RegistryCompareViewer() override { stopSession(); }

    QString title() const {
        const auto name = [](const QString& p) { return p.isEmpty() ? QString("(missing)") : QFileInfo(p).fileName(); };
        return name(leftPath_) + " <-> " + name(rightPath_);
    }
    QString detailedTitle() const { return leftPath_ + "  <->  " + rightPath_; }
    QString leftText() const { return leftPath_; }
    QString rightText() const { return rightPath_; }

    // Drop target entry point (common/drop_router.h): an empty argument leaves that side untouched.
    void applyDroppedPaths(const QString& left, const QString& right) {
        const QString wanted[2] = {cleanPathInput(left), cleanPathInput(right)};
        for (std::uint8_t side = 0; side < 2; ++side) {
            if (wanted[side].isEmpty()) continue;
            commitPath(side, wanted[side]);
            if ((side == 0 ? leftPath_ : rightPath_) != wanted[side]) return;  // refused or cancelled: stop
        }
    }

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    // For the tab host: call before closing the tab. confirmClose() shows the two-sided
    // dialog when there are unsaved edits and returns false if the user cancels.
    bool hasUnsavedChanges() const { return dirty_[0] || dirty_[1]; }
    bool confirmClose() { return confirmDiscard(); }

    QList<QAction*> editMenuItems() const { return {saveAction_, saveLeftAction_, saveRightAction_}; }
    QList<QAction*> searchMenuItems() const { return {nextAction_, prevAction_}; }
    QList<QAction*> viewMenuItems() const { return {showAllAction_, showDiffsAction_, contextAction_}; }
    QList<QAction*> actionsMenuItems() const { return {copyRightAction_, copyLeftAction_, swapAction_, reloadAction_}; }

private:
    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_registry_view_destroy(static_cast<OpenBcRegistryView*>(handle));
    }

    void startSession() {
        stopSession();
        if (changes_) { changes_->track(0, leftPath_); changes_->track(1, rightPath_); }
        view_.reset();
        const QByteArray left = leftPath_.toUtf8(), right = rightPath_.toUtf8();
        session_ = openbc_registry_session_create(left.constData(), right.constData(), BackendChannel::callback(), channel_.user());
        status_->setText("Loading...");
        if (onTitleChanged) onTitleChanged();
    }

    void stopSession() {
        if (!session_) return;
        openbc_registry_session_destroy(session_);
        session_ = nullptr;
    }

    void send(bool accepted) {
        if (!accepted) status_->setText("Backend is busy; try again in a moment.");
    }

    void onEvent(const BackendChannel::Event& event) {
        switch (event.kind) {
        case OPENBC_EV_STATUS:
            status_->setText(event.message);
            break;
        case OPENBC_EV_ERROR:
            status_->setText(event.message);
            model_->discardPending();  // a rejected edit must not keep showing its typed value
            finishSave(false, event.message);
            break;
        case OPENBC_EV_SAVED:
            status_->setText("Saved " + QFileInfo(event.message.section('|', 1)).fileName());
            finishSave(true, {});
            break;
        case OPENBC_EV_RESULT:
            applyView(std::static_pointer_cast<OpenBcRegistryView>(event.handle));
            break;
        default:
            break;
        }
    }

    void applyView(const RegistryViewPtr& view) {
        view_ = view;
        const OpenBcRegistryView* v = view_.get();
        for (std::uint8_t side = 0; side < 2; ++side) {
            const std::uint32_t flags = openbc_registry_side_flags(v, side);
            dirty_[side] = flags & 4;
            editable_[side] = flags & 1;
            model_->setEditable(side, editable_[side]);
            std::size_t length = 0;
            const QString error = backendString(openbc_registry_side_error(v, side, &length), length);
            const QString path = side == 0 ? leftPath_ : rightPath_;
            selectors_[side]->setText(path);
            labels_[side]->setToolTip(error);
            (side == 0 ? saveLeftAction_ : saveRightAction_)->setEnabled(flags & 1);
            refreshSaveUi(side, error);
        }
        rebuildVisible();

        const int rows = static_cast<int>(openbc_registry_view_rows(v));
        const int bucket = std::max(1, (rows + 3999) / 4000);
        QVector<TextDiffLine> overview;
        QSet<int> editedBuckets;  // same green marks the text minimap shows for edited lines
        for (int start = 0; start < rows; start += bucket) {
            bool changed = false;
            for (int i = start; i < std::min(rows, start + bucket); ++i) {
                changed |= openbc_registry_row_kind(v, i) != 0;
                if (openbc_registry_row_edited(v, i, 0) || openbc_registry_row_edited(v, i, 1)) editedBuckets.insert(start / bucket);
            }
            overview.push_back({QString(), QString(), start + 1, start + 1, changed, false});
        }
        minimapBucket_ = bucket;
        miniMap_->setRows(overview);
        miniMap_->setDirtyRows(editedBuckets);

        const int groups = groupCount();
        currentGroup_ = groups ? std::min(currentGroup_, groups - 1) : -1;
        status_->setText(groups ? QString("%1 difference %2.").arg(groups).arg(groups == 1 ? "section" : "sections")
                                : QString("Registry exports match: no differences."));
        if (onTitleChanged) onTitleChanged();
    }

    void rebuildVisible() {
        if (!view_) return;
        const OpenBcRegistryView* v = view_.get();
        const int rows = static_cast<int>(openbc_registry_view_rows(v));
        model_->setView(view_, diffgrid::visibleRows(rows, [v](int i) { return openbc_registry_row_kind(v, i) != 0; }, filter_));
    }

    int groupCount() const { return view_ ? static_cast<int>(openbc_registry_group_count(view_.get())) : 0; }

    void selectGroup(int group) {
        if (!view_ || group < 0 || group >= groupCount()) return;
        const int start = static_cast<int>(openbc_registry_group_start(view_.get(), group));
        const int index = diffgrid::firstVisibleAtOrAfter(model_->visible(), start);
        if (index < 0) return;
        grid_->setCurrentIndex(model_->index(index, 0));
        grid_->scrollTo(model_->index(index, 0), QAbstractItemView::PositionAtCenter);
        currentGroup_ = group;
    }

    void jumpSection(int delta) {
        const int count = groupCount();
        if (!count) return;
        selectGroup(currentGroup_ < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(currentGroup_ + delta, 0, count - 1));
    }

    void copyGroup(int group, bool toRight) {
        if (!view_ || !session_ || group < 0 || group >= groupCount()) return;
        send(openbc_registry_copy(session_, openbc_registry_group_start(view_.get(), group),
                                  openbc_registry_group_end(view_.get(), group), toRight ? 1 : 0));
    }

    // Saves one side in place and reports the outcome through `done` (used by the dialog and Ctrl+S).
    // The backend takes one command at a time, so a save issued right after an edit is retried briefly.
    void startSave(bool left, SaveDone done, int retries = 8) {
        if (!session_) {
            if (done) done(false, "No open comparison.");
            return;
        }
        if (!openbc_registry_save(session_, left ? 1 : 0, nullptr)) {
            if (retries > 0) {
                const QPointer<RegistryCompareViewer> self(this);
                QTimer::singleShot(120, this, [self, left, done, retries]() mutable {
                    if (self) self->startSave(left, std::move(done), retries - 1);
                });
                return;
            }
            status_->setText("Backend is busy; try again in a moment.");
            if (done) done(false, "The backend is busy; try again in a moment.");
            return;
        }
        saveDone_ = std::move(done);
    }

    void finishSave(bool ok, const QString& error) {
        SaveDone done;
        std::swap(done, saveDone_);
        if (done) done(ok, error);
    }

    int changeCount(std::uint8_t side) const {
        if (!dirty_[side]) return 0;
        int count = 0;
        if (view_) {
            const std::size_t rows = openbc_registry_view_rows(view_.get());
            for (std::size_t i = 0; i < rows; ++i) count += openbc_registry_row_edited(view_.get(), i, side) != 0;
        }
        return std::max(count, 1);  // dirty but not row-countable (e.g. a removed key) still counts as one
    }

    // Blue save button + "N unsaved changes", the same cues the text compare gives a side with edits.
    void refreshSaveUi(std::uint8_t side, const QString& error = QString()) {
        const bool dirty = dirty_[side];
        const int count = changeCount(side);
        labels_[side]->setText(!error.isEmpty() ? "<span style='color:#e5534b'>" + error.toHtmlEscaped() + "</span>"
                               : dirty        ? QString("%1 unsaved %2").arg(count).arg(count == 1 ? "change" : "changes")
                                              : QString());
        saves_[side]->setObjectName("saveSideBtn");
        saves_[side]->setIcon(icons::saveIcon(dirty ? QColor(0x4e, 0x9c, 0xff) : QColor(0xdd, 0xe0, 0xee)));
        saves_[side]->setProperty("dirty", dirty);
        saves_[side]->setToolTip(dirty ? "Unsaved changes on this side - click or Ctrl+S to write to disk"
                                       : (side == 0 ? "Save left file (Ctrl+S while this side is active)"
                                                    : "Save right file (Ctrl+S while this side is active)"));
        saves_[side]->style()->unpolish(saves_[side]);
        saves_[side]->style()->polish(saves_[side]);
    }

    // Ctrl+S: the side of the current cell. The Name / copy columns keep the last chosen side.
    void saveActiveSide() {
        commitEditors();
        const std::uint8_t side = static_cast<std::uint8_t>(activeSide_);
        const QString name = side == 0 ? "left" : "right";
        if (!dirty_[side]) {
            status_->setText("Nothing to save on the " + name + " side.");
            return;
        }
        status_->setText("Saving the " + name + " file...");
        startSave(side == 0, {});
    }

    // A cell editor that is still open has not reached the model yet: leaving it commits the text.
    void commitEditors() {
        QWidget* focus = QApplication::focusWidget();
        if (focus && focus != grid_ && grid_->isAncestorOf(focus)) grid_->setFocus();
    }

    // Strips the quotes Explorer's "Copy as path" adds, plus surrounding whitespace.
    static QString cleanPathInput(QString raw) {
        raw = raw.trimmed();
        if (raw.size() >= 2 && raw.startsWith('"') && raw.endsWith('"')) raw = raw.mid(1, raw.size() - 2);
        return raw.trimmed();
    }

    void commitPath(std::uint8_t side, const QString& raw) {
        QString& current = side == 0 ? leftPath_ : rightPath_;
        PathSelector* selector = selectors_[side];
        const QString path = cleanPathInput(raw);
        if (path == current) return;
        if (!path.isEmpty() && !QFileInfo(path).isFile()) {
            status_->setText("Not a file: " + path);
            selector->setText(current);
            return;
        }
        if (!confirmDiscard()) {
            selector->setText(current);
            return;
        }
        current = path;
        if (!path.isEmpty()) selector->remember(path);
        startSession();
    }

    // One dialog for both sides: Save per side (only for a side with changes), Continue, Cancel.
    bool confirmDiscard() {
        commitEditors();
        if (!dirty_[0] && !dirty_[1]) return true;
        return confirmUnsavedChanges(this, {leftPath_, changeCount(0)}, {rightPath_, changeCount(1)},
                                     [this](bool left, SaveDone done) { startSave(left, std::move(done)); });
    }

    void buildUi() {
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
        homeAction_ = toolbar_->addAction(icons::glyph(Glyph::Home), "Home");
        sessionsAction_ = toolbar_->addAction(icons::glyph(Glyph::Sessions), "Sessions");
        toolbar_->addSeparator();
        auto* group = new QActionGroup(this);
        auto addFilter = [&](Glyph glyph, const QString& text, diffgrid::RowFilter filter, bool checked) {
            QAction* action = toolbar_->addAction(icons::glyph(glyph), text);
            action->setCheckable(true);
            action->setChecked(checked);
            group->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, filter]() { filter_ = filter; rebuildVisible(); });
            return action;
        };
        showAllAction_ = addFilter(Glyph::ShowAll, "All", diffgrid::RowFilter::All, true);
        showDiffsAction_ = addFilter(Glyph::ShowDiffs, "Diffs", diffgrid::RowFilter::DiffsOnly, false);
        contextAction_ = addFilter(Glyph::Context, "Context", diffgrid::RowFilter::Context, false);
        toolbar_->addSeparator();
        copyRightAction_ = toolbar_->addAction(icons::glyph(Glyph::CopyLine), "Copy ->");
        copyLeftAction_ = toolbar_->addAction(icons::glyph(Glyph::CopyLine), "<- Copy");
        nextAction_ = toolbar_->addAction(icons::glyph(Glyph::NextSection), "Next Section");
        prevAction_ = toolbar_->addAction(icons::glyph(Glyph::PrevSection), "Prev Section");
        nextAction_->setShortcut(QKeySequence(Qt::Key_F3));
        prevAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
        toolbar_->addSeparator();
        swapAction_ = toolbar_->addAction(icons::glyph(Glyph::Swap), "Swap");
        reloadAction_ = toolbar_->addAction(icons::glyph(Glyph::Refresh), "Reload");
        saveLeftAction_ = new QAction(icons::glyph(Glyph::Save), "Save Left", this);
        saveRightAction_ = new QAction(icons::glyph(Glyph::Save), "Save Right", this);
        // Ctrl+S saves the active side; scoped to this tab (and its children) like text compare's save action.
        saveAction_ = new QAction(icons::glyph(Glyph::Save), "Save", this);
        saveAction_->setShortcut(QKeySequence::Save);
        saveAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        saveAction_->setToolTip("Save the active side (the side of the current cell)");
        addAction(saveAction_);
        root->addWidget(toolbar_);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(6, 2, 6, 2);
        ignoreCase_ = new QCheckBox("Ignore data case", this);
        header->addWidget(ignoreCase_);
        for (int side = 0; side < 2; ++side) {
            selectors_[side] = makeCommitSelector(
                this, side == 0 ? "Left" : "Right", side == 0 ? leftPath_ : rightPath_,
                "Registry exports (*.reg);;All files (*.*)",
                [this, side](QString p) { commitPath(std::uint8_t(side), p); });
            labels_[side] = new QLabel(this);   // status line under the path: unsaved / error
            labels_[side]->setTextFormat(Qt::RichText);
            saves_[side] = new QToolButton(this);
            saves_[side]->setAutoRaise(true);
            saves_[side]->setIcon(icons::glyph(Glyph::Save));
            saves_[side]->setToolTip(side == 0 ? "Save left" : "Save right");
            auto* column = new QVBoxLayout;
            column->setSpacing(2);
            auto* line = new QHBoxLayout;
            line->addWidget(selectors_[side], 1);
            line->addWidget(saves_[side]);
            column->addLayout(line);
            column->addWidget(labels_[side]);
            header->addLayout(column, 1);
        }
        root->addLayout(header);

        auto* body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        miniMap_ = new DifferenceOverview(this);
        miniMap_->setFixedWidth(22);
        model_ = new RegistryDiffModel(this);
        model_->onEdit = [this](int viewRow, int side, const QString& text) {
            if (!session_) return;
            const QByteArray bytes = text.toUtf8();
            send(openbc_registry_set_value(session_, static_cast<std::size_t>(viewRow), static_cast<std::uint8_t>(side), bytes.constData()));
            dirty_[side] = true;  // until the backend's Result confirms it, so closing right now still asks
            refreshSaveUi(static_cast<std::uint8_t>(side));
        };
        grid_ = new QTableView(this);
        grid_->setModel(model_);
        grid_->setItemDelegateForColumn(RegistryDiffModel::Copy, new RegistryCopyDelegate(grid_));
        grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid_->setSelectionMode(QAbstractItemView::SingleSelection);
        grid_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        grid_->setWordWrap(false);
        grid_->verticalHeader()->setDefaultSectionSize(22);
        grid_->verticalHeader()->hide();
        grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        grid_->horizontalHeader()->setStretchLastSection(true);
        grid_->setColumnWidth(RegistryDiffModel::Name, 360);
        for (int c : {RegistryDiffModel::LeftType, RegistryDiffModel::RightType}) grid_->setColumnWidth(c, 110);
        for (int c : {RegistryDiffModel::LeftData, RegistryDiffModel::RightData}) grid_->setColumnWidth(c, 260);
        grid_->setColumnWidth(RegistryDiffModel::Copy, 44);
        body->addWidget(miniMap_);
        body->addWidget(grid_, 1);
        root->addLayout(body, 1);
        status_ = new QLabel(this);
        status_->setContentsMargins(6, 2, 6, 2);
        root->addWidget(status_);

        QObject::connect(grid_, &QTableView::clicked, this, [this](const QModelIndex& index) {
            const int group = index.data(RegistryDiffModel::GroupRole).toInt();
            if (group >= 0) currentGroup_ = group;
            if (index.column() != RegistryDiffModel::Copy || group < 0) return;
            const QPoint local = grid_->viewport()->mapFromGlobal(QCursor::pos());
            copyGroup(group, local.x() >= grid_->visualRect(index).center().x());
        });
        QObject::connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
            const int column = current.column();
            if (column == RegistryDiffModel::LeftType || column == RegistryDiffModel::LeftData) activeSide_ = 0;
            else if (column == RegistryDiffModel::RightType || column == RegistryDiffModel::RightData) activeSide_ = 1;
        });
        QObject::connect(grid_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex& current, const QModelIndex&) {
            const int row = model_->viewRow(current.row());
            for (const auto g : model_->groupRows().keys()) (void)g;
            if (!view_ || row < 0) return;
            for (int g = 0; g < groupCount(); ++g) {
                if (row >= static_cast<int>(openbc_registry_group_start(view_.get(), g)) && row <= static_cast<int>(openbc_registry_group_end(view_.get(), g))) {
                    currentGroup_ = g;
                    break;
                }
            }
        });
        miniMap_->onRowClicked = [this](int bucketRow) {
            const int index = diffgrid::firstVisibleAtOrAfter(model_->visible(), bucketRow * minimapBucket_);
            if (index >= 0) grid_->scrollTo(model_->index(index, 0), QAbstractItemView::PositionAtCenter);
        };
        QObject::connect(ignoreCase_, &QCheckBox::toggled, this, [this](bool on) {
            if (session_) send(openbc_registry_set_options(session_, on ? 1 : 0));
        });
        for (int side = 0; side < 2; ++side) QObject::connect(saves_[side], &QToolButton::clicked, this, [this, side]() { commitEditors(); startSave(side == 0, {}); });
        QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
        QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
        QObject::connect(copyRightAction_, &QAction::triggered, this, [this]() { copyGroup(currentGroup_, true); });
        QObject::connect(copyLeftAction_, &QAction::triggered, this, [this]() { copyGroup(currentGroup_, false); });
        QObject::connect(nextAction_, &QAction::triggered, this, [this]() { jumpSection(+1); });
        QObject::connect(prevAction_, &QAction::triggered, this, [this]() { jumpSection(-1); });
        QObject::connect(swapAction_, &QAction::triggered, this, [this]() {
            if (!confirmDiscard()) return;
            std::swap(leftPath_, rightPath_);
            startSession();
        });
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() { if (session_ && confirmDiscard()) send(openbc_registry_reload(session_)); });
        QObject::connect(saveLeftAction_, &QAction::triggered, this, [this]() { commitEditors(); startSave(true, {}); });
        QObject::connect(saveRightAction_, &QAction::triggered, this, [this]() { commitEditors(); startSave(false, {}); });
        QObject::connect(saveAction_, &QAction::triggered, this, [this]() { saveActiveSide(); });
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcRegistrySession* session_ = nullptr;
    RegistryViewPtr view_;
    int currentGroup_ = -1;
    int minimapBucket_ = 1;
    bool dirty_[2] = {false, false};
    bool editable_[2] = {false, false};
    int activeSide_ = 0;     // side of the current cell; target of Ctrl+S
    SaveDone saveDone_;      // completion of the save currently in flight (dialog / Ctrl+S)
    diffgrid::RowFilter filter_ = diffgrid::RowFilter::All;

    QToolBar* toolbar_ = nullptr;
    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* showAllAction_ = nullptr;
    QAction* showDiffsAction_ = nullptr;
    QAction* contextAction_ = nullptr;
    QAction* copyRightAction_ = nullptr;
    QAction* copyLeftAction_ = nullptr;
    QAction* nextAction_ = nullptr;
    QAction* prevAction_ = nullptr;
    QAction* swapAction_ = nullptr;
    QAction* reloadAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveLeftAction_ = nullptr;
    QAction* saveRightAction_ = nullptr;
    QCheckBox* ignoreCase_ = nullptr;
    PathSelector* selectors_[2] = {nullptr, nullptr};
    std::unique_ptr<FileChangeController> changes_;  // external-change notice + reload (common/file_change_controller.h)
    QLabel* labels_[2] = {nullptr, nullptr};
    QToolButton* saves_[2] = {nullptr, nullptr};
    RegistryDiffModel* model_ = nullptr;
    QTableView* grid_ = nullptr;
    DifferenceOverview* miniMap_ = nullptr;
    QLabel* status_ = nullptr;
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app
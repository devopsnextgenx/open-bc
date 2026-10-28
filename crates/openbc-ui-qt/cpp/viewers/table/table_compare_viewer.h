// table_compare_viewer.h
// ---------------------------------------------------------------------------
// TableCompareViewer: the CSV / Excel (xlsx, xls, xlsb, ods) compare tab.
//
// Presentation only. Loading, decoding, row alignment, cell diffing, copy edits
// and saving all happen in openbc-engine / openbc-compute / openbc-core; this
// header reads the finished result through the openbc_table_* C ABI
// (compare_backend_api.h) and renders it the way TextCompareViewer renders text:
//   * difference groups with one pair of copy arrows each (CopyGutter),
//   * All / Diffs / Context filtering, next / previous section, minimap,
//   * per-side unsaved indicator and Save (CSV in place; spreadsheets are
//     read-only, so their Save is "export sheet as CSV").
// Editing (CSV sides): double-click or F2 a cell. The edit goes to the backend
// (openbc_table_set_cell), which re-diffs and answers with a fresh Result. Edited
// rows are tinted green on the edited side and marked on the minimap, like edited
// lines in text compare; the side's Save button turns blue. Ctrl+S saves the
// active side (the side whose grid / header has focus). Closing the tab, Reload,
// Swap and path changes with unsaved edits show the shared two-sided
// UnsavedChangesDialog.
// Header-only and free of Q_OBJECT, like the rest of the app.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
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
#include "common/remote_selector.h"
#include "common/unsaved_changes_dialog.h"
#include "common/file_change_controller.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

using TableViewPtr = std::shared_ptr<OpenBcTableView>;

// One side of the comparison as a read-only model over the backend result.
class TableDiffModel : public QAbstractTableModel {
public:
    TableDiffModel(std::uint8_t side, QObject* parent) : QAbstractTableModel(parent), side_(side) {}

    // Called after the user commits a cell edit: (backend view row, column, new text).
    std::function<void(int, int, const QString&)> onEdit;
    void setEditable(bool editable) { editable_ = editable; }

    // Same tint text compare paints on edited lines.
    static QColor editedColor() { return QColor(40, 170, 90, 170); }

    void setView(const TableViewPtr& view, const QVector<int>& visible, int columns) {
        beginResetModel();
        pending_.clear();  // the backend result already contains every accepted edit
        pendingRows_.clear();
        view_ = view;
        visible_ = visible;
        columns_ = columns;
        endResetModel();
    }

    int viewRow(int modelRow) const { return modelRow >= 0 && modelRow < visible_.size() ? visible_[modelRow] : -1; }
    const QVector<int>& visible() const { return visible_; }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : visible_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : columns_; }

    // Drops optimistic values (the backend rejected an edit) and repaints from the backend view.
    void discardPending() {
        if (pending_.isEmpty()) return;
        pending_.clear();
        pendingRows_.clear();
        if (!visible_.isEmpty() && columns_ > 0) emit dataChanged(index(0, 0), index(visible_.size() - 1, columns_ - 1));
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        // Only real cells of an editable (CSV) side; the empty half of an inserted/missing row is not a cell.
        if (editable_ && view_ && index.isValid() &&
            openbc_table_row_number(view_.get(), static_cast<std::size_t>(visible_[index.row()]), side_) != 0)
            f |= Qt::ItemIsEditable;
        return f;
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role) override {
        if (role != Qt::EditRole || !onEdit || !(flags(index) & Qt::ItemIsEditable)) return false;
        const QString text = value.toString();
        if (text == index.data(Qt::DisplayRole).toString()) return false;  // nothing changed
        pending_.insert(pendingKey(index.row(), index.column()), text);
        pendingRows_.insert(index.row());
        emit dataChanged(index.sibling(index.row(), 0), index.sibling(index.row(), columns_ - 1));
        onEdit(visible_[index.row()], index.column(), text);
        return true;
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!view_ || !index.isValid()) return {};
        const std::size_t row = static_cast<std::size_t>(visible_[index.row()]);
        const std::size_t col = static_cast<std::size_t>(index.column());
        const std::uint32_t number = openbc_table_row_number(view_.get(), row, side_);
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            if (!number) return {};
            const auto it = pending_.constFind(pendingKey(index.row(), index.column()));
            if (it != pending_.constEnd()) return *it;
            std::size_t length = 0;
            const auto* text = openbc_table_cell(view_.get(), row, side_, col, &length);
            return backendString(text, length);
        }
        if (role == Qt::BackgroundRole) {
            // Edited on this side (typed here or copied in): the same green as edited text lines.
            if (number != 0 && (pendingRows_.contains(index.row()) || openbc_table_row_edited(view_.get(), row, side_) != 0))
                return QBrush(editedColor());
            const int kind = openbc_table_row_kind(view_.get(), row);
            const bool cellChanged = kind == diffgrid::Changed && openbc_table_cell_changed(view_.get(), row, col);
            return diffgrid::cellBrush(kind, number != 0, cellChanged);
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole) return {};
        if (orientation == Qt::Horizontal) return diffgrid::columnLetters(section);
        if (!view_ || section >= visible_.size()) return {};
        const std::uint32_t number = openbc_table_row_number(view_.get(), static_cast<std::size_t>(visible_[section]), side_);
        return number ? QVariant(number) : QVariant(QString());
    }

private:
    static quint64 pendingKey(int modelRow, int column) { return (quint64(quint32(modelRow)) << 32) | quint32(column); }

    std::uint8_t side_;
    TableViewPtr view_;
    QVector<int> visible_;
    int columns_ = 0;
    bool editable_ = false;
    QHash<quint64, QString> pending_;  // optimistic cell text until the backend result arrives
    QSet<int> pendingRows_;
};

class TableCompareViewer : public QWidget {
public:
    TableCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &TableCompareViewer::destroyHandle,
                   [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("tableCompareView");
        buildUi();
        changes_ = std::make_unique<FileChangeController>(this, [this](std::uint8_t) { reloadLatest(); }, /*wholeTab=*/true,
                                                          [this]() { return confirmDiscard(); });
        startSession();
    }

    // The backend guarantees no callbacks once the session is destroyed.
    ~TableCompareViewer() override { stopSession(); }

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
    QList<QAction*> viewMenuItems() const { return {showAllAction_, showDiffsAction_, contextAction_, pixelMinimapAction_}; }
    QList<QAction*> actionsMenuItems() const { return {copyRightAction_, copyLeftAction_, swapAction_, reloadAction_}; }

private:
    // ---------------------------------------------------------------- session
    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_table_view_destroy(static_cast<OpenBcTableView*>(handle));
    }

    void startSession() {
        stopSession();
        if (changes_) { changes_->track(0, leftPath_); changes_->track(1, rightPath_); }
        view_.reset();
        const QByteArray left = openbc_remote::localCopy(leftPath_).toUtf8();
        const QByteArray right = openbc_remote::localCopy(rightPath_).toUtf8();
        session_ = openbc_table_session_create(left.constData(), right.constData(), BackendChannel::callback(), channel_.user());
        status_->setText("Loading...");
        if (onTitleChanged) onTitleChanged();
    }

    // "Reload Latest" after an external change: remote files are downloaded again, then everything is re-read.
    void reloadLatest() {
        QString local, error;
        for (const QString& path : {leftPath_, rightPath_}) {
            if (path.isEmpty()) continue;
            if (!openbc_remote::resolveLocalPath(path, &local, &error, /*refresh=*/true)) { status_->setText(error); return; }
        }
        startSession();
    }

    void stopSession() {
        if (!session_) return;
        openbc_table_session_destroy(session_);
        session_ = nullptr;
    }

    void send(bool accepted) {
        if (!accepted) status_->setText("Backend is busy; try again in a moment.");
    }

    // Window-level filter: keeps the minimap's viewport box right when the grid is resized.
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize && grids_[0] && watched == grids_[0]->viewport()) updateViewport();
        return QWidget::eventFilter(watched, event);
    }

    void onEvent(const BackendChannel::Event& event) {
        switch (event.kind) {
        case OPENBC_EV_STATUS:
            status_->setText(event.message);
            break;
        case OPENBC_EV_ERROR:
            status_->setText(event.message);
            for (TableDiffModel* model : {models_[0], models_[1]}) model->discardPending();  // a rejected edit must not keep its typed value
            finishSave(false, event.message);
            break;
        case OPENBC_EV_SAVED: {
            const QStringList parts = event.message.split('|');
            status_->setText("Saved " + QFileInfo(parts.value(1)).fileName());
            finishSave(true, {});
            break;
        }
        case OPENBC_EV_RESULT:
            applyView(std::static_pointer_cast<OpenBcTableView>(event.handle));
            break;
        default:
            break;
        }
    }

    // --------------------------------------------------------------- rendering
    void applyView(const TableViewPtr& view) {
        view_ = view;
        const OpenBcTableView* v = view_.get();
        const int rows = static_cast<int>(openbc_table_view_rows(v));
        columns_ = static_cast<int>(openbc_table_view_columns(v));
        for (std::uint8_t side = 0; side < 2; ++side) refreshSide(side);
        rebuildVisible();

        // The minimap is capped at a few thousand buckets so huge tables stay cheap.
        const int bucket = std::max(1, (rows + 3999) / 4000);
        QVector<TextDiffLine> overview;
        QSet<int> editedBuckets;  // same green marks the text minimap shows for edited lines
        for (int start = 0; start < rows; start += bucket) {
            bool changed = false;
            int leftNumber = 0, rightNumber = 0;
            for (int i = start; i < std::min(rows, start + bucket); ++i) {
                changed |= openbc_table_row_kind(v, i) != 0;
                if (openbc_table_row_edited(v, i, 0) || openbc_table_row_edited(v, i, 1)) editedBuckets.insert(start / bucket);
                if (!leftNumber) leftNumber = static_cast<int>(openbc_table_row_number(v, i, 0));
                if (!rightNumber) rightNumber = static_cast<int>(openbc_table_row_number(v, i, 1));
            }
            overview.push_back({QString(), QString(), leftNumber, rightNumber, changed, false});
        }
        minimapBucket_ = bucket;
        miniMap_->setRows(overview);
        miniMap_->setDirtyRows(editedBuckets);
        updateViewport();

        const int groups = static_cast<int>(openbc_table_group_count(v));
        currentGroup_ = groups ? std::min(currentGroup_, groups - 1) : -1;
        gutter_->setCurrentGroup(currentGroup_);
        status_->setText(groups == 0 ? QString("Tables match: no differences.")
                                     : QString("%1 difference %2 across %3 rows.")
                                           .arg(groups)
                                           .arg(groups == 1 ? "section" : "sections")
                                           .arg(rows));
        const bool sameName = std::all_of(sides_, sides_ + 2, [](const SidePane& s) { return s.sheets->count() > 0; });
        (void)sameName;
        if (onTitleChanged) onTitleChanged();
    }

    void refreshSide(std::uint8_t side) {
        const OpenBcTableView* v = view_.get();
        SidePane& pane = sides_[side];
        {
            const QSignalBlocker blocker(pane.sheets);
            pane.sheets->clear();
            const std::size_t count = openbc_table_sheet_count(v, side);
            for (std::size_t i = 0; i < count; ++i) {
                std::size_t length = 0;
                const auto* name = openbc_table_sheet_name(v, side, i, &length);
                pane.sheets->addItem(backendString(name, length));
            }
            pane.sheets->setCurrentIndex(openbc_table_selected_sheet(v, side));
            pane.sheets->setVisible(count > 1);
        }
        const std::uint32_t flags = openbc_table_side_flags(v, side);
        const bool loaded = flags & 1, editable = flags & 2;
        dirty_[side] = flags & 4;
        std::size_t errorLength = 0;
        const QString error = backendString(openbc_table_side_error(v, side, &errorLength), errorLength);
        const QString path = side == 0 ? leftPath_ : rightPath_;
        pane.path->setText(path);
        pane.path->setToolTip(path);
        editable_[side] = editable;
        models_[side]->setEditable(editable);
        loaded_[side] = loaded;
        errors_[side] = error;
        refreshSaveUi(side);
        (side == 0 ? saveLeftAction_ : saveRightAction_)->setEnabled(loaded);
        gutter_->setEnabledSides(true, true);
    }

    int changeCount(std::uint8_t side) const {
        if (!dirty_[side]) return 0;
        int count = 0;
        if (view_) {
            const std::size_t rows = openbc_table_view_rows(view_.get());
            for (std::size_t i = 0; i < rows; ++i) count += openbc_table_row_edited(view_.get(), i, side) != 0;
        }
        return std::max(count, 1);  // dirty but not row-countable (e.g. a removed row) still counts as one
    }

    // Blue save button + "N unsaved changes", the same cues the text compare gives a side with edits.
    void refreshSaveUi(std::uint8_t side) {
        SidePane& pane = sides_[side];
        const QString path = side == 0 ? leftPath_ : rightPath_;
        const bool dirty = dirty_[side];
        const int count = changeCount(side);
        const QString changes = QString("%1 unsaved %2").arg(count).arg(count == 1 ? "change" : "changes");
        if (!errors_[side].isEmpty()) pane.info->setText("<span style='color:#e5534b'>" + errors_[side].toHtmlEscaped() + "</span>");
        else if (!loaded_[side]) pane.info->setText(path.isEmpty() ? "No file" : "Not loaded");
        else pane.info->setText(QString(editable_[side] ? "CSV" : "Spreadsheet (read-only)") + (dirty ? " - " + changes : QString()));
        pane.save->setObjectName("saveSideBtn");
        pane.save->setIcon(icons::saveIcon(dirty ? QColor(0x4e, 0x9c, 0xff) : QColor(0xdd, 0xe0, 0xee)));
        pane.save->setProperty("dirty", dirty);
        pane.save->setToolTip(dirty ? (editable_[side] ? "Unsaved changes on this side - click or Ctrl+S to write to disk"
                                                       : "Unsaved changes - click or Ctrl+S to export this sheet as CSV...")
                                    : (editable_[side] ? "Save (Ctrl+S while this side is active)" : "Export this sheet as CSV..."));
        pane.save->setEnabled(loaded_[side] || !path.isEmpty());
        pane.save->style()->unpolish(pane.save);
        pane.save->style()->polish(pane.save);
    }

    void rebuildVisible() {
        if (!view_) return;
        const OpenBcTableView* v = view_.get();
        const int rows = static_cast<int>(openbc_table_view_rows(v));
        const QVector<int> visible = diffgrid::visibleRows(rows, [v](int i) { return openbc_table_row_kind(v, i) != 0; }, filter_);
        for (TableDiffModel* model : {models_[0], models_[1]}) model->setView(view_, visible, columns_);
        updateGutter();
    }

    int groupCount() const { return view_ ? static_cast<int>(openbc_table_group_count(view_.get())) : 0; }

    void updateGutter() {
        QVector<diffgrid::GutterGroup> groups;
        const QVector<int>& visible = models_[0]->visible();
        for (int g = 0; g < groupCount(); ++g) {
            const int start = static_cast<int>(openbc_table_group_start(view_.get(), g));
            const int end = static_cast<int>(openbc_table_group_end(view_.get(), g));
            const int first = diffgrid::firstVisibleAtOrAfter(visible, start);
            if (first < 0 || visible[first] > end) continue;
            int last = first;
            while (last + 1 < visible.size() && visible[last + 1] <= end) ++last;
            groups.push_back({first, last, g});
        }
        gutter_->setGroups(groups, currentGroup_);
    }

    int groupContaining(int diffRow) const {
        if (!view_) return -1;
        int lo = 0, hi = groupCount() - 1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (diffRow < static_cast<int>(openbc_table_group_start(view_.get(), mid))) hi = mid - 1;
            else if (diffRow > static_cast<int>(openbc_table_group_end(view_.get(), mid))) lo = mid + 1;
            else return mid;
        }
        return -1;
    }

    // -------------------------------------------------------------- navigation
    void selectGroup(int group) {
        if (!view_ || group < 0 || group >= groupCount()) return;
        const int start = static_cast<int>(openbc_table_group_start(view_.get(), group));
        const int visibleIndex = diffgrid::firstVisibleAtOrAfter(models_[0]->visible(), start);
        if (visibleIndex < 0) return;
        const QModelIndex index = models_[0]->index(visibleIndex, 0);
        grids_[0]->setCurrentIndex(index);
        grids_[0]->scrollTo(index, QAbstractItemView::PositionAtCenter);
        currentGroup_ = group;
        gutter_->setCurrentGroup(group);
    }

    void jumpSection(int delta) {
        const int count = groupCount();
        if (!count) return;
        selectGroup(currentGroup_ < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(currentGroup_ + delta, 0, count - 1));
    }

    void copyGroup(int group, bool toRight) {
        if (!view_ || !session_ || group < 0 || group >= groupCount()) return;
        const auto start = openbc_table_group_start(view_.get(), group);
        const auto end = openbc_table_group_end(view_.get(), group);
        send(openbc_table_copy(session_, start, end, toRight ? 1 : 0));
    }

    // ------------------------------------------------------------------ actions
    void sendOptions() {
        if (!session_) return;
        QVector<std::uint32_t> keys;
        for (const QString& token : keysEdit_->text().split(',', Qt::SkipEmptyParts)) {
            const int column = diffgrid::columnFromToken(token);
            if (column >= 0) keys.push_back(static_cast<std::uint32_t>(column));
        }
        send(openbc_table_set_options(session_, ignoreCase_->isChecked(), trim_->isChecked(), tolerance_->value(),
                                      keys.isEmpty() ? nullptr : keys.constData(), static_cast<std::size_t>(keys.size())));
    }

    // Saves one side. A CSV side is written in place; a spreadsheet side (read-only format) is
    // exported as CSV, so it always asks for a path. `done` reports the outcome (dialog / Ctrl+S).
    void saveSide(bool left, bool askForPath, SaveDone done = {}) {
        if (!session_) {
            if (done) done(false, "No open comparison.");
            return;
        }
        const std::uint8_t side = left ? 0 : 1;
        QByteArray target;
        if (askForPath || !editable_[side]) {
            const QString source = left ? leftPath_ : rightPath_;
            const QString suggested = QFileInfo(source).completeBaseName() + ".csv";
            const QString chosen = QFileDialog::getSaveFileName(this, "Save sheet as CSV", suggested, "CSV files (*.csv)");
            if (chosen.isEmpty()) {
                if (done) done(false, "Save cancelled.");
                return;
            }
            target = chosen.toUtf8();
        }
        submitSave(left, target, std::move(done), 8);
    }

    // The backend takes one command at a time, so a save issued right after an edit is retried briefly.
    void submitSave(bool left, const QByteArray& target, SaveDone done, int retries) {
        if (!session_) {
            if (done) done(false, "No open comparison.");
            return;
        }
        if (!openbc_table_save(session_, left ? 1 : 0, target.isEmpty() ? nullptr : target.constData())) {
            if (retries > 0) {
                const QPointer<TableCompareViewer> self(this);
                QTimer::singleShot(120, this, [self, left, target, done, retries]() mutable {
                    if (self) self->submitSave(left, target, std::move(done), retries - 1);
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

    // Ctrl+S: the side whose grid or header last had focus.
    void saveActiveSide() {
        commitEditors();
        const std::uint8_t side = static_cast<std::uint8_t>(activeSide_);
        const QString name = side == 0 ? "left" : "right";
        if (!dirty_[side]) {
            status_->setText("Nothing to save on the " + name + " side.");
            return;
        }
        status_->setText("Saving the " + name + " file...");
        saveSide(side == 0, false);
    }

    // A cell editor that is still open has not reached the model yet: leaving it commits the text.
    void commitEditors() {
        QWidget* focus = QApplication::focusWidget();
        for (QTableView* grid : grids_)
            if (focus && focus != grid && grid->isAncestorOf(focus)) grid->setFocus();
    }

    // One dialog for both sides: Save per side (only for a side with changes), Continue, Cancel.
    bool confirmDiscard() {
        commitEditors();
        if (!dirty_[0] && !dirty_[1]) return true;
        return confirmUnsavedChanges(this, {leftPath_, changeCount(0)}, {rightPath_, changeCount(1)},
                                     [this](bool left, SaveDone done) { saveSide(left, false, std::move(done)); });
    }

    // Strips the quotes Explorer's "Copy as path" adds, plus surrounding whitespace.
    static QString cleanPathInput(QString raw) {
        raw = raw.trimmed();
        if (raw.size() >= 2 && raw.startsWith('"') && raw.endsWith('"')) raw = raw.mid(1, raw.size() - 2);
        return raw.trimmed();
    }

    void commitPath(std::uint8_t side, const QString& raw) {
        QString& current = side == 0 ? leftPath_ : rightPath_;
        PathSelector* selector = sides_[side].path;
        const QString path = cleanPathInput(raw);
        if (path == current) return;
        QString localPath, resolveError;
        if (!path.isEmpty() && !openbc_remote::resolveLocalPath(path, &localPath, &resolveError)) {
            status_->setText(resolveError);
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

    void swapSides() {
        if (!confirmDiscard()) return;
        std::swap(leftPath_, rightPath_);
        startSession();
    }

    void reload() {
        if (session_ && confirmDiscard()) send(openbc_table_reload(session_));
    }

    void setFilter(diffgrid::RowFilter filter) {
        filter_ = filter;
        rebuildVisible();
    }

    // ----------------------------------------------------------------------- UI
    struct SidePane {
        QWidget* box = nullptr;  // whole side (header + grid): focus anywhere in it makes this the active side
        PathSelector* path = nullptr;
        QLabel* info = nullptr;
        QComboBox* sheets = nullptr;
        QToolButton* save = nullptr;
    };

    QWidget* buildSide(std::uint8_t side) {
        auto* box = new QWidget(this);
        auto* column = new QVBoxLayout(box);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);

        SidePane& pane = sides_[side];
        pane.box = box;
        auto* header = new QHBoxLayout;
        header->setContentsMargins(4, 2, 4, 0);
        pane.path = openbc_remote::makeRemoteFileSelector(
            box, side == 0 ? "Left" : "Right", side == 0 ? leftPath_ : rightPath_,
            "Tables (*.csv *.tsv *.xlsx *.xlsm *.xlsb *.xls *.ods);;All files (*.*)",
            [this, side](QString p) { commitPath(side, p); });
        pane.sheets = new QComboBox(box);
        pane.sheets->setToolTip("Sheet");
        pane.save = new QToolButton(box);
        pane.save->setAutoRaise(true);
        pane.save->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new QMenu(pane.save);
        menu->addAction("Save As CSV...", this, [this, side]() { saveSide(side == 0, true); });
        pane.save->setMenu(menu);
        header->addWidget(pane.path, 1);
        header->addWidget(pane.sheets);
        header->addWidget(pane.save);
        column->addLayout(header);
        pane.info = new QLabel(box);
        pane.info->setContentsMargins(4, 0, 4, 0);
        column->addWidget(pane.info);

        models_[side] = new TableDiffModel(side, this);
        models_[side]->onEdit = [this, side](int viewRow, int column, const QString& text) {
            if (!session_) return;
            const QByteArray bytes = text.toUtf8();
            send(openbc_table_set_cell(session_, side, static_cast<std::size_t>(viewRow), static_cast<std::size_t>(column), bytes.constData()));
            dirty_[side] = true;  // until the backend's Result confirms it, so closing right now still asks
            refreshSaveUi(side);
        };
        auto* grid = new QTableView(box);
        grid->setModel(models_[side]);
        grid->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid->setSelectionMode(QAbstractItemView::SingleSelection);
        grid->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);  // CSV sides only (model flags)
        grid->setWordWrap(false);
        grid->setTextElideMode(Qt::ElideRight);
        grid->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        grid->verticalHeader()->setDefaultSectionSize(22);
        grid->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        grid->horizontalHeader()->setDefaultSectionSize(110);
        grids_[side] = grid;
        column->addWidget(grid, 1);

        connect_(pane.sheets, side);
        return box;
    }

    void connect_(QComboBox* combo, std::uint8_t side) {
        QObject::connect(combo, qOverload<int>(&QComboBox::activated), this, [this, side](int index) {
            if (session_) send(openbc_table_select_sheets(session_, side == 0 ? index : -1, side == 1 ? index : -1));
        });
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

        auto* filterGroup = new QActionGroup(this);
        auto addFilter = [&](Glyph glyph, const QString& text, diffgrid::RowFilter filter, bool checked) {
            QAction* action = toolbar_->addAction(icons::glyph(glyph), text);
            action->setCheckable(true);
            action->setChecked(checked);
            filterGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, filter]() { setFilter(filter); });
            return action;
        };
        showAllAction_ = addFilter(Glyph::ShowAll, "All", diffgrid::RowFilter::All, true);
        showDiffsAction_ = addFilter(Glyph::ShowDiffs, "Diffs", diffgrid::RowFilter::DiffsOnly, false);
        contextAction_ = addFilter(Glyph::Context, "Context", diffgrid::RowFilter::Context, false);
        toolbar_->addSeparator();

        copyRightAction_ = toolbar_->addAction(icons::glyph(Glyph::CopyLine), "Copy ->");
        copyRightAction_->setToolTip("Copy the current difference left -> right");
        copyLeftAction_ = toolbar_->addAction(icons::glyph(Glyph::CopyLine), "<- Copy");
        copyLeftAction_->setToolTip("Copy the current difference right -> left");
        nextAction_ = toolbar_->addAction(icons::glyph(Glyph::NextSection), "Next Section");
        prevAction_ = toolbar_->addAction(icons::glyph(Glyph::PrevSection), "Prev Section");
        nextAction_->setShortcut(QKeySequence(Qt::Key_F3));
        prevAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
        toolbar_->addSeparator();
        swapAction_ = toolbar_->addAction(icons::glyph(Glyph::Swap), "Swap");
        reloadAction_ = toolbar_->addAction(icons::glyph(Glyph::Refresh), "Reload");
        toolbar_->addSeparator();
        pixelMinimapAction_ = toolbar_->addAction(icons::glyph(Glyph::MinimapPixel), "Minimap");
        pixelMinimapAction_->setCheckable(true);
        pixelMinimapAction_->setChecked(true);
        pixelMinimapAction_->setToolTip("Minimap: one pixel-thin line per differing row, instead of one bar per change block");
        saveLeftAction_ = new QAction(icons::glyph(Glyph::Save), "Save Left", this);
        saveRightAction_ = new QAction(icons::glyph(Glyph::Save), "Save Right", this);
        // Ctrl+S saves the active side; scoped to this tab (and its children) like text compare's save action.
        saveAction_ = new QAction(icons::glyph(Glyph::Save), "Save", this);
        saveAction_->setShortcut(QKeySequence::Save);
        saveAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        saveAction_->setToolTip("Save the active side (the side whose grid has focus)");
        addAction(saveAction_);
        root->addWidget(toolbar_);

        // Comparison options, applied by the backend (never evaluated here).
        auto* options = new QWidget(this);
        auto* optionsRow = new QHBoxLayout(options);
        optionsRow->setContentsMargins(6, 2, 6, 2);
        ignoreCase_ = new QCheckBox("Ignore case", options);
        trim_ = new QCheckBox("Trim cells", options);
        tolerance_ = new QDoubleSpinBox(options);
        tolerance_->setDecimals(6);
        tolerance_->setRange(0.0, 1e12);
        tolerance_->setToolTip("Numeric cells closer than this are equal");
        keysEdit_ = new QLineEdit(options);
        keysEdit_->setPlaceholderText("Key columns, e.g. A,C (blank: align by row order)");
        keysEdit_->setClearButtonEnabled(true);
        optionsRow->addWidget(ignoreCase_);
        optionsRow->addWidget(trim_);
        optionsRow->addWidget(new QLabel("Tolerance", options));
        optionsRow->addWidget(tolerance_);
        optionsRow->addWidget(keysEdit_, 1);
        root->addWidget(options);

        optionsTimer_ = new QTimer(this);
        optionsTimer_->setSingleShot(true);
        optionsTimer_->setInterval(300);
        QObject::connect(optionsTimer_, &QTimer::timeout, this, [this]() { sendOptions(); });
        const auto restart = [this]() { optionsTimer_->start(); };
        QObject::connect(ignoreCase_, &QCheckBox::toggled, this, restart);
        QObject::connect(trim_, &QCheckBox::toggled, this, restart);
        QObject::connect(tolerance_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, restart);
        QObject::connect(keysEdit_, &QLineEdit::textChanged, this, restart);

        auto* body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        // Built exactly like text compare's minimap (second argument true), in pixel-line mode by default.
        miniMap_ = new DifferenceOverview(this, true);
        miniMap_->setFixedWidth(22);
        miniMap_->setPixelLineMode(true);
        body->addWidget(miniMap_);
        body->addWidget(buildSide(0), 1);
        gutter_ = new diffgrid::CopyGutter(grids_[0], this);
        body->addWidget(gutter_);
        body->addWidget(buildSide(1), 1);
        root->addLayout(body, 1);

        status_ = new QLabel(this);
        status_->setContentsMargins(6, 2, 6, 2);
        root->addWidget(status_);

        diffgrid::syncScrollBars(grids_[0]->verticalScrollBar(), grids_[1]->verticalScrollBar());
        diffgrid::syncScrollBars(grids_[0]->horizontalScrollBar(), grids_[1]->horizontalScrollBar());
        for (int side = 0; side < 2; ++side) {
            QObject::connect(grids_[side]->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                             [this, side](const QModelIndex& current, const QModelIndex&) { onCurrentRow(side, current.row()); });
            QObject::connect(sides_[side].save, &QToolButton::clicked, this, [this, side]() { commitEditors(); saveSide(side == 0, false); });
        }
        gutter_->onCopy = [this](int group, bool toRight) { copyGroup(group, toRight); };
        gutter_->onGroupClicked = [this](int group) { currentGroup_ = group; gutter_->setCurrentGroup(group); };
        miniMap_->onRowClicked = [this](int bucketRow) {
            if (!view_) return;
            const int row = bucketRow * minimapBucket_;
            const int index = diffgrid::firstVisibleAtOrAfter(models_[0]->visible(), row);
            if (index >= 0) grids_[0]->scrollTo(models_[0]->index(index, 0), QAbstractItemView::PositionAtCenter);
        };
        QObject::connect(grids_[0]->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { updateViewport(); });
        grids_[0]->viewport()->installEventFilter(this);  // resize -> minimap viewport box
        QObject::connect(pixelMinimapAction_, &QAction::toggled, this, [this](bool on) { miniMap_->setPixelLineMode(on); });

        // Ctrl+S target: whichever side (header or grid) last held keyboard focus.
        QObject::connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (!now) return;
            for (int side = 0; side < 2; ++side) {
                QWidget* box = sides_[side].box;
                if (box && (box == now || box->isAncestorOf(now))) activeSide_ = side;
            }
        });
        QObject::connect(saveAction_, &QAction::triggered, this, [this]() { saveActiveSide(); });

        QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
        QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
        QObject::connect(copyRightAction_, &QAction::triggered, this, [this]() { copyGroup(currentGroup_, true); });
        QObject::connect(copyLeftAction_, &QAction::triggered, this, [this]() { copyGroup(currentGroup_, false); });
        QObject::connect(nextAction_, &QAction::triggered, this, [this]() { jumpSection(+1); });
        QObject::connect(prevAction_, &QAction::triggered, this, [this]() { jumpSection(-1); });
        QObject::connect(swapAction_, &QAction::triggered, this, [this]() { swapSides(); });
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() { reload(); });
        QObject::connect(saveLeftAction_, &QAction::triggered, this, [this]() { commitEditors(); saveSide(true, false); });
        QObject::connect(saveRightAction_, &QAction::triggered, this, [this]() { commitEditors(); saveSide(false, false); });
    }

    void onCurrentRow(int side, int modelRow) {
        if (syncingSelection_ || modelRow < 0) return;
        syncingSelection_ = true;
        const int other = 1 - side;
        grids_[other]->setCurrentIndex(models_[other]->index(modelRow, 0));
        syncingSelection_ = false;
        const int viewRow = models_[side]->viewRow(modelRow);
        if (viewRow >= 0) miniMap_->setCurrentRow(viewRow / std::max(1, minimapBucket_));
        const int group = groupContaining(viewRow);
        if (group >= 0) {
            currentGroup_ = group;
            gutter_->setCurrentGroup(group);
        }
    }

    void updateViewport() {
        if (!view_) return;
        const int total = models_[0]->rowCount();
        if (!total) return;
        const int first = grids_[0]->rowAt(0);
        const int last = grids_[0]->rowAt(grids_[0]->viewport()->height() - 1);
        const int shown = (last < 0 ? total : last + 1) - std::max(first, 0);
        const int firstRow = models_[0]->viewRow(std::max(first, 0));
        const int rows = static_cast<int>(openbc_table_view_rows(view_.get()));
        if (rows > 0 && firstRow >= 0) miniMap_->setViewport(qreal(firstRow) / rows, qreal(shown) / rows);
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcTableSession* session_ = nullptr;
    TableViewPtr view_;
    int columns_ = 0;
    int currentGroup_ = -1;
    int minimapBucket_ = 1;
    bool dirty_[2] = {false, false};
    bool editable_[2] = {false, false};
    bool loaded_[2] = {false, false};
    QString errors_[2];
    int activeSide_ = 0;     // side that last had focus; target of Ctrl+S
    SaveDone saveDone_;      // completion of the save currently in flight (dialog / Ctrl+S)
    bool syncingSelection_ = false;
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
    QAction* pixelMinimapAction_ = nullptr;
    QCheckBox* ignoreCase_ = nullptr;
    QCheckBox* trim_ = nullptr;
    QDoubleSpinBox* tolerance_ = nullptr;
    QLineEdit* keysEdit_ = nullptr;
    QTimer* optionsTimer_ = nullptr;
    SidePane sides_[2];
    std::unique_ptr<FileChangeController> changes_;  // external-change notice + reload (common/file_change_controller.h)
    TableDiffModel* models_[2] = {nullptr, nullptr};
    QTableView* grids_[2] = {nullptr, nullptr};
    diffgrid::CopyGutter* gutter_ = nullptr;
    DifferenceOverview* miniMap_ = nullptr;
    QLabel* status_ = nullptr;
    BackendChannel channel_;  // last member: destroyed first, after the session is stopped
};

}  // namespace openbc::app
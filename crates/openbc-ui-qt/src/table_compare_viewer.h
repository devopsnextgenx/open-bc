// table_compare_viewer.h
// ---------------------------------------------------------------------------
// TableCompareViewer: the CSV / Excel (xlsx, xls, xlsb, ods) compare tab.
//
// Presentation only. Loading, decoding, row alignment, cell diffing, copy edits
// and saving all happen in openbc-engine / openbc-compute / openbc-core; this
// header reads the finished result through the openbc_table_* C ABI
// (compare_backend_api.h) and renders it the way TextCompareView renders text:
//   * difference groups with one pair of copy arrows each (CopyGutter),
//   * All / Diffs / Context filtering, next / previous section, minimap,
//   * per-side unsaved indicator and Save (CSV in place; spreadsheets are
//     read-only, so their Save is "export sheet as CSV").
// Header-only and free of Q_OBJECT, like the rest of the app.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
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

#include "backend_channel.h"
#include "compare_backend_api.h"
#include "diff_algorithms.h"
#include "diff_grid_common.h"
#include "minimap.h"
#include "qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

using TableViewPtr = std::shared_ptr<OpenBcTableView>;

// One side of the comparison as a read-only model over the backend result.
class TableDiffModel : public QAbstractTableModel {
public:
    TableDiffModel(std::uint8_t side, QObject* parent) : QAbstractTableModel(parent), side_(side) {}

    void setView(const TableViewPtr& view, const QVector<int>& visible, int columns) {
        beginResetModel();
        view_ = view;
        visible_ = visible;
        columns_ = columns;
        endResetModel();
    }

    int viewRow(int modelRow) const { return modelRow >= 0 && modelRow < visible_.size() ? visible_[modelRow] : -1; }
    const QVector<int>& visible() const { return visible_; }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : visible_.size(); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : columns_; }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!view_ || !index.isValid()) return {};
        const std::size_t row = static_cast<std::size_t>(visible_[index.row()]);
        const std::size_t col = static_cast<std::size_t>(index.column());
        const std::uint32_t number = openbc_table_row_number(view_.get(), row, side_);
        if (role == Qt::DisplayRole) {
            if (!number) return {};
            std::size_t length = 0;
            const auto* text = openbc_table_cell(view_.get(), row, side_, col, &length);
            return backendString(text, length);
        }
        if (role == Qt::BackgroundRole) {
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
    std::uint8_t side_;
    TableViewPtr view_;
    QVector<int> visible_;
    int columns_ = 0;
};

class TableCompareViewer : public QWidget {
public:
    TableCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &TableCompareViewer::destroyHandle,
                   [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("tableCompareView");
        buildUi();
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

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> editMenuItems() const { return {saveLeftAction_, saveRightAction_}; }
    QList<QAction*> searchMenuItems() const { return {nextAction_, prevAction_}; }
    QList<QAction*> viewMenuItems() const { return {showAllAction_, showDiffsAction_, contextAction_}; }
    QList<QAction*> actionsMenuItems() const { return {copyRightAction_, copyLeftAction_, swapAction_, reloadAction_}; }

private:
    // ---------------------------------------------------------------- session
    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_table_view_destroy(static_cast<OpenBcTableView*>(handle));
    }

    void startSession() {
        stopSession();
        view_.reset();
        const QByteArray left = leftPath_.toUtf8();
        const QByteArray right = rightPath_.toUtf8();
        session_ = openbc_table_session_create(left.constData(), right.constData(), BackendChannel::callback(), channel_.user());
        status_->setText("Loading...");
        if (onTitleChanged) onTitleChanged();
    }

    void stopSession() {
        if (!session_) return;
        openbc_table_session_destroy(session_);
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
            break;
        case OPENBC_EV_SAVED: {
            const QStringList parts = event.message.split('|');
            status_->setText("Saved " + QFileInfo(parts.value(1)).fileName());
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
        for (int start = 0; start < rows; start += bucket) {
            bool changed = false;
            int leftNumber = 0, rightNumber = 0;
            for (int i = start; i < std::min(rows, start + bucket); ++i) {
                changed |= openbc_table_row_kind(v, i) != 0;
                if (!leftNumber) leftNumber = static_cast<int>(openbc_table_row_number(v, i, 0));
                if (!rightNumber) rightNumber = static_cast<int>(openbc_table_row_number(v, i, 1));
            }
            overview.push_back({QString(), QString(), leftNumber, rightNumber, changed, false});
        }
        minimapBucket_ = bucket;
        miniMap_->setRows(overview);

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
        pane.path->setText(path.isEmpty() ? "(missing)" : path);
        pane.path->setToolTip(path);
        if (!error.isEmpty()) pane.info->setText("<span style='color:#e5534b'>" + error.toHtmlEscaped() + "</span>");
        else if (!loaded) pane.info->setText(path.isEmpty() ? "No file" : "Not loaded");
        else pane.info->setText(QString(editable ? "CSV" : "Spreadsheet (read-only)") + (dirty_[side] ? " - unsaved changes" : ""));
        pane.save->setIcon(icons::saveIcon(dirty_[side] ? QColor(0x6f, 0x9f, 0xdb) : QColor(0xdd, 0xe0, 0xee)));
        pane.save->setToolTip(editable ? "Save" : "Export this sheet as CSV...");
        pane.save->setEnabled(loaded || !path.isEmpty());
        editable_[side] = editable;
        (side == 0 ? saveLeftAction_ : saveRightAction_)->setEnabled(loaded);
        gutter_->setEnabledSides(true, true);
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

    void saveSide(bool left, bool askForPath) {
        if (!session_) return;
        const std::uint8_t side = left ? 0 : 1;
        if (editable_[side] && !askForPath) {
            send(openbc_table_save(session_, left ? 1 : 0, nullptr));
            return;
        }
        const QString source = left ? leftPath_ : rightPath_;
        const QString suggested = QFileInfo(source).completeBaseName() + ".csv";
        const QString target = QFileDialog::getSaveFileName(this, "Save sheet as CSV", suggested, "CSV files (*.csv)");
        if (target.isEmpty()) return;
        const QByteArray bytes = target.toUtf8();
        send(openbc_table_save(session_, left ? 1 : 0, bytes.constData()));
    }

    bool confirmDiscard() {
        if (!dirty_[0] && !dirty_[1]) return true;
        return QMessageBox::question(this, "Unsaved changes", "Discard unsaved changes in this comparison?") == QMessageBox::Yes;
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
        QLabel* path = nullptr;
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
        auto* header = new QHBoxLayout;
        header->setContentsMargins(4, 2, 4, 0);
        pane.path = new QLabel(box);
        pane.path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        pane.path->setTextFormat(Qt::PlainText);
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
        auto* grid = new QTableView(box);
        grid->setModel(models_[side]);
        grid->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid->setSelectionMode(QAbstractItemView::SingleSelection);
        grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
        saveLeftAction_ = new QAction(icons::glyph(Glyph::Save), "Save Left", this);
        saveRightAction_ = new QAction(icons::glyph(Glyph::Save), "Save Right", this);
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
        miniMap_ = new DifferenceOverview(this);
        miniMap_->setFixedWidth(22);
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
            QObject::connect(sides_[side].save, &QToolButton::clicked, this, [this, side]() { saveSide(side == 0, false); });
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

        QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
        QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
        QObject::connect(copyRightAction_, &QAction::triggered, this, [this]() { copyGroup(currentGroup_, true); });
        QObject::connect(copyLeftAction_, &QAction::triggered, this, [this]() { copyGroup(currentGroup_, false); });
        QObject::connect(nextAction_, &QAction::triggered, this, [this]() { jumpSection(+1); });
        QObject::connect(prevAction_, &QAction::triggered, this, [this]() { jumpSection(-1); });
        QObject::connect(swapAction_, &QAction::triggered, this, [this]() { swapSides(); });
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() { reload(); });
        QObject::connect(saveLeftAction_, &QAction::triggered, this, [this]() { saveSide(true, false); });
        QObject::connect(saveRightAction_, &QAction::triggered, this, [this]() { saveSide(false, false); });
    }

    void onCurrentRow(int side, int modelRow) {
        if (syncingSelection_ || modelRow < 0) return;
        syncingSelection_ = true;
        const int other = 1 - side;
        grids_[other]->setCurrentIndex(models_[other]->index(modelRow, 0));
        syncingSelection_ = false;
        const int group = groupContaining(models_[side]->viewRow(modelRow));
        if (group >= 0) {
            currentGroup_ = group;
            gutter_->setCurrentGroup(group);
        }
    }

    void updateViewport() {
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
    QAction* saveLeftAction_ = nullptr;
    QAction* saveRightAction_ = nullptr;
    QCheckBox* ignoreCase_ = nullptr;
    QCheckBox* trim_ = nullptr;
    QDoubleSpinBox* tolerance_ = nullptr;
    QLineEdit* keysEdit_ = nullptr;
    QTimer* optionsTimer_ = nullptr;
    SidePane sides_[2];
    TableDiffModel* models_[2] = {nullptr, nullptr};
    QTableView* grids_[2] = {nullptr, nullptr};
    diffgrid::CopyGutter* gutter_ = nullptr;
    DifferenceOverview* miniMap_ = nullptr;
    QLabel* status_ = nullptr;
    BackendChannel channel_;  // last member: destroyed first, after the session is stopped
};

}  // namespace openbc::app
// registry_compare_viewer.h
// ---------------------------------------------------------------------------
// RegistryCompareViewer: compares two Windows registry exports (.reg).
//
// Presentation only: parsing, key/value alignment, copy edits and writing the
// .reg file are done by openbc-engine / openbc-core through the
// openbc_registry_* C ABI. One unified grid (Key/Value | Left | copy | Right):
// the copy column shows a pair of arrows on the first row of each difference
// group, mirroring the copy arrows of the text compare view.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
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

using RegistryViewPtr = std::shared_ptr<OpenBcRegistryView>;

class RegistryDiffModel : public QAbstractTableModel {
public:
    enum Column { Name = 0, LeftType, LeftData, Copy, RightType, RightData, ColumnCount };
    static constexpr int GroupRole = Qt::UserRole + 1;  // backend group index on a group's first row, else -1

    using QAbstractTableModel::QAbstractTableModel;

    void setView(const RegistryViewPtr& view, const QVector<int>& visible) {
        beginResetModel();
        view_ = view;
        visible_ = visible;
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

        if (role == Qt::DisplayRole) {
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
            return diffgrid::cellBrush(kind, present, !isType);
        }
        return {};
    }

private:
    RegistryViewPtr view_;
    QVector<int> visible_;
    QHash<int, int> firstRowOfGroup_;
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

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> editMenuItems() const { return {saveLeftAction_, saveRightAction_}; }
    QList<QAction*> searchMenuItems() const { return {nextAction_, prevAction_}; }
    QList<QAction*> viewMenuItems() const { return {showAllAction_, showDiffsAction_, contextAction_}; }
    QList<QAction*> actionsMenuItems() const { return {copyRightAction_, copyLeftAction_, swapAction_, reloadAction_}; }

private:
    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_registry_view_destroy(static_cast<OpenBcRegistryView*>(handle));
    }

    void startSession() {
        stopSession();
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
        case OPENBC_EV_ERROR:
            status_->setText(event.message);
            break;
        case OPENBC_EV_SAVED:
            status_->setText("Saved " + QFileInfo(event.message.section('|', 1)).fileName());
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
            std::size_t length = 0;
            const QString error = backendString(openbc_registry_side_error(v, side, &length), length);
            const QString path = side == 0 ? leftPath_ : rightPath_;
            labels_[side]->setText((path.isEmpty() ? QString("(missing)") : path) + (dirty_[side] ? "  - unsaved changes" : ""));
            labels_[side]->setToolTip(error);
            if (!error.isEmpty()) labels_[side]->setText("<span style='color:#e5534b'>" + error.toHtmlEscaped() + "</span>");
            (side == 0 ? saveLeftAction_ : saveRightAction_)->setEnabled(flags & 1);
            saves_[side]->setIcon(icons::saveIcon(dirty_[side] ? QColor(0x6f, 0x9f, 0xdb) : QColor(0xdd, 0xe0, 0xee)));
        }
        rebuildVisible();

        const int rows = static_cast<int>(openbc_registry_view_rows(v));
        const int bucket = std::max(1, (rows + 3999) / 4000);
        QVector<TextDiffLine> overview;
        for (int start = 0; start < rows; start += bucket) {
            bool changed = false;
            for (int i = start; i < std::min(rows, start + bucket) && !changed; ++i) changed = openbc_registry_row_kind(v, i) != 0;
            overview.push_back({QString(), QString(), start + 1, start + 1, changed, false});
        }
        minimapBucket_ = bucket;
        miniMap_->setRows(overview);

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

    void saveSide(bool left, bool askForPath) {
        if (!session_) return;
        QByteArray bytes;
        if (askForPath) {
            const QString target = QFileDialog::getSaveFileName(this, "Save registry export", left ? leftPath_ : rightPath_, "Registry files (*.reg)");
            if (target.isEmpty()) return;
            bytes = target.toUtf8();
        }
        send(openbc_registry_save(session_, left ? 1 : 0, askForPath ? bytes.constData() : nullptr));
    }

    bool confirmDiscard() {
        if (!dirty_[0] && !dirty_[1]) return true;
        return QMessageBox::question(this, "Unsaved changes", "Discard unsaved changes in this comparison?") == QMessageBox::Yes;
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
        root->addWidget(toolbar_);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(6, 2, 6, 2);
        ignoreCase_ = new QCheckBox("Ignore data case", this);
        header->addWidget(ignoreCase_);
        for (int side = 0; side < 2; ++side) {
            labels_[side] = new QLabel(this);
            labels_[side]->setTextFormat(Qt::RichText);
            saves_[side] = new QToolButton(this);
            saves_[side]->setAutoRaise(true);
            saves_[side]->setIcon(icons::glyph(Glyph::Save));
            saves_[side]->setToolTip(side == 0 ? "Save left" : "Save right");
            header->addWidget(labels_[side], 1);
            header->addWidget(saves_[side]);
        }
        root->addLayout(header);

        auto* body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        miniMap_ = new DifferenceOverview(this);
        miniMap_->setFixedWidth(22);
        model_ = new RegistryDiffModel(this);
        grid_ = new QTableView(this);
        grid_->setModel(model_);
        grid_->setItemDelegateForColumn(RegistryDiffModel::Copy, new RegistryCopyDelegate(grid_));
        grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        grid_->setSelectionMode(QAbstractItemView::SingleSelection);
        grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
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
        for (int side = 0; side < 2; ++side) QObject::connect(saves_[side], &QToolButton::clicked, this, [this, side]() { saveSide(side == 0, false); });
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
        QObject::connect(saveLeftAction_, &QAction::triggered, this, [this]() { saveSide(true, false); });
        QObject::connect(saveRightAction_, &QAction::triggered, this, [this]() { saveSide(false, false); });
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcRegistrySession* session_ = nullptr;
    RegistryViewPtr view_;
    int currentGroup_ = -1;
    int minimapBucket_ = 1;
    bool dirty_[2] = {false, false};
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
    QLabel* labels_[2] = {nullptr, nullptr};
    QToolButton* saves_[2] = {nullptr, nullptr};
    RegistryDiffModel* model_ = nullptr;
    QTableView* grid_ = nullptr;
    DifferenceOverview* miniMap_ = nullptr;
    QLabel* status_ = nullptr;
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app
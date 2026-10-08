// hex_compare_viewer.h
// ---------------------------------------------------------------------------
// HexCompareViewer: byte-level side-by-side compare of two files.
//
// The backend (openbc-engine hex actor + openbc-compute Rayon byte compare)
// streams both files in bounded chunks and returns the list of differing byte
// runs; the UI never reads file data itself. It asks the backend for a window
// of bytes around what is on screen (openbc_hex_request_window) and paints
// that window, marking bytes the backend reported as different. Read-only.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractScrollArea>
#include <QAction>
#include <QByteArray>
#include <QComboBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProgressBar>
#include <QScrollBar>
#include <QTimer>
#include <QToolBar>
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

using HexSummaryPtr = std::shared_ptr<OpenBcHexSummary>;

// State shared by the two panes (owned by HexCompareViewer).
struct HexShared {
    HexSummaryPtr summary;
    QByteArray window[2];
    quint64 windowOffset = 0;
    quint64 size[2] = {0, 0};
    int bytesPerRow = 16;
    quint64 maxSize() const { return std::max(size[0], size[1]); }
};

class HexPane : public QAbstractScrollArea {
public:
    HexPane(std::uint8_t side, const HexShared* shared, QWidget* parent = nullptr)
        : QAbstractScrollArea(parent), side_(side), shared_(shared) {
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        viewport()->setBackgroundRole(QPalette::Base);
        verticalScrollBar()->setSingleStep(1);
        QObject::connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { notify(); });
    }

    std::function<void()> onViewChanged;

    quint64 totalRows() const { return (shared_->maxSize() + shared_->bytesPerRow - 1) / shared_->bytesPerRow; }
    int visibleRows() const { return std::max(1, viewport()->height() / rowHeight()); }

    quint64 firstRow() const { return static_cast<quint64>(verticalScrollBar()->value()) * scale_; }
    quint64 firstOffset() const { return firstRow() * shared_->bytesPerRow; }
    quint64 lastOffset() const { return std::min(shared_->maxSize(), (firstRow() + visibleRows() + 1) * shared_->bytesPerRow); }

    void goToOffset(quint64 offset) {
        verticalScrollBar()->setValue(static_cast<int>(std::min<quint64>((offset / shared_->bytesPerRow) / scale_, verticalScrollBar()->maximum())));
        viewport()->update();
    }

    // Called whenever the size, row width or window changes.
    void refreshGeometry() {
        const quint64 rows = totalRows();
        const quint64 limit = 1u << 30;
        scale_ = rows > limit ? (rows + limit - 1) / limit : 1;
        const quint64 maxFirst = rows > static_cast<quint64>(visibleRows()) ? rows - visibleRows() : 0;
        verticalScrollBar()->setRange(0, static_cast<int>(maxFirst / scale_));
        verticalScrollBar()->setPageStep(visibleRows());
        viewport()->update();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QAbstractScrollArea::resizeEvent(event);
        refreshGeometry();
        notify();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(viewport());
        const QFontMetrics fm(font());
        const int rowH = rowHeight(), charW = fm.horizontalAdvance('0');
        const int bpr = shared_->bytesPerRow;
        const int offsetW = 12 * charW, hexX = offsetW + charW;
        const int asciiX = hexX + (bpr * 3 + 1) * charW;
        const quint64 first = firstRow();
        const quint64 mySize = shared_->size[side_];
        p.fillRect(viewport()->rect(), palette().base());

        for (int r = 0; r < visibleRows() + 1; ++r) {
            const quint64 rowOffset = (first + r) * bpr;
            if (rowOffset >= shared_->maxSize()) break;
            const int y = r * rowH;
            p.setPen(palette().color(QPalette::Disabled, QPalette::Text));
            p.drawText(QRect(0, y, offsetW, rowH), Qt::AlignRight | Qt::AlignVCenter, QString("%1").arg(rowOffset, 10, 16, QChar('0')).toUpper());
            for (int i = 0; i < bpr; ++i) {
                const quint64 off = rowOffset + i;
                if (off >= shared_->maxSize()) break;
                const QRect hexCell(hexX + i * 3 * charW, y, 2 * charW + 2, rowH);
                const QRect asciiCell(asciiX + i * charW, y, charW, rowH);
                QString hex, ch;
                bool known = true;
                if (off >= mySize) {
                    p.fillRect(hexCell.united(asciiCell.adjusted(0, 0, 0, 0)).intersected(hexCell), QBrush(QColor(0x8a, 0x7a, 0x2d, 110), Qt::BDiagPattern));
                    p.fillRect(asciiCell, QBrush(QColor(0x8a, 0x7a, 0x2d, 110), Qt::BDiagPattern));
                    continue;
                }
                const int value = byteAt(off);
                if (value < 0) {
                    known = false;
                    hex = "..";
                    ch = ".";
                } else {
                    hex = QString("%1").arg(value, 2, 16, QChar('0')).toUpper();
                    ch = value >= 32 && value < 127 ? QString(QChar(value)) : QString(".");
                }
                const bool differs = known && shared_->summary && openbc_hex_summary_run_at(shared_->summary.get(), off) >= 0;
                if (differs) {
                    p.fillRect(hexCell, diffgrid::changedCellColor());
                    p.fillRect(asciiCell, diffgrid::changedCellColor());
                }
                p.setPen(known ? palette().color(QPalette::Text) : palette().color(QPalette::Disabled, QPalette::Text));
                p.drawText(hexCell, Qt::AlignLeft | Qt::AlignVCenter, hex);
                p.drawText(asciiCell, Qt::AlignCenter, ch);
            }
        }
    }

private:
    int rowHeight() const { return QFontMetrics(font()).height() + 2; }

    // -1: not in the loaded window yet.
    int byteAt(quint64 off) const {
        const QByteArray& w = shared_->window[side_];
        if (off < shared_->windowOffset || off - shared_->windowOffset >= static_cast<quint64>(w.size())) return -1;
        return static_cast<unsigned char>(w[static_cast<int>(off - shared_->windowOffset)]);
    }

    void notify() {
        viewport()->update();
        if (onViewChanged) onViewChanged();
    }

    std::uint8_t side_;
    const HexShared* shared_;
    quint64 scale_ = 1;
};

class HexCompareViewer : public QWidget {
public:
    HexCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &HexCompareViewer::destroyHandle, [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("hexCompareView");
        buildUi();
        startSession();
    }

    ~HexCompareViewer() override { stopSession(); }

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

    QList<QAction*> searchMenuItems() const { return {nextAction_, prevAction_}; }
    QList<QAction*> actionsMenuItems() const { return {reloadAction_}; }

private:
    static constexpr quint64 kWindowBytes = 256 * 1024;

    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_hex_summary_destroy(static_cast<OpenBcHexSummary*>(handle));
        else if (kind == OPENBC_EV_WINDOW) openbc_hex_window_destroy(static_cast<OpenBcHexWindow*>(handle));
    }

    void startSession() {
        stopSession();
        shared_.summary.reset();
        shared_.window[0].clear();
        shared_.window[1].clear();
        shared_.size[0] = shared_.size[1] = 0;
        const QByteArray left = leftPath_.toUtf8(), right = rightPath_.toUtf8();
        session_ = openbc_hex_session_create(left.constData(), right.constData(), BackendChannel::callback(), channel_.user());
        status_->setText("Scanning...");
        progress_->setValue(0);
        progress_->show();
        requestWindow(0);
        if (onTitleChanged) onTitleChanged();
    }

    void stopSession() {
        if (!session_) return;
        openbc_hex_session_destroy(session_);
        session_ = nullptr;
    }

    void onEvent(const BackendChannel::Event& event) {
        switch (event.kind) {
        case OPENBC_EV_PROGRESS: {
            const quint64 done = event.message.section('|', 0, 0).toULongLong();
            const quint64 total = std::max<quint64>(1, event.message.section('|', 1, 1).toULongLong());
            progress_->setValue(static_cast<int>(done * 100 / total));
            status_->setText(QString("Scanning... %1%").arg(done * 100 / total));
            break;
        }
        case OPENBC_EV_RESULT:
            applySummary(std::static_pointer_cast<OpenBcHexSummary>(event.handle));
            break;
        case OPENBC_EV_WINDOW:
            applyWindow(static_cast<OpenBcHexWindow*>(event.handle.get()));
            break;
        case OPENBC_EV_ERROR:
            status_->setText(event.message);
            break;
        default:
            break;
        }
    }

    void applySummary(const HexSummaryPtr& summary) {
        shared_.summary = summary;
        const OpenBcHexSummary* s = summary.get();
        shared_.size[0] = openbc_hex_summary_size(s, 0);
        shared_.size[1] = openbc_hex_summary_size(s, 1);
        progress_->hide();

        QStringList errors;
        for (std::uint8_t side = 0; side < 2; ++side) {
            std::size_t length = 0;
            const QString error = backendString(openbc_hex_summary_error(s, side, &length), length);
            if (!error.isEmpty()) errors << error;
        }
        const std::size_t runs = openbc_hex_summary_run_count(s);
        QString text = errors.isEmpty()
                           ? (runs ? QString("%1 difference %2, %3 bytes differ.").arg(runs).arg(runs == 1 ? "run" : "runs").arg(openbc_hex_summary_diff_bytes(s))
                                   : QString("Files are identical."))
                           : errors.join("  |  ");
        if (openbc_hex_summary_truncated(s)) text += " (run list truncated)";
        status_->setText(text + QString("   Left: %1 bytes  Right: %2 bytes").arg(shared_.size[0]).arg(shared_.size[1]));

        // Overview: at most 2000 buckets across the longer file.
        const quint64 total = std::max<quint64>(1, shared_.maxSize());
        const int buckets = 2000;
        QVector<TextDiffLine> overview(buckets);
        for (int i = 0; i < buckets; ++i) overview[i] = {QString(), QString(), i + 1, i + 1, false, false};
        for (std::size_t i = 0; i < runs; ++i) {
            const quint64 off = openbc_hex_summary_run_offset(s, i), len = std::max<quint64>(1, openbc_hex_summary_run_len(s, i));
            const int a = static_cast<int>(off * buckets / total), b = static_cast<int>(std::min<quint64>(buckets - 1, (off + len - 1) * buckets / total));
            for (int k = a; k <= b; ++k) overview[k].changed = true;
        }
        miniMap_->setRows(overview);
        refreshPanes();
        requestWindow(panes_[0]->firstOffset());
        if (onTitleChanged) onTitleChanged();
    }

    void applyWindow(const OpenBcHexWindow* window) {
        if (!window || openbc_hex_window_id(window) != requestId_) return;  // stale answer
        shared_.windowOffset = openbc_hex_window_offset(window);
        for (std::uint8_t side = 0; side < 2; ++side) {
            std::size_t length = 0;
            const auto* bytes = openbc_hex_window_bytes(window, side, &length);
            shared_.window[side] = QByteArray(reinterpret_cast<const char*>(bytes), static_cast<int>(length));
            // Before the summary arrives the sizes are unknown; the longest window is a lower bound.
            if (!shared_.summary) shared_.size[side] = std::max<quint64>(shared_.size[side], shared_.windowOffset + length);
        }
        refreshPanes();
    }

    void refreshPanes() {
        for (HexPane* pane : panes_) {
            pane->refreshGeometry();
            pane->viewport()->update();
        }
    }

    void requestWindow(quint64 aroundOffset) {
        if (!session_) return;
        const quint64 margin = kWindowBytes / 4;
        const quint64 start = (aroundOffset > margin ? aroundOffset - margin : 0) / 16 * 16;
        requestId_ = ++nextId_;
        wantedStart_ = start;
        openbc_hex_request_window(session_, requestId_, start, static_cast<std::size_t>(kWindowBytes));
    }

    bool windowCovers(quint64 first, quint64 last) const {
        const quint64 loaded = std::max<quint64>(shared_.window[0].size(), shared_.window[1].size());
        return first >= shared_.windowOffset && last <= shared_.windowOffset + loaded;
    }

    void onViewChanged() {
        const quint64 first = panes_[0]->firstOffset(), last = panes_[0]->lastOffset();
        if (!windowCovers(first, last) && !(requestId_ != 0 && first >= wantedStart_ && last <= wantedStart_ + kWindowBytes && inFlight())) requestTimer_->start();
        const quint64 total = std::max<quint64>(1, shared_.maxSize());
        miniMap_->setViewport(qreal(first) / total, qreal(last - first) / total);
    }

    bool inFlight() const { return requestId_ != 0 && shared_.windowOffset != wantedStart_; }

    void goTo(quint64 offset) {
        panes_[0]->goToOffset(offset);
        requestWindow(offset);
    }

    void jumpRun(bool forward) {
        if (!shared_.summary) return;
        const quint64 here = panes_[0]->firstOffset();
        const std::int64_t i = forward ? openbc_hex_summary_next_run(shared_.summary.get(), here)
                                       : openbc_hex_summary_prev_run(shared_.summary.get(), here);
        if (i >= 0) goTo(openbc_hex_summary_run_offset(shared_.summary.get(), static_cast<std::size_t>(i)));
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
        nextAction_ = toolbar_->addAction(icons::glyph(Glyph::NextSection), "Next Diff");
        prevAction_ = toolbar_->addAction(icons::glyph(Glyph::PrevSection), "Prev Diff");
        nextAction_->setShortcut(QKeySequence(Qt::Key_F3));
        prevAction_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
        reloadAction_ = toolbar_->addAction(icons::glyph(Glyph::Refresh), "Reload");
        toolbar_->addSeparator();
        gotoEdit_ = new QLineEdit(toolbar_);
        gotoEdit_->setPlaceholderText("Go to offset (hex)");
        gotoEdit_->setMaximumWidth(160);
        toolbar_->addWidget(gotoEdit_);
        widthBox_ = new QComboBox(toolbar_);
        for (int w : {8, 16, 32}) widthBox_->addItem(QString("%1 bytes/row").arg(w), w);
        widthBox_->setCurrentIndex(1);
        toolbar_->addWidget(widthBox_);
        root->addWidget(toolbar_);

        auto* body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(2);
        miniMap_ = new DifferenceOverview(this);
        miniMap_->setFixedWidth(22);
        body->addWidget(miniMap_);
        for (std::uint8_t side = 0; side < 2; ++side) {
            auto* column = new QVBoxLayout;
            column->setSpacing(0);
            auto* caption = new QLabel(side == 0 ? leftPath_ : rightPath_, this);
            caption->setContentsMargins(4, 2, 4, 2);
            caption->setToolTip(caption->text());
            panes_[side] = new HexPane(side, &shared_, this);
            panes_[side]->onViewChanged = [this]() { onViewChanged(); };
            column->addWidget(caption);
            column->addWidget(panes_[side], 1);
            body->addLayout(column, 1);
        }
        root->addLayout(body, 1);

        auto* statusRow = new QHBoxLayout;
        statusRow->setContentsMargins(6, 2, 6, 2);
        status_ = new QLabel(this);
        progress_ = new QProgressBar(this);
        progress_->setRange(0, 100);
        progress_->setMaximumWidth(160);
        statusRow->addWidget(status_, 1);
        statusRow->addWidget(progress_);
        root->addLayout(statusRow);

        diffgrid::syncScrollBars(panes_[0]->verticalScrollBar(), panes_[1]->verticalScrollBar());
        requestTimer_ = new QTimer(this);
        requestTimer_->setSingleShot(true);
        requestTimer_->setInterval(40);
        QObject::connect(requestTimer_, &QTimer::timeout, this, [this]() { requestWindow(panes_[0]->firstOffset()); });

        miniMap_->onRowClicked = [this](int bucket) { goTo(static_cast<quint64>(bucket) * std::max<quint64>(1, shared_.maxSize()) / 2000); };
        QObject::connect(gotoEdit_, &QLineEdit::returnPressed, this, [this]() {
            bool ok = false;
            const quint64 offset = gotoEdit_->text().trimmed().remove("0x").toULongLong(&ok, 16);
            if (ok) goTo(offset);
        });
        QObject::connect(widthBox_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
            const quint64 keep = panes_[0]->firstOffset();
            shared_.bytesPerRow = widthBox_->itemData(index).toInt();
            refreshPanes();
            goTo(keep);
        });
        QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
        QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
        QObject::connect(nextAction_, &QAction::triggered, this, [this]() { jumpRun(true); });
        QObject::connect(prevAction_, &QAction::triggered, this, [this]() { jumpRun(false); });
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() { startSession(); });
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcHexSession* session_ = nullptr;
    HexShared shared_;
    std::uint64_t requestId_ = 0;
    std::uint64_t nextId_ = 0;
    quint64 wantedStart_ = 0;

    QToolBar* toolbar_ = nullptr;
    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* nextAction_ = nullptr;
    QAction* prevAction_ = nullptr;
    QAction* reloadAction_ = nullptr;
    QLineEdit* gotoEdit_ = nullptr;
    QComboBox* widthBox_ = nullptr;
    HexPane* panes_[2] = {nullptr, nullptr};
    DifferenceOverview* miniMap_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTimer* requestTimer_ = nullptr;
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app
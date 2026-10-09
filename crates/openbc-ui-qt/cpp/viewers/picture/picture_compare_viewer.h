// picture_compare_viewer.h
// ---------------------------------------------------------------------------
// Picture Compare tab: two preview panes, a Tol/Range/Blend result pane, an
// offset/zoom/parameter panel and a 9x9 pixel-details probe, modelled on the
// Beyond Compare picture compare screens.
//
// All decoding, resizing and comparing happens in the Rust backend
// (openbc-engine -> openbc-compute). This widget only:
//   * pushes option changes (latest-wins, non-blocking),
//   * receives owned events on a Rust thread and re-posts them to the GUI thread,
//   * paints QImages and queries the 9x9 probe on hover (a cheap memory read).
//
// Header-only and free of Q_OBJECT on purpose (matches home_view.h): signals
// are plain std::function callbacks, cross-thread delivery uses the functor
// overload of QMetaObject::invokeMethod.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QRunnable>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "viewers/picture/picture_compare_api.h"
#include "viewers/picture/picture_extensions.h"
#include "viewers/picture/picture_metadata.h"
#include "style/qt_style.h"
#include "common/path_selector.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

struct PictureOptions {
    int mode = 0;  // 0 Tol, 1 Range, 2 Blend
    int tolerance = 25;
    int rangeMin = 0;
    int rangeMax = 255;
    int blend = 50;
    int resize = 0;  // 0 auto, 1 native, 2 fit-left, 3 fit-right, 4 fit-larger, 5 fit-smaller
    bool compareAlpha = false;
    QPoint offset;
};

// Zoom/pan shared by all three panes so they scroll together.
struct PictureViewState {
    bool fit = true;
    double zoom = 1.0;  // used when !fit
    QPointF pan;        // in image pixels, used when !fit
};

// ---------------------------------------------------------------------------
// One image pane. Hover/crosshair positions are expressed in *canvas* pixels;
// the pane converts using (scale, origin) so a resized or offset image still
// lines up with the other two panes.
// ---------------------------------------------------------------------------
class PictureImagePane : public QWidget {
public:
    std::function<void(const QPoint& canvas)> onHover;
    std::function<void()> onLeave;
    std::function<void(const QPoint& deltaCanvas)> onDragOffset;  // result pane only
    std::function<void(const QPointF& deltaImage)> onPan;
    std::function<void(const QPointF& panImage)> onPanTo;  // absolute pan, from the scroll bars
    std::function<void(int steps)> onWheelZoom;
    std::function<void()> onResized;

    PictureImagePane(PictureViewState* state, QWidget* parent = nullptr) : QWidget(parent), state_(state) {
        setMouseTracking(true);
        setMinimumSize(400, 400);
        setFocusPolicy(Qt::ClickFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        // Thin overlay scroll bars (they never take space from the viewport, so showing or
        // hiding them cannot change the overflow and feed back into itself).
        hBar_ = new QScrollBar(Qt::Horizontal, this);
        vBar_ = new QScrollBar(Qt::Vertical, this);
        hBar_->hide();
        vBar_->hide();
        connect(hBar_, &QScrollBar::valueChanged, this, [this](int v) {
            if (!onPanTo || state_->fit) return;
            QPointF p = state_->pan;
            p.setX((hBar_->maximum() / 2.0 - v) / effectiveScale());
            onPanTo(p);
        });
        connect(vBar_, &QScrollBar::valueChanged, this, [this](int v) {
            if (!onPanTo || state_->fit) return;
            QPointF p = state_->pan;
            p.setY((vBar_->maximum() / 2.0 - v) / effectiveScale());
            onPanTo(p);
        });
    }

    void setImage(const QImage& image) { image_ = image; viewChanged(); }

    // Call after the shared zoom/pan changed: repaint and refresh the scroll bars.
    void viewChanged() {
        syncScrollBars();
        update();
    }

    // How far the scaled image exceeds the viewport, in widget pixels (0 when it fits).
    QSizeF overflow() const {
        if (image_.isNull() || state_->fit) return {};
        const double s = effectiveScale();
        return QSizeF(std::max(0.0, image_.width() * s - width()), std::max(0.0, image_.height() * s - height()));
    }

    // Keeps the image edges from leaving the viewport; an axis that fits stays centred.
    QPointF clampPan(const QPointF& pan) const {
        if (image_.isNull() || state_->fit) return QPointF();
        const double s = effectiveScale();
        const QSizeF ov = overflow();
        const double mx = ov.width() / 2.0 / s;
        const double my = ov.height() / 2.0 / s;
        return QPointF(std::clamp(pan.x(), -mx, mx), std::clamp(pan.y(), -my, my));
    }
    void setPlaceholder(const QString& text) { placeholder_ = text; update(); }
    // canvas = native * scale + origin
    void setMapping(const QSizeF& scale, const QPoint& origin) { scale_ = scale; origin_ = origin; update(); }
    void setCrosshair(const std::optional<QPoint>& canvas) { cross_ = canvas; update(); }
    void setDragMovesOffset(bool enabled) {
        dragMovesOffset_ = enabled;
        setCursor(enabled ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }
    bool hasImage() const { return !image_.isNull(); }

    double effectiveScale() const {
        if (image_.isNull()) return 1.0;
        if (state_->fit) {
            return std::min(1.0, std::min(double(width()) / image_.width(), double(height()) / image_.height()));
        }
        return state_->zoom;
    }

protected:
    QPointF topLeft() const {
        const double s = effectiveScale();
        const QPointF pan = clampPan(state_->pan);
        return QPointF((width() - image_.width() * s) / 2.0, (height() - image_.height() * s) / 2.0) + pan * s;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window).darker(115));
        if (image_.isNull()) {
            p.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, placeholder_);
            return;
        }
        const double s = effectiveScale();
        const QPointF tl = topLeft();
        const QRectF target(tl, QSizeF(image_.width() * s, image_.height() * s));
        p.setRenderHint(QPainter::SmoothPixmapTransform, s < 1.0);
        p.drawImage(target, image_);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));

        if (cross_ && scale_.width() > 0 && scale_.height() > 0) {
            const QPointF native((cross_->x() - origin_.x()) / scale_.width(),
                                 (cross_->y() - origin_.y()) / scale_.height());
            if (native.x() >= 0 && native.y() >= 0 && native.x() < image_.width() && native.y() < image_.height()) {
                const QPointF c = tl + (native + QPointF(0.5, 0.5)) * s;
                const double half = std::max(12.0, 4.5 * s);
                p.setPen(QPen(QColor(0xe0, 0x20, 0x20), 2));
                p.drawRect(QRectF(c.x() - half, c.y() - half, half * 2, half * 2));
                p.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1));
                p.drawLine(QPointF(c.x() - 5, c.y()), QPointF(c.x() + 5, c.y()));
                p.drawLine(QPointF(c.x(), c.y() - 5), QPointF(c.x(), c.y() + 5));
            }
        }
    }

    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        syncScrollBars();
        if (onResized) onResized();
    }

    void mousePressEvent(QMouseEvent* e) override {
        lastPos_ = e->position();
        remainder_ = QPointF();
        if (e->button() == Qt::LeftButton && dragMovesOffset_ && hasImage()) {
            dragging_ = true;
            setCursor(Qt::ClosedHandCursor);
        } else if (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton) {
            panning_ = true;
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override {
        dragging_ = false;
        panning_ = false;
        setCursor(dragMovesOffset_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF pos = e->position();
        const double s = effectiveScale();
        if (dragging_ && onDragOffset) {
            remainder_ += (pos - lastPos_) / s;
            const int dx = int(std::trunc(remainder_.x()));
            const int dy = int(std::trunc(remainder_.y()));
            remainder_ -= QPointF(dx, dy);
            if (dx || dy) onDragOffset(QPoint(dx, dy));
        } else if (panning_ && onPan && !state_->fit) {
            onPan((pos - lastPos_) / s);
        } else if (!image_.isNull()) {
            const QPointF native = (pos - topLeft()) / s;
            const QPoint np(int(std::floor(native.x())), int(std::floor(native.y())));
            if (QRect(0, 0, image_.width(), image_.height()).contains(np)) {
                if (onHover) {
                    onHover(QPoint(int(std::floor(np.x() * scale_.width() + origin_.x())),
                                   int(std::floor(np.y() * scale_.height() + origin_.y()))));
                }
            } else if (onLeave) {
                onLeave();
            }
        }
        lastPos_ = pos;
    }

    void leaveEvent(QEvent*) override {
        if (onLeave) onLeave();
    }

    void wheelEvent(QWheelEvent* e) override {
        const QSizeF ov = overflow();
        if ((e->modifiers() & Qt::ControlModifier) && onWheelZoom) {
            onWheelZoom(e->angleDelta().y() > 0 ? 1 : -1);
            e->accept();
        } else if (onPan && (ov.width() > 0 || ov.height() > 0)) {
            // Plain wheel scrolls vertically, Shift+wheel horizontally (all panes follow).
            QPointF d = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) / 120.0 * 60.0
                                                 : QPointF(e->pixelDelta());
            if ((e->modifiers() & Qt::ShiftModifier) && d.x() == 0) d = QPointF(d.y(), 0);
            onPan(d / effectiveScale());
            e->accept();
        } else {
            e->ignore();
        }
    }

private:
    void syncScrollBars() {
        const QSizeF ov = overflow();
        const bool needH = ov.width() > 0.5;
        const bool needV = ov.height() > 0.5;
        hBar_->setVisible(needH);
        vBar_->setVisible(needV);

        const int th = hBar_->sizeHint().height();
        const int tw = vBar_->sizeHint().width();
        hBar_->setGeometry(0, height() - th, width() - (needV ? tw : 0), th);
        vBar_->setGeometry(width() - tw, 0, tw, height() - (needH ? th : 0));
        hBar_->raise();
        vBar_->raise();

        const double s = effectiveScale();
        const QPointF pan = clampPan(state_->pan);
        if (needH) {
            const QSignalBlocker block(hBar_);
            hBar_->setRange(0, int(std::ceil(ov.width())));
            hBar_->setPageStep(std::max(1, width()));
            hBar_->setSingleStep(40);
            hBar_->setValue(int(std::lround(ov.width() / 2.0 - pan.x() * s)));
        }
        if (needV) {
            const QSignalBlocker block(vBar_);
            vBar_->setRange(0, int(std::ceil(ov.height())));
            vBar_->setPageStep(std::max(1, height()));
            vBar_->setSingleStep(40);
            vBar_->setValue(int(std::lround(ov.height() / 2.0 - pan.y() * s)));
        }
    }

    PictureViewState* state_;
    QScrollBar* hBar_ = nullptr;
    QScrollBar* vBar_ = nullptr;
    QImage image_;
    QString placeholder_;
    QSizeF scale_{1.0, 1.0};
    QPoint origin_;
    std::optional<QPoint> cross_;
    bool dragMovesOffset_ = false;
    bool dragging_ = false;
    bool panning_ = false;
    QPointF lastPos_;
    QPointF remainder_;
};

// 9x9 pixel grid with the centre cell outlined.
class PicturePixelGrid : public QWidget {
public:
    explicit PicturePixelGrid(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(kCell * 9 + 1, kCell * 9 + 1);
    }
    void clear() {
        valid_ = false;
        update();
    }
    void setCells(const std::uint8_t* present, const std::uint8_t* rgba) {
        for (int i = 0; i < 81; ++i) {
            present_[i] = present[i] != 0;
            color_[i] = QColor(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
        }
        valid_ = true;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window).darker(130));
        if (!valid_) return;
        for (int row = 0; row < 9; ++row) {
            for (int col = 0; col < 9; ++col) {
                const int i = row * 9 + col;
                const QRect cell(col * kCell, row * kCell, kCell, kCell);
                if (present_[i]) p.fillRect(cell, color_[i]);
            }
        }
        p.setPen(QPen(QColor(0xdd, 0xdd, 0xdd, 160), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(4 * kCell, 4 * kCell, kCell, kCell));
    }

private:
    static constexpr int kCell = 11;
    bool valid_ = false;
    bool present_[81] = {};
    QColor color_[81];
};

// ---------------------------------------------------------------------------
class PictureCompareViewer : public QWidget {
public:
    std::function<void()> onTitleChanged;
    std::function<void(const QString& left, const QString& right)> onComparisonRequested;

    explicit PictureCompareViewer(QSettings* preferences, QWidget* parent = nullptr)
        : QWidget(parent), prefs_(preferences) {
        setAcceptDrops(true);
        setFocusPolicy(Qt::StrongFocus);
        loadPreferences();

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        createActions();  // the toolbar and the controls panel are built from these
        root->addWidget(buildToolbar());
        root->addWidget(buildPathRows());
        root->addWidget(buildBody(), 1);
        root->addWidget(buildStatusRow());
        syncControlsFromOptions();
        wirePanes();

        const OpenbcImageCompareOptions native = nativeOptions();
        handle_ = openbc_image_compare_create(&PictureCompareViewer::onNativeEvent, this, &native);
    }

    ~PictureCompareViewer() override {
        savePreferences();
        // Blocks until any in-flight callback finished; no callback runs afterwards.
        if (handle_) openbc_image_compare_destroy(handle_);
    }

    // ----- public API used by the main window -------------------------------
    void setPaths(const QString& left, const QString& right) {
        leftSelector_->setText(left);
        rightSelector_->setText(right);
        if (onTitleChanged) onTitleChanged();
    }

    void compare() {
        const QString left = leftSelector_->text().trimmed();
        const QString right = rightSelector_->text().trimmed();
        if (left.isEmpty() || right.isEmpty() || !handle_) return;
        const QByteArray l = left.toUtf8();
        const QByteArray r = right.toUtf8();
        const OpenbcImageCompareOptions native = nativeOptions();
        openbc_image_compare_set_options(handle_, &native);
        openbc_image_compare_open(handle_, reinterpret_cast<const std::uint8_t*>(l.constData()),
                                  std::size_t(l.size()), reinterpret_cast<const std::uint8_t*>(r.constData()),
                                  std::size_t(r.size()));
        leftSelector_->remember(left);   // MRU history in the path dropdown
        rightSelector_->remember(right);
        if (onComparisonRequested) onComparisonRequested(left, right);
        if (onTitleChanged) onTitleChanged();
    }

    QString leftText() const { return leftSelector_->text(); }
    QString rightText() const { return rightSelector_->text(); }

    QString title() const {
        const QString l = QFileInfo(leftSelector_->text()).fileName();
        const QString r = QFileInfo(rightSelector_->text()).fileName();
        if (l.isEmpty() && r.isEmpty()) return "Picture Compare";
        return l + " <-> " + r;
    }
    QString detailedTitle() const { return leftSelector_->text() + " <-> " + rightSelector_->text(); }

    QList<QAction*> actionsMenuItems() const {
        return {actSwap_, actReload_, nullptr, actTol_, actRange_, actBlend_, nullptr, actResetOffset_};
    }
    QList<QAction*> editMenuItems() const { return {}; }
    QList<QAction*> searchMenuItems() const { return {}; }
    QList<QAction*> toolsMenuItems() const { return {}; }
    QList<QAction*> viewMenuItems() const {
        return {actZoomIn_, actZoomOut_, actFit_, actActual_, nullptr, actControlsRight_};
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }

    void dropEvent(QDropEvent* e) override {
        QStringList files;
        for (const QUrl& url : e->mimeData()->urls()) {
            if (url.isLocalFile()) files << url.toLocalFile();
        }
        if (files.isEmpty()) return;
        if (files.size() >= 2) {
            setPaths(files[0], files[1]);
        } else if (leftSelector_->text().isEmpty() || !rightSelector_->text().isEmpty()) {
            setPaths(files[0], rightSelector_->text());
        } else {
            setPaths(leftSelector_->text(), files[0]);
        }
        compare();
        e->acceptProposedAction();
    }

    // Arrow keys nudge the right image (Shift = 10px), like dragging in the result pane.
    void keyPressEvent(QKeyEvent* e) override {
        const int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
        QPoint d;
        switch (e->key()) {
            case Qt::Key_Left: d = {-step, 0}; break;
            case Qt::Key_Right: d = {step, 0}; break;
            case Qt::Key_Up: d = {0, -step}; break;
            case Qt::Key_Down: d = {0, step}; break;
            default: QWidget::keyPressEvent(e); return;
        }
        moveOffset(d);
    }

private:
    // ===== native event plumbing =============================================
    struct ViewInfo {
        QString path, format;
        qint64 size = 0;
        qint64 modifiedMs = 0;
        bool hasModified = false;
        int width = 0, height = 0, bpp = 0;
    };

    struct ViewEvent {
        std::uint32_t kind = 0, side = 0;
        quint64 seq = 0;
        ViewInfo left, right;
        QImage leftImage, rightImage, overlay;
        QString path, message;
        PictureOptions options;
        std::uint32_t verdict = 0, backend = 0;
        bool binaryIdentical = false;
        int maxDelta = 0;
        QSize rightAligned;
        quint64 compared = 0, different = 0, similar = 0;
    };

    static QString str(const OpenbcStr& s) {
        return s.ptr ? QString::fromUtf8(reinterpret_cast<const char*>(s.ptr), qsizetype(s.len)) : QString();
    }
    static QImage rgba(const std::uint8_t* p, int w, int h) {
        if (!p || w <= 0 || h <= 0) return {};
        return QImage(p, w, h, qsizetype(w) * 4, QImage::Format_RGBA8888).copy();  // deep copy: Rust frees `p`
    }
    static ViewInfo info(const OpenbcImageInfo& i) {
        ViewInfo v;
        v.path = str(i.path);
        v.format = str(i.format);
        v.size = qint64(i.sizeBytes);
        v.modifiedMs = i.modifiedUnixMs;
        v.hasModified = i.hasModified != 0;
        v.width = int(i.width);
        v.height = int(i.height);
        v.bpp = int(i.bitsPerPixel);
        return v;
    }
    static PictureOptions optionsFromNative(const OpenbcImageCompareOptions& o) {
        PictureOptions p;
        p.mode = o.mode;
        p.tolerance = o.tolerance;
        p.rangeMin = o.rangeMin;
        p.rangeMax = o.rangeMax;
        p.blend = o.blendPercent;
        p.resize = o.resize;
        p.compareAlpha = o.compareAlpha != 0;
        p.offset = QPoint(o.offsetX, o.offsetY);
        return p;
    }

    // Runs on a Rust worker thread: copy everything, then hop to the GUI thread.
    static void onNativeEvent(void* user, const OpenbcImageCompareEvent* ev) {
        auto* self = static_cast<PictureCompareViewer*>(user);
        auto event = std::make_shared<ViewEvent>();
        event->kind = ev->kind;
        event->side = ev->side;
        event->seq = ev->seq;
        event->path = str(ev->path);
        event->message = str(ev->message);
        if (ev->kind == OPENBC_IMAGE_LOADED) {
            event->left = info(ev->left);
            event->right = info(ev->right);
            event->leftImage = rgba(ev->leftRgba, event->left.width, event->left.height);
            event->rightImage = rgba(ev->rightRgba, event->right.width, event->right.height);
            event->binaryIdentical = ev->binaryIdentical != 0;
        } else if (ev->kind == OPENBC_IMAGE_RESULT) {
            event->options = optionsFromNative(ev->options);
            event->verdict = ev->verdict;
            event->backend = ev->backend;
            event->binaryIdentical = ev->binaryIdentical != 0;
            event->maxDelta = ev->maxDelta;
            event->overlay = rgba(ev->overlayRgba, int(ev->overlayWidth), int(ev->overlayHeight));
            event->rightAligned = QSize(int(ev->rightWidth), int(ev->rightHeight));
            event->compared = ev->comparedPixels;
            event->different = ev->differentPixels;
            event->similar = ev->similarPixels;
        }
        QMetaObject::invokeMethod(self, [self, event]() { self->applyEvent(*event); }, Qt::QueuedConnection);
    }

    OpenbcImageCompareOptions nativeOptions() const {
        OpenbcImageCompareOptions o{};
        o.mode = std::uint8_t(options_.mode);
        o.tolerance = std::uint8_t(options_.tolerance);
        o.rangeMin = std::uint8_t(options_.rangeMin);
        o.rangeMax = std::uint8_t(options_.rangeMax);
        o.blendPercent = std::uint8_t(options_.blend);
        o.resize = std::uint8_t(options_.resize);
        o.compareAlpha = options_.compareAlpha ? 1 : 0;
        o.offsetX = options_.offset.x();
        o.offsetY = options_.offset.y();
        return o;
    }

    void sendOptions() {
        if (!handle_) return;
        const OpenbcImageCompareOptions native = nativeOptions();
        openbc_image_compare_set_options(handle_, &native);
        if (leftPane_->hasImage() && !busy_) setStatus("Comparing...", false);
    }

    // ===== applying events (GUI thread) ======================================
    void applyEvent(const ViewEvent& e) {
        switch (e.kind) {
            case OPENBC_IMAGE_LOADING:
                busy_ = true;
                loadTimer_.start();
                loadTimerArmed_ = true;
                setStatus("Loading...", false);
                break;
            case OPENBC_IMAGE_LOADED:
                leftInfo_ = e.left;
                rightInfo_ = e.right;
                leftImage_ = e.leftImage;
                rightImage_ = e.rightImage;
                leftPane_->setImage(leftImage_);
                rightPane_->setImage(rightImage_);
                updateInfoLabels();
                refreshMetadata(true);
                break;
            case OPENBC_IMAGE_RESULT:
                applyResult(e);
                break;
            case OPENBC_IMAGE_PATH_ERROR: {
                busy_ = false;
                const bool right = e.side == 1;
                (right ? rightInfoLabel_ : leftInfoLabel_)->setText(e.message);
                (right ? rightInfoLabel_ : leftInfoLabel_)->setStyleSheet("color:#e5534b;");
                (right ? rightPane_ : leftPane_)->setImage(QImage());
                (right ? rightInfo_ : leftInfo_) = ViewInfo();
                refreshMetadata(false);
                overlayPane_->setImage(QImage());
                clearProbe();
                setStatus("Cannot open " + QFileInfo(e.path).fileName(), true);
                break;
            }
            case OPENBC_IMAGE_FAILED:
                busy_ = false;
                setStatus("Compare failed: " + e.message, true);
                break;
            default: break;
        }
    }

    void applyResult(const ViewEvent& e) {
        busy_ = false;
        // The engine echoes the options it used (e.g. a negated offset after Swap).
        options_ = e.options;
        syncControlsFromOptions();
        overlayPane_->setImage(e.overlay);

        const QSize canvas = e.overlay.size();
        leftPane_->setMapping(mappingScale(canvas, leftImage_.size()), QPoint(0, 0));
        rightPane_->setMapping(mappingScale(e.rightAligned, rightImage_.size()), options_.offset);
        overlayPane_->setMapping(QSizeF(1, 1), QPoint(0, 0));
        updateOffsetLabel();

        static const char* texts[] = {"Binary same", "Pixels same", "Minor differences", "Important differences"};
        const bool bad = e.verdict == 3;
        setStatus(texts[std::min<std::uint32_t>(e.verdict, 3)], bad);
        statsLabel_->setText(QString("%1 of %2 pixels differ - max delta %3%4")
                                 .arg(QLocale().toString(qulonglong(e.different)),
                                      QLocale().toString(qulonglong(e.compared)))
                                 .arg(e.maxDelta)
                                 .arg(e.backend == 1 ? " - GPU" : ""));
        if (loadTimerArmed_) {
            loadTimerArmed_ = false;
            timeLabel_->setText(QString("Load time: %1 seconds").arg(loadTimer_.elapsed() / 1000.0, 0, 'f', 2));
        }
        if (hover_) hoverAt(*hover_);  // keep the probe in step with the new alignment
        if (onTitleChanged) onTitleChanged();
    }

    static QSizeF mappingScale(const QSize& aligned, const QSize& native) {
        if (native.isEmpty() || aligned.isEmpty()) return QSizeF(1, 1);
        return QSizeF(double(aligned.width()) / native.width(), double(aligned.height()) / native.height());
    }

    // ===== hover / probe ======================================================
    void hoverAt(const QPoint& canvas) {
        hover_ = canvas;
        for (auto* pane : {leftPane_, rightPane_, overlayPane_}) pane->setCrosshair(canvas);
        OpenbcPixelProbe probe;
        if (!handle_ || !openbc_image_compare_probe(handle_, canvas.x(), canvas.y(), &probe)) {
            clearProbe(false);
            return;
        }
        leftGrid_->setCells(probe.leftPresent, probe.leftRgba);
        rightGrid_->setCells(probe.rightPresent, probe.rightRgba);
        leftPixelLabel_->setText(pixelText(canvas, probe.leftPresent[40], probe.leftRgba + 40 * 4));
        rightPixelLabel_->setText(pixelText(canvas, probe.rightPresent[40], probe.rightRgba + 40 * 4));
    }

    static QString pixelText(const QPoint& at, std::uint8_t present, const std::uint8_t* px) {
        const QString xy = QString("X:%1 Y:%2").arg(at.x()).arg(at.y());
        if (!present) return xy + "\nRGB: -\nA: -";
        return xy + QString("\nRGB:%1,%2,%3\nA:%4").arg(px[0]).arg(px[1]).arg(px[2]).arg(px[3]);
    }

    void clearProbe(bool dropCrosshair = true) {
        if (dropCrosshair) {
            hover_.reset();
            for (auto* pane : {leftPane_, rightPane_, overlayPane_}) pane->setCrosshair(std::nullopt);
        }
        leftGrid_->clear();
        rightGrid_->clear();
        leftPixelLabel_->setText(QString());
        rightPixelLabel_->setText(QString());
    }

    // ===== offset / zoom ======================================================
    void moveOffset(const QPoint& delta) {
        options_.offset += delta;
        updateOffsetLabel();
        sendOptions();
    }

    void updateOffsetLabel() {
        offsetLabel_->setText(QString("Offset:  (%1,%2)").arg(options_.offset.x()).arg(options_.offset.y()));
    }

    void updateAllPanes() {
        for (auto* pane : {leftPane_, rightPane_, overlayPane_}) pane->viewChanged();
        updateZoomLabel();
    }

    void updateZoomLabel() {
        const int percent = int(std::lround(leftPane_->effectiveScale() * 100.0));
        zoomLabel_->setText(QString("Zoom:  %1%").arg(percent));
        const QSignalBlocker block(zoomSlider_);
        zoomSlider_->setValue(std::clamp(percent, zoomSlider_->minimum(), zoomSlider_->maximum()));
        if (fitButton_) {
            const QSignalBlocker fitBlock(fitButton_);
            fitButton_->setChecked(view_.fit);
        }
    }

    void setZoomPercent(int percent) {
        view_.fit = false;
        view_.zoom = std::clamp(percent, 5, 800) / 100.0;
        updateAllPanes();
    }

    void zoomBy(int steps) {
        const double current = leftPane_->effectiveScale();
        setZoomPercent(int(std::lround(current * 100.0 * std::pow(1.25, steps))));
    }

    void fitToWindow() {
        view_.fit = true;
        view_.pan = QPointF();
        updateAllPanes();
    }

    // ===== controls panel side ================================================
    void setControlsOnRight(bool right) {
        controlsRight_ = right;
        if (prefs_) prefs_->setValue("picture/controlsRight", right);
        applyControlsSide();
    }

    // (Re)inserts the controls panel and the result pane into the bottom row, in
    // the order the preference asks for, and syncs the toggle button + menu item.
    void applyControlsSide() {
        if (bottomLayout_ && controlsPanel_ && overlayPane_) {
            bottomLayout_->removeWidget(controlsPanel_);
            bottomLayout_->removeWidget(overlayPane_);
            if (controlsRight_) {
                bottomLayout_->addWidget(overlayPane_, 1);
                bottomLayout_->addWidget(controlsPanel_);
            } else {
                bottomLayout_->addWidget(controlsPanel_);
                bottomLayout_->addWidget(overlayPane_, 1);
            }
        }
        if (sideButton_) {
            const QSignalBlocker b(sideButton_);
            sideButton_->setChecked(controlsRight_);
            sideButton_->setToolTip(controlsRight_ ? "Controls are on the right - click to move them left"
                                                   : "Controls are on the left - click to move them right");
        }
        if (actControlsRight_) {
            const QSignalBlocker b(actControlsRight_);
            actControlsRight_->setChecked(controlsRight_);
        }
    }

    // ===== options <-> controls ===============================================
    void setMode(int mode) {
        options_.mode = mode;
        syncControlsFromOptions();
        sendOptions();
    }

    void syncControlsFromOptions() {
        if (actTol_) {
            QAction* acts[3] = {actTol_, actRange_, actBlend_};
            for (int i = 0; i < 3; ++i) {
                const QSignalBlocker b(acts[i]);
                acts[i]->setChecked(i == options_.mode);
            }
        }
        paramStack_->setCurrentIndex(options_.mode);
        {
            const QSignalBlocker b1(tolSlider_), b2(rangeMinSlider_), b3(rangeMaxSlider_), b4(blendSlider_),
                b5(resizeCombo_), b6(alphaCheck_);
            tolSlider_->setValue(options_.tolerance);
            rangeMinSlider_->setValue(options_.rangeMin);
            rangeMaxSlider_->setValue(options_.rangeMax);
            blendSlider_->setValue(options_.blend);
            resizeCombo_->setCurrentIndex(options_.resize);
            alphaCheck_->setChecked(options_.compareAlpha);
        }
        tolLabel_->setText(QString("Tolerance:  %1").arg(options_.tolerance));
        rangeLabel_->setText(QString("Range:  %1 - %2").arg(options_.rangeMin).arg(options_.rangeMax));
        blendLabel_->setText(QString("Blend:  %1").arg(options_.blend));
        updateOffsetLabel();
    }

    void loadPreferences() {
        if (!prefs_) return;
        options_.mode = std::clamp(prefs_->value("picture/mode", 0).toInt(), 0, 2);
        options_.tolerance = std::clamp(prefs_->value("picture/tolerance", 25).toInt(), 0, 255);
        options_.resize = std::clamp(prefs_->value("picture/resize", 0).toInt(), 0, 5);
        options_.compareAlpha = prefs_->value("picture/compareAlpha", false).toBool();
        showMetadata_ = prefs_->value("picture/showMetadata", true).toBool();
        metaDiffOnly_ = prefs_->value("picture/metadataDiffOnly", false).toBool();
        controlsRight_ = prefs_->value("picture/controlsRight", false).toBool();
    }

    void savePreferences() {
        if (!prefs_) return;
        prefs_->setValue("picture/mode", options_.mode);
        prefs_->setValue("picture/tolerance", options_.tolerance);
        prefs_->setValue("picture/resize", options_.resize);
        prefs_->setValue("picture/compareAlpha", options_.compareAlpha);
        prefs_->setValue("picture/showMetadata", showMetadata_);
        prefs_->setValue("picture/controlsRight", controlsRight_);
        prefs_->setValue("picture/metadataDiffOnly", metaPanel_ ? metaPanel_->onlyDifferences() : metaDiffOnly_);
    }

    // ===== info rows / status ================================================
    static QString infoText(const ViewInfo& i) {
        QStringList parts;
        if (i.hasModified) {
            parts << QLocale().toString(QDateTime::fromMSecsSinceEpoch(i.modifiedMs), QLocale::ShortFormat);
        }
        parts << QLocale().toString(i.size) + " bytes";
        if (!i.format.isEmpty()) parts << i.format;
        parts << QString("%1 x %2 x %3").arg(i.width).arg(i.height).arg(i.bpp);
        return parts.join("    ");
    }

    void updateInfoLabels() {
        leftInfoLabel_->setStyleSheet(QString());
        rightInfoLabel_->setStyleSheet(QString());
        leftInfoLabel_->setText(infoText(leftInfo_));
        rightInfoLabel_->setText(infoText(rightInfo_));
    }

    void setStatus(const QString& text, bool emphasise) {
        statusLabel_->setText(text);
        statusLabel_->setStyleSheet(emphasise ? "color:#e5534b; font-weight:600;" : QString());
    }

    // ===== metadata ===========================================================
    // Rows the backend already reported (file + image) are available instantly;
    // the header-only extras (EXIF, GPS, text chunks, frames) are read on a
    // worker thread and merged in when they arrive.
    PictureMetadata composeMetadata(const ViewInfo& i) const {
        PictureMetadata rows;
        if (i.path.isEmpty()) return rows;
        const QFileInfo file(i.path);
        rows.push_back({"File", "Name", file.fileName()});
        rows.push_back({"File", "Folder", QDir::toNativeSeparators(file.absolutePath())});
        rows.push_back({"File", "Size", QLocale().toString(i.size) + " bytes"});
        if (i.hasModified) {
            rows.push_back({"File", "Modified",
                            QDateTime::fromMSecsSinceEpoch(i.modifiedMs).toString("yyyy-MM-dd HH:mm:ss")});
        }
        if (!i.format.isEmpty()) rows.push_back({"Image", "Format", i.format});
        if (i.width > 0 && i.height > 0) {
            rows.push_back({"Image", "Dimensions", QString("%1 x %2").arg(i.width).arg(i.height)});
            rows.push_back({"Image", "Megapixels",
                            QString::number(double(i.width) * double(i.height) / 1e6, 'f', 2)});
        }
        rows.push_back({"Image", "Bits per pixel", QString::number(i.bpp)});
        rows += metaExtra_.value(i.path);
        return rows;
    }

    // fetch = true after a (re)load: drop the cache and read both files again.
    void refreshMetadata(bool fetch) {
        if (fetch) {
            metaExtra_.clear();
            const int generation = ++metaGeneration_;
            QStringList paths;
            for (const QString& path : {leftInfo_.path, rightInfo_.path}) {
                if (!path.isEmpty() && !paths.contains(path)) paths << path;
            }
            QPointer<PictureCompareViewer> self(this);
            for (const QString& path : paths) {
                QThreadPool::globalInstance()->start(QRunnable::create([self, path, generation]() {
                    PictureMetadata extra = collectPictureMetadata(path);
                    QMetaObject::invokeMethod(
                        qApp,
                        [self, path, generation, extra = std::move(extra)]() mutable {
                            if (!self || generation != self->metaGeneration_) return;  // superseded
                            self->metaExtra_.insert(path, std::move(extra));
                            self->refreshMetadata(false);
                        },
                        Qt::QueuedConnection);
                }));
            }
        }
        if (metaPanel_) metaPanel_->setMetadata(composeMetadata(leftInfo_), composeMetadata(rightInfo_));
    }

    // ===== actions ============================================================
    void swapSides() {
        if (!handle_) return;
        openbc_image_compare_swap(handle_);  // engine swaps sides and negates the offset
        const QString l = leftSelector_->text();
        leftSelector_->setText(rightSelector_->text());
        rightSelector_->setText(l);
        std::swap(leftInfo_, rightInfo_);
        std::swap(leftImage_, rightImage_);
        leftPane_->setImage(leftImage_);
        rightPane_->setImage(rightImage_);
        options_.offset = -options_.offset;  // shown immediately; the Result echo confirms it
        updateInfoLabels();
        refreshMetadata(false);  // extras are cached per path, so a swap needs no re-read
        updateOffsetLabel();
        if (onTitleChanged) onTitleChanged();
    }

    void reload() {
        if (handle_) openbc_image_compare_reload(handle_);
    }

    void createActions() {
        auto make = [this](const QString& text, const QKeySequence& key, std::function<void()> fn, bool checkable = false) {
            auto* a = new QAction(text, this);
            if (!key.isEmpty()) a->setShortcut(key);
            a->setCheckable(checkable);
            connect(a, &QAction::triggered, this, [fn](bool) { fn(); });
            return a;
        };
        using icons::Glyph;
        actSwap_ = make("Swap Sides", QKeySequence(), [this]() { swapSides(); });
        actSwap_->setIcon(icons::glyph(Glyph::Swap));
        actSwap_->setIconText("Swap");  // short caption under the toolbar icon
        actSwap_->setToolTip("Swap left and right");
        actReload_ = make("Reload", QKeySequence("F5"), [this]() { reload(); });
        actReload_->setIcon(icons::glyph(Glyph::Refresh));
        actReload_->setToolTip("Reload both images (F5)");

        actTol_ = make("Tolerance Mode", QKeySequence(), [this]() { setMode(0); }, true);
        actTol_->setIcon(icons::pictureicons::tolerance());
        actTol_->setIconText("Tol");
        actTol_->setToolTip("Highlight pixels that differ by more than the tolerance");
        actRange_ = make("Range Mode", QKeySequence(), [this]() { setMode(1); }, true);
        actRange_->setIcon(icons::pictureicons::range());
        actRange_->setIconText("Range");
        actRange_->setToolTip("Show per-pixel difference magnitude inside a range");
        actBlend_ = make("Blend Mode", QKeySequence(), [this]() { setMode(2); }, true);
        actBlend_->setIcon(icons::pictureicons::blend());
        actBlend_->setIconText("Blend");
        actBlend_->setToolTip("Blend both images");
        for (QAction* a : {actTol_, actRange_, actBlend_}) a->setIconVisibleInMenu(false);

        actMetadata_ = new QAction("Metadata", this);
        actMetadata_->setCheckable(true);
        actMetadata_->setChecked(showMetadata_);
        actMetadata_->setIcon(icons::pictureicons::info());
        actMetadata_->setIconVisibleInMenu(false);
        actMetadata_->setToolTip("Show a side-by-side list of file / image / EXIF properties");
        connect(actMetadata_, &QAction::toggled, this, [this](bool on) {
            showMetadata_ = on;
            if (metaPanel_) metaPanel_->setVisible(on);
        });

        actResetOffset_ = make("Reset Offset", QKeySequence(), [this]() {
            options_.offset = QPoint(0, 0);
            updateOffsetLabel();
            sendOptions();
        });
        actZoomIn_ = make("Zoom In", QKeySequence(Qt::CTRL | Qt::Key_Plus), [this]() { zoomBy(1); });
        actZoomOut_ = make("Zoom Out", QKeySequence(Qt::CTRL | Qt::Key_Minus), [this]() { zoomBy(-1); });
        actFit_ = make("Fit to Window", QKeySequence(Qt::CTRL | Qt::Key_0), [this]() { fitToWindow(); });
        actFit_->setIcon(icons::pictureicons::fit());
        actActual_ = make("Actual Size (1:1)", QKeySequence(Qt::CTRL | Qt::Key_1), [this]() { setZoomPercent(100); });

        actControlsRight_ = new QAction("Controls on Right", this);
        actControlsRight_->setCheckable(true);
        actControlsRight_->setChecked(controlsRight_);
        connect(actControlsRight_, &QAction::triggered, this,
                [this](bool on) { setControlsOnRight(on); });
    }

    // ===== widget construction ================================================
    // Same look as the Text Compare toolbar (icon over caption, styled through the
    // shared "textCompareToolbar" rule in qt_style.h); every button is an action.
    QWidget* buildToolbar() {
        toolbar_ = new QToolBar(this);
        toolbar_->setObjectName("textCompareToolbar");
        toolbar_->setMovable(false);
        toolbar_->setFloatable(false);
        toolbar_->setIconSize(QSize(20, 20));
        toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

        toolbar_->addAction(actTol_);
        toolbar_->addAction(actRange_);
        toolbar_->addAction(actBlend_);
        toolbar_->addSeparator();
        toolbar_->addAction(actSwap_);
        toolbar_->addAction(actReload_);
        toolbar_->addSeparator();

        toolbar_->addWidget(new QLabel("Size:", toolbar_));
        resizeCombo_ = new QComboBox(toolbar_);
        resizeCombo_->addItems({"Auto scale", "Native size", "Fit to left", "Fit to right", "Fit to larger",
                                "Fit to smaller"});
        resizeCombo_->setToolTip("How differently sized images are matched before comparing");
        connect(resizeCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
            options_.resize = i;
            sendOptions();
        });
        toolbar_->addWidget(resizeCombo_);

        alphaCheck_ = new QCheckBox("Compare alpha", toolbar_);
        connect(alphaCheck_, &QCheckBox::toggled, this, [this](bool on) {
            options_.compareAlpha = on;
            sendOptions();
        });
        toolbar_->addWidget(alphaCheck_);
        toolbar_->addSeparator();
        toolbar_->addAction(actMetadata_);
        return toolbar_;
    }

    QWidget* buildPathRows() {
        auto* box = new QWidget(this);
        auto* row = new QHBoxLayout(box);
        row->setContentsMargins(6, 0, 6, 4);
        row->setSpacing(8);
        auto side = [&](PathSelector*& selector, QLabel*& infoLabel, const QString& name) {
            auto* col = new QVBoxLayout;
            col->setSpacing(2);
            // Same row as every other compare viewer: combo + Browse, MRU history, Enter commits.
            selector = new PathSelector(PathSelector::Mode::File, name, box, false, pictureFileDialogFilter(), false);
            selector->onBrowsed = [this](QString) { compare(); };
            connect(selector->combo()->lineEdit(), &QLineEdit::returnPressed, this, [this]() { compare(); });
            connect(selector->combo(), QOverload<int>::of(&QComboBox::activated), this, [this](int) { compare(); });
            infoLabel = new QLabel(box);
            infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            col->addWidget(selector);
            col->addWidget(infoLabel);
            row->addLayout(col, 1);
        };
        side(leftSelector_, leftInfoLabel_, "Left");
        side(rightSelector_, rightInfoLabel_, "Right");
        return box;
    }

    QWidget* buildBody() {
        auto* vertical = new QSplitter(Qt::Vertical, this);

        auto* top = new QSplitter(Qt::Horizontal, vertical);
        leftPane_ = new PictureImagePane(&view_, top);
        rightPane_ = new PictureImagePane(&view_, top);
        leftPane_->setPlaceholder("Drop or choose the left image");
        rightPane_->setPlaceholder("Drop or choose the right image");
        top->addWidget(leftPane_);
        top->addWidget(rightPane_);
        metaPanel_ = new PictureMetadataPanel(top);
        metaPanel_->setOnlyDifferences(metaDiffOnly_);
        metaPanel_->setVisible(showMetadata_);
        top->addWidget(metaPanel_);
        top->setChildrenCollapsible(false);
        top->setStretchFactor(0, 3);
        top->setStretchFactor(1, 3);
        top->setStretchFactor(2, 2);

        auto* bottom = new QWidget(vertical);
        bottomLayout_ = new QHBoxLayout(bottom);
        bottomLayout_->setContentsMargins(6, 6, 6, 6);
        bottomLayout_->setSpacing(8);
        controlsPanel_ = buildControls(bottom);
        overlayPane_ = new PictureImagePane(&view_, bottom);
        overlayPane_->setPlaceholder("Result");
        overlayPane_->setDragMovesOffset(true);
        applyControlsSide();  // adds controls + result in the saved order

        vertical->addWidget(top);
        vertical->addWidget(bottom);
        vertical->setStretchFactor(0, 3);
        vertical->setStretchFactor(1, 2);
        return vertical;
    }

    QWidget* buildControls(QWidget* parent) {
        auto* panel = new QWidget(parent);
        panel->setFixedWidth(250);
        auto* col = new QVBoxLayout(panel);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(4);

        offsetLabel_ = new QLabel(panel);
        zoomLabel_ = new QLabel("Zoom:  100%", panel);

        auto* head = new QHBoxLayout;
        head->setContentsMargins(0, 0, 0, 0);
        head->setSpacing(4);
        head->addWidget(offsetLabel_, 1);
        sideButton_ = new QToolButton(panel);
        sideButton_->setIcon(icons::pictureicons::controlsSide());
        sideButton_->setIconSize(QSize(16, 16));
        sideButton_->setCheckable(true);
        sideButton_->setCursor(Qt::PointingHandCursor);
        connect(sideButton_, &QToolButton::clicked, this, [this](bool on) { setControlsOnRight(on); });
        head->addWidget(sideButton_);
        col->addLayout(head);
        col->addWidget(zoomLabel_);

        auto* zoomRow = new QHBoxLayout;
        zoomSlider_ = new QSlider(Qt::Horizontal, panel);
        zoomSlider_->setRange(5, 400);
        connect(zoomSlider_, &QSlider::valueChanged, this, [this](int v) { setZoomPercent(v); });
        auto* actual = new QToolButton(panel);
        actual->setText("1:1");
        actual->setToolTip("Actual size (Ctrl+1)");
        actual->setMinimumSize(32, 26);
        QFont actualFont = actual->font();
        actualFont.setBold(true);
        actual->setFont(actualFont);
        connect(actual, &QToolButton::clicked, this, [this]() { setZoomPercent(100); });
        fitButton_ = new QToolButton(panel);
        fitButton_->setIcon(icons::pictureicons::fit());
        fitButton_->setIconSize(QSize(16, 16));
        fitButton_->setToolTip("Fit to window (Ctrl+0)");
        fitButton_->setCheckable(true);  // lit while the view is in fit mode
        fitButton_->setChecked(view_.fit);
        fitButton_->setMinimumSize(32, 26);
        fitButton_->setStyleSheet("QToolButton:checked { border-color: #0a64c8; }");
        connect(fitButton_, &QToolButton::clicked, this, [this]() { fitToWindow(); });
        zoomRow->addWidget(zoomSlider_, 1);
        zoomRow->addWidget(actual);
        zoomRow->addWidget(fitButton_);
        col->addLayout(zoomRow);

        paramStack_ = new QStackedWidget(panel);
        paramStack_->addWidget(sliderPage(tolLabel_, tolSlider_, 0, 255, [this](int v) {
            options_.tolerance = v;
            tolLabel_->setText(QString("Tolerance:  %1").arg(v));
            sendOptions();
        }));
        paramStack_->addWidget(rangePage());
        paramStack_->addWidget(sliderPage(blendLabel_, blendSlider_, 0, 100, [this](int v) {
            options_.blend = v;
            blendLabel_->setText(QString("Blend:  %1").arg(v));
            sendOptions();
        }));
        col->addWidget(paramStack_);

        col->addSpacing(6);
        col->addWidget(new QLabel("Pixel details:", panel));
        auto* grids = new QHBoxLayout;
        for (int s = 0; s < 2; ++s) {
            auto* g = new QVBoxLayout;
            auto*& grid = s == 0 ? leftGrid_ : rightGrid_;
            auto*& label = s == 0 ? leftPixelLabel_ : rightPixelLabel_;
            grid = new PicturePixelGrid(panel);
            label = new QLabel(panel);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            g->addWidget(grid);
            g->addWidget(label);
            g->addStretch(1);
            grids->addLayout(g);
        }
        col->addLayout(grids);
        col->addStretch(1);
        return panel;
    }

    QWidget* sliderPage(QLabel*& label, QSlider*& slider, int lo, int hi, std::function<void(int)> onChange) {
        auto* page = new QWidget;
        auto* col = new QVBoxLayout(page);
        col->setContentsMargins(0, 0, 0, 0);
        label = new QLabel(page);
        slider = new QSlider(Qt::Horizontal, page);
        slider->setRange(lo, hi);
        connect(slider, &QSlider::valueChanged, this, [onChange](int v) { onChange(v); });
        col->addWidget(label);
        col->addWidget(slider);
        return page;
    }

    QWidget* rangePage() {
        auto* page = new QWidget;
        auto* col = new QVBoxLayout(page);
        col->setContentsMargins(0, 0, 0, 0);
        rangeLabel_ = new QLabel(page);
        rangeMinSlider_ = new QSlider(Qt::Horizontal, page);
        rangeMaxSlider_ = new QSlider(Qt::Horizontal, page);
        for (QSlider* s : {rangeMinSlider_, rangeMaxSlider_}) s->setRange(0, 255);
        connect(rangeMinSlider_, &QSlider::valueChanged, this, [this](int v) {
            options_.rangeMin = v;
            if (options_.rangeMax < v) {
                options_.rangeMax = v;
                const QSignalBlocker b(rangeMaxSlider_);
                rangeMaxSlider_->setValue(v);
            }
            rangeLabel_->setText(QString("Range:  %1 - %2").arg(options_.rangeMin).arg(options_.rangeMax));
            sendOptions();
        });
        connect(rangeMaxSlider_, &QSlider::valueChanged, this, [this](int v) {
            options_.rangeMax = v;
            if (options_.rangeMin > v) {
                options_.rangeMin = v;
                const QSignalBlocker b(rangeMinSlider_);
                rangeMinSlider_->setValue(v);
            }
            rangeLabel_->setText(QString("Range:  %1 - %2").arg(options_.rangeMin).arg(options_.rangeMax));
            sendOptions();
        });
        col->addWidget(rangeLabel_);
        col->addWidget(rangeMinSlider_);
        col->addWidget(rangeMaxSlider_);
        return page;
    }

    QWidget* buildStatusRow() {
        auto* bar = new QWidget(this);
        auto* row = new QHBoxLayout(bar);
        row->setContentsMargins(8, 3, 8, 3);
        statusLabel_ = new QLabel("Choose two images to compare", bar);
        statsLabel_ = new QLabel(bar);
        timeLabel_ = new QLabel(bar);
        row->addWidget(statusLabel_);
        row->addSpacing(16);
        row->addWidget(statsLabel_, 1);
        row->addWidget(timeLabel_);
        return bar;
    }

    void wirePanes() {
        for (auto* pane : {leftPane_, rightPane_, overlayPane_}) {
            pane->onHover = [this](const QPoint& canvas) { hoverAt(canvas); };
            pane->onLeave = [this]() { clearProbe(); };
            pane->onWheelZoom = [this](int steps) { zoomBy(steps); };
            // Pan state is shared; each pane clamps against its own size, so the pane the
            // user is interacting with decides the limits (no dead zone past the edge).
            pane->onPan = [this, pane](const QPointF& d) {
                view_.pan = pane->clampPan(view_.pan + d);
                updateAllPanes();
            };
            pane->onPanTo = [this, pane](const QPointF& p) {
                view_.pan = pane->clampPan(p);
                updateAllPanes();
            };
        }
        overlayPane_->onDragOffset = [this](const QPoint& d) { moveOffset(d); };
        leftPane_->onResized = [this]() { updateZoomLabel(); };
    }

    // ===== state ==============================================================
    QSettings* prefs_ = nullptr;
    OpenbcImageCompare* handle_ = nullptr;
    PictureOptions options_;
    PictureViewState view_;
    std::optional<QPoint> hover_;
    bool busy_ = false;
    bool loadTimerArmed_ = false;
    QElapsedTimer loadTimer_;

    ViewInfo leftInfo_, rightInfo_;
    PictureMetadataPanel* metaPanel_ = nullptr;
    QHash<QString, PictureMetadata> metaExtra_;  // by path; header-only extras
    int metaGeneration_ = 0;
    bool showMetadata_ = true;
    bool metaDiffOnly_ = false;
    QImage leftImage_, rightImage_;

    PathSelector* leftSelector_ = nullptr;
    PathSelector* rightSelector_ = nullptr;
    QLabel* leftInfoLabel_ = nullptr;
    QLabel* rightInfoLabel_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    QComboBox* resizeCombo_ = nullptr;
    QCheckBox* alphaCheck_ = nullptr;
    PictureImagePane* leftPane_ = nullptr;
    PictureImagePane* rightPane_ = nullptr;
    PictureImagePane* overlayPane_ = nullptr;
    QHBoxLayout* bottomLayout_ = nullptr;
    QWidget* controlsPanel_ = nullptr;
    QToolButton* sideButton_ = nullptr;  // controls left/right toggle
    QToolButton* fitButton_ = nullptr;
    bool controlsRight_ = false;         // pref "picture/controlsRight"
    QLabel* offsetLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QSlider* zoomSlider_ = nullptr;
    QStackedWidget* paramStack_ = nullptr;
    QLabel* tolLabel_ = nullptr;
    QSlider* tolSlider_ = nullptr;
    QLabel* rangeLabel_ = nullptr;
    QSlider* rangeMinSlider_ = nullptr;
    QSlider* rangeMaxSlider_ = nullptr;
    QLabel* blendLabel_ = nullptr;
    QSlider* blendSlider_ = nullptr;
    PicturePixelGrid* leftGrid_ = nullptr;
    PicturePixelGrid* rightGrid_ = nullptr;
    QLabel* leftPixelLabel_ = nullptr;
    QLabel* rightPixelLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;

    QAction* actSwap_ = nullptr;
    QAction* actReload_ = nullptr;
    QAction* actTol_ = nullptr;
    QAction* actRange_ = nullptr;
    QAction* actBlend_ = nullptr;
    QAction* actMetadata_ = nullptr;
    QAction* actControlsRight_ = nullptr;
    QAction* actResetOffset_ = nullptr;
    QAction* actZoomIn_ = nullptr;
    QAction* actZoomOut_ = nullptr;
    QAction* actFit_ = nullptr;
    QAction* actActual_ = nullptr;
};

}  // namespace openbc::app
// diff_navigator.h
// ---------------------------------------------------------------------------
// Narrow sidebar on the right edge of the right-hand editor (Beyond Compare layout): a full-height strip
// with two small buttons stacked at its bottom end. The upper one jumps to the previous difference
// section, the lower one to the next. Shared by TextCompareViewer and VirtualTextCompareViewer; it is a
// normal layout item placed to the right of the right editor, so it never overlaps the text or gutter.
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QToolTip>
#include <QWidget>
#include <functional>

namespace openbc::app {

class DiffNavigator : public QWidget {
public:
    static constexpr int kWidth = 20;
    static constexpr int kButton = 20;

    explicit DiffNavigator(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("diffNavigator"));
        setFixedWidth(kWidth);
        setMouseTracking(true);
        // Beyond Compare shows these tips on a yellow note.
        setStyleSheet(QStringLiteral(
            "QToolTip { background: #ffffe1; color: #000000; border: 1px solid #808080; padding: 2px; }"));
    }

    std::function<void()> onPrevious;
    std::function<void()> onNext;

protected:
    QRect previousRect() const { return QRect(0, height() - 2 * kButton, kWidth, kButton); }
    QRect nextRect() const { return QRect(0, height() - kButton, kWidth, kButton); }
    int buttonAt(const QPoint& p) const {
        if (!isEnabled()) return 0;
        return previousRect().contains(p) ? 1 : nextRect().contains(p) ? 2 : 0;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        p.setPen(QColor(0x3a, 0x3a, 0x3a));
        p.drawLine(0, 0, 0, height());
        const auto button = [&](const QRect& r, bool up, bool hot) {
            if (hot) p.fillRect(r.adjusted(1, 1, -1, -1), QColor(0x44, 0x47, 0x5a));
            p.setRenderHint(QPainter::Antialiasing, true);
            const qreal cx = r.center().x() + 0.5, cy = r.center().y() + 0.5;
            QPainterPath path;
            if (up) { path.moveTo(cx - 5, cy + 3); path.lineTo(cx + 5, cy + 3); path.lineTo(cx, cy - 4); }
            else    { path.moveTo(cx - 5, cy - 3); path.lineTo(cx + 5, cy - 3); path.lineTo(cx, cy + 4); }
            path.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(!isEnabled() ? QColor(0x70, 0x70, 0x70)
                                    : hot ? QColor(0xf2, 0xc0, 0x4a) : QColor(0xd8, 0xd8, 0xd8));
            p.drawPath(path);
            p.setRenderHint(QPainter::Antialiasing, false);
        };
        button(previousRect(), true, hover_ == 1);
        button(nextRect(), false, hover_ == 2);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const int h = buttonAt(e->position().toPoint());
        if (h != hover_) { hover_ = h; update(); setCursor(h ? Qt::PointingHandCursor : Qt::ArrowCursor); }
    }
    void leaveEvent(QEvent*) override { hover_ = 0; update(); }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        const int h = buttonAt(e->position().toPoint());
        if (h == 1 && onPrevious) onPrevious();
        else if (h == 2 && onNext) onNext();
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            const auto* he = static_cast<QHelpEvent*>(e);
            const int h = buttonAt(he->pos());
            if (h) QToolTip::showText(he->globalPos(),
                                      h == 1 ? tr("Previous Difference Section") : tr("Next Difference Section"), this);
            else QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

private:
    int hover_ = 0;
};

}  // namespace openbc::app

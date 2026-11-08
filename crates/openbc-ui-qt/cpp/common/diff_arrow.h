// diff_arrow.h
// ---------------------------------------------------------------------------
// The copy arrow drawn in the gutters of the compare editors (LineNumberEditor and
// VirtualTextEditor), as one function so the bottom line-compare strip draws the very same shape.
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QPainter>
#include <QPointF>
#include <QPolygonF>

namespace openbc::app {

inline QColor copyArrowColor() { return QColor(0xf2, 0xc0, 0x4a, 150); }

// `arrowX` is the left edge of an arrow gutter `width` pixels wide; `mid` is the row's vertical centre.
// `pointsRight` is true for the left file's arrow (copies to the right file).
inline void paintCopyArrow(QPainter& p, qreal arrowX, qreal width, qreal mid, bool pointsRight,
                           const QColor& color = copyArrowColor()) {
    const auto x = [=](qreal offset) { return arrowX + (pointsRight ? offset : width - offset); };
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QPolygonF arrow;
    arrow << QPointF(x(2), mid - 2) << QPointF(x(9), mid - 2) << QPointF(x(9), mid - 5)
          << QPointF(x(16), mid) << QPointF(x(9), mid + 5) << QPointF(x(9), mid + 2)
          << QPointF(x(2), mid + 2);
    p.drawPolygon(arrow);
    p.restore();
}

}  // namespace openbc::app

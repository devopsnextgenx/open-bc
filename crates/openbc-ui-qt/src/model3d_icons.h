// model3d_icons.h - code-drawn toolbar glyphs for the 3D Compare tab (16x16 design space, like
// qt_style.h). Kept out of qt_style.h so the 3D tab stays self-contained.
#pragma once
#include <QLinearGradient>
#include "qt_style.h"

namespace openbc::ui::icons::model3dicons {

inline const QColor kInk(0xdd, 0xe0, 0xee);
inline const QColor kRed(0xe8, 0x40, 0x40);
inline const QColor kBlue(0x3c, 0x82, 0xf0);
inline const QColor kAccent(0x4e, 0x9c, 0xff);

inline QIcon twoUp() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.2)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.4, 3.0, 5.6, 10.0), 1.0, 1.0);
        p.drawRoundedRect(QRectF(9.0, 3.0, 5.6, 10.0), 1.0, 1.0);
    });
}
inline QIcon oneUp() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.2)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(2.0, 3.0, 12.0, 10.0), 1.2, 1.2);
    });
}
inline QIcon split() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.2)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(2.0, 3.0, 12.0, 10.0), 1.2, 1.2);
        p.setPen(Qt::NoPen); p.setBrush(QColor(0xdd, 0xe0, 0xee, 90));
        p.drawRect(QRectF(2.6, 3.6, 5.0, 8.8));
        p.setPen(QPen(kAccent, 1.4)); p.drawLine(QPointF(8.0, 2.0), QPointF(8.0, 14.0));
    });
}
inline QIcon overlay() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.1));
        p.setBrush(QColor(0xdd, 0xe0, 0xee, 70)); p.drawRoundedRect(QRectF(1.5, 1.5, 8.5, 8.5), 1.0, 1.0);
        p.setBrush(QColor(0x4e, 0x9c, 0xff, 110)); p.drawRoundedRect(QRectF(6.0, 6.0, 8.5, 8.5), 1.0, 1.0);
    });
}
inline QIcon heatmap() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        QLinearGradient g(2, 0, 14, 0);
        g.setColorAt(0.0, kBlue); g.setColorAt(0.5, QColor(0xcc, 0xcc, 0xcc)); g.setColorAt(1.0, kRed);
        p.setPen(Qt::NoPen); p.setBrush(g);
        p.drawRoundedRect(QRectF(1.5, 3.0, 13.0, 10.0), 1.5, 1.5);
        p.setPen(QPen(QColor(0, 0, 0, 90), 1.0)); p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(5, 4.5), QPointF(11, 11.5));
    });
}
inline QIcon changes() {   // red "-" tile + blue "+" tile
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(kRed); p.drawRoundedRect(QRectF(1.2, 3.0, 6.4, 10.0), 1.0, 1.0);
        p.setBrush(kBlue); p.drawRoundedRect(QRectF(8.4, 3.0, 6.4, 10.0), 1.0, 1.0);
        p.setPen(QPen(Qt::white, 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2.9, 8.0), QPointF(5.9, 8.0));
        p.drawLine(QPointF(10.1, 8.0), QPointF(13.1, 8.0)); p.drawLine(QPointF(11.6, 6.5), QPointF(11.6, 9.5));
    });
}
inline QIcon wireframe() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.1)); p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2.0, 5.0, 8.0, 8.0)); p.drawRect(QRectF(5.0, 2.0, 8.0, 8.0));
        p.drawLine(QPointF(2, 5), QPointF(5, 2)); p.drawLine(QPointF(10, 5), QPointF(13, 2));
        p.drawLine(QPointF(2, 13), QPointF(5, 10)); p.drawLine(QPointF(10, 13), QPointF(13, 10));
    });
}
inline QIcon alignment() {   // axes with arrow tips
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(3, 2), QPointF(3, 13)); p.drawLine(QPointF(3, 13), QPointF(14, 13));
        p.drawLine(QPointF(1.4, 4), QPointF(3, 2)); p.drawLine(QPointF(4.6, 4), QPointF(3, 2));
        p.drawLine(QPointF(12, 11.4), QPointF(14, 13)); p.drawLine(QPointF(12, 14.6), QPointF(14, 13));
        p.setPen(QPen(kAccent, 1.4)); p.drawLine(QPointF(6, 10), QPointF(10, 6));
    });
}
inline QIcon sidePanel() {   // window with a right strip; filled when checked (QIcon::On)
    return make([](QPainter& p, QIcon::Mode, QIcon::State s) {
        p.setPen(QPen(kInk, 1.2)); p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.6, 2.6, 12.8, 10.8), 1.2, 1.2);
        p.setPen(Qt::NoPen); p.setBrush(s == QIcon::On ? kAccent : QColor(0xdd, 0xe0, 0xee, 70));
        p.drawRect(QRectF(9.4, 3.2, 4.4, 9.6));
    });
}
inline QIcon settings() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2, 5), QPointF(14, 5)); p.drawLine(QPointF(2, 11), QPointF(14, 11));
        p.setBrush(kBlue.lighter(130)); p.setPen(QPen(kInk, 1.0));
        p.drawEllipse(QPointF(5.5, 5), 1.9, 1.9); p.drawEllipse(QPointF(10.5, 11), 1.9, 1.9);
    });
}
inline QIcon more() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen); p.setBrush(kInk);
        for (double x : {3.0, 8.0, 13.0}) p.drawEllipse(QPointF(x, 8.0), 1.4, 1.4);
    });
}
inline QIcon section() {   // box cut by a plane
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.1)); p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(2.5, 4.5, 9.0, 9.0));
        p.setPen(QPen(kAccent, 1.4)); p.drawLine(QPointF(1, 9), QPointF(15, 4));
    });
}
inline QIcon prev() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({QPointF(10, 3), QPointF(5, 8), QPointF(10, 13)}));
    });
}
inline QIcon next() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        p.drawPolyline(QPolygonF({QPointF(6, 3), QPointF(11, 8), QPointF(6, 13)}));
    });
}
inline QIcon fitModel() {   // corner brackets
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.4, Qt::SolidLine, Qt::RoundCap)); p.setBrush(Qt::NoBrush);
        for (int sx : {0, 1}) for (int sy : {0, 1}) {
            const double x = sx ? 14 : 2, y = sy ? 14 : 2, dx = sx ? -4 : 4, dy = sy ? -4 : 4;
            p.drawLine(QPointF(x, y), QPointF(x + dx, y)); p.drawLine(QPointF(x, y), QPointF(x, y + dy));
        }
        p.setBrush(kAccent); p.setPen(Qt::NoPen); p.drawRect(QRectF(6, 6, 4, 4));
    });
}
inline QIcon probe() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.3)); p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8, 8), 4.5, 4.5);
        p.drawLine(QPointF(8, 1), QPointF(8, 4)); p.drawLine(QPointF(8, 12), QPointF(8, 15));
        p.drawLine(QPointF(1, 8), QPointF(4, 8)); p.drawLine(QPointF(12, 8), QPointF(15, 8));
    });
}
inline QIcon icp() {   // two point clouds converging
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(kRed); for (QPointF c : {QPointF(3, 4), QPointF(6, 3), QPointF(4, 7)}) p.drawEllipse(c, 1.2, 1.2);
        p.setBrush(kBlue); for (QPointF c : {QPointF(11, 12), QPointF(13, 9), QPointF(9, 13)}) p.drawEllipse(c, 1.2, 1.2);
        p.setPen(QPen(kInk, 1.2)); p.drawLine(QPointF(6, 6), QPointF(10, 10));
    });
}
inline QIcon parts() {   // separate pieces snapping into place
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(kInk, 1.1)); p.setBrush(QColor(0xdd, 0xe0, 0xee, 70));
        p.drawRect(QRectF(1.5, 1.5, 5.5, 5.5)); p.drawRect(QRectF(9, 9, 5.5, 5.5));
        p.setPen(QPen(kAccent, 1.2)); p.drawLine(QPointF(7.5, 7.5), QPointF(8.8, 8.8));
    });
}

}  // namespace openbc::ui::icons::model3dicons

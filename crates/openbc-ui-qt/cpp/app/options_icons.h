// options_icons.h
// ---------------------------------------------------------------------------
// Sidebar icons for the Options dialog pages (Startup, Tabs, Appearance, ...).
// They are drawn as vectors through openbc::ui::icons::make(), like every other
// icon in the app, so no image resources are needed and they stay crisp on
// HiDPI. Each paints in the engine's 16x16 design space.
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QtMath>

#include "style/qt_style.h"

namespace openbc::app::options_icons {

namespace detail {

constexpr double kPi = 3.14159265358979323846;

// A toothed gear outline with a round hole in the middle.
inline QPainterPath gear(QPointF center, qreal outer, qreal inner, int teeth, qreal hole) {
    QPainterPath path;
    const qreal step = 2.0 * kPi / teeth;
    for (int i = 0; i < teeth; ++i) {
        const qreal a = i * step;
        const qreal angles[4] = {a - step * 0.32, a - step * 0.17, a + step * 0.17, a + step * 0.32};
        const qreal radii[4] = {inner, outer, outer, inner};
        for (int k = 0; k < 4; ++k) {
            const QPointF pt(center.x() + radii[k] * qCos(angles[k]),
                             center.y() + radii[k] * qSin(angles[k]));
            if (i == 0 && k == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
    }
    path.closeSubpath();
    QPainterPath cut;
    cut.addEllipse(center, hole, hole);
    return path.subtracted(cut);
}

inline QPen stroke(const QColor& color, qreal width) {
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

}  // namespace detail

inline QIcon startup() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setBrush(Qt::NoBrush);
        p.setPen(detail::stroke(QColor(0x6f, 0xb3, 0xa2), 1.9));
        p.drawArc(QRectF(2.8, 3.2, 10.4, 10.4), 120 * 16, 300 * 16);   // power ring, gap at the top
        p.drawLine(QPointF(8, 1.6), QPointF(8, 7.4));
    });
}

inline QIcon tabs() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        const QColor orange(0xe8, 0x8f, 0x3a);
        p.setBrush(orange);
        p.drawRoundedRect(QRectF(1.5, 1.6, 6.0, 3.6), 1.0, 1.0);        // the tab
        p.drawRoundedRect(QRectF(1.5, 3.6, 13.0, 10.8), 1.2, 1.2);      // the window
        p.setBrush(QColor(0x3a, 0x3a, 0x3a));
        p.drawRect(QRectF(3.0, 7.0, 10.0, 5.8));
    });
}

inline QIcon appearance() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        const QRectF r(1.5, 1.5, 13.0, 13.0);
        struct Slice { QColor color; int start; int span; };
        const Slice slices[] = {
            {QColor(0xe8, 0x8f, 0x3a), 0 * 16, 150 * 16},
            {QColor(0x4a, 0x86, 0xc8), 150 * 16, 90 * 16},
            {QColor(0x6a, 0xa8, 0x4f), 240 * 16, 80 * 16},
            {QColor(0xd9, 0x53, 0x4f), 320 * 16, 40 * 16},
        };
        for (const auto& s : slices) {
            p.setBrush(s.color);
            p.drawPie(r, s.start, s.span);
        }
    });
}

inline QIcon textEditing() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe6, 0xe6, 0xe6));
        p.drawRoundedRect(QRectF(2.0, 1.5, 9.5, 13.0), 1.2, 1.2);       // note page
        p.setPen(detail::stroke(QColor(0x8a, 0x8a, 0x8a), 0.9));
        for (qreal y : {4.6, 7.0, 9.4}) p.drawLine(QPointF(3.8, y), QPointF(9.4, y));
        p.setPen(detail::stroke(QColor(0x4a, 0x86, 0xc8), 2.3));        // pencil
        p.drawLine(QPointF(8.2, 12.4), QPointF(13.4, 4.4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x4a, 0x86, 0xc8));
        p.drawPolygon(QPolygonF({QPointF(6.8, 14.6), QPointF(7.4, 12.2), QPointF(9.0, 13.2)}));
    });
}

inline QIcon syntaxHighlighting() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2d, 0x2d, 0x30));
        p.drawRoundedRect(QRectF(1.2, 2.0, 13.6, 12.0), 1.4, 1.4);                  // editor
        struct Bar { qreal x, y, w; QColor color; };
        const Bar bars[] = {                                                          // colored tokens
            {3.0, 4.4, 3.0, QColor(0xc5, 0x86, 0xc0)}, {6.8, 4.4, 4.6, QColor(0x56, 0x9c, 0xd6)},
            {3.0, 7.2, 1.6, QColor(0x4e, 0xc9, 0xb0)}, {5.4, 7.2, 5.6, QColor(0xce, 0x91, 0x78)},
            {3.0, 10.0, 2.6, QColor(0xd7, 0xba, 0x7d)}, {6.4, 10.0, 5.0, QColor(0xd4, 0xd4, 0xd4)},
        };
        for (const Bar& b : bars) {
            p.setBrush(b.color);
            p.drawRoundedRect(QRectF(b.x, b.y, b.w, 1.6), 0.6, 0.6);
        }
    });
}

inline QIcon nextDifference() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x4a, 0x7f, 0xc1));
        p.drawEllipse(QPointF(8, 8), 7.0, 7.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(detail::stroke(Qt::white, 1.7));
        p.drawLine(QPointF(8, 4.2), QPointF(8, 11.2));
        p.drawLine(QPointF(5.2, 8.5), QPointF(8, 11.4));
        p.drawLine(QPointF(10.8, 8.5), QPointF(8, 11.4));
    });
}

inline QIcon backups() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xe6, 0xe6, 0xe6));
        p.drawRoundedRect(QRectF(2.0, 1.5, 9.0, 12.5), 1.2, 1.2);
        p.setPen(detail::stroke(QColor(0x8a, 0x8a, 0x8a), 0.9));
        for (qreal y : {4.4, 6.8}) p.drawLine(QPointF(3.8, y), QPointF(9.2, y));
        p.setPen(detail::stroke(QColor(0x4f, 0xb2, 0x86), 2.2));         // "+"
        p.drawLine(QPointF(9.6, 11.6), QPointF(14.6, 11.6));
        p.drawLine(QPointF(12.1, 9.1), QPointF(12.1, 14.1));
    });
}

inline QIcon fileOperations() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(detail::stroke(QColor(0xf0, 0xb0, 0x30), 1.6));
        p.setBrush(QColor(0xf0, 0xb0, 0x30));
        p.drawPolygon(QPolygonF({QPointF(8, 1.8), QPointF(14.6, 13.8), QPointF(1.4, 13.8)}));   // warning triangle
        p.setPen(detail::stroke(QColor(0x3a, 0x3a, 0x3a), 1.7));
        p.drawLine(QPointF(8, 6.0), QPointF(8, 9.6));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3a, 0x3a, 0x3a));
        p.drawEllipse(QPointF(8, 11.8), 0.95, 0.95);
    });
}

inline QIcon archiveTypes() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xb9, 0x78, 0x3a));
        p.drawRoundedRect(QRectF(2.0, 5.0, 12.0, 9.5), 1.0, 1.0);        // box
        p.setBrush(QColor(0xdc, 0xa0, 0x5c));
        p.drawRoundedRect(QRectF(1.2, 2.0, 13.6, 4.2), 1.0, 1.0);        // lid
        p.setBrush(QColor(0x4a, 0x30, 0x18));
        p.drawRoundedRect(QRectF(5.8, 8.4, 4.4, 1.8), 0.6, 0.6);         // handle slot
    });
}

inline QIcon commands() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xc4, 0xc4, 0xc4));
        p.drawRoundedRect(QRectF(0.8, 3.4, 14.4, 9.6), 1.4, 1.4);        // keyboard
        p.setBrush(QColor(0x45, 0x45, 0x45));
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 5; ++col)
                p.drawRect(QRectF(2.2 + col * 2.6, 4.9 + row * 2.6, 1.8, 1.8));
        p.drawRect(QRectF(4.0, 10.3, 8.0, 1.6));                         // space bar
    });
}

inline QIcon openWith() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x4a, 0x7f, 0xc1));
        p.drawRoundedRect(QRectF(1.2, 2.0, 13.4, 10.8), 1.2, 1.2);       // window
        p.setBrush(QColor(0x8f, 0xb4, 0xe3));
        p.drawRect(QRectF(1.2, 2.0, 13.4, 3.0));                         // title bar
        p.setBrush(QColor(0xf0, 0xb0, 0x30));
        p.drawPath(detail::gear(QPointF(11.4, 11.4), 4.2, 3.0, 8, 1.2)); // gear badge
    });
}

inline QIcon tweaks() {
    return openbc::ui::icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf0, 0xb0, 0x30));
        p.drawPath(detail::gear(QPointF(6.2, 9.4), 5.2, 3.8, 8, 1.6));
        p.setBrush(QColor(0xe0, 0x9a, 0x20));
        p.drawPath(detail::gear(QPointF(12.0, 4.4), 3.2, 2.2, 6, 1.0));
    });
}

// Icon for a page by its title; empty QIcon for anything unknown.
inline QIcon forPage(const QString& title) {
    if (title == "Startup") return startup();
    if (title == "Tabs") return tabs();
    if (title == "Appearance") return appearance();
    if (title == "Text Editing") return textEditing();
    if (title == "Syntax Highlighting") return syntaxHighlighting();
    if (title == "Next Difference") return nextDifference();
    if (title == "Backups") return backups();
    if (title == "File Operations") return fileOperations();
    if (title == "Archive Types") return archiveTypes();
    if (title == "Commands") return commands();
    if (title == "Open With") return openWith();
    if (title == "Tweaks") return tweaks();
    return QIcon();
}

}  // namespace openbc::app::options_icons
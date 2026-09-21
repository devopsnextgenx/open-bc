#pragma once

// Look & feel for the OpenBC Qt front-end (dark, Beyond-Compare-like).
//
// Everything here is header-only and free of Q_OBJECT so it works with any
// build setup (no moc step, no .qrc resources): icons are drawn with QPainter
// and the tree's dotted connector lines are painted by CompareTree.

#include <QAbstractItemView>
#include <QColor>
#include <QFontMetrics>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QIconEngine>
#include <QItemSelectionModel>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QScrollBar>
#include <QString>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <functional>
#include <vector>

namespace openbc::ui {

// ---------------------------------------------------------------------------
// Comparison vocabulary shared by the UI and the comparison engine
// ---------------------------------------------------------------------------

// Status of ONE side of a row (the two sides of a pair can differ, e.g. the
// left file is Newer while the right one is Older).
enum class RowStatus { Pending, Equal, Newer, Older, Different, Orphan };

// Status of the PAIR, used by the "show/hide" toggles in the toolbar.
enum class PairClass {
    Pending = -1,
    Same = 0,
    OrphanLeft,
    OrphanRight,
    LeftNewer,
    RightNewer,
    Different,
};
constexpr int kPairClassCount = 6;

constexpr int kPathRole = Qt::UserRole;
constexpr int kStatusRole = Qt::UserRole + 1;
constexpr int kClassRole = Qt::UserRole + 2;
constexpr int kIsDirRole = Qt::UserRole + 3;

inline constexpr quint32 statusBit(RowStatus status) {
    return 1u << static_cast<int>(status);
}

// ---------------------------------------------------------------------------
// Palette (sampled from the reference screenshot)
// ---------------------------------------------------------------------------

namespace color {
inline QColor windowBg() { return QColor(0x2d, 0x2d, 0x2d); }
inline QColor chromeBg() { return QColor(0x3e, 0x3e, 0x3e); }
inline QColor inputBg() { return QColor(0x23, 0x23, 0x23); }
inline QColor headerBg() { return QColor(0x36, 0x36, 0x36); }
inline QColor border() { return QColor(0x50, 0x50, 0x50); }
inline QColor rowA() { return QColor(0x26, 0x28, 0x33); }
inline QColor rowB() { return QColor(0x2d, 0x2f, 0x3e); }
inline QColor selection() { return QColor(0x44, 0x47, 0x5a); }
inline QColor selectionBorder() { return QColor(0x5f, 0x63, 0x7f); }
inline QColor text() { return QColor(0xf8, 0xf8, 0xf2); }
inline QColor dimText() { return QColor(0x62, 0x72, 0xa4); }
inline QColor treeLine() { return QColor(0x76, 0x7a, 0x92); }

inline QColor same() { return QColor(0xf1, 0xfa, 0x8c); }
inline QColor newer() { return QColor(0x50, 0xfa, 0x7b); }
inline QColor older() { return QColor(0xfa, 0xb6, 0x8d); }
inline QColor different() { return QColor(0xff, 0x55, 0x55); }
inline QColor orphan() { return QColor(0xbd, 0x93, 0xf9); }
inline QColor equalMarker() { return QColor(0x9a, 0xa0, 0xb8); }

inline QColor folderYellow() { return QColor(0xff, 0xe9, 0x8f); }
inline QColor folderRed() { return QColor(0xff, 0x8f, 0x8f); }
inline QColor folderGreen() { return QColor(0x6e, 0xf0, 0x8f); }
inline QColor folderOrange() { return QColor(0xff, 0xb2, 0x7a); }
inline QColor folderPurple() { return QColor(0xb7, 0x87, 0xff); }
}  // namespace color

inline QColor statusTextColor(RowStatus status) {
    switch (status) {
    case RowStatus::Newer:
        return color::newer();
    case RowStatus::Older:
        return color::older();
    case RowStatus::Different:
        return color::different();
    case RowStatus::Orphan:
        return color::orphan();
    case RowStatus::Equal:
    case RowStatus::Pending:
    default:
        return color::text();
    }
}

inline QPalette darkPalette() {
    QPalette p;
    p.setColor(QPalette::Window, color::windowBg());
    p.setColor(QPalette::WindowText, color::text());
    p.setColor(QPalette::Base, color::inputBg());
    p.setColor(QPalette::AlternateBase, color::rowB());
    p.setColor(QPalette::ToolTipBase, color::inputBg());
    p.setColor(QPalette::ToolTipText, color::text());
    p.setColor(QPalette::Text, color::text());
    p.setColor(QPalette::Button, color::chromeBg());
    p.setColor(QPalette::ButtonText, color::text());
    p.setColor(QPalette::BrightText, QColor("#ffffff"));
    p.setColor(QPalette::Highlight, color::selection());
    p.setColor(QPalette::HighlightedText, color::text());
    p.setColor(QPalette::Link, QColor("#8be9fd"));
    p.setColor(QPalette::PlaceholderText, QColor("#7b809a"));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#777b8f"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#777b8f"));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#777b8f"));
    return p;
}

// ---------------------------------------------------------------------------
// Application style sheet
// ---------------------------------------------------------------------------

inline QString applicationStyleSheet() {
    return QStringLiteral(R"(
        QMainWindow, QDialog { background: #2d2d2d; }
        QWidget { color: #f8f8f2; }
        QToolTip {
            background: #232323; color: #f8f8f2;
            border: 1px solid #505050; padding: 3px;
        }

        /* ---- menus ---- */
        QMenuBar { background: #2d2d2d; color: #f8f8f2; padding: 1px 2px; }
        QMenuBar::item { background: transparent; padding: 4px 8px; }
        QMenuBar::item:selected { background: #44475a; }
        QMenu { background: #2d2d2d; border: 1px solid #505050; padding: 3px 0; }
        QMenu::item { background: transparent; padding: 5px 30px 5px 30px; }
        QMenu::item:selected { background: #44475a; }
        QMenu::item:disabled { color: #777b8f; }
        QMenu::icon { padding-left: 6px; }
        QMenu::separator { height: 1px; background: #505050; margin: 4px 6px; }

        /* ---- session tabs ---- */
        QTabWidget::pane { border: 0; background: #3e3e3e; }
        QTabBar { background: #2d2d2d; }
        QTabBar::tab {
            background: #333333; color: #b9b9b9;
            border: 1px solid #4a4a4a; border-bottom: none;
            border-top-left-radius: 5px; border-top-right-radius: 5px;
            padding: 6px 8px 6px 10px; margin: 6px 2px 0 0;
            min-width: 110px; max-width: 300px;
        }
        QTabBar::tab:hover:!selected { background: #3a3a3a; color: #e0e0e0; }
        QTabBar::tab:selected {
            background: #3e3e3e; color: #ffffff; border-color: #5f5f5f;
        }
        QTabBar QToolButton { background: #333333; border: 0; }
        QToolButton#tabClose {
            background: transparent; border: 0; border-radius: 3px; padding: 1px;
        }
        QToolButton#tabClose:hover { background: #55585f; }
        QToolButton#newTab { background: transparent; border: 0; border-radius: 3px; margin: 6px 4px 0 4px; padding: 3px; }
        QToolButton#newTab:hover { background: #4d4d4d; }

        /* ---- per-tab toolbar ---- */
        QToolBar {
            background: #3e3e3e; border: 0; border-top: 1px solid #505050;
            spacing: 3px; padding: 3px 5px;
        }
        QToolBar::separator { background: #5a5a5a; width: 1px; margin: 4px 5px; }
        QToolBar QLabel { background: transparent; padding: 0 3px 0 6px; }
        QToolButton {
            background: transparent; border: 1px solid transparent;
            border-radius: 3px; padding: 2px;
        }
        QToolButton:hover { background: #4d4d4d; border-color: #5f5f5f; }
        QToolButton:pressed { background: #232323; }
        QToolButton:checked { background: #232323; border-color: #232323; }
        QToolButton:checked:hover { border-color: #5f5f5f; }
        QToolButton:disabled { background: transparent; }

        /* ---- inputs ---- */
        QLineEdit, QComboBox {
            background: #232323; color: #f8f8f2;
            border: 1px solid #4a4a4a; border-radius: 2px;
            padding: 1px 6px; min-height: 22px;
            selection-background-color: #44475a; selection-color: #f8f8f2;
        }
        QLineEdit:focus, QComboBox:focus { border-color: #6272a4; }
        QComboBox QAbstractItemView {
            background: #2d2d2d; border: 1px solid #505050; color: #f8f8f2;
            selection-background-color: #44475a; selection-color: #f8f8f2;
            outline: 0;
        }
        QPushButton {
            padding: 4px 10px; min-height: 22px;
            border: 1px solid #5a5a5a; border-radius: 3px;
            background: #454545; color: #f8f8f2;
        }
        QPushButton:hover { background: #505050; }

        /* ---- panes ---- */
        QSplitter::handle:horizontal { background: #505050; width: 3px; }
        QSplitter::handle:vertical { background: #2d2d2d; height: 4px; }
        QFrame#pathBar { background: #232323; border: 0; }
        QFrame#pathBar QComboBox { border-color: #232323; border-radius: 0; }
        QFrame#pathBar QComboBox:focus { border-color: #6272a4; }
        QFrame#pathBar QToolButton { margin: 0 1px; }
        QFrame#footer { background: #3e3e3e; border: 0; border-top: 1px solid #505050; }
        QFrame#footer QLabel { background: transparent; padding: 3px 6px; }
        QFrame#footerDivider { background: #5a5a5a; max-width: 1px; }
        QFrame#gutterHeader {
            background: #363636; border: 0; border-bottom: 1px solid #505050;
        }
        QFrame#gutterBody { background: #262833; border: 0; }

        QTreeView, QTreeWidget {
            background: #262833; border: 0; outline: 0;
        }
        QHeaderView { background: #363636; border: 0; }
        QHeaderView::section {
            background: #363636; color: #f8f8f2;
            padding: 4px 7px; border: 0;
            border-right: 1px solid #4a4a4a; border-bottom: 1px solid #505050;
        }

        QPlainTextEdit#console {
            background: #232323; color: #f8f8f2; border: 0;
            border-top: 1px solid #505050; padding: 3px 6px;
            selection-background-color: #44475a;
        }

        QScrollBar:vertical { background: #232323; width: 13px; margin: 0; border: 0; }
        QScrollBar:horizontal { background: #232323; height: 13px; margin: 0; border: 0; }
        QScrollBar::handle:vertical { background: #555a6b; min-height: 28px; border-radius: 4px; margin: 2px; }
        QScrollBar::handle:horizontal { background: #555a6b; min-width: 28px; border-radius: 4px; margin: 2px; }
        QScrollBar::handle:hover { background: #6b7189; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )");
}

// ---------------------------------------------------------------------------
// Vector icons (no image resources needed)
// ---------------------------------------------------------------------------

namespace icons {

// Paints in a 16x16 design space; the engine scales that to whatever size /
// devicePixelRatio Qt asks for, so icons stay crisp on HiDPI displays.
using PaintFn = std::function<void(QPainter&, QIcon::Mode, QIcon::State)>;

class Engine : public QIconEngine {
public:
    explicit Engine(PaintFn fn) : fn_(std::move(fn)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (mode == QIcon::Disabled) {
            painter->setOpacity(painter->opacity() * 0.4);
        }
        painter->translate(rect.topLeft());
        painter->scale(rect.width() / 16.0, rect.height() / 16.0);
        fn_(*painter, mode, state);
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        paint(&painter, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QIconEngine* clone() const override { return new Engine(fn_); }

private:
    PaintFn fn_;
};

inline QIcon make(PaintFn fn) { return QIcon(new Engine(std::move(fn))); }

// Folder: closed (QIcon::Off) or open (QIcon::On, used for expanded rows).
// A second colour paints the lower-right diagonal half ("mixed" folder).
inline QIcon folderIcon(const QColor& base, const QColor& second = QColor()) {
    return make([base, second](QPainter& p, QIcon::Mode, QIcon::State state) {
        p.setPen(Qt::NoPen);
        const QColor back = base.darker(155);
        QPainterPath tab;
        tab.addRoundedRect(QRectF(1.2, 2.4, 6.4, 3.4), 1.0, 1.0);
        if (state != QIcon::On) {
            QPainterPath body;
            body.addRoundedRect(QRectF(1.0, 4.6, 14.0, 9.6), 1.3, 1.3);
            p.setBrush(back);
            p.drawPath(tab);
            p.setBrush(base);
            p.drawPath(body);
            if (second.isValid()) {
                p.save();
                p.setClipPath(body);
                p.setBrush(second);
                p.drawPolygon(QPolygonF({QPointF(0.5, 14.6), QPointF(15.6, 14.6),
                                         QPointF(15.6, 4.2)}));
                p.restore();
            }
        } else {
            QPainterPath backBody;
            backBody.addRoundedRect(QRectF(1.0, 3.6, 12.6, 10.2), 1.2, 1.2);
            p.setBrush(back);
            p.drawPath(tab);
            p.drawPath(backBody);
            QPainterPath flap;
            flap.moveTo(3.6, 6.8);
            flap.lineTo(15.7, 6.8);
            flap.lineTo(13.0, 14.2);
            flap.lineTo(0.8, 14.2);
            flap.closeSubpath();
            p.setBrush(base);
            p.drawPath(flap);
            if (second.isValid()) {
                p.save();
                p.setClipPath(flap);
                p.setBrush(second);
                p.drawPolygon(QPolygonF({QPointF(0.5, 14.6), QPointF(15.8, 14.6),
                                         QPointF(15.8, 6.4)}));
                p.restore();
            }
        }
    });
}

// Folder icon coloured by which kinds of differences it contains.
inline QIcon folderIconForMask(quint32 mask) {
    static QHash<quint32, QIcon> cache;
    const auto found = cache.constFind(mask);
    if (found != cache.constEnd()) {
        return *found;
    }
    std::vector<QColor> colors;
    if (mask & statusBit(RowStatus::Different)) colors.push_back(color::folderRed());
    if (mask & statusBit(RowStatus::Newer)) colors.push_back(color::folderGreen());
    if (mask & statusBit(RowStatus::Older)) colors.push_back(color::folderOrange());
    if (mask & statusBit(RowStatus::Orphan)) colors.push_back(color::folderPurple());
    QIcon icon;
    if (colors.empty()) {
        icon = folderIcon(color::folderYellow());
    } else if (colors.size() == 1) {
        icon = folderIcon(colors[0]);
    } else {
        icon = folderIcon(colors[0], colors[1]);
    }
    cache.insert(mask, icon);
    return icon;
}

// Small square shown in front of file names, coloured by status.
inline QIcon markerIcon(RowStatus status) {
    static QHash<int, QIcon> cache;
    const int key = static_cast<int>(status);
    const auto found = cache.constFind(key);
    if (found != cache.constEnd()) {
        return *found;
    }
    const QColor c = status == RowStatus::Equal || status == RowStatus::Pending
                         ? color::equalMarker()
                         : statusTextColor(status);
    QIcon icon = make([c](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(4.5, 4.5, 7.0, 7.0), 1.0, 1.0);
    });
    cache.insert(key, icon);
    return icon;
}

// Two coloured dashes (either may be invalid = empty slot). Bright when
// checked (QIcon::On), dimmed when unchecked - like the legend toggles in
// Beyond Compare's toolbar.
inline QIcon dashIcon(const QColor& left, const QColor& right) {
    return make([left, right](QPainter& p, QIcon::Mode, QIcon::State state) {
        p.setPen(Qt::NoPen);
        if (state != QIcon::On) {
            p.setOpacity(p.opacity() * 0.35);
        }
        if (left.isValid()) {
            p.setBrush(left);
            p.drawRoundedRect(QRectF(0.8, 6.2, 6.4, 3.6), 0.8, 0.8);
        }
        if (right.isValid()) {
            p.setBrush(right);
            p.drawRoundedRect(QRectF(8.8, 6.2, 6.4, 3.6), 0.8, 0.8);
        }
    });
}

enum class Glyph {
    Compare,
    Refresh,
    Swap,
    ExpandAll,
    CollapseAll,
    Contents,
    Timestamps,
    Filter,
    FilterClear,
    FolderOpen,
    FolderUp,
    Plus,
    Close,
};

inline QIcon glyph(Glyph which) {
    const QColor gold(0xf2, 0xc0, 0x4a);
    const QColor ink(0xdd, 0xe0, 0xee);
    switch (which) {
    case Glyph::Compare:
        return make([](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(color::newer(), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            QPainterPath check;
            check.moveTo(2.8, 8.6);
            check.lineTo(6.4, 12.2);
            check.lineTo(13.4, 3.8);
            p.drawPath(check);
        });
    case Glyph::Refresh:
        return make([gold](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(gold, 1.9, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(2.6, 2.6, 10.8, 10.8), 50 * 16, 270 * 16);
            p.setPen(Qt::NoPen);
            p.setBrush(gold);
            p.drawPolygon(QPolygonF({QPointF(14.6, 2.0), QPointF(14.6, 7.4), QPointF(9.4, 6.2)}));
        });
    case Glyph::Swap:
        return make([](QPainter& p, QIcon::Mode, QIcon::State) {
            const QColor c(0xf7, 0xc2, 0x3f);
            p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::FlatCap));
            p.drawLine(QPointF(3.2, 5.6), QPointF(12.0, 5.6));
            p.drawLine(QPointF(4.0, 10.4), QPointF(12.8, 10.4));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPolygon(QPolygonF({QPointF(15.4, 5.6), QPointF(10.4, 2.2), QPointF(10.4, 9.0)}));
            p.drawPolygon(QPolygonF({QPointF(0.6, 10.4), QPointF(5.6, 7.0), QPointF(5.6, 13.8)}));
        });
    case Glyph::ExpandAll:
    case Glyph::CollapseAll: {
        const bool expand = which == Glyph::ExpandAll;
        return make([expand, ink](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(QColor(0x8b, 0x90, 0xa8), 1.0));
            p.drawLine(QPointF(3.5, 3.2), QPointF(3.5, 13.0));
            p.drawLine(QPointF(3.5, 8.0), QPointF(6.0, 8.0));
            p.drawLine(QPointF(3.5, 13.0), QPointF(6.0, 13.0));
            p.setPen(Qt::NoPen);
            p.setBrush(color::folderYellow());
            p.drawRoundedRect(QRectF(1.0, 1.4, 5.0, 3.4), 0.8, 0.8);
            p.drawRoundedRect(QRectF(6.2, 6.6, 3.0, 2.8), 0.6, 0.6);
            p.drawRoundedRect(QRectF(6.2, 11.6, 3.0, 2.8), 0.6, 0.6);
            p.setBrush(ink);
            p.drawRoundedRect(QRectF(10.0, 7.2, 5.0, 1.6), 0.6, 0.6);
            p.drawRoundedRect(QRectF(10.0, 12.2, 5.0, 1.6), 0.6, 0.6);
            p.setPen(QPen(expand ? color::newer() : color::different(), 1.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(8.2, 3.1), QPointF(13.6, 3.1));
            if (expand) {
                p.drawLine(QPointF(10.9, 0.6), QPointF(10.9, 5.6));
            }
        });
    }
    case Glyph::Contents:
        return make([ink](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            for (const qreal dy : {0.0, 3.8}) {
                QPainterPath wave;
                wave.moveTo(2.2, 5.4 + dy);
                wave.cubicTo(4.0, 2.6 + dy, 6.2, 2.6 + dy, 8.0, 5.4 + dy);
                wave.cubicTo(9.8, 8.2 + dy, 12.0, 8.2 + dy, 13.8, 5.4 + dy);
                p.drawPath(wave);
            }
        });
    case Glyph::Timestamps:
        return make([ink](QPainter& p, QIcon::Mode, QIcon::State state) {
            p.setPen(QPen(ink, 1.4));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(2.2, 2.2, 11.6, 11.6));
            p.drawLine(QPointF(8.0, 4.6), QPointF(8.0, 8.2));
            p.drawLine(QPointF(8.0, 8.2), QPointF(10.6, 9.8));
            if (state == QIcon::On) {  // checked = "ignore timestamps"
                p.setPen(QPen(color::different(), 1.9, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(2.0, 14.0), QPointF(14.0, 2.0));
            }
        });
    case Glyph::Filter:
    case Glyph::FilterClear: {
        const bool clear = which == Glyph::FilterClear;
        return make([clear](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x62, 0xd6, 0xe8));
            QPainterPath funnel;
            funnel.moveTo(1.6, 2.6);
            funnel.lineTo(14.4, 2.6);
            funnel.lineTo(9.6, 8.4);
            funnel.lineTo(9.6, 13.6);
            funnel.lineTo(6.4, 12.0);
            funnel.lineTo(6.4, 8.4);
            funnel.closeSubpath();
            p.drawPath(funnel);
            if (clear) {
                p.setPen(QPen(color::different(), 1.8, Qt::SolidLine, Qt::RoundCap));
                p.drawLine(QPointF(9.6, 9.6), QPointF(14.6, 14.6));
                p.drawLine(QPointF(14.6, 9.6), QPointF(9.6, 14.6));
            }
        });
    }
    case Glyph::FolderOpen: {
        const QIcon folder = folderIcon(color::folderYellow());
        return make([folder](QPainter& p, QIcon::Mode, QIcon::State) {
            folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal, QIcon::On);
        });
    }
    case Glyph::FolderUp: {
        const QIcon folder = folderIcon(color::folderYellow());
        return make([folder](QPainter& p, QIcon::Mode, QIcon::State) {
            folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x3b, 0x8e, 0xea));
            p.drawPolygon(QPolygonF({QPointF(8.0, 5.6), QPointF(12.4, 10.6), QPointF(9.6, 10.6),
                                     QPointF(9.6, 14.2), QPointF(6.4, 14.2), QPointF(6.4, 10.6),
                                     QPointF(3.6, 10.6)}));
        });
    }
    case Glyph::Plus:
        return make([ink](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(8.0, 3.2), QPointF(8.0, 12.8));
            p.drawLine(QPointF(3.2, 8.0), QPointF(12.8, 8.0));
        });
    case Glyph::Close:
        return make([](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(QColor(0xa8, 0xac, 0xbe), 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(4.4, 4.4), QPointF(11.6, 11.6));
            p.drawLine(QPointF(11.6, 4.4), QPointF(4.4, 11.6));
        });
    }
    return QIcon();
}


// ---------------------------------------------------------------------------
// Context-menu icons. Same 16x16 design space as the toolbar glyphs, drawn in
// code so the menus need no image resources.
// ---------------------------------------------------------------------------
namespace menuicons {

enum class SyncDir { Right, Left, Both };

// Solid block arrow; `right` = false mirrors it.
inline void paintArrow(QPainter& p, const QColor& fill, bool right) {
    p.save();
    if (!right) {
        p.translate(16.0, 0.0);
        p.scale(-1.0, 1.0);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPolygon(QPolygonF({QPointF(1.5, 5.8), QPointF(8.2, 5.8), QPointF(8.2, 2.8),
                             QPointF(14.5, 8.0), QPointF(8.2, 13.2), QPointF(8.2, 10.2),
                             QPointF(1.5, 10.2)}));
    p.restore();
}

// "Copy to ..." (yellow) and "Move to ..." (red) share the arrow shape.
inline QIcon arrow(const QColor& fill, bool right) {
    return make([fill, right](QPainter& p, QIcon::Mode, QIcon::State) { paintArrow(p, fill, right); });
}
inline QIcon copyArrow(bool right) { return arrow(QColor(0xf5, 0xc2, 0x3d), right); }
inline QIcon moveArrow(bool right) { return arrow(QColor(0xe8, 0x4a, 0x4a), right); }

// Folder with a small up-arrow badge (Copy to Folder / Move to Folder).
inline QIcon folderWithArrow(const QColor& badge) {
    const QIcon folder = folderIcon(color::folderYellow());
    return make([folder, badge](QPainter& p, QIcon::Mode, QIcon::State) {
        folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        p.setPen(QPen(QColor(0x1e, 0x1e, 0x1e), 0.7));
        p.setBrush(badge);
        p.drawPolygon(QPolygonF({QPointF(4.5, 6.0), QPointF(8.2, 10.0), QPointF(5.9, 10.0),
                                 QPointF(5.9, 14.6), QPointF(3.1, 14.6), QPointF(3.1, 10.0),
                                 QPointF(0.8, 10.0)}));
    });
}
inline QIcon copyToFolder() { return folderWithArrow(QColor(0xf5, 0xc2, 0x3d)); }
inline QIcon moveToFolder() { return folderWithArrow(QColor(0xe8, 0x4a, 0x4a)); }

// Folder with a green "+" (New Folder).
inline QIcon newFolder() {
    const QIcon folder = folderIcon(color::folderYellow());
    return make([folder](QPainter& p, QIcon::Mode, QIcon::State) {
        folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
        p.setPen(QPen(QColor(0x2f, 0xc4, 0x4f), 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(11.5, 8.5), QPointF(11.5, 14.5));
        p.drawLine(QPointF(8.5, 11.5), QPointF(14.5, 11.5));
    });
}

// "=?" (Compare Contents)
inline QIcon compareContents() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(QColor(0xdd, 0xe0, 0xee), 1.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(1.5, 6.0), QPointF(7.0, 6.0));
        p.drawLine(QPointF(1.5, 10.0), QPointF(7.0, 10.0));
        QFont font;
        font.setPixelSize(13);
        font.setBold(true);
        p.setFont(font);
        p.setPen(QColor(0xf2, 0xc0, 0x4a));
        p.drawText(QRectF(7.5, 0.5, 8.0, 15.0), Qt::AlignCenter, QStringLiteral("?"));
    });
}

// Red cross (Delete)
inline QIcon remove() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(QColor(0xe8, 0x3f, 0x3f), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.5, 3.5), QPointF(12.5, 12.5));
        p.drawLine(QPointF(12.5, 3.5), QPointF(3.5, 12.5));
    });
}

// Text box with a caret (Rename)
inline QIcon rename() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(QColor(0xdd, 0xe0, 0xee), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.5, 4.5, 13.0, 7.0), 1.0, 1.0);
        p.setPen(QPen(QColor(0x5a, 0xa9, 0xff), 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8.0, 2.5), QPointF(8.0, 13.5));
    });
}

// Calendar + clock (Touch = change timestamp)
inline QIcon touch() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        const QColor outline(0x1e, 0x1e, 0x1e);
        p.setPen(QPen(outline, 0.7));
        p.setBrush(QColor(0xe6, 0xe8, 0xf2));
        p.drawRoundedRect(QRectF(1.5, 2.5, 10.0, 11.0), 1.0, 1.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3b, 0x8e, 0xea));
        p.drawRect(QRectF(2.0, 3.0, 9.0, 2.6));
        p.setPen(QPen(outline, 0.8));
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(11.5, 11.5), 3.6, 3.6);
        p.drawLine(QPointF(11.5, 11.5), QPointF(11.5, 9.3));
        p.drawLine(QPointF(11.5, 11.5), QPointF(13.0, 12.3));
    });
}

// Green tick (Ignored, when active)
inline QIcon check() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(QPen(QColor(0x2f, 0xc4, 0x4f), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.0, 8.6), QPointF(6.6, 12.2));
        p.drawLine(QPointF(6.6, 12.2), QPointF(13.2, 4.6));
    });
}

// Printer (File Compare Report)
inline QIcon report() {
    return make([](QPainter& p, QIcon::Mode, QIcon::State) {
        const QColor outline(0x1e, 0x1e, 0x1e);
        p.setPen(QPen(outline, 0.7));
        p.setBrush(Qt::white);
        p.drawRect(QRectF(4.0, 1.8, 8.0, 5.0));    // paper in
        p.setBrush(QColor(0x9a, 0xa0, 0xb8));
        p.drawRoundedRect(QRectF(1.5, 6.0, 13.0, 6.0), 1.0, 1.0);  // body
        p.setBrush(Qt::white);
        p.drawRect(QRectF(4.0, 10.0, 8.0, 4.2));   // paper out
        p.setPen(QPen(QColor(0x80, 0x84, 0x98), 0.7));
        p.drawLine(QPointF(5.2, 11.8), QPointF(10.8, 11.8));
        p.drawLine(QPointF(5.2, 13.2), QPointF(9.0, 13.2));
    });
}

// Double arrowheads (Synchronize submenu). Green = update, red = mirror.
inline QIcon chevrons(const QColor& fill, SyncDir dir) {
    return make([fill, dir](QPainter& p, QIcon::Mode, QIcon::State) {
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        auto head = [&p](double x, bool right) {  // 6 wide, 9 tall
            const double tip = right ? x + 6.0 : x;
            const double base = right ? x : x + 6.0;
            p.drawPolygon(QPolygonF({QPointF(base, 3.5), QPointF(tip, 8.0), QPointF(base, 12.5)}));
        };
        switch (dir) {
        case SyncDir::Right: head(1.5, true); head(8.0, true); break;
        case SyncDir::Left: head(1.5, false); head(8.0, false); break;
        case SyncDir::Both: head(1.5, false); head(8.5, true); break;
        }
    });
}
inline QIcon update(SyncDir dir) { return chevrons(QColor(0x3f, 0xe0, 0x4a), dir); }
inline QIcon mirror(SyncDir dir) { return chevrons(QColor(0xf0, 0x50, 0x50), dir); }

}  // namespace menuicons

}  // namespace icons

// ---------------------------------------------------------------------------
// Tree view: banded rows, status-coloured text, dotted connector lines
// ---------------------------------------------------------------------------

inline int treeRowHeight(const QFontMetrics& fm) {
    const int h = qMax(22, fm.height() + 6);
    return h + (h & 1);  // keep even so the dotted lines stay in phase
}

// Row banding is derived from the row's position so the delegate (cells) and
// CompareTree::drawBranches (the indent area) always agree, and so both trees
// of a comparison band identically.
inline bool oddBand(const QAbstractItemView* view, int viewportTop, int rowHeight) {
    const int scrolled = view && view->verticalScrollBar() ? view->verticalScrollBar()->value() : 0;
    return rowHeight > 0 && (((viewportTop + scrolled * rowHeight) / rowHeight) & 1);
}

inline void paintRowBackground(QPainter* painter, const QRect& r, bool odd, bool selected) {
    painter->fillRect(r, selected ? color::selection() : (odd ? color::rowB() : color::rowA()));
    if (selected) {
        painter->setPen(color::selectionBorder());
        painter->drawLine(r.left(), r.top(), r.right(), r.top());
        painter->drawLine(r.left(), r.bottom(), r.right(), r.bottom());
    }
}

class CompareDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(treeRowHeight(option.fontMetrics));
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QRect r = opt.rect;
        const bool selected = opt.state & QStyle::State_Selected;
        const auto* view = qobject_cast<const QAbstractItemView*>(opt.widget);

        painter->save();
        paintRowBackground(painter, r, oddBand(view, r.top(), r.height()), selected);

        QRect content = r.adjusted(6, 0, -6, 0);
        if (index.column() == 0 && !opt.icon.isNull()) {
            constexpr int iconSize = 16;
            const QRect iconRect(content.left(), r.top() + (r.height() - iconSize) / 2, iconSize,
                                 iconSize);
            const bool open = opt.state & QStyle::State_Open;
            opt.icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal,
                           open ? QIcon::On : QIcon::Off);
            content.setLeft(iconRect.right() + 6);
        }

        const QBrush fg = index.data(Qt::ForegroundRole).value<QBrush>();
        painter->setPen(fg.style() == Qt::NoBrush ? color::text() : fg.color());
        painter->setFont(opt.font);
        const Qt::Alignment align = opt.displayAlignment & Qt::AlignHorizontal_Mask
                                        ? opt.displayAlignment
                                        : Qt::AlignLeft | Qt::AlignVCenter;
        painter->drawText(content, static_cast<int>(align | Qt::AlignVCenter),
                          opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, content.width()));
        painter->restore();
    }
};

class CompareTree : public QTreeWidget {
public:
    explicit CompareTree(QWidget* parent = nullptr) : QTreeWidget(parent) {
        setColumnCount(5);
        setHeaderLabels({"Name", "Ext", "Size", "Modified", "Attributes"});
        setItemDelegate(new CompareDelegate(this));
        setRootIsDecorated(true);
        setIndentation(20);
        setUniformRowHeights(true);
        setAlternatingRowColors(false);  // banding is painted by the delegate
        setAllColumnsShowFocus(true);
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
        setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setFrameShape(QFrame::NoFrame);
        setTextElideMode(Qt::ElideRight);

        QHeaderView* h = header();
        h->setStretchLastSection(false);
        h->setSectionsClickable(false);
        h->setHighlightSections(false);
        h->setMinimumSectionSize(36);
        h->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 5; ++column) {
            h->setSectionResizeMode(column, QHeaderView::Interactive);
        }
        h->resizeSection(1, 64);
        h->resizeSection(2, 110);
        h->resizeSection(3, 150);
        h->resizeSection(4, 78);
        headerItem()->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }

protected:
    // Classic tree look: dotted connectors between siblings, small [+]/[-]
    // boxes for expandable folders. Top-level rows get no connectors.
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override {
        if (!index.isValid()) {
            return;
        }
        const int rowH = rect.height();
        const bool selected = selectionModel() && selectionModel()->isSelected(index);
        const bool odd = oddBand(this, rect.top(), rowH);
        const QColor bg = selected ? color::selection() : (odd ? color::rowB() : color::rowA());

        painter->save();
        paintRowBackground(painter, rect, odd, selected);
        painter->setRenderHint(QPainter::Antialiasing, false);

        std::vector<QModelIndex> chain;  // root ancestor ... index
        for (QModelIndex i = index; i.isValid(); i = i.parent()) {
            chain.push_back(i);
        }
        std::reverse(chain.begin(), chain.end());
        const int depth = static_cast<int>(chain.size()) - 1;
        const int ind = indentation();
        const int top = rect.top();
        const int bottom = top + rowH - 1;
        const int mid = top + rowH / 2;
        auto cx = [&](int level) { return rect.left() + level * ind + ind / 2; };

        QPen dotted(color::treeLine());
        dotted.setStyle(Qt::CustomDashLine);
        dotted.setDashPattern({1.0, 1.0});
        painter->setPen(dotted);

        // Vertical trunks of every ancestor level that still continues.
        for (int level = 0; level < depth; ++level) {
            const bool next = hasNextVisibleSibling(chain[level + 1]);
            const int x = cx(level);
            if (level < depth - 1) {
                if (next) {
                    painter->drawLine(x, top, x, bottom);
                }
            } else {
                painter->drawLine(x, top, x, next ? bottom : mid);
            }
        }

        const bool expandable = model()->hasChildren(index);
        if (depth >= 1) {
            const int x0 = cx(depth - 1);
            const int x1 = expandable ? cx(depth) - 5 : cx(depth) + 3;
            if (x1 > x0) {
                painter->drawLine(x0, mid, x1, mid);
            }
        }

        if (expandable) {
            const bool open = isExpanded(index);
            const int x = cx(depth);
            painter->setBrush(bg);
            painter->setPen(QPen(color::treeLine(), 1));
            painter->drawRect(QRect(x - 4, mid - 4, 8, 8));
            painter->setPen(QPen(QColor(0xd0, 0xd3, 0xe4), 1));
            painter->drawLine(x - 2, mid, x + 2, mid);
            if (!open) {
                painter->drawLine(x, mid - 2, x, mid + 2);
            } else {
                painter->setPen(dotted);
                painter->drawLine(x, mid + 5, x, bottom);
            }
        }
        painter->restore();
    }

private:
    bool hasNextVisibleSibling(const QModelIndex& idx) const {
        const QModelIndex parent = idx.parent();
        const int count = model()->rowCount(parent);
        for (int row = idx.row() + 1; row < count; ++row) {
            if (!isRowHidden(row, parent)) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace openbc::ui
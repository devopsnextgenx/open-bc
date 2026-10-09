// diff_grid_common.h
// ---------------------------------------------------------------------------
// Presentation helpers shared by the table / registry / hex compare viewers:
// row colours, All/Diffs/Context filtering of already-computed rows, scroll
// syncing, and CopyGutter - the strip between two grids that draws one pair of
// copy arrows per difference group, like the arrows in the text compare view.
// Nothing here computes a difference; kinds and groups come from the backend.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractScrollArea>
#include <QBrush>
#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QTableView>
#include <QToolTip>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <functional>

namespace openbc::app::diffgrid {

enum RowKind { Equal = 0, Changed = 1, LeftOnly = 2, RightOnly = 3 };

// Same palette as the text compare preview strip (diff_line_preview.h).
inline QColor changedRowColor() { return QColor(0x7a, 0x2d, 0x2d, 90); }
inline QColor changedCellColor() { return QColor(0xb0, 0x3a, 0x3a, 170); }
inline QColor onlyRowColor() { return QColor(0x8a, 0x7a, 0x2d, 90); }

// Background for a cell. `present` is false where this side has no row/value: those are
// hatched, echoing the editors' "missing line" pattern.
inline QBrush cellBrush(int kind, bool present, bool cellChanged) {
    if (kind == Equal) return QBrush(Qt::NoBrush);
    if (!present) return QBrush(onlyRowColor().darker(130), Qt::BDiagPattern);
    if (kind == Changed) return QBrush(cellChanged ? changedCellColor() : changedRowColor());
    return QBrush(onlyRowColor());
}

// A, B, ..., Z, AA, AB ... (spreadsheet column names)
inline QString columnLetters(int column) {
    QString out;
    for (int n = column + 1; n > 0; n = (n - 1) / 26) out.prepend(QChar('A' + (n - 1) % 26));
    return out;
}

// Inverse of columnLetters / 1-based numbers; -1 when `token` is neither.
inline int columnFromToken(const QString& token) {
    const QString t = token.trimmed().toUpper();
    if (t.isEmpty()) return -1;
    bool numeric = false;
    const int n = t.toInt(&numeric);
    if (numeric) return n >= 1 ? n - 1 : -1;
    int value = 0;
    for (const QChar c : t) {
        if (c < 'A' || c > 'Z') return -1;
        value = value * 26 + (c.unicode() - 'A' + 1);
    }
    return value - 1;
}

enum class RowFilter { All, DiffsOnly, Context };

// Indexes (into the backend's row list) that stay visible under `filter`.
inline QVector<int> visibleRows(int rowCount, const std::function<bool(int)>& isDifferent,
                                RowFilter filter, int context = 2) {
    QVector<int> rows;
    rows.reserve(rowCount);
    if (filter == RowFilter::All) {
        for (int i = 0; i < rowCount; ++i) rows.push_back(i);
        return rows;
    }
    int keepUntil = -1;
    QVector<char> keep(rowCount, 0);
    for (int i = 0; i < rowCount; ++i) {
        if (!isDifferent(i)) continue;
        const int first = filter == RowFilter::Context ? std::max(0, i - context) : i;
        const int last = filter == RowFilter::Context ? std::min(rowCount - 1, i + context) : i;
        for (int j = std::max(first, keepUntil + 1); j <= last; ++j) keep[j] = 1;
        keep[i] = 1;
        keepUntil = std::max(keepUntil, last);
    }
    for (int i = 0; i < rowCount; ++i)
        if (keep[i]) rows.push_back(i);
    return rows;
}

// Index in `visible` (ascending) of the first row >= `row`, or -1.
inline int firstVisibleAtOrAfter(const QVector<int>& visible, int row) {
    const auto it = std::lower_bound(visible.begin(), visible.end(), row);
    return it == visible.end() ? -1 : static_cast<int>(it - visible.begin());
}

// Two-way scrollbar sync (re-entrancy guarded).
inline void syncScrollBars(QScrollBar* a, QScrollBar* b) {
    auto* guard = new bool(false);
    auto link = [guard](QScrollBar* from, QScrollBar* to) {
        QObject::connect(from, &QScrollBar::valueChanged, to, [guard, to](int v) {
            if (*guard) return;
            *guard = true;
            to->setValue(v);
            *guard = false;
        });
    };
    link(a, b);
    link(b, a);
    QObject::connect(a, &QObject::destroyed, [guard]() { delete guard; });
}

struct GutterGroup {
    int firstVisibleRow = 0;  // row in the grid's model
    int lastVisibleRow = 0;
    int group = 0;            // backend group index
};

// Strip of copy arrows aligned with `grid`'s rows. Left half copies right -> left,
// right half copies left -> right (each arrow points where the data goes).
class CopyGutter : public QWidget {
public:
    explicit CopyGutter(QTableView* grid, QWidget* parent = nullptr) : QWidget(parent), grid_(grid) {
        setFixedWidth(44);
        setMouseTracking(true);
        connect_();
    }

    void setGroups(const QVector<GutterGroup>& groups, int current = -1) {
        groups_ = groups;
        current_ = current;
        update();
    }
    void setCurrentGroup(int group) {
        current_ = group;
        update();
    }
    void setEnabledSides(bool toLeft, bool toRight) {
        toLeft_ = toLeft;
        toRight_ = toRight;
        update();
    }

    std::function<void(int group, bool toRight)> onCopy;
    std::function<void(int group)> onGroupClicked;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().window());
        for (const GutterGroup& g : groups_) {
            const QRect box = boxFor(g);
            if (box.isNull()) continue;
            const QRect leftRect(box.left(), box.top(), box.width() / 2, box.height());
            const QRect rightRect(leftRect.right() + 1, box.top(), box.width() - leftRect.width(), box.height());
            const bool current = g.group == current_;
            p.setPen(Qt::NoPen);
            p.setBrush(current ? QColor(0x3a, 0x5a, 0x8a) : QColor(0x33, 0x33, 0x3d));
            p.drawRoundedRect(box, 3, 3);
            drawArrow(p, leftRect, /*pointsRight=*/false, toLeft_);
            drawArrow(p, rightRect, /*pointsRight=*/true, toRight_);
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        const QPoint pos = event->position().toPoint();
        for (const GutterGroup& g : groups_) {
            const QRect box = boxFor(g);
            if (box.isNull() || !box.contains(pos)) continue;
            const bool toRight = pos.x() >= box.center().x();
            if (onGroupClicked) onGroupClicked(g.group);
            if (onCopy && (toRight ? toRight_ : toLeft_)) onCopy(g.group, toRight);
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QPoint pos = event->position().toPoint();
        for (const GutterGroup& g : groups_) {
            const QRect box = boxFor(g);
            if (!box.isNull() && box.contains(pos)) {
                setCursor(Qt::PointingHandCursor);
                QToolTip::showText(event->globalPosition().toPoint(),
                                   pos.x() >= box.center().x() ? "Copy this block left -> right"
                                                               : "Copy this block right -> left",
                                   this);
                return;
            }
        }
        unsetCursor();
    }

private:
    void connect_() {
        if (!grid_) return;
        QObject::connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { update(); });
        QObject::connect(grid_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this]() { update(); });
    }

    // Box of the first row of the group, in gutter coordinates; null when scrolled out.
    QRect boxFor(const GutterGroup& g) const {
        if (!grid_ || !grid_->model()) return {};
        const int y = grid_->rowViewportPosition(g.firstVisibleRow);
        if (y < 0) return {};
        const int h = std::max(14, std::min(grid_->rowHeight(g.firstVisibleRow), 22));
        const int originY = grid_->viewport()->mapTo(window(), QPoint(0, 0)).y() - mapTo(window(), QPoint(0, 0)).y();
        const QRect box(4, originY + y + 1, width() - 8, h);
        return box.bottom() < 0 || box.top() > height() ? QRect() : box;
    }

    static void drawArrow(QPainter& p, const QRect& r, bool pointsRight, bool enabled) {
        const QPointF c = r.center();
        const qreal s = 4.0;
        QPolygonF tri;
        if (pointsRight) tri << QPointF(c.x() - s, c.y() - s) << QPointF(c.x() + s, c.y()) << QPointF(c.x() - s, c.y() + s);
        else tri << QPointF(c.x() + s, c.y() - s) << QPointF(c.x() - s, c.y()) << QPointF(c.x() + s, c.y() + s);
        p.setBrush(enabled ? QColor(0xe8, 0xea, 0xf4) : QColor(0x70, 0x70, 0x78));
        p.drawPolygon(tri);
    }

    QPointer<QTableView> grid_;
    QVector<GutterGroup> groups_;
    int current_ = -1;
    bool toLeft_ = true;
    bool toRight_ = true;
};

}  // namespace openbc::app::diffgrid
// minimap.h
// ---------------------------------------------------------------------------
// DifferenceOverview: the vertical strip next to the left editor that gives
// an at-a-glance map of every difference in the file (like an IDE minimap or
// Beyond Compare's overview bar).
//
// Two rendering modes, toggled with setPixelLineMode():
//   - Block mode (default): one bar per contiguous diff group, thickness
//     proportional to how many lines the group spans.
//   - Pixel-line mode: every differing row gets its own single-pixel-thick
//     line at its proportional position, regardless of how large the group
//     around it is - so a 40-line change and a 2-line change both show as
//     a stack of individual hairlines rather than one block dominating the
//     map.
// In both modes, a group where one whole side has no corresponding line at
// all (a pure insert/delete, not a same-line edit) is drawn in a third,
// muted colour with a diagonal hash texture in block mode, echoing the
// hashed "missing lines" pattern the editors already show for those rows -
// so the minimap reads the same way the editors do.
// ---------------------------------------------------------------------------
#pragma once

#include <QBrush>
#include <QColor>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <functional>

#include "diff_algorithms.h"

namespace openbc::app {

// Vertical mini-map: a thin strip that represents the WHOLE file, top to
// bottom, the way an IDE minimap or Beyond Compare's overview bar does. Each
// contiguous diff block is drawn as one red (or blue, for whitespace-only)
// line whose thickness is proportional to how many lines that block spans
// relative to the whole file, so a big change reads as a thick bar and a
// one-line tweak reads as a hairline. A black-bordered rectangle shows which
// slice of the file the editor's viewport currently covers, and drags/clicks
// on the strip jump the editors to that position - exactly like clicking in
// an IDE minimap.
class DifferenceOverview : public QFrame {
public:
    explicit DifferenceOverview(QWidget* parent = nullptr, bool vertical = false)
        : QFrame(parent), vertical_(vertical) {
        if (vertical_) {
            setMinimumWidth(22);
            setMaximumWidth(22);
        } else {
            setMinimumHeight(18);
            setMaximumHeight(18);
        }
        setCursor(Qt::PointingHandCursor);
    }

    void setRows(const QVector<TextDiffLine>& rows) {
        rows_ = rows;
        blocks_ = groupDiffRows(rows_);
        update();
    }

    // Pixel-line mode: every differing row draws as its own 1px hairline at
    // its proportional position, instead of one solid bar per group whose
    // thickness scales with the group's line count. Off by default (the
    // classic block-bar minimap); toggled from TextCompareView's toolbar.
    void setPixelLineMode(bool enabled) {
        if (pixelLineMode_ == enabled) return;
        pixelLineMode_ = enabled;
        update();
    }
    bool pixelLineMode() const { return pixelLineMode_; }

    void setCurrentRow(int row) {
        currentRow_ = row;
        update();
    }

    // `firstFraction`/`spanFraction` are both in [0, 1] and describe, as a
    // fraction of the whole file's height, where the editor's viewport
    // currently starts and how tall it is. Called whenever the editor
    // scrolls or resizes so the black rectangle always matches what is on
    // screen.
    void setViewport(qreal firstFraction, qreal spanFraction) {
        viewportStart_ = qBound<qreal>(0.0, firstFraction, 1.0);
        viewportSpan_ = qBound<qreal>(0.0, spanFraction, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect(), QColor("#232323"));
        if (rows_.isEmpty()) return;
        const qreal extent = vertical_ ? height() : width();
        const qreal rowSize = extent / rows_.size();

        if (pixelLineMode_) {
            paintPixelLines(painter, rowSize);
        } else {
            paintBlocks(painter, rowSize);
        }

        if (currentRow_ >= 0 && currentRow_ < rows_.size()) {
            const qreal at = currentRow_ * rowSize;
            painter.fillRect(vertical_ ? QRectF(0, at, width(), qMax<qreal>(1, rowSize))
                                       : QRectF(at, 0, qMax<qreal>(1, rowSize), height()),
                            QColor("#f8f8f2"));
        }

        // The viewport rectangle: a hollow black-bordered box, sized and
        // positioned from the fractions the editor last reported, showing
        // exactly which portion of the full file height is on screen.
        if (vertical_ && viewportSpan_ > 0.0) {
            const int top = qRound(viewportStart_ * height());
            const int h = qMax(3, qRound(viewportSpan_ * height()));
            QPen pen(QColor(0, 0, 0));
            pen.setWidth(1);
            painter.setPen(pen);
            painter.setBrush(QColor(255, 255, 255, 28));
            painter.drawRect(QRect(0, top, width() - 1, h - 1));
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        jumpFromPosition(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            jumpFromPosition(event);
        }
    }

public:
    std::function<void(int)> onRowClicked;

private:
    void jumpFromPosition(QMouseEvent* event) {
        if (rows_.isEmpty() || !onRowClicked) return;
        const qreal position = vertical_ ? event->position().y() : event->position().x();
        const qreal extent = vertical_ ? height() : width();
        onRowClicked(qBound(0, static_cast<int>(position * rows_.size() / extent), rows_.size() - 1));
    }

    // A group is "missing" when every row in it lacks a counterpart on one
    // whole side (leftNumber == 0 for all of them, or rightNumber == 0 for
    // all of them) - a pure insert/delete block, as opposed to a group where
    // the same line exists on both sides but its text differs.
    bool groupIsMissing(const DiffGroup& group) const {
        bool allNoLeft = true;
        bool allNoRight = true;
        for (int row = group.start; row <= group.end && row < rows_.size(); ++row) {
            if (rows_[row].leftNumber != 0) allNoLeft = false;
            if (rows_[row].rightNumber != 0) allNoRight = false;
        }
        return allNoLeft || allNoRight;
    }

    QColor colorFor(bool whitespaceOnly, bool missing) const {
        if (missing) return QColor("#9aa0b8");     // muted grey-violet, matches equalMarker()
        if (whitespaceOnly) return QColor("#4ea1ff");
        return QColor("#ff5c5c");
    }

    // Block mode: one bar per contiguous diff group, thickness proportional
    // to the group's line count. Missing-content groups additionally get a
    // diagonal hash texture painted over their base colour, echoing the
    // hashed background the editors use for the same rows.
    void paintBlocks(QPainter& painter, qreal rowSize) {
        for (const DiffGroup& block : blocks_) {
            const qreal start = block.start * rowSize;
            const qreal thickness = qMax<qreal>(2.0, (block.end - block.start + 1) * rowSize);
            const bool missing = groupIsMissing(block);
            const QColor color = colorFor(block.whitespaceOnly, missing);
            const QRectF bar = vertical_ ? QRectF(3, start, width() - 6, thickness)
                                         : QRectF(start, 2, thickness, height() - 4);
            painter.fillRect(bar, color);
            if (missing && thickness > 3.0) {
                painter.fillRect(bar, QBrush(QColor(0, 0, 0, 90), Qt::BDiagPattern));
            }
        }
    }

    // Pixel-line mode: every differing row gets its own 1px-thick hairline,
    // so a huge change and a one-line tweak both draw as a stack of equally
    // thin lines instead of one dominating the map - useful once files get
    // long enough that block-mode's proportional bars all blur together.
    void paintPixelLines(QPainter& painter, qreal rowSize) {
        for (int row = 0; row < rows_.size(); ++row) {
            const TextDiffLine& line = rows_[row];
            if (!line.changed) continue;
            const bool missing = line.leftNumber == 0 || line.rightNumber == 0;
            const QColor color = colorFor(line.whitespaceOnly, missing);
            const qreal at = row * rowSize;
            if (vertical_) {
                painter.fillRect(QRectF(3, at, width() - 6, 1.0), color);
            } else {
                painter.fillRect(QRectF(at, 2, 1.0, height() - 4), color);
            }
        }
    }

    QVector<TextDiffLine> rows_;
    QVector<DiffGroup> blocks_;
    bool vertical_ = false;
    bool pixelLineMode_ = false;
    int currentRow_ = -1;
    qreal viewportStart_ = 0.0;
    qreal viewportSpan_ = 0.0;
};

}  // namespace openbc::app

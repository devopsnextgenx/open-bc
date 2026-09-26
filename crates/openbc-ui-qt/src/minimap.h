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
//
// Content scale vs. viewport:
//   The strip itself always fills the available editor height (no scrollbar
//   on the minimap). Diff markers and the sliding viewport box are painted
//   inside a "content region" whose height scales with file length:
//     - Short files (< kFullScaleLines): content region is only a fraction
//       of the strip (proportional to rows / kFullScaleLines), pinned at the
//       top. The viewport box is then the *exact* scrollbar fraction of that
//       content region - so when the whole short file is on screen the box
//       fills the content region (~1/10 of the strip for a 40-line file),
//       and it stays in sync while scrolling.
//     - Long files (>= kFullScaleLines): content region = full strip height;
//       viewport shrinks naturally as the document grows.
// ---------------------------------------------------------------------------
#pragma once

#include <QBrush>
#include <QColor>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSet>
#include <functional>

#include "diff_algorithms.h"

namespace openbc::app {

// Vertical mini-map: a thin strip that represents the WHOLE file, top to
// bottom, the way an IDE minimap or Beyond Compare's overview bar does. Each
// contiguous diff block is drawn as one red (or blue, for whitespace-only)
// line whose thickness is proportional to how many lines that block spans
// relative to the whole file, so a big change reads as a thick bar and a
// one-line tweak reads as a hairline. A light-bordered rectangle shows which
// slice of the file the editor's viewport currently covers. On short files
// the content (markers + viewport) is compressed into a top portion of the
// strip so the viewport stays proportional and in sync; on long files the
// full strip height is used. Drags/clicks jump the editors to that position.
class DifferenceOverview : public QFrame {
public:
    explicit DifferenceOverview(QWidget* parent = nullptr, bool vertical = false)
        : QFrame(parent), vertical_(vertical) {
        if (vertical_) {
            setMinimumWidth(28);
            setMaximumWidth(28);
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

    // Rows (indices into the currently-displayed rows_) that have an
    // in-memory edit not yet written to disk on either side. Drawn as a
    // dedicated bright-green tick in its own thin column next to the diff
    // bars, independent of block thickness, so a single dirty line inside a
    // large change block - or a dirty line inside a tiny one - is equally
    // visible instead of being lost proportionally to the block's size.
    void setDirtyRows(const QSet<int>& rows) {
        if (dirtyRows_ == rows) return;
        dirtyRows_ = rows;
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
    // currently starts and how tall it is. The painted viewport box is
    // always the exact same fraction of the content region (which itself
    // may be shorter than the strip on short files), so it stays in sync
    // with the editor while scrolling.
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

        // Content region: fraction of the strip used to map the document.
        // Short files compress into the top portion; long files use full height.
        const qreal stripExtent = vertical_ ? height() : width();
        const qreal contentExtent = contentExtentFor(stripExtent);
        const qreal rowSize = contentExtent / rows_.size();

        if (pixelLineMode_) {
            paintPixelLines(painter, rowSize, contentExtent);
        } else {
            paintBlocks(painter, rowSize, contentExtent);
        }

        if (!dirtyRows_.isEmpty()) {
            paintDirtyTicks(painter, rowSize, contentExtent);
        }

        if (currentRow_ >= 0 && currentRow_ < rows_.size()) {
            const qreal at = currentRow_ * rowSize;
            painter.fillRect(vertical_ ? QRectF(0, at, width(), qMax<qreal>(1, rowSize))
                                       : QRectF(at, 0, qMax<qreal>(1, rowSize), height()),
                            QColor(248, 248, 242, 60));
        }

        // Viewport rectangle: exact scrollbar fraction of the content region.
        // When the whole short file is visible, the box fills the (compressed)
        // content region. When scrolling a long file, the box shrinks and
        // slides within the full strip - always in sync with the editor.
        if (vertical_ && viewportSpan_ > 0.0) {
            const qreal span = qBound<qreal>(0.0, viewportSpan_, 1.0);
            const qreal start = qBound<qreal>(0.0, viewportStart_, 1.0 - span);
            const int top = qRound(start * contentExtent);
            const int h = qMax(4, qRound(span * contentExtent));
            QPen pen(QColor(235, 235, 235));
            pen.setWidth(1);
            painter.setPen(pen);
            painter.setBrush(QColor(255, 255, 255, 40));
            painter.drawRect(QRect(0, top, width() - 1, h - 1));
        }

        // Bottom edge of the content region when it does not fill the strip
        // (short files). Makes the end of the mapped document obvious against
        // the empty lower part of the minimap.
        if (vertical_ && contentExtent + 0.5 < stripExtent) {
            const int y = qRound(contentExtent) - 1;
            painter.setPen(QPen(QColor(160, 165, 180), 1));
            painter.drawLine(0, y, width() - 1, y);
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
    // Height (or width, when horizontal) of the region that maps the document.
    // Never exceeds the strip; for short files scales down so the viewport
    // box stays a true proportion of that region and remains visually compact.
    qreal contentExtentFor(qreal stripExtent) const {
        if (rows_.isEmpty()) return stripExtent;
        if (rows_.size() >= kFullScaleLines) return stripExtent;
        // Scale content height with line count; keep a usable minimum so a
        // handful of lines still produce a visible content region + viewport.
        const qreal scaled = stripExtent * (static_cast<qreal>(rows_.size()) / kFullScaleLines);
        return qMax(stripExtent * kMinContentFraction, scaled);
    }

    void jumpFromPosition(QMouseEvent* event) {
        if (rows_.isEmpty() || !onRowClicked) return;
        const qreal position = vertical_ ? event->position().y() : event->position().x();
        const qreal stripExtent = vertical_ ? height() : width();
        const qreal contentExtent = contentExtentFor(stripExtent);
        // Clicks below the content region map to the last row.
        const qreal clamped = qBound<qreal>(0.0, position, contentExtent);
        onRowClicked(qBound(0, static_cast<int>(clamped * rows_.size() / contentExtent),
                             rows_.size() - 1));
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
    // to the group's line count within the content region.
    void paintBlocks(QPainter& painter, qreal rowSize, qreal /*contentExtent*/) {
        const qreal barWidth = vertical_ ? width() - kDirtyColumnWidth - 6 : width() - 4;
        for (const DiffGroup& block : blocks_) {
            const qreal start = block.start * rowSize;
            const qreal thickness = qMax<qreal>(3.0, (block.end - block.start + 1) * rowSize);
            const bool missing = groupIsMissing(block);
            const QColor color = colorFor(block.whitespaceOnly, missing);
            const QRectF bar = vertical_ ? QRectF(3, start, barWidth, thickness)
                                         : QRectF(start, 2, thickness, height() - 4);
            painter.fillRect(bar, color);
            if (missing && thickness > 3.0) {
                painter.fillRect(bar, QBrush(QColor(0, 0, 0, 90), Qt::BDiagPattern));
            }
        }
    }

    // Dedicated column (the strip's inner edge, next to the splitter) that
    // marks rows with an unsaved edit - a small bright tick per dirty row,
    // always the same size regardless of how thick the diff bar behind it
    // is, so one dirty line in a 40-line block is exactly as visible as one
    // dirty line on its own.
    void paintDirtyTicks(QPainter& painter, qreal rowSize, qreal /*contentExtent*/) {
        static const QColor kDirty(60, 220, 130);
        const qreal x = vertical_ ? width() - kDirtyColumnWidth + 1 : 0;
        for (int row : dirtyRows_) {
            if (row < 0 || row >= rows_.size()) continue;
            const qreal at = row * rowSize;
            const qreal thickness = qMax<qreal>(2.0, rowSize);
            if (vertical_) {
                painter.fillRect(QRectF(x, at, kDirtyColumnWidth - 2, thickness), kDirty);
            } else {
                painter.fillRect(QRectF(at, height() - 4, thickness, 3), kDirty);
            }
        }
    }

    // Pixel-line mode: every differing row gets its own 1px-thick hairline
    // inside the content region.
    void paintPixelLines(QPainter& painter, qreal rowSize, qreal /*contentExtent*/) {
        const qreal barWidth = vertical_ ? width() - kDirtyColumnWidth - 6 : width() - 4;
        for (int row = 0; row < rows_.size(); ++row) {
            const TextDiffLine& line = rows_[row];
            if (!line.changed) continue;
            const bool missing = line.leftNumber == 0 || line.rightNumber == 0;
            const QColor color = colorFor(line.whitespaceOnly, missing);
            const qreal at = row * rowSize;
            const qreal thickness = qMax<qreal>(1.5, rowSize * 0.9);
            if (vertical_) {
                painter.fillRect(QRectF(3, at, barWidth, thickness), color);
            } else {
                painter.fillRect(QRectF(at, 2, thickness, height() - 4), color);
            }
        }
    }

    static constexpr qreal kDirtyColumnWidth = 6.0;
    // Line count at which the content region fills the entire strip.
    // Below this, content height scales with rows/kFullScaleLines so the
    // viewport box stays a true proportion of that region (and stays compact
    // for short files). Above it, large files share the fixed strip height
    // and the viewport shrinks proportionally.
    static constexpr int kFullScaleLines = 400;
    // Floor for the content-region fraction of the strip (avoids a near-
    // invisible region for very tiny files).
    static constexpr qreal kMinContentFraction = 0.05;

    QVector<TextDiffLine> rows_;
    QVector<DiffGroup> blocks_;
    QSet<int> dirtyRows_;
    bool vertical_ = false;
    bool pixelLineMode_ = false;
    int currentRow_ = -1;
    qreal viewportStart_ = 0.0;
    qreal viewportSpan_ = 0.0;
};

}  // namespace openbc::app
// line_number_editor.h
// ---------------------------------------------------------------------------
// (header comment unchanged)
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPair>
#include <QPen>
#include <QPolygonF>
#include <QTextCursor>
#include <QGuiApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QRect>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextLine>
#include <QVector>
#include <QWidget>
#include <functional>

#include "diff_algorithms.h"

namespace openbc::app {

class LineNumberEditor : public QPlainTextEdit {
public:
    enum class GutterSide { None, Left, Right };

    explicit LineNumberEditor(GutterSide numberSide = GutterSide::Left,
                              GutterSide arrowSide = GutterSide::None, QWidget* parent = nullptr)
        : QPlainTextEdit(parent), numberSide_(numberSide), arrowSide_(arrowSide) {
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setTabStopDistance(fontMetrics().horizontalAdvance(' ') * 4);
        connect(this, &QPlainTextEdit::blockCountChanged, this,
                [this](int) { updateMarginWidths(); });
        connect(this, &QPlainTextEdit::updateRequest, this,
                [this](const QRect& rect, int delta) {
                    if (delta) {
                        lineNumberArea_->scroll(0, delta);
                        if (arrowArea_) arrowArea_->scroll(0, delta);
                    } else {
                        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
                        if (arrowArea_) arrowArea_->update(0, rect.y(), arrowArea_->width(), rect.height());
                    }
                    if (rect.contains(viewport()->rect())) {
                        updateMarginWidths();
                        repositionGutters();
                    }
                });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
            lineNumberArea_->update();
            if (arrowArea_) arrowArea_->update();
        });
        connect(this, &QPlainTextEdit::selectionChanged, this, [this]() {
            if (arrowArea_) arrowArea_->update();
        });
        lineNumberArea_ = new QWidget(this);
        lineNumberArea_->setObjectName("lineNumberGutter");
        lineNumberArea_->setCursor(Qt::ArrowCursor);
        lineNumberArea_->installEventFilter(this);
        if (arrowSide_ != GutterSide::None) {
            arrowArea_ = new QWidget(this);
            arrowArea_->setObjectName("diffArrowGutter");
            arrowArea_->setCursor(Qt::PointingHandCursor);
            arrowArea_->setToolTip(arrowSide_ == GutterSide::Right
                                        ? "Copy this change to the right"
                                        : "Copy this change to the left");
            arrowArea_->installEventFilter(this);
        }
        // The scrollbar can appear/disappear without the widget being
        // resized (e.g. the document got shorter); reposition the right
        // gutter so it stays clear of the scrollbar in that case too.
        if (QScrollBar* sb = verticalScrollBar()) {
            connect(sb, &QScrollBar::rangeChanged, this,
                    [this](int, int) { repositionGutters(); });
        }
        updateMarginWidths();
        repositionGutters();
    }

    void setLineNumbers(const QVector<int>& numbers) {
        lineNumbers_ = numbers;
        updateMarginWidths();
        lineNumberArea_->update();
    }

    void setDiffData(const QVector<TextDiffLine>& rows, const QVector<DiffGroup>& groups) {
        diffRows_ = rows;
        diffGroups_ = groups;
        gutterRow_ = -1;
        if (arrowArea_) arrowArea_->update();
    }

    void setDirtyRows(const QSet<int>& rows) {
        dirtyRows_ = rows;
        if (arrowArea_) arrowArea_->update();
    }

    void setInlineDiffs(const QVector<QVector<CharSegment>>& perBlockSegments) {
        inlineDiffs_ = perBlockSegments;
        viewport()->update();
    }

    std::function<void(int, int)> onArrowClicked;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == lineNumberArea_ && event->type() == QEvent::Paint) {
            paintLineNumbers(static_cast<QPaintEvent*>(event));
            return true;
        }
        if (arrowArea_ && watched == arrowArea_) {
            if (event->type() == QEvent::Paint) {
                paintArrows(static_cast<QPaintEvent*>(event));
                return true;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                handleArrowClick(static_cast<QMouseEvent*>(event));
                return true;
            }
        }
        return QPlainTextEdit::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        repositionGutters();
    }

    // The blue arrow follows focus and any user-driven cursor movement.
    void focusInEvent(QFocusEvent* event) override {
        QPlainTextEdit::focusInEvent(event);
        if (arrowArea_) arrowArea_->update();
    }
    void focusOutEvent(QFocusEvent* event) override {
        QPlainTextEdit::focusOutEvent(event);
        if (arrowArea_) arrowArea_->update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        gutterRow_ = -1;
        QPlainTextEdit::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        gutterRow_ = -1;
        QPlainTextEdit::keyPressEvent(event);
    }

    // (paintEvent unchanged)
    void paintEvent(QPaintEvent* event) override {
        QPlainTextEdit::paintEvent(event);
        if (inlineDiffs_.isEmpty()) return;
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        QFont marker = font();
        marker.setPointSizeF(qMax(6.0, font().pointSizeF() * 0.9));
        painter.setFont(marker);
        painter.setPen(QColor(0xa9, 0xd4, 0xff));
        forEachVisibleBlock(event, [&](int blockNumber, int top, int bottom) {
            if (blockNumber >= inlineDiffs_.size()) return;
            const QTextBlock block = document()->findBlockByNumber(blockNumber);
            if (!block.isValid()) return;
            const QString text = block.text();
            QTextLayout* layout = block.layout();
            if (!layout) return;
            for (const CharSegment& segment : inlineDiffs_[blockNumber]) {
                if (segment.kind != CharSegmentKind::WhitespaceMismatch) continue;
                for (int i = segment.start; i < segment.start + segment.length && i < text.size();
                     ++i) {
                    const QChar ch = text.at(i);
                    if (ch != ' ' && ch != '\t') continue;
                    const QTextLine line = layout->lineForTextPosition(i);
                    if (!line.isValid()) continue;
                    const qreal x = line.cursorToX(i) + contentOffset().x();
                    const qreal nextX = line.cursorToX(i + 1) + contentOffset().x();
                    const QRectF rect(x, top, qMax<qreal>(6.0, nextX - x), bottom - top);
                    painter.drawText(rect, Qt::AlignCenter,
                                     ch == ' ' ? QStringLiteral("\u00b7") : QStringLiteral("\u2192"));
                }
            }
        });
    }

private:
    static constexpr int kArrowWidth = 18;

    int lineNumberDigitsWidth() const {
        return fontMetrics().horizontalAdvance(QString::number(qMax(1, lineNumbers_.size()))) + 16;
    }

    void updateMarginWidths() {
        const int numbers = lineNumberDigitsWidth();
        const int arrows = arrowSide_ != GutterSide::None ? kArrowWidth : 0;
        int leftMargin = 0;
        int rightMargin = 0;
        if (numberSide_ == GutterSide::Left) leftMargin += numbers; else rightMargin += numbers;
        if (arrowSide_ == GutterSide::Left) leftMargin += arrows;
        else if (arrowSide_ == GutterSide::Right) rightMargin += arrows;
        setViewportMargins(leftMargin, 0, rightMargin, 0);
    }

    // Places the line-number and copy-arrow gutter widgets. The right-hand
    // limit excludes the vertical scrollbar (when visible) so a right-side
    // gutter - the left pane's copy-arrow strip, which also paints the
    // green "copied, needs saving" markers - is not covered by the
    // scrollbar.
    void repositionGutters() {
        const QRect cr = contentsRect();
        const int sbWidth = (verticalScrollBar() && verticalScrollBar()->isVisible())
                                ? verticalScrollBar()->width() : 0;
        const int rightLimit = cr.right() - sbWidth;  // inclusive
        int left = cr.left();
        int right = rightLimit;

        if (numberSide_ == GutterSide::Left) {
            lineNumberArea_->setGeometry(left, cr.top(), lineNumberDigitsWidth(), cr.height());
            left += lineNumberDigitsWidth();
        }
        if (arrowSide_ == GutterSide::Left) {
            arrowArea_->setGeometry(left, cr.top(), kArrowWidth, cr.height());
            left += kArrowWidth;
        }
        if (numberSide_ == GutterSide::Right) {
            const int w = lineNumberDigitsWidth();
            right -= w;
            lineNumberArea_->setGeometry(right + 1, cr.top(), w, cr.height());
        }
        if (arrowSide_ == GutterSide::Right) {
            right -= kArrowWidth;
            arrowArea_->setGeometry(right + 1, cr.top(), kArrowWidth, cr.height());
        }
    }

    void paintLineNumbers(QPaintEvent* event) {
        QPainter painter(lineNumberArea_);
        painter.fillRect(event->rect(), QGuiApplication::palette().color(QPalette::Base));
        const int textMargin = numberSide_ == GutterSide::Left ? 8 : 6;
        forEachVisibleBlock(event, [&](int blockNumber, int top, int bottom) {
            const int number = blockNumber < lineNumbers_.size() ? lineNumbers_[blockNumber] : 0;
            painter.setPen(number ? QGuiApplication::palette().color(QPalette::Text)
                                  : QGuiApplication::palette().color(QPalette::Disabled,
                                                                      QPalette::Text));
            painter.drawText(0, top, lineNumberArea_->width() - textMargin, bottom - top,
                             Qt::AlignRight, number ? QString::number(number) : QString());
        });
    }

    // Geometry of the rows currently shown, indexed from firstRow.
    struct VisibleRows {
        int firstRow = 0;
        QVector<QPair<int, int>> spans;  // (top, bottom) per consecutive row

        int rowAt(int y) const {
            for (int i = 0; i < spans.size(); ++i) {
                if (y >= spans[i].first && y < spans[i].second) return firstRow + i;
            }
            return -1;
        }

        // Vertical extent of rows [start, end] clipped to what is visible.
        // `startVisible` / `endVisible` are false when the span's first / last
        // row is scrolled out of view.
        bool extent(int start, int end, int& top, int& bottom, bool& startVisible,
                    bool& endVisible) const {
            if (spans.isEmpty()) return false;
            const int lo = qMax(start, firstRow);
            const int hi = qMin(end, firstRow + static_cast<int>(spans.size()) - 1);
            if (lo > hi) return false;
            top = spans[lo - firstRow].first;
            bottom = spans[hi - firstRow].second;
            startVisible = start >= firstRow;
            endVisible = end <= firstRow + static_cast<int>(spans.size()) - 1;
            return true;
        }
    };

    VisibleRows collectVisibleRows() {
        VisibleRows rows;
        bool first = true;
        forEachVisibleBlock(nullptr, [&](int blockNumber, int top, int bottom) {
            if (first) {
                rows.firstRow = blockNumber;
                first = false;
            }
            rows.spans.push_back({top, bottom});
        });
        return rows;
    }

    // The range the blue arrow acts on: the selected lines of the focused
    // pane, or the single line picked by clicking the blank gutter.
    bool selectedRowRange(int& start, int& end) const {
        if (!hasFocus()) return false;
        const QTextCursor cursor = textCursor();
        if (cursor.hasSelection()) {
            start = document()->findBlock(cursor.selectionStart()).blockNumber();
            const QTextBlock endBlock = document()->findBlock(cursor.selectionEnd());
            end = endBlock.blockNumber();
            // A selection that ends at column 0 doesn't include that line.
            if (end > start && cursor.selectionEnd() == endBlock.position()) --end;
            return true;
        }
        if (gutterRow_ >= 0 && cursor.blockNumber() == gutterRow_) {
            start = end = gutterRow_;
            return true;
        }
        return false;
    }

    void paintArrows(QPaintEvent* event) {
        QPainter painter(arrowArea_);
        painter.fillRect(event->rect(), QGuiApplication::palette().color(QPalette::Base));
        painter.setRenderHint(QPainter::Antialiasing, true);
        const VisibleRows rows = collectVisibleRows();

        if (!dirtyRows_.isEmpty()) {
            for (int i = 0; i < rows.spans.size(); ++i) {
                if (!dirtyRows_.contains(rows.firstRow + i)) continue;
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0x2e, 0xcc, 0x71));
                painter.drawRoundedRect(QRectF(2, rows.spans[i].first + 2, kArrowWidth - 4,
                                               rows.spans[i].second - rows.spans[i].first - 4),
                                        2, 2);
            }
        }

        int selStart = -1;
        int selEnd = -1;
        const bool hasSelection = selectedRowRange(selStart, selEnd);

        // Yellow: differing / missing text. One arrow at the top of the
        // group with a line spanning its height. A selection starting on the
        // same row takes over that spot with the blue arrow.
        for (const DiffGroup& group : diffGroups_) {
            if (hasSelection && selStart == group.start) continue;
            int top = 0;
            int bottom = 0;
            bool startVisible = false;
            bool endVisible = false;
            if (!rows.extent(group.start, group.end, top, bottom, startVisible, endVisible)) continue;
            const QColor color = group.whitespaceOnly ? QColor(0xb0, 0x8d, 0x3a)
                                                      : QColor(0xf2, 0xc0, 0x4a);
            drawSpanArrow(painter, top, bottom, startVisible, endVisible, color);
        }

        // Blue: the selected line / block, copied as a whole when clicked.
        if (hasSelection) {
            int top = 0;
            int bottom = 0;
            bool startVisible = false;
            bool endVisible = false;
            if (rows.extent(selStart, selEnd, top, bottom, startVisible, endVisible)) {
                drawSpanArrow(painter, top, bottom, startVisible, endVisible, QColor(0x4e, 0x8f, 0xff));
            }
        }
    }

    // Arrow aligned with the first line of the span, plus a thin vertical
    // line with a closing tick that shows how far the span reaches.
    void drawSpanArrow(QPainter& painter, int top, int bottom, bool drawArrow, bool drawEnd,
                       const QColor& color) {
        const bool right = arrowSide_ == GutterSide::Right;
        const auto x = [&](qreal v) { return right ? v : kArrowWidth - v; };
        const int lineHeight = fontMetrics().lineSpacing();
        const int mid = top + lineHeight / 2;

        if (bottom - top > lineHeight + 2) {
            painter.setPen(QPen(QColor(0x9a, 0x9a, 0x9a), 1));
            const qreal lineX = x(2.5);
            const qreal lineEnd = drawEnd ? bottom - 2.5 : bottom;
            painter.drawLine(QPointF(lineX, drawArrow ? mid + 7 : top), QPointF(lineX, lineEnd));
            if (drawEnd) painter.drawLine(QPointF(lineX, lineEnd), QPointF(x(8.5), lineEnd));
        }
        if (!drawArrow) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        QPolygonF arrow;
        arrow << QPointF(x(2), mid - 2) << QPointF(x(9), mid - 2) << QPointF(x(9), mid - 5)
              << QPointF(x(16), mid) << QPointF(x(9), mid + 5) << QPointF(x(9), mid + 2)
              << QPointF(x(2), mid + 2);
        painter.drawPolygon(arrow);
    }

    // Clicking the arrow copies its span; clicking blank gutter selects that
    // row, which makes the blue arrow appear on it.
    void handleArrowClick(QMouseEvent* event) {
        if (event->button() != Qt::LeftButton) return;
        const int y = static_cast<int>(event->position().y());
        const VisibleRows rows = collectVisibleRows();
        const int row = rows.rowAt(y);
        if (row < 0) return;

        int selStart = -1;
        int selEnd = -1;
        const bool hasSelection = selectedRowRange(selStart, selEnd);
        if (hasSelection && selStart == row) {
            if (onArrowClicked) onArrowClicked(selStart, selEnd);
            return;
        }
        for (const DiffGroup& group : diffGroups_) {
            if (group.start == row) {
                if (onArrowClicked) onArrowClicked(group.start, group.end);
                return;
            }
        }
        const QTextBlock block = document()->findBlockByNumber(row);
        if (!block.isValid()) return;
        gutterRow_ = row;
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        setTextCursor(cursor);
        setFocus();
        arrowArea_->update();
    }

    template <typename Fn>
    void forEachVisibleBlock(QPaintEvent* event, Fn&& fn) {
        QTextBlock block = firstVisibleBlock();
        int blockNumber = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        int bottom = top + qRound(blockBoundingRect(block).height());
        const int limit = event ? event->rect().bottom() : viewport()->rect().bottom();
        while (block.isValid() && top <= limit) {
            if (block.isVisible() && bottom >= 0) {
                fn(blockNumber, top, bottom);
            }
            block = block.next();
            top = bottom;
            if (!block.isValid()) break;
            bottom = top + qRound(blockBoundingRect(block).height());
            ++blockNumber;
        }
    }

    QWidget* lineNumberArea_ = nullptr;
    QWidget* arrowArea_ = nullptr;
    GutterSide numberSide_ = GutterSide::Left;
    GutterSide arrowSide_ = GutterSide::None;
    QVector<int> lineNumbers_;
    QVector<TextDiffLine> diffRows_;
    QVector<DiffGroup> diffGroups_;
    QSet<int> dirtyRows_;
    int gutterRow_ = -1;  // row picked by clicking the blank arrow gutter

    QVector<QVector<CharSegment>> inlineDiffs_;

    friend class TextCompareView;
};

}  // namespace openbc::app
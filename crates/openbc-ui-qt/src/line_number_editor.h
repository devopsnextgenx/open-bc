// line_number_editor.h
// ---------------------------------------------------------------------------
// The per-side text editor used by TextCompareView: a QPlainTextEdit with a
// line-number gutter, a copy-arrow gutter, and (new) an overlay that paints
// VS Code-style dot/arrow glyphs over runs of whitespace that mismatch their
// counterpart line (space vs. tab, trailing spaces, etc). Character-level
// "this text differs" red colouring and the whitespace-mismatch background
// tint themselves are applied by SyntaxHighlighter (syntax_highlighter.h),
// which TextCompareView attaches to this editor's document - Qt does not
// support stacking two independent QSyntaxHighlighters on one document, so
// diff colouring and language syntax colouring have to share one
// highlighter; this class only draws the extra whitespace glyphs, which
// need pixel-accurate glyph positions a QTextCharFormat can't express.
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QRect>
#include <QResizeEvent>
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
    // Which margin a gutter widget lives in. Line numbers and the copy-arrow
    // strip are configured independently so the right-hand editor can show
    // its numbers on the outer (right) edge while still keeping its arrows
    // on the inner edge next to the splitter.
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
                    if (rect.contains(viewport()->rect())) updateMarginWidths();
                });
        connect(this, &QPlainTextEdit::cursorPositionChanged, this,
                [this]() { lineNumberArea_->update(); });
        lineNumberArea_ = new QWidget(this);
        lineNumberArea_->setStyleSheet("background:#232323; color:#777b8f;");
        lineNumberArea_->setCursor(Qt::ArrowCursor);
        lineNumberArea_->installEventFilter(this);
        if (arrowSide_ != GutterSide::None) {
            arrowArea_ = new QWidget(this);
            arrowArea_->setStyleSheet("background:#232323;");
            arrowArea_->setCursor(Qt::PointingHandCursor);
            arrowArea_->setToolTip(arrowSide_ == GutterSide::Right
                                        ? "Copy this change to the right"
                                        : "Copy this change to the left");
            arrowArea_->installEventFilter(this);
        }
        updateMarginWidths();
    }

    void setLineNumbers(const QVector<int>& numbers) {
        lineNumbers_ = numbers;
        updateMarginWidths();
        lineNumberArea_->update();
    }

    // Rows currently shown (same list on both editors) plus the grouping
    // used to draw/hit-test one arrow per contiguous change. Call with two
    // empty containers to hide all arrows, e.g. once free-form edits have
    // made the row mapping stale until the next Reload.
    void setDiffData(const QVector<TextDiffLine>& rows, const QVector<DiffGroup>& groups) {
        diffRows_ = rows;
        diffGroups_ = groups;
        if (arrowArea_) arrowArea_->update();
    }

    // Rows (by block number in the currently displayed document) that were
    // just copied in from the other side and are not yet saved to disk. They
    // get a green marker in the arrow gutter - green being "copied, needs
    // saving" - which is independent of (and outlives) the yellow/red diff
    // arrow, since once the copy lands both sides read the same and the
    // ordinary diff group for that row disappears on the next Reload.
    void setDirtyRows(const QSet<int>& rows) {
        dirtyRows_ = rows;
        if (arrowArea_) arrowArea_->update();
    }

    // Per-block (row) inline-diff segments for THIS side, computed by
    // TextCompareView via computeInlineDiff() and handed to both editors
    // whenever rebuild() runs. Used to paint mismatched whitespace with a
    // VS Code-style dot/arrow glyph on top of the background the syntax
    // highlighter's diff formatting already tinted (see
    // syntax_highlighter.h - SyntaxHighlighter::setInlineDiffProvider).
    void setInlineDiffs(const QVector<QVector<CharSegment>>& perBlockSegments) {
        inlineDiffs_ = perBlockSegments;
        viewport()->update();
    }

    // (start row, end row) of the group that was clicked, inclusive.
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
        const QRect cr = contentsRect();
        int left = cr.left();
        int right = cr.right();
        if (numberSide_ == GutterSide::Left) {
            lineNumberArea_->setGeometry(left, cr.top(), lineNumberDigitsWidth(), cr.height());
            left += lineNumberDigitsWidth();
        }
        if (arrowSide_ == GutterSide::Left) {
            arrowArea_->setGeometry(left, cr.top(), kArrowWidth, cr.height());
            left += kArrowWidth;
        }
        if (numberSide_ == GutterSide::Right) {
            right -= lineNumberDigitsWidth();
            lineNumberArea_->setGeometry(right, cr.top(), lineNumberDigitsWidth(), cr.height());
        }
        if (arrowSide_ == GutterSide::Right) {
            right -= kArrowWidth;
            arrowArea_->setGeometry(right, cr.top(), kArrowWidth, cr.height());
        }
    }

    // Draws small "·" (space) / "→" (tab) glyphs over whitespace that
    // differs from the counterpart line - e.g. one side indented with
    // spaces, the other with a tab - the same convention VS Code's "Render
    // Whitespace" mode uses. The mismatch's tinted background comes from
    // the syntax highlighter's format (setInlineDiffProvider); this only
    // adds the glyph on top of it so the *kind* of whitespace is legible,
    // not just that something there differs.
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

    void paintLineNumbers(QPaintEvent* event) {
        QPainter painter(lineNumberArea_);
        painter.fillRect(event->rect(), QColor("#232323"));
        const int textMargin = numberSide_ == GutterSide::Left ? 8 : 6;
        forEachVisibleBlock(event, [&](int blockNumber, int top, int bottom) {
            const int number = blockNumber < lineNumbers_.size() ? lineNumbers_[blockNumber] : 0;
            painter.setPen(number ? QColor("#a6adc8") : QColor("#555a6b"));
            painter.drawText(0, top, lineNumberArea_->width() - textMargin, bottom - top,
                             Qt::AlignRight, number ? QString::number(number) : QString());
        });
    }

    // A group with more than one visible row gets a short bar spanning its
    // full height in addition to the arrowhead, so a big change reads as one
    // wide marker instead of a giant single triangle.
    void paintArrows(QPaintEvent* event) {
        QPainter painter(arrowArea_);
        painter.fillRect(event->rect(), QColor("#232323"));
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (const DiffGroup& group : diffGroups_) {
            if (!groupHasSource(group)) continue;
            int top = -1;
            int bottom = -1;
            forEachVisibleBlock(event, [&](int blockNumber, int blockTop, int blockBottom) {
                if (blockNumber < group.start || blockNumber > group.end) return;
                if (top < 0) top = blockTop;
                bottom = blockBottom;
            });
            if (top < 0) continue;
            drawGroupArrow(painter, top, bottom, group.whitespaceOnly);
        }
        if (!dirtyRows_.isEmpty()) {
            forEachVisibleBlock(event, [&](int blockNumber, int top, int bottom) {
                if (!dirtyRows_.contains(blockNumber)) return;
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0x2e, 0xcc, 0x71));
                painter.drawRoundedRect(QRectF(2, top + 2, kArrowWidth - 4, bottom - top - 4), 2, 2);
            });
        }
    }

    void drawGroupArrow(QPainter& painter, int top, int bottom, bool whitespaceOnly) {
        const QColor color = whitespaceOnly ? QColor(0x4e, 0xa1, 0xff) : QColor(0xf2, 0xc0, 0x4a);
        const int mid = (top + bottom) / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        if (bottom - top > fontMetrics().height() + 4) {
            const int stripeX = arrowSide_ == GutterSide::Right ? 2 : kArrowWidth - 5;
            painter.fillRect(QRectF(stripeX, top + 2, 3, bottom - top - 4), color);
        }
        QPolygonF arrow;
        if (arrowSide_ == GutterSide::Right) {
            arrow << QPointF(3, mid - 6) << QPointF(3, mid + 6) << QPointF(kArrowWidth - 4, mid);
        } else {
            arrow << QPointF(kArrowWidth - 3, mid - 6) << QPointF(kArrowWidth - 3, mid + 6)
                  << QPointF(4, mid);
        }
        painter.drawPolygon(arrow);
    }

    bool groupHasSource(const DiffGroup& group) const {
        for (int row = group.start; row <= group.end && row < diffRows_.size(); ++row) {
            const bool has = arrowSide_ == GutterSide::Right ? diffRows_[row].leftNumber != 0
                                                             : diffRows_[row].rightNumber != 0;
            if (has) return true;
        }
        return false;
    }

    void handleArrowClick(QMouseEvent* event) {
        const qreal y = event->position().y();
        for (const DiffGroup& group : diffGroups_) {
            if (!groupHasSource(group)) continue;
            int top = -1;
            int bottom = -1;
            forEachVisibleBlock(nullptr, [&](int blockNumber, int blockTop, int blockBottom) {
                if (blockNumber < group.start || blockNumber > group.end) return;
                if (top < 0) top = blockTop;
                bottom = blockBottom;
            });
            if (top < 0) continue;
            if (y >= top && y < bottom && onArrowClicked) {
                onArrowClicked(group.start, group.end);
                return;
            }
        }
    }

    // Walks the blocks currently on screen, handing each one's row number and
    // pixel top/bottom to `fn`. `event` narrows the walk to the repainted
    // rect when painting; pass nullptr to cover the whole viewport (used for
    // hit-testing on click).
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

    QVector<QVector<CharSegment>> inlineDiffs_;  // per-block whitespace/mismatch segments

    friend class TextCompareView;
};

}  // namespace openbc::app

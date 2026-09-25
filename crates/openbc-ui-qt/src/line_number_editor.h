// line_number_editor.h
// ---------------------------------------------------------------------------
// (header comment unchanged)
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
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
        connect(this, &QPlainTextEdit::cursorPositionChanged, this,
                [this]() { lineNumberArea_->update(); });
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

    void paintArrows(QPaintEvent* event) {
        QPainter painter(arrowArea_);
        painter.fillRect(event->rect(), QGuiApplication::palette().color(QPalette::Base));
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

    QVector<QVector<CharSegment>> inlineDiffs_;

    friend class TextCompareView;
};

}  // namespace openbc::app
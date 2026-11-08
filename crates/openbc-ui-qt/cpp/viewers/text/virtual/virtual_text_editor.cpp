// virtual_text_editor.cpp — see virtual_text_editor.h for the design.
#include "viewers/text/virtual/virtual_text_editor.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QTextOption>
#include <QThreadPool>
#include <QWheelEvent>
#include <algorithm>

#include "common/compare_font.h"
#include "options/appearance_state.h"

namespace openbc::app {

namespace {
constexpr int kMaxFindMatchesPerLine = 200;
constexpr int kMaxHighlightLines = 400;

enum CellFlag : quint8 {
    Bold = 1, Italic = 2, InlineMismatch = 4, InlineWhitespace = 8, Selected = 16, Match = 32
};
}  // namespace

VirtualTextEditor::VirtualTextEditor(QWidget* parent) : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setCursor(Qt::IBeamCursor);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    font_ = defaultCompareFont();
    setFont(font_);
    lineCache_.setMaxCost(cacheLimitChars_);
    spanCache_.setMaxCost(20000);
    autoScroll_.setInterval(40);
    connect(&autoScroll_, &QTimer::timeout, this, [this] { autoScrollTick(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (settingScrollbar_) return;
        setTopRow(mapper_.toTop(value), true);
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (settingScrollbar_) return;
        setHorizontalPixelOffset(value);
    });
    updateMetrics();
}

VirtualTextEditor::~VirtualTextEditor() {
    alive_->store(false);
    if (findCancel_) findCancel_->cancel();
}

// ---- configuration -----------------------------------------------------------------------------

void VirtualTextEditor::setModel(std::shared_ptr<TextModel> model, Side side) {
    model_ = std::move(model);
    side_ = side;
    topRow_ = 0;
    hOffset_ = 0;
    maxContentWidth_ = 0;
    cursor_ = anchor_ = Pos{};
    haveMatch_ = false;
    lineCache_.clear();
    spanCache_.clear();
    ++highlightRequestId_;
    updateMetrics();
    if (onTopRowChanged) onTopRowChanged(0);
    requestHighlight();
    viewport()->update();
}

void VirtualTextEditor::setLanguage(const QString& language) {
    language_ = language;
    syntax_.reset();
    spanCache_.clear();
    ++highlightRequestId_;
    requestHighlight();
    viewport()->update();
}

void VirtualTextEditor::setSyntaxEnabled(bool enabled) {
    syntaxEnabled_ = enabled;
    if (!enabled) spanCache_.clear();
    else requestHighlight();
    viewport()->update();
}

void VirtualTextEditor::setDarkSyntax(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    syntax_.reset();
    spanCache_.clear();
    ++highlightRequestId_;
    requestHighlight();
    viewport()->update();
}

void VirtualTextEditor::setCacheLimitChars(int chars) {
    cacheLimitChars_ = qMax(64 * 1024, chars);
    lineCache_.setMaxCost(cacheLimitChars_);
}

void VirtualTextEditor::setEditorFont(const QFont& font) {
    font_ = font;
    setFont(font);
    maxContentWidth_ = 0;
    updateMetrics();
    viewport()->update();
}

void VirtualTextEditor::updateMetrics() {
    const QFontMetrics fm(font_);
    lineHeight_ = qMax(1, fm.lineSpacing());
    charWidth_ = qMax(1, fm.horizontalAdvance(QLatin1Char('0')));
    ascent_ = fm.ascent();
    if (showLineNumbers_) {
        const quint64 lines = model_ ? qMax<quint64>(1, model_->lineCount(side_)) : 1;
        int digits = 1;
        for (quint64 v = lines; v >= 10; v /= 10) ++digits;
        numberGutterWidth_ = charWidth_ * qMax(3, digits) + 18;
    } else {
        numberGutterWidth_ = 0;
    }
    gutterWidth_ = numberGutterWidth_ + kArrowGutterWidth;
    visibleRows_ = qMax(1, viewport()->height() / lineHeight_);
    lineCache_.clear();
    updateScrollbars();
}

int VirtualTextEditor::leftGutterWidth() const {
    return (numberGutterSide_ == GutterSide::Left ? numberGutterWidth_ : 0) +
           (arrowGutterSide_ == GutterSide::Left ? kArrowGutterWidth : 0);
}

int VirtualTextEditor::rightGutterWidth() const {
    return (numberGutterSide_ == GutterSide::Right ? numberGutterWidth_ : 0) +
           (arrowGutterSide_ == GutterSide::Right ? kArrowGutterWidth : 0);
}

int VirtualTextEditor::contentLeft() const { return leftGutterWidth(); }

int VirtualTextEditor::contentRight() const {
    return qMax(contentLeft(), viewport()->width() - rightGutterWidth());
}

void VirtualTextEditor::updateScrollbars() {
    mapper_.totalRows = model_ ? model_->rowCount() : 0;
    mapper_.visibleRows = static_cast<quint64>(visibleRows_);
    topRow_ = qMin(topRow_, mapper_.maxTop());
    settingScrollbar_ = true;
    verticalScrollBar()->setRange(0, mapper_.scrollbarMax());
    verticalScrollBar()->setPageStep(mapper_.pageStep());
    verticalScrollBar()->setSingleStep(1);
    verticalScrollBar()->setValue(mapper_.toScrollbar(topRow_));
    const int textArea = qMax(1, contentRight() - contentLeft());
    const int hmax = qMax(0, maxContentWidth_ + charWidth_ * 2 - textArea);
    horizontalScrollBar()->setRange(0, hmax);
    horizontalScrollBar()->setPageStep(textArea);
    horizontalScrollBar()->setSingleStep(charWidth_ * 4);
    hOffset_ = qMin(hOffset_, hmax);
    horizontalScrollBar()->setValue(hOffset_);
    settingScrollbar_ = false;
}

// ---- scrolling ----------------------------------------------------------------------------------

void VirtualTextEditor::setTopRow(quint64 row, bool fromScrollbar) {
    row = qMin(row, mapper_.maxTop());
    if (row == topRow_) return;
    topRow_ = row;
    if (!fromScrollbar) {
        settingScrollbar_ = true;
        verticalScrollBar()->setValue(mapper_.toScrollbar(topRow_));
        settingScrollbar_ = false;
    }
    viewport()->update();
    if (onTopRowChanged) onTopRowChanged(topRow_);
    requestHighlight();
    QTimer::singleShot(0, this, [this] { prefetch(); });
}

void VirtualTextEditor::setFirstVisibleRow(std::uint64_t row) { setTopRow(row); }

void VirtualTextEditor::setHorizontalPixelOffset(int px) {
    px = qBound(0, px, horizontalScrollBar()->maximum());
    if (px == hOffset_) return;
    hOffset_ = px;
    if (hasWindowedCached_) { lineCache_.clear(); hasWindowedCached_ = false; }
    settingScrollbar_ = true;
    horizontalScrollBar()->setValue(px);
    settingScrollbar_ = false;
    viewport()->update();
    if (onHorizontalOffsetChanged) onHorizontalOffsetChanged(px);
}

void VirtualTextEditor::ensureRowVisible(quint64 row, bool center) {
    if (center) {
        const quint64 half = static_cast<quint64>(visibleRows_ / 2);
        if (row < topRow_ || row >= topRow_ + static_cast<quint64>(visibleRows_))
            setTopRow(row > half ? row - half : 0);
    } else if (row < topRow_) {
        setTopRow(row);
    } else if (row >= topRow_ + static_cast<quint64>(visibleRows_)) {
        setTopRow(row - static_cast<quint64>(visibleRows_) + 1);
    }
}

void VirtualTextEditor::ensureColumnVisible(int pixelX) {
    const int area = qMax(1, contentRight() - contentLeft());
    if (pixelX < hOffset_) setHorizontalPixelOffset(qMax(0, pixelX - charWidth_ * 4));
    else if (pixelX > hOffset_ + area - charWidth_) setHorizontalPixelOffset(pixelX - area + charWidth_ * 4);
}

void VirtualTextEditor::resizeEvent(QResizeEvent* e) {
    QAbstractScrollArea::resizeEvent(e);
    visibleRows_ = qMax(1, viewport()->height() / lineHeight_);
    updateScrollbars();
    requestHighlight();
}

void VirtualTextEditor::wheelEvent(QWheelEvent* e) {
    if (!model_) return;
    const QPoint d = e->angleDelta();
    if (d.x() != 0 || (e->modifiers() & Qt::ShiftModifier)) {
        const int steps = (d.x() != 0 ? d.x() : d.y()) / 120;
        setHorizontalPixelOffset(hOffset_ - steps * charWidth_ * 6);
    } else if (d.y() != 0) {
        const qint64 rows = static_cast<qint64>(d.y() / 120.0 * 3.0);
        const qint64 next = static_cast<qint64>(topRow_) - (rows == 0 ? (d.y() > 0 ? 1 : -1) : rows);
        setTopRow(next < 0 ? 0 : static_cast<quint64>(next));
    }
    e->accept();
}

// ---- model access ------------------------------------------------------------------------------

qint64 VirtualTextEditor::lineForRow(quint64 row) const {
    if (!model_) return -1;
    for (quint64 r = row, n = 0; r < model_->rowCount() && n < 1000; ++r, ++n) {
        const ModelRow m = model_->row(r);
        const qint64 line = side_ == Side::Left ? m.left : m.right;
        if (line >= 0) return line;
    }
    return qMax<qint64>(0, static_cast<qint64>(model_->lineCount(side_)) - 1);
}

VirtualTextEditor::LineData VirtualTextEditor::buildLine(quint64 row) {
    LineData d;
    d.row = model_->row(row);
    d.line = side_ == Side::Left ? d.row.left : d.row.right;
    d.filler = d.line < 0;
    if (d.filler) return d;
    d.lineBytes = static_cast<quint32>(qMax<qint64>(0, model_->lineBytes(side_, static_cast<quint64>(d.line))));
    if (d.lineBytes > static_cast<quint32>(kMaxRenderChars)) {
        const quint64 start = static_cast<quint64>(qMax(0, hOffset_ / charWidth_ - 8));
        const quint32 span = static_cast<quint32>(viewport()->width() / charWidth_ + 48);
        const LineWindow w = model_->window(side_, static_cast<quint64>(d.line), start, span);
        d.text = w.text;
        d.windowStart = start;
        d.windowed = true;
        hasWindowedCached_ = true;
        return d;
    }
    d.text = model_->window(side_, static_cast<quint64>(d.line), 0, kMaxRenderChars).text;
    if (d.row.kind == RowKind::Replace) d.inlineRanges = model_->inlineRanges(row, side_, d.text);
    return d;
}

VirtualTextEditor::LineData VirtualTextEditor::lineData(quint64 row) {
    if (LineData* hit = lineCache_.object(row)) return *hit;
    LineData built = buildLine(row);
    lineCache_.insert(row, new LineData(built), built.text.size() + 48);
    return built;
}

void VirtualTextEditor::prefetch() {
    if (!model_ || prefetchRows_ == 0) return;
    const quint64 total = model_->rowCount();
    const quint64 first = topRow_ > static_cast<quint64>(prefetchRows_) ? topRow_ - prefetchRows_ : 0;
    const quint64 last = qMin(total, topRow_ + static_cast<quint64>(visibleRows_ + prefetchRows_));
    for (quint64 r = first; r < last; ++r)
        if (!lineCache_.contains(r)) lineData(r);
}

int VirtualTextEditor::lineCharCount(quint64 row) {
    const LineData d = lineData(row);
    return d.windowed ? static_cast<int>(d.windowStart) + d.text.size() : d.text.size();
}

int VirtualTextEditor::contentWidthPixels() const { return maxContentWidth_; }

qint64 VirtualTextEditor::rowAt(int y) const {
    if (!model_ || y < 0) return -1;
    const quint64 row = topRow_ + static_cast<quint64>(y / lineHeight_);
    return row < model_->rowCount() ? static_cast<qint64>(row) : -1;
}

// ---- painting ----------------------------------------------------------------------------------

std::unique_ptr<QTextLayout> VirtualTextEditor::makeLayout(const LineData& d) const {
    auto layout = std::make_unique<QTextLayout>(d.text, font_);
    QTextOption opt;
    opt.setWrapMode(QTextOption::NoWrap);
    opt.setTabStopDistance(charWidth_ * tabColumns_);
    layout->setTextOption(opt);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (line.isValid()) line.setLineWidth(1e7);
    layout->endLayout();
    return layout;
}

QColor VirtualTextEditor::rowBackground(const LineData& d) const {
    namespace ap = openbc::ui::appearance;
    switch (d.row.kind) {
    case RowKind::Replace: return ap::background("File Views/Different", QColor(150, 120, 30, 150));
    case RowKind::Insert: return side_ == Side::Right ? ap::background("File Views/Right only", QColor(70, 70, 70, 90)) : QColor();
    case RowKind::Delete: return side_ == Side::Left ? ap::background("File Views/Left only", QColor(70, 70, 70, 90)) : QColor();
    case RowKind::Equal: break;
    }
    return QColor();
}

void VirtualTextEditor::paintRow(QPainter& p, quint64 row, int y) {
    const LineData d = lineData(row);
    const int width = viewport()->width();
    const QRect full(0, y, width, lineHeight_);
    const QPalette& pal = palette();

    // Row background.
    if (d.filler) {
        p.fillRect(full, pal.color(QPalette::Base));
        p.fillRect(full, QBrush(QColor(0x50, 0x50, 0x50, 130), Qt::BDiagPattern));
    } else {
        p.fillRect(full, pal.color(QPalette::Base));
        const QColor bg = rowBackground(d);
        if (bg.isValid()) p.fillRect(full, bg);
    }
    if (row == cursor_.row) p.fillRect(full, QColor(255, 255, 255, 22));

    const int textLeft = contentLeft();
    const int textRight = contentRight();
    const int originX = textLeft + (d.windowed ? static_cast<int>(d.windowStart) * charWidth_ : 0) - hOffset_;

    if (!d.filler && !d.text.isEmpty()) {
        const int n = d.text.size();
        QVector<quint32> fg(n, 0);
        QVector<quint8> flags(n, 0);
        if (!d.windowed) {
            if (QVector<StyledSpan>* spans = spanCache_.object(static_cast<quint64>(d.line))) {
                for (const StyledSpan& s : *spans) {
                    const int a = qBound(0, s.start, n), b = qBound(0, s.start + s.length, n);
                    for (int i = a; i < b; ++i) {
                        fg[i] = s.foreground;
                        flags[i] = static_cast<quint8>((flags[i] & ~(Bold | Italic)) | (s.bold ? Bold : 0) | (s.italic ? Italic : 0));
                    }
                }
            }
            for (const InlineRange& r : d.inlineRanges) {
                const int a = qBound(0, r.start, n), b = qBound(0, r.start + r.length, n);
                for (int i = a; i < b; ++i) flags[i] |= r.whitespaceOnly ? InlineWhitespace : InlineMismatch;
            }
            if (!findText_.isEmpty()) {
                int from = 0, found = 0;
                const Qt::CaseSensitivity cs = findCase_ ? Qt::CaseSensitive : Qt::CaseInsensitive;
                while (found++ < kMaxFindMatchesPerLine) {
                    const int at = d.text.indexOf(findText_, from, cs);
                    if (at < 0) break;
                    for (int i = at; i < at + findText_.size() && i < n; ++i) flags[i] |= Match;
                    from = at + qMax(1, findText_.size());
                }
            }
        }
        // Selection overlay.
        const Pos a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
        const bool selecting = !(anchor_ == cursor_) && row >= a.row && row <= b.row;
        int selFrom = 0, selTo = 0;
        bool selToEnd = false;
        if (selecting) {
            const int base = d.windowed ? static_cast<int>(d.windowStart) : 0;
            selFrom = (row == a.row ? a.col : 0) - base;
            selTo = (row == b.row ? b.col : n + base) - base;
            selToEnd = row != b.row;
            selFrom = qBound(0, selFrom, n);
            selTo = qBound(0, selTo, n);
            for (int i = selFrom; i < selTo; ++i) flags[i] |= Selected;
        }

        std::unique_ptr<QTextLayout> layout = makeLayout(d);
        QList<QTextLayout::FormatRange> formats;
        int i = 0;
        while (i < n) {
            int j = i + 1;
            while (j < n && fg[j] == fg[i] && flags[j] == flags[i]) ++j;
            const quint8 f = flags[i];
            if (fg[i] != 0 || f != 0) {
                QTextLayout::FormatRange fr;
                fr.start = i;
                fr.length = j - i;
                if (fg[i] != 0) fr.format.setForeground(QColor((fg[i] >> 24) & 255, (fg[i] >> 16) & 255, (fg[i] >> 8) & 255));
                if (f & Bold) fr.format.setFontWeight(QFont::Bold);
                if (f & Italic) fr.format.setFontItalic(true);
                if (f & InlineMismatch) { fr.format.setForeground(QColor(0xff, 0x55, 0x55)); fr.format.setFontWeight(QFont::Bold); }
                if (f & InlineWhitespace) fr.format.setBackground(QColor(0x2d, 0x5a, 0x87, 170));
                if (f & Match) fr.format.setBackground(QColor(255, 215, 0, 110));
                if (f & Selected) fr.format.setBackground(pal.color(QPalette::Highlight));
                formats.append(fr);
            }
            i = j;
        }
        if (selToEnd && layout->lineCount() > 0) {
            const int endX = originX + qRound(layout->lineAt(0).naturalTextWidth());
            p.fillRect(QRect(qMax(endX, textLeft), y, textRight - qMax(endX, textLeft), lineHeight_),
                       pal.color(QPalette::Highlight));
        }
        p.save();
        p.setClipRect(QRect(textLeft, y, textRight - textLeft, lineHeight_));
        p.setPen(pal.color(QPalette::Text));
        layout->draw(&p, QPointF(originX, y), formats);
        p.restore();

        // Track widest line (lower bound) for the horizontal scrollbar; applied after painting.
        const int px = d.windowed ? static_cast<int>(qMin<qint64>(static_cast<qint64>(d.lineBytes) * charWidth_, 0x3fffffff))
                                  : qRound(layout->lineAt(0).naturalTextWidth());
        if (px > maxContentWidth_) { maxContentWidth_ = px; contentWidthDirty_ = true; }

        if (hasFocus() && row == cursor_.row && layout->lineCount() > 0) {
            const int col = qBound(0, cursor_.col - (d.windowed ? static_cast<int>(d.windowStart) : 0), n);
            const int cx = originX + qRound(layout->lineAt(0).cursorToX(col));
            p.fillRect(QRect(cx, y, 2, lineHeight_), pal.color(QPalette::Text));
        }
    } else if (hasFocus() && row == cursor_.row) {
        p.fillRect(QRect(originX, y, 2, lineHeight_), pal.color(QPalette::Text));
    }

    // Number and copy-arrow gutters mirror the paired LineNumberEditors.
    const QColor gutter = pal.color(QPalette::Window);
    if (leftGutterWidth() > 0) p.fillRect(QRect(0, y, leftGutterWidth(), lineHeight_), gutter);
    if (rightGutterWidth() > 0)
        p.fillRect(QRect(width - rightGutterWidth(), y, rightGutterWidth(), lineHeight_), gutter);
    if (showLineNumbers_ && !d.filler) {
        const int numberX = numberGutterSide_ == GutterSide::Left ? 0 : width - numberGutterWidth_;
        p.setPen(QColor(0x62, 0x72, 0xa4));
        p.drawText(QRect(numberX, y, numberGutterWidth_ - 6, lineHeight_),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(d.line + 1));
    }
}

void VirtualTextEditor::paintEvent(QPaintEvent*) {
    QPainter p(viewport());
    p.setFont(font_);
    p.fillRect(viewport()->rect(), palette().color(QPalette::Base));
    if (!model_) return;
    const quint64 total = model_->rowCount();
    const int rows = (viewport()->height() + lineHeight_ - 1) / lineHeight_;
    for (int i = 0; i < rows; ++i) {
        const quint64 row = topRow_ + static_cast<quint64>(i);
        if (row >= total) break;
        paintRow(p, row, i * lineHeight_);
    }

    const auto isChanged = [this, total](quint64 row) {
        return row < total && model_->row(row).kind != RowKind::Equal;
    };
    const int arrowX = arrowGutterSide_ == GutterSide::Left
                           ? 0
                           : viewport()->width() - kArrowGutterWidth;
    const auto drawSpanArrow = [this, &p, arrowX](int top, int bottom, bool drawArrow,
                                                   bool drawEnd) {
        const bool right = arrowGutterSide_ == GutterSide::Right;
        const auto x = [right, arrowX](qreal offset) {
            return arrowX + (right ? offset : kArrowGutterWidth - offset);
        };
        const int mid = top + lineHeight_ / 2;
        if (bottom - top > lineHeight_ + 2) {
            p.setPen(QPen(QColor(0x9a, 0x9a, 0x9a), 1));
            const qreal lineX = x(2.5);
            const qreal lineEnd = drawEnd ? bottom - 2.5 : bottom;
            p.drawLine(QPointF(lineX, drawArrow ? mid + 7 : top),
                       QPointF(lineX, lineEnd));
            if (drawEnd)
                p.drawLine(QPointF(lineX, lineEnd), QPointF(x(8.5), lineEnd));
        }
        if (!drawArrow) return;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf2, 0xc0, 0x4a, 150));
        QPolygonF arrow;
        arrow << QPointF(x(2), mid - 2) << QPointF(x(9), mid - 2)
              << QPointF(x(9), mid - 5) << QPointF(x(16), mid)
              << QPointF(x(9), mid + 5) << QPointF(x(9), mid + 2)
              << QPointF(x(2), mid + 2);
        p.drawPolygon(arrow);
    };

    int row = 0;
    while (row < rows && topRow_ + static_cast<quint64>(row) < total) {
        const quint64 firstRow = topRow_ + static_cast<quint64>(row);
        if (!isChanged(firstRow)) {
            ++row;
            continue;
        }

        const int start = row;
        while (row + 1 < rows &&
               topRow_ + static_cast<quint64>(row + 1) < total &&
               isChanged(topRow_ + static_cast<quint64>(row + 1))) {
            ++row;
        }
        const int end = row;
        const quint64 after = topRow_ + static_cast<quint64>(end) + 1;
        const bool startVisible = start != 0 || topRow_ == 0 || !isChanged(topRow_ - 1);
        const bool endVisible = end != rows - 1 || after >= total || !isChanged(after);
        const int top = start * lineHeight_;
        const int bottom = qMin((end + 1) * lineHeight_, viewport()->height());

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        drawSpanArrow(top, bottom, startVisible, endVisible);
        p.restore();
        ++row;
    }

    if (arrowGutterSide_ == GutterSide::Right)
        p.fillRect(QRect(width() - rightGutterWidth(), 0, 1, viewport()->height()),
                   palette().color(QPalette::Mid));
    else
        p.fillRect(QRect(leftGutterWidth() - 1, 0, 1, viewport()->height()),
                   palette().color(QPalette::Mid));
    if (contentWidthDirty_) {
        contentWidthDirty_ = false;
        QTimer::singleShot(0, this, [this] { updateScrollbars(); });
    }
}

// ---- caret, selection, keyboard -----------------------------------------------------------------

void VirtualTextEditor::clampPos(Pos& p) {
    const quint64 total = model_ ? model_->rowCount() : 0;
    if (total == 0) { p = Pos{}; return; }
    p.row = qMin(p.row, total - 1);
    p.col = qBound(0, p.col, lineCharCount(p.row));
}

void VirtualTextEditor::moveCaret(Pos next, bool extend) {
    if (model_ && !inEdit_) model_->sealUndo(side_);   // navigating ends a typing run
    clampPos(next);
    const bool rowChanged = next.row != cursor_.row;
    cursor_ = next;
    if (!extend) anchor_ = cursor_;
    ensureRowVisible(cursor_.row);
    const LineData d = lineData(cursor_.row);
    if (!d.filler) {
        std::unique_ptr<QTextLayout> layout = makeLayout(d);
        if (layout->lineCount() > 0) {
            const int base = d.windowed ? static_cast<int>(d.windowStart) : 0;
            const int x = (d.windowed ? base * charWidth_ : 0) + qRound(layout->lineAt(0).cursorToX(qBound(0, cursor_.col - base, d.text.size())));
            ensureColumnVisible(x);
        }
    }
    viewport()->update();
    if (rowChanged) if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
    notifySelection();
}

void VirtualTextEditor::setCaret(quint64 row, int col, bool extend, bool ensure) {
    Pos next{row, col};
    clampPos(next);
    if (!ensure) {
        cursor_ = next;
        if (!extend) anchor_ = cursor_;
        viewport()->update();
        if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
        notifySelection();
        return;
    }
    const bool visible = next.row >= topRow_ && next.row < topRow_ + static_cast<quint64>(visibleRows_);
    if (!visible) ensureRowVisible(next.row, true);
    moveCaret(next, extend);
}

void VirtualTextEditor::setCurrentRowQuietly(quint64 row) {
    if (!model_ || model_->rowCount() == 0) return;
    cursor_.row = qMin(row, model_->rowCount() - 1);
    cursor_.col = 0;
    if (!hasSelection() || true) anchor_ = cursor_;
    viewport()->update();
}

void VirtualTextEditor::notifySelection() { if (onSelectionChanged) onSelectionChanged(); }

void VirtualTextEditor::keyPressEvent(QKeyEvent* e) {
    if (!model_) { e->ignore(); return; }
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    const quint64 total = model_->rowCount();
    Pos next = cursor_;
    if (e->matches(QKeySequence::Copy)) { copySelection(); return; }
    if (e->matches(QKeySequence::SelectAll)) { selectAllText(); return; }
    if (e->matches(QKeySequence::Paste)) { pasteClipboard(); e->accept(); return; }
    if (e->matches(QKeySequence::Cut)) { cutSelection(); e->accept(); return; }
    if (e->matches(QKeySequence::Undo)) { undo(); e->accept(); return; }
    if (e->matches(QKeySequence::Redo)) { redo(); e->accept(); return; }
    switch (e->key()) {
    case Qt::Key_Backspace: deleteBackward(); e->accept(); return;
    case Qt::Key_Delete: deleteForward(); e->accept(); return;
    case Qt::Key_Return:
    case Qt::Key_Enter: insertNewline(); e->accept(); return;
    case Qt::Key_Tab: insertText(QStringLiteral("\t")); e->accept(); return;
    case Qt::Key_Left:
        if (next.col > 0) --next.col;
        else if (next.row > 0) { --next.row; next.col = lineCharCount(next.row); }
        break;
    case Qt::Key_Right:
        if (next.col < lineCharCount(next.row)) ++next.col;
        else if (next.row + 1 < total) { ++next.row; next.col = 0; }
        break;
    case Qt::Key_Up: if (next.row > 0) --next.row; break;
    case Qt::Key_Down: if (next.row + 1 < total) ++next.row; break;
    case Qt::Key_PageUp:
        next.row = next.row > static_cast<quint64>(visibleRows_) ? next.row - visibleRows_ : 0;
        setTopRow(topRow_ > static_cast<quint64>(visibleRows_) ? topRow_ - visibleRows_ : 0);
        break;
    case Qt::Key_PageDown:
        next.row = qMin(total ? total - 1 : 0, next.row + static_cast<quint64>(visibleRows_));
        setTopRow(topRow_ + static_cast<quint64>(visibleRows_));
        break;
    case Qt::Key_Home:
        if (ctrl) next.row = 0;
        next.col = 0;
        break;
    case Qt::Key_End:
        if (ctrl && total) next.row = total - 1;
        next.col = lineCharCount(next.row);
        break;
    default: {
        // Printable text (AltGr shows up as Ctrl+Alt on some platforms and must still type).
        const Qt::KeyboardModifiers mods = e->modifiers();
        const bool command = (mods & Qt::ControlModifier) && !(mods & Qt::AltModifier);
        const char32_t first = e->text().toUcs4().value(0, 0);   // a code point: emoji are surrogate pairs
        if (!e->text().isEmpty() && !command && !(mods & Qt::MetaModifier) && QChar::isPrint(first)) {
            insertText(e->text());
            e->accept();
            return;
        }
        e->ignore();
        return;
    }
    }
    moveCaret(next, shift);
    e->accept();
}

QString VirtualTextEditor::fullLineText(quint64 row) {
    const ModelRow m = model_->row(row);
    const qint64 line = side_ == Side::Left ? m.left : m.right;
    if (line < 0) return QString();
    return model_->window(side_, static_cast<quint64>(line), 0, 4u * 1024u * 1024u).text;
}

QString VirtualTextEditor::selectedText(qint64 maxBytes, bool* truncated) const {
    if (truncated) *truncated = false;
    if (!model_ || anchor_ == cursor_) return QString();
    auto* self = const_cast<VirtualTextEditor*>(this);
    const Pos a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
    QString out;
    qint64 bytes = 0;
    for (quint64 row = a.row; row <= b.row; ++row) {
        const ModelRow m = model_->row(row);
        const qint64 line = side_ == Side::Left ? m.left : m.right;
        if (line < 0) continue;   // filler rows contribute nothing
        QString text = self->fullLineText(row);
        const int from = row == a.row ? qBound(0, a.col, text.size()) : 0;
        const int to = row == b.row ? qBound(from, b.col, text.size()) : text.size();
        QString piece = text.mid(from, to - from);
        if (row != b.row) piece += QLatin1Char('\n');
        bytes += piece.size() * 2;
        out += piece;
        if (bytes > maxBytes) {
            if (truncated) *truncated = true;
            break;
        }
        if (row == std::numeric_limits<quint64>::max()) break;
    }
    return out;
}

void VirtualTextEditor::copySelection() {
    bool truncated = false;
    const QString text = selectedText(kDefaultCopyLimit, &truncated);
    if (text.isEmpty()) return;
    QGuiApplication::clipboard()->setText(text);
    if (truncated) if (onStatusMessage) onStatusMessage(tr("Selection was larger than 64 MB; only the first part was copied."));
}

void VirtualTextEditor::selectAllText() {
    if (!model_ || model_->rowCount() == 0) return;
    anchor_ = Pos{0, 0};
    cursor_ = Pos{model_->rowCount() - 1, 0};
    cursor_.col = lineCharCount(cursor_.row);
    viewport()->update();
    if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
    notifySelection();
}

// ---- mouse ---------------------------------------------------------------------------------------

VirtualTextEditor::Pos VirtualTextEditor::hitTest(const QPoint& pt) {
    Pos pos;
    if (!model_ || model_->rowCount() == 0) return pos;
    const int y = qMax(0, pt.y());
    pos.row = qMin(model_->rowCount() - 1, topRow_ + static_cast<quint64>(y / lineHeight_));
    const LineData d = lineData(pos.row);
    if (d.filler) return pos;
    std::unique_ptr<QTextLayout> layout = makeLayout(d);
    if (layout->lineCount() == 0) return pos;
    const int originX = contentLeft() +
                        (d.windowed ? static_cast<int>(d.windowStart) * charWidth_ : 0) - hOffset_;
    const int col = layout->lineAt(0).xToCursor(pt.x() - originX);
    pos.col = col + (d.windowed ? static_cast<int>(d.windowStart) : 0);
    return pos;
}

void VirtualTextEditor::mousePressEvent(QMouseEvent* e) {
    setFocus();
    if (onFocused) onFocused();
    if (e->button() != Qt::LeftButton || !model_) return;
    if (quint64 arrowRow = 0; arrowHit(e->pos(), &arrowRow)) {
        if (onCopyBlockRequested) onCopyBlockRequested(arrowRow);
        return;                                   // a click on the arrow never moves the caret
    }
    model_->sealUndo(side_);
    dragging_ = true;
    lastMouse_ = e->pos();
    Pos pos = hitTest(e->pos());
    clampPos(pos);
    cursor_ = pos;
    if (!(e->modifiers() & Qt::ShiftModifier)) anchor_ = pos;
    viewport()->update();
    if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
    notifySelection();
}

void VirtualTextEditor::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_ && model_) {
        quint64 row = 0;
        viewport()->setCursor(arrowHit(e->pos(), &row) ? Qt::PointingHandCursor : Qt::IBeamCursor);
    }
    if (!dragging_ || !model_) return;
    lastMouse_ = e->pos();
    const bool outside = e->pos().y() < 0 || e->pos().y() > viewport()->height();
    if (outside && !autoScroll_.isActive()) autoScroll_.start();
    if (!outside) autoScroll_.stop();
    Pos pos = hitTest(QPoint(e->pos().x(), qBound(0, e->pos().y(), viewport()->height() - 1)));
    clampPos(pos);
    if (!(pos == cursor_)) {
        const bool rowChanged = pos.row != cursor_.row;
        cursor_ = pos;
        viewport()->update();
        if (rowChanged) if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
        notifySelection();
    }
}

void VirtualTextEditor::autoScrollTick() {
    if (!dragging_ || !model_) { autoScroll_.stop(); return; }
    const int overshoot = lastMouse_.y() < 0 ? -1 : (lastMouse_.y() > viewport()->height() ? 1 : 0);
    if (overshoot == 0) { autoScroll_.stop(); return; }
    const quint64 step = static_cast<quint64>(qMax(1, qAbs(lastMouse_.y() < 0 ? lastMouse_.y() : lastMouse_.y() - viewport()->height()) / lineHeight_));
    if (overshoot < 0) setTopRow(topRow_ > step ? topRow_ - step : 0);
    else setTopRow(topRow_ + step);
    Pos pos = hitTest(QPoint(lastMouse_.x(), overshoot < 0 ? 0 : viewport()->height() - 1));
    clampPos(pos);
    cursor_ = pos;
    viewport()->update();
    notifySelection();
}

void VirtualTextEditor::mouseReleaseEvent(QMouseEvent*) {
    dragging_ = false;
    autoScroll_.stop();
}

QString VirtualTextEditor::wordAt(const Pos& p, int* start, int* end) {
    const QString text = fullLineText(p.row);
    int a = qBound(0, p.col, text.size()), b = a;
    auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    while (a > 0 && isWord(text[a - 1])) --a;
    while (b < text.size() && isWord(text[b])) ++b;
    *start = a;
    *end = b;
    return text.mid(a, b - a);
}

void VirtualTextEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !model_) return;
    Pos pos = hitTest(e->pos());
    clampPos(pos);
    int a = 0, b = 0;
    wordAt(pos, &a, &b);
    anchor_ = Pos{pos.row, a};
    cursor_ = Pos{pos.row, b};
    viewport()->update();
    notifySelection();
}

void VirtualTextEditor::focusInEvent(QFocusEvent* e) {
    QAbstractScrollArea::focusInEvent(e);
    viewport()->update();
    if (onFocused) onFocused();
}

void VirtualTextEditor::focusOutEvent(QFocusEvent* e) {
    QAbstractScrollArea::focusOutEvent(e);
    viewport()->update();
}

void VirtualTextEditor::changeEvent(QEvent* e) {
    QAbstractScrollArea::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) viewport()->update();
}

void VirtualTextEditor::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    // Edit entries stay enabled while read-only so choosing one flashes the lock (like typing does).
    menu.addAction(tr("Undo"), this, [this] { undo(); })->setEnabled(readOnly_ || canUndo());
    menu.addAction(tr("Redo"), this, [this] { redo(); })->setEnabled(readOnly_ || canRedo());
    menu.addSeparator();
    menu.addAction(tr("Cut"), this, [this] { cutSelection(); });
    menu.addAction(tr("Copy"), this, [this] { copySelection(); })->setEnabled(hasSelection());
    menu.addAction(tr("Paste"), this, [this] { pasteClipboard(); });
    menu.addAction(tr("Select All"), this, [this] { selectAllText(); });
    menu.addSeparator();
    menu.addAction(tr("Next Difference"), this, [this] { goToNextDifference(); });
    menu.addAction(tr("Previous Difference"), this, [this] { goToPreviousDifference(); });
    menu.exec(e->globalPos());
}

// ---- navigation and search ------------------------------------------------------------------------

void VirtualTextEditor::goToLine(std::uint64_t line) {
    if (!model_ || line == 0) return;
    const quint64 count = model_->lineCount(side_);
    if (count == 0) return;
    const quint64 index = qMin<quint64>(line - 1, count - 1);
    if (const auto row = model_->rowForLine(side_, index)) setCaret(*row, 0, false, true);
}

void VirtualTextEditor::goToNextDifference() {
    if (!model_) return;
    if (const auto row = model_->nextDifference(cursor_.row)) setCaret(*row, 0, false, true);
    else if (onStatusMessage) onStatusMessage(tr("No more differences."));
}

void VirtualTextEditor::goToPreviousDifference() {
    if (!model_) return;
    if (const auto row = model_->prevDifference(cursor_.row)) setCaret(*row, 0, false, true);
    else if (onStatusMessage) onStatusMessage(tr("No earlier differences."));
}

bool VirtualTextEditor::findText(const QString& text, bool caseSensitive, bool forward) {
    findText_ = text;
    findCase_ = caseSensitive;
    if (!model_ || text.isEmpty()) { haveMatch_ = false; viewport()->update(); return false; }

    qint64 startLine = -1;
    int startCol = 0;
    if (haveMatch_ && (anchor_ == matchStart_ || anchor_ == matchEnd_ || cursor_ == matchEnd_ || cursor_ == matchStart_)) {
        startLine = lineForRow(matchStart_.row);
        startCol = matchStart_.col;
    } else {
        const Pos from = hasSelection() && forward ? std::max(anchor_, cursor_) : std::min(anchor_, cursor_);
        const qint64 line = lineForRow(from.row);
        if (forward) {
            if (from.col > 0) { startLine = line; startCol = from.col - 1; }
            else if (line > 0) { startLine = line - 1; startCol = std::numeric_limits<int>::max() / 2; }
        } else {
            startLine = line;
            startCol = from.col;
        }
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool failed = false;
    if (!findCancel_) findCancel_ = std::make_unique<Cancellation>();
    const auto hit = model_->find(side_, text, caseSensitive, startLine, startCol, forward, findCancel_->get(), &failed);
    QApplication::restoreOverrideCursor();
    if (!hit) {
        haveMatch_ = false;
        viewport()->update();
        if (onStatusMessage) onStatusMessage(failed ? tr("Search was cancelled.") : tr("Not found: %1").arg(text));
        return false;
    }
    const auto row = model_->rowForLine(side_, hit->line);
    if (!row) return false;
    matchStart_ = Pos{*row, hit->start};
    matchEnd_ = Pos{*row, hit->start + hit->length};
    haveMatch_ = true;
    anchor_ = matchStart_;
    cursor_ = matchEnd_;
    ensureRowVisible(*row, true);
    moveCaret(matchEnd_, true);
    if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
    return true;
}

// ---- editing ---------------------------------------------------------------------------------------

void VirtualTextEditor::setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    if (model_) model_->sealUndo(side_);
    viewport()->update();
}

bool VirtualTextEditor::guardEdit() {
    if (!model_) return false;
    if (readOnly_) {
        if (onEditBlocked) onEditBlocked();
        return false;
    }
    return true;
}

quint64 VirtualTextEditor::sideCursor(quint64 row) const {
    const auto c = model_->lineCursors(row);
    return side_ == Side::Left ? c.first : c.second;
}

// Replaces the text between two positions (row, QString column). Works in LINE coordinates of this
// side, so selections spanning filler rows, joining and splitting lines all reduce to one
// "replace N lines starting at L by this text" call on the model.
bool VirtualTextEditor::applyReplace(Pos from, Pos to, const QString& textIn, bool coalesce) {
    if (!model_) return false;
    clampPos(from);
    clampPos(to);
    if (to < from) std::swap(from, to);
    QString text = textIn;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const bool haveRows = model_->rowCount() > 0;
    const auto lineOf = [this](const ModelRow& m) { return side_ == Side::Left ? m.left : m.right; };
    const qint64 lf = haveRows ? lineOf(model_->row(from.row)) : -1;
    const qint64 lt = haveRows ? lineOf(model_->row(to.row)) : -1;
    for (const qint64 line : {lf, lt}) {
        if (line >= 0 && model_->lineBytes(side_, static_cast<quint64>(line)) > kMaxRenderChars) {
            if (onStatusMessage) onStatusMessage(tr("This line is too long to edit in the Virtual editor."));
            return false;
        }
    }
    const quint64 startLine = lf >= 0 ? static_cast<quint64>(lf) : (haveRows ? sideCursor(from.row) : 0);
    const quint64 endCursor = haveRows ? sideCursor(to.row + 1) : 0;
    const quint64 remove = endCursor > startLine ? endCursor - startLine : 0;
    const QString prefix = lf >= 0 ? fullLineText(from.row).left(from.col) : QString();
    const QString suffix = lt >= 0 ? fullLineText(to.row).mid(to.col) : QString();
    if (lf < 0 && lt < 0 && text == QLatin1String("\n")) text.clear();   // Enter on a filler row adds ONE empty line
    if (remove == 0 && text.isEmpty() && prefix.isEmpty() && suffix.isEmpty() && lf < 0) return false;

    const QString combined = prefix + text + suffix;
    QString error;
    const auto edit = model_->replaceLines(side_, startLine, remove, &combined, coalesce, &error);
    if (!edit) {
        if (onStatusMessage && !error.isEmpty()) onStatusMessage(error);
        return false;
    }
    if (onEdited) onEdited(*edit);
    else modelEdited(*edit);

    const int newlines = static_cast<int>(text.count(QLatin1Char('\n')));
    const quint64 caretLine = startLine + static_cast<quint64>(newlines);
    const int caretCol = newlines ? static_cast<int>(text.size() - text.lastIndexOf(QLatin1Char('\n')) - 1)
                                  : static_cast<int>(prefix.size() + text.size());
    if (const auto row = model_->rowForLine(side_, caretLine)) {
        inEdit_ = true;                    // placing the caret after typing keeps the undo run open
        setCaret(*row, caretCol, false, true);
        inEdit_ = false;
    }
    return true;
}

bool VirtualTextEditor::insertText(const QString& text) {
    if (!guardEdit() || text.isEmpty()) return false;
    const Pos a = std::min(anchor_, cursor_), b = std::max(anchor_, cursor_);
    // Single characters typed without a selection are merged into one undo step.
    const bool typing = !hasSelection() && text.size() <= 2 && !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r'));
    return applyReplace(a, b, text, typing);
}

bool VirtualTextEditor::deleteBackward() {
    if (!guardEdit()) return false;
    if (hasSelection()) return applyReplace(std::min(anchor_, cursor_), std::max(anchor_, cursor_), QString(), false);
    const LineData d = lineData(cursor_.row);
    if (d.filler) return false;
    if (cursor_.col > 0) {
        int from = cursor_.col - 1;
        if (from > 0 && d.text.at(from).isLowSurrogate() && d.text.at(from - 1).isHighSurrogate()) --from;   // whole emoji
        return applyReplace(Pos{cursor_.row, from}, cursor_, QString(), true);
    }
    if (d.line <= 0) return false;
    const auto prevRow = model_->rowForLine(side_, static_cast<quint64>(d.line - 1));
    if (!prevRow) return false;
    return applyReplace(Pos{*prevRow, lineCharCount(*prevRow)}, Pos{cursor_.row, 0}, QString(), false);
}

bool VirtualTextEditor::deleteForward() {
    if (!guardEdit()) return false;
    if (hasSelection()) return applyReplace(std::min(anchor_, cursor_), std::max(anchor_, cursor_), QString(), false);
    const LineData d = lineData(cursor_.row);
    if (d.filler) return false;
    if (cursor_.col < d.text.size()) {
        int to = cursor_.col + 1;
        if (to < d.text.size() && d.text.at(to).isLowSurrogate() && d.text.at(to - 1).isHighSurrogate()) ++to;
        return applyReplace(cursor_, Pos{cursor_.row, to}, QString(), true);
    }
    if (static_cast<quint64>(d.line) + 1 >= model_->lineCount(side_)) return false;
    const auto nextRow = model_->rowForLine(side_, static_cast<quint64>(d.line + 1));
    if (!nextRow) return false;
    return applyReplace(Pos{cursor_.row, static_cast<int>(d.text.size())}, Pos{*nextRow, 0}, QString(), false);
}

bool VirtualTextEditor::insertNewline() {
    if (!guardEdit()) return false;
    QString text = QStringLiteral("\n");
    if (autoIndent_) {
        const LineData d = lineData(std::min(anchor_, cursor_).row);
        if (!d.filler) {
            const QString before = d.text.left(std::min(anchor_, cursor_).col);
            int n = 0;
            while (n < before.size() && (before[n] == QLatin1Char(' ') || before[n] == QLatin1Char('\t'))) ++n;
            text += before.left(n);
        }
    }
    return applyReplace(std::min(anchor_, cursor_), std::max(anchor_, cursor_), text, false);
}

bool VirtualTextEditor::cutSelection() {
    if (!guardEdit() || !hasSelection()) return false;
    copySelection();
    return applyReplace(std::min(anchor_, cursor_), std::max(anchor_, cursor_), QString(), false);
}

bool VirtualTextEditor::pasteClipboard() {
    if (!guardEdit()) return false;
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) return false;
    if (text.size() > 64 * 1024 * 1024) {
        if (onStatusMessage) onStatusMessage(tr("The clipboard text is too large to paste."));
        return false;
    }
    return insertText(text);
}

bool VirtualTextEditor::undo() {
    if (!guardEdit()) return false;
    QString error;
    const auto edit = model_->undo(side_, &error);
    if (!edit) {
        if (!error.isEmpty() && onStatusMessage) onStatusMessage(error);
        return false;
    }
    if (onEdited) onEdited(*edit);
    else modelEdited(*edit);
    setCaret(edit->firstRow, 0, false, true);
    return true;
}

bool VirtualTextEditor::redo() {
    if (!guardEdit()) return false;
    QString error;
    const auto edit = model_->redo(side_, &error);
    if (!edit) {
        if (!error.isEmpty() && onStatusMessage) onStatusMessage(error);
        return false;
    }
    if (onEdited) onEdited(*edit);
    else modelEdited(*edit);
    setCaret(edit->firstRow, 0, false, true);
    return true;
}

void VirtualTextEditor::modelEdited(const ModelEdit& e) {
    lineCache_.clear();                     // keyed by display row: every row after the edit moved
    haveMatch_ = false;
    const auto shift = [&e](Pos& p) {
        if (p.row >= e.firstRow + e.oldRows) {
            p.row = p.row - e.oldRows + e.newRows;
        } else if (p.row >= e.firstRow) {   // inside the replaced rows: park at the start of the region
            p.row = e.firstRow;
            p.col = 0;
        }
    };
    shift(cursor_);
    shift(anchor_);
    if (e.side == side_) {
        spanCache_.clear();                 // keyed by FILE line: lines at/after the edit changed
        if (syntax_) syntax_->invalidateFrom(e.firstLine);
        ++highlightRequestId_;
    }
    updateMetrics();                        // line-number gutter width may change
    if (model_ && model_->rowCount() > 0) {
        cursor_.row = qMin(cursor_.row, model_->rowCount() - 1);
        anchor_.row = qMin(anchor_.row, model_->rowCount() - 1);
    } else {
        cursor_ = anchor_ = Pos{};
    }
    viewport()->update();
    if (e.side == side_) requestHighlight();
    if (onCurrentRowChanged) onCurrentRowChanged(cursor_.row);
}

void VirtualTextEditor::invalidateAll() {
    lineCache_.clear();
    haveMatch_ = false;
    updateMetrics();
    if (model_ && model_->rowCount() > 0) {
        cursor_.row = qMin(cursor_.row, model_->rowCount() - 1);
        anchor_.row = qMin(anchor_.row, model_->rowCount() - 1);
    } else {
        cursor_ = anchor_ = Pos{};
    }
    viewport()->update();
}

bool VirtualTextEditor::arrowHit(const QPoint& p, quint64* row) const {
    if (!model_) return false;
    const bool right = arrowGutterSide_ == GutterSide::Right;
    const int arrowX = right ? viewport()->width() - kArrowGutterWidth : 0;
    if (p.x() < arrowX || p.x() >= arrowX + kArrowGutterWidth) return false;
    const qint64 r = rowAt(p.y());
    if (r < 0) return false;
    const quint64 urow = static_cast<quint64>(r);
    if (model_->row(urow).kind == RowKind::Equal) return false;
    if (urow > 0 && model_->row(urow - 1).kind != RowKind::Equal) return false;   // arrow sits on a block's first row
    *row = urow;
    return true;
}

bool VirtualTextEditor::focusNextPrevChild(bool next) {
    if (next && !readOnly_) return false;
    return QAbstractScrollArea::focusNextPrevChild(next);
}

void VirtualTextEditor::inputMethodEvent(QInputMethodEvent* e) {
    if (!e->commitString().isEmpty()) {
        insertText(e->commitString());      // read-only: guardEdit flashes the lock
    }
    e->accept();
}

QVariant VirtualTextEditor::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled: return true;
    case Qt::ImCursorRectangle: {
        const int y = static_cast<int>(qBound<qint64>(0, static_cast<qint64>(cursor_.row) - static_cast<qint64>(topRow_), static_cast<qint64>(visibleRows_))) * lineHeight_;
        return QRect(gutterWidth_ + cursor_.col * charWidth_ - hOffset_, y, 2, lineHeight_);
    }
    case Qt::ImFont: return font_;
    default: return QAbstractScrollArea::inputMethodQuery(query);
    }
}

// ---- syntax (worker thread) ----------------------------------------------------------------------------

void VirtualTextEditor::requestHighlight() {
    if (!syntaxEnabled_ || !model_ || language_.isEmpty() || model_->rowCount() == 0) return;
    if (highlightRunning_) { highlightDirty_ = true; return; }

    // Lines needed by the viewport (+ prefetch), as a contiguous file-line range of this side.
    const quint64 total = model_->rowCount();
    const quint64 firstRow = topRow_ > static_cast<quint64>(prefetchRows_) ? topRow_ - prefetchRows_ : 0;
    const quint64 lastRow = qMin(total, topRow_ + static_cast<quint64>(visibleRows_ + prefetchRows_));
    qint64 minLine = -1, maxLine = -1;
    for (quint64 r = firstRow; r < lastRow; ++r) {
        const ModelRow m = model_->row(r);
        const qint64 line = side_ == Side::Left ? m.left : m.right;
        if (line < 0) continue;
        if (minLine < 0) minLine = line;
        maxLine = line;
    }
    if (minLine < 0) return;
    bool allCached = true;
    for (qint64 l = minLine; l <= maxLine; ++l)
        if (!spanCache_.contains(static_cast<quint64>(l))) { allCached = false; break; }
    if (allCached) return;

    const quint64 first = static_cast<quint64>(minLine);
    const quint64 count = qMin<quint64>(static_cast<quint64>(maxLine - minLine + 1), kMaxHighlightLines);
    const quint64 generation = highlightRequestId_;
    highlightRunning_ = true;
    QPointer<VirtualTextEditor> self(this);
    auto model = model_;
    auto syntax = syntax_;
    const Side side = side_;
    const QString language = language_;
    const bool dark = dark_;
    auto alive = alive_;

    QThreadPool::globalInstance()->start([=]() {
        std::shared_ptr<SyntaxSession> session = syntax;
        if (!session) session = std::make_shared<SyntaxSession>(language, dark);   // loads grammars: slow, off the GUI thread
        auto textOf = [&](quint64 line) { return model->window(side, line, 0, kMaxRenderChars).text; };
        QVector<QVector<StyledSpan>> spans = session->highlight(*model, side, first, count, textOf);
        if (!alive->load()) return;
        QMetaObject::invokeMethod(self.data(), [self, session, generation, first, spans]() mutable {
            if (!self) return;
            if (!self->syntax_ && generation == self->highlightRequestId_) self->syntax_ = session;
            self->applyHighlight(generation, first, std::move(spans));
        }, Qt::QueuedConnection);
    });
}

void VirtualTextEditor::applyHighlight(quint64 generation, quint64 first, QVector<QVector<StyledSpan>> spans) {
    highlightRunning_ = false;
    if (generation == highlightRequestId_) {
        for (int i = 0; i < spans.size(); ++i)
            spanCache_.insert(first + static_cast<quint64>(i), new QVector<StyledSpan>(spans[i]), spans[i].size() + 4);
        viewport()->update();
    }
    if (highlightDirty_ || generation != highlightRequestId_) {
        highlightDirty_ = false;
        requestHighlight();
    }
}

}  // namespace openbc::app

// virtual_text_editor.h
// ---------------------------------------------------------------------------
// VirtualTextEditor: a read-only, viewport-based text compare editor.
//
//   * QAbstractScrollArea; NO QTextDocument, NO QString/QStringList of the file, NO per-line widgets.
//   * Renders only the visible rows (+ a small prefetch margin) with QPainter + QTextLayout.
//   * Everything it shows comes from a TextModel (the Qt-independent engine): row alignment, line
//     text (decoded lazily), inline diffs, search, syntax spans.
//   * Decoded lines, inline ranges and syntax spans live in bounded LRU caches (QCache).
//   * Rows are uint64; the vertical scrollbar uses ScrollMapper so nothing overflows an int.
//   * Very long lines (> kMaxRenderChars bytes) are shown as a window around the horizontal scroll
//     position (plain text, no syntax / inline diff) so a 1 GB single-line file stays responsive.
//   * Syntax highlighting is computed on a worker thread; rows paint plain text until spans arrive.
//
// Two editors (left and right) share one model; the tab wires their scroll/cursor signals.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractScrollArea>
#include <QCache>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

#include "viewers/text/text_compare_editor.h"
#include "viewers/text/virtual/scroll_mapping.h"

class QTextLayout;

namespace openbc::app {

class VirtualTextEditor : public QAbstractScrollArea, public ITextCompareEditor {
    // No Q_OBJECT: like the rest of the app this is header/moc-free; notifications are callbacks.
public:
    // Lines longer than this many bytes are windowed instead of decoded whole.
    static constexpr int kMaxRenderChars = 16 * 1024;

    explicit VirtualTextEditor(QWidget* parent = nullptr);
    ~VirtualTextEditor() override;

    // ---- ITextCompareEditor ----------------------------------------------------------------
    void setModel(std::shared_ptr<TextModel> model, Side side) override;
    void setReadOnly(bool) override {}                 // always read-only for now (documented)
    bool isReadOnly() const override { return true; }
    void goToLine(std::uint64_t line) override;
    void goToNextDifference() override;
    void goToPreviousDifference() override;
    bool findText(const QString& text, bool caseSensitive, bool forward) override;
    void setEditorFont(const QFont& font) override;
    QString selectedText() const override { return selectedText(kDefaultCopyLimit); }
    void copySelection() override;
    void selectAllText() override;
    std::uint64_t currentRow() const override { return cursor_.row; }
    std::uint64_t firstVisibleRow() const override { return topRow_; }
    void setFirstVisibleRow(std::uint64_t row) override;
    QWidget* widget() override { return this; }

    // ---- configuration ---------------------------------------------------------------------
    // File name or language for syntax highlighting ("main.rs", "Makefile", ...); empty = none.
    void setLanguage(const QString& fileNameOrLanguage);
    void setSyntaxEnabled(bool enabled);
    void setDarkSyntax(bool dark);
    void setPrefetchRows(int rows) { prefetchRows_ = qBound(0, rows, 500); }
    // Upper bound on decoded characters kept in the line cache (default 2 Mi chars).
    void setCacheLimitChars(int chars);
    void setTabWidth(int columns) { tabColumns_ = qBound(1, columns, 16); update(); }
    void setShowLineNumbers(bool show) { showLineNumbers_ = show; updateMetrics(); }

    // ---- queries (also used by the tests) ----------------------------------------------------
    QString selectedText(qint64 maxBytes, bool* truncated = nullptr) const;
    bool hasSelection() const { return !(anchor_ == cursor_); }
    int visibleRowCount() const { return visibleRows_; }
    int cachedLines() const { return lineCache_.count(); }
    int cachedSpanLines() const { return spanCache_.count(); }
    int gutterWidth() const { return gutterWidth_; }
    Side side() const { return side_; }
    const std::shared_ptr<TextModel>& model() const { return model_; }
    // True once the worker finished the highlight request for the current viewport.
    bool highlightIdle() const { return !highlightRunning_; }
    // Syntax spans currently cached for a FILE line (empty when not highlighted yet).
    QVector<StyledSpan> spansForLine(quint64 line) const {
        const QVector<StyledSpan>* s = spanCache_.object(line);
        return s ? *s : QVector<StyledSpan>();
    }
    // Row under a viewport y coordinate, or -1.
    qint64 rowAt(int y) const;
    // Caret position as (row, QString column).
    std::pair<quint64, int> caret() const { return {cursor_.row, cursor_.col}; }
    void setCaret(quint64 row, int col, bool extendSelection = false, bool ensureVisible = true);
    // Make `row` the current row without scrolling and without touching the selection (peer sync).
    void setCurrentRowQuietly(quint64 row);
    int horizontalPixelOffset() const { return hOffset_; }
    void setHorizontalPixelOffset(int px);

    // ---- notifications (set by the owner; always invoked on the GUI thread) ------------------
    std::function<void(quint64 row)> onTopRowChanged;
    std::function<void(int pixels)> onHorizontalOffsetChanged;
    std::function<void(quint64 row)> onCurrentRowChanged;
    std::function<void()> onFocused;
    std::function<void()> onSelectionChanged;
    std::function<void(const QString& text)> onStatusMessage;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void focusInEvent(QFocusEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void changeEvent(QEvent*) override;

private:
    struct Pos {
        quint64 row = 0;
        int col = 0;
        bool operator==(const Pos& o) const { return row == o.row && col == o.col; }
        bool operator<(const Pos& o) const { return row < o.row || (row == o.row && col < o.col); }
    };

    struct LineData {
        QString text;                 // decoded text (whole line, or the window when `windowed`)
        quint64 windowStart = 0;      // first character of `text` within the line
        bool windowed = false;
        bool filler = false;          // this side has no line on this row
        qint64 line = -1;
        ModelRow row;
        QVector<InlineRange> inlineRanges;
        quint32 lineBytes = 0;
    };

    LineData lineData(quint64 row);
    LineData buildLine(quint64 row);
    void updateMetrics();
    void updateScrollbars();
    void setTopRow(quint64 row, bool fromScrollbar = false);
    void ensureRowVisible(quint64 row, bool center = false);
    void ensureColumnVisible(int pixelX);
    QPoint pixelFor(const Pos& p);
    Pos hitTest(const QPoint& viewportPos);
    int lineCharCount(quint64 row);
    int contentWidthPixels() const;
    void paintRow(QPainter& p, quint64 row, int y);
    std::unique_ptr<QTextLayout> makeLayout(const LineData& d) const;
    QString fullLineText(quint64 row);
    qint64 lineForRow(quint64 row) const;
    void moveCaret(Pos next, bool extend);
    void clampPos(Pos& p);
    void requestHighlight();
    void applyHighlight(quint64 requestId, quint64 first, QVector<QVector<StyledSpan>> spans);
    void prefetch();
    void notifySelection();
    void autoScrollTick();
    QColor rowBackground(const LineData& d) const;
    QString wordAt(const Pos& p, int* start, int* end);

    std::shared_ptr<TextModel> model_;
    Side side_ = Side::Left;
    QFont font_;
    int lineHeight_ = 16;
    int charWidth_ = 8;
    int ascent_ = 12;
    int gutterWidth_ = 0;
    int visibleRows_ = 1;
    int tabColumns_ = 4;
    int prefetchRows_ = 24;
    bool showLineNumbers_ = true;

    quint64 topRow_ = 0;
    int hOffset_ = 0;
    int maxContentWidth_ = 0;      // widest line seen so far, in pixels (lower bound, grows)
    ScrollMapper mapper_;
    bool settingScrollbar_ = false;

    Pos cursor_;
    Pos anchor_;
    bool dragging_ = false;
    QPoint lastMouse_;
    QTimer autoScroll_;

    // Search state
    QString findText_;
    bool findCase_ = false;
    bool haveMatch_ = false;
    Pos matchStart_, matchEnd_;
    std::unique_ptr<Cancellation> findCancel_;

    // Caches (bounded)
    QCache<quint64, LineData> lineCache_;
    QCache<quint64, QVector<StyledSpan>> spanCache_;   // keyed by FILE line
    int cacheLimitChars_ = 2 * 1024 * 1024;

    // Syntax
    QString language_;
    bool syntaxEnabled_ = true;
    bool dark_ = true;
    std::shared_ptr<SyntaxSession> syntax_;
    bool syntaxCreating_ = false;
    bool hasWindowedCached_ = false;
    bool contentWidthDirty_ = false;
    quint64 highlightRequestId_ = 0;
    bool highlightRunning_ = false;
    bool highlightDirty_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    static constexpr qint64 kDefaultCopyLimit = 64LL * 1024 * 1024;
};

}  // namespace openbc::app

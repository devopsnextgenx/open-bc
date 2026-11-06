#pragma once

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QCache>
#include <QVector>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>

#include "backend/backend_api.h"
#include "common/diff_algorithms.h"
#include "options/syntax_mappings.h"
#include "viewers/text/engine/text_model.h"

namespace openbc::app {

// Qt owns only document formatting. Parsing and style calculation are backend work.
struct BackendHighlightSpan {
    int line = 0;
    int start = 0;
    int length = 0;
    QColor foreground;
    QColor background;
    bool bold = false;
    bool italic = false;
};

class SyntaxHighlighter : public QSyntaxHighlighter {
public:
    enum class Style { Vibrant, Classic, HighContrast, VsCodeDark };
    explicit SyntaxHighlighter(QTextDocument* document, const QString& sessionId)
        : QSyntaxHighlighter(document), sessionId_(sessionId) {
        // Remember where the text really changed so the stateful lexer only forgets state from there.
        connect(document, &QTextDocument::contentsChange, this, [this](int position, int, int) {
            if (applyingFormats_ || refreshing_) return;
            pendingInvalidateLine_ = qMin(pendingInvalidateLine_, this->document()->findBlock(position).blockNumber());
        });
        connect(document, &QTextDocument::contentsChanged, this, [this]() {
            refreshAndRehighlight();
        });
        // Tools > Options > Syntax Highlighting: a changed extension mapping re-highlights this
        // document right away. Subscribing also makes the store load the profile (and hand it to
        // the backend) before this highlighter asks for its first spans.
        SyntaxMappingStore::instance().addListener(this, [this]() { refreshAndRehighlight(); });
    }

    // Large-file (incremental) mode. Lines are highlighted with a STATEFUL session: the lexer state
    // flows from line to line (multi-line comments and strings stay correct), is checkpointed in Rust,
    // and results live in a bounded cache (kIncrementalCacheLines entries), never in an ever-growing
    // map. The returned reference stays valid until the next call.
    const QVector<BackendHighlightSpan>& incrementalSpansForLine(int line, const QString&) {
        if (const QVector<BackendHighlightSpan>* hit = incrementalCache_.object(line)) {
            lastLineSpans_ = *hit;
            return lastLineSpans_;
        }
        if (!session_) session_ = std::make_unique<SyntaxSession>(extension_, style_ == Style::VsCodeDark);
        const int chunkFirst = line - line % kIncrementalChunk;
        const int count = qMax(1, qMin(kIncrementalChunk, document()->blockCount() - chunkFirst));
        const QVector<QVector<StyledSpan>> lines = session_->highlight(
            static_cast<quint64>(chunkFirst), static_cast<quint64>(count), &SyntaxHighlighter::provideLine, this,
            [this](quint64 n) { return document()->findBlockByNumber(static_cast<int>(n)).text(); });
        for (int i = 0; i < lines.size(); ++i) {
            auto* converted = new QVector<BackendHighlightSpan>();
            converted->reserve(lines[i].size());
            for (const StyledSpan& sp : lines[i]) {
                converted->push_back({chunkFirst + i, sp.start, sp.length,
                                      QColor((sp.foreground >> 24) & 255, (sp.foreground >> 16) & 255,
                                             (sp.foreground >> 8) & 255, 255),
                                      QColor(), sp.bold, sp.italic});
            }
            incrementalCache_.insert(chunkFirst + i, converted, 1);
        }
        const QVector<BackendHighlightSpan>* found = incrementalCache_.object(line);
        lastLineSpans_ = found ? *found : emptySpans_;
        return lastLineSpans_;
    }

    // `language` is whatever identifies the file: its name ("Makefile", "main.rs"), an extension
    // or a language name. The backend resolves it (built-in table first overridden by the user's
    // mappings), so pass the full file name rather than only the suffix.
    void setLanguage(const QString& extension) {
        if (extension_ == extension) return;
        extension_ = extension;
        refreshAndRehighlight();
    }

    void setEnabled(bool enabled) {
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        refreshAndRehighlight();
    }

    void setDeferred(bool deferred) {
        if (deferred_ == deferred) return;
        deferred_ = deferred;
        if (!deferred_ && !incremental_) refreshAndRehighlight();
    }

    // Lines currently held by the bounded incremental span cache (never exceeds kIncrementalCacheLines).
    int incrementalCachedLines() const { return incrementalCache_.count(); }

    void setIncremental(bool incremental) {
        if (incremental_ == incremental) return;
        incremental_ = incremental;
        incrementalCache_.clear();
        session_.reset();
        if (incremental_) spans_.clear();
        else refreshAndRehighlight();
    }

    void clearCachedSpans() {
        incrementalCache_.clear();
        session_.reset();
        decltype(spans_){}.swap(spans_);
        decltype(emptySpans_){}.swap(emptySpans_);
    }

    void rehighlightVisibleBlocks(int first, int last) {
        if (deferred_ || !incremental_ || !enabled_) return;
        for (int line = qMax(0, first); line <= last; ++line) {
            const QTextBlock block = document()->findBlockByNumber(line);
            if (!block.isValid()) continue;
            applyingFormats_ = true;      // Qt reports format-only changes as document changes: ignore them
            rehighlightBlock(block);
            applyingFormats_ = false;
        }
    }

    void setStyle(Style style) {
        if (style_ == style) return;
        style_ = style;
        refreshAndRehighlight();
    }

    void setInlineDiffProvider(std::function<QVector<CharSegment>(int)> provider) {
        inlineDiffProvider_ = std::move(provider);
    }

    void refreshInlineDiffs() {
        if (deferred_) return;
        manualRehighlight_ = true;
        rehighlight();
        manualRehighlight_ = false;
    }

    // True while this highlighter is re-applying formats. Qt reports those
    // format-only changes through QTextDocument::contentsChanged, so owners
    // use this to tell them apart from real text edits (dirty tracking).
    bool isHighlighting() const { return refreshing_ || manualRehighlight_; }

protected:
    void highlightBlock(const QString& text) override {
        if (!enabled_ || deferred_) return;
        const int line = currentBlock().blockNumber();
        // Look only at THIS line's spans. (The old loop scanned every span of the file for every
        // block, which is quadratic, and copied the whole span vector per block.)
        const QVector<BackendHighlightSpan>& source = incremental_ ? incrementalSpansForLine(line, text) : spans_;
        auto it = incremental_ ? source.cbegin()
                               : std::lower_bound(source.cbegin(), source.cend(), line,
                                                  [](const BackendHighlightSpan& s, int l) { return s.line < l; });
        for (; it != source.cend() && it->line == line; ++it) {
            const BackendHighlightSpan& span = *it;
            if (span.start >= text.size()) continue;
            QTextCharFormat format;
            format.setForeground(styledForeground(span.foreground));
            if (span.bold) format.setFontWeight(QFont::Bold);
            if (span.italic) format.setFontItalic(true);
            setFormat(span.start, qMin(span.length, text.size() - span.start), format);
        }

        if (!inlineDiffProvider_) return;
        const QVector<CharSegment> segments = inlineDiffProvider_(line);
        for (const CharSegment& segment : segments) {
            if (segment.kind == CharSegmentKind::Equal) continue;
            QTextCharFormat format;
            if (segment.kind == CharSegmentKind::Mismatch) {
                format.setForeground(QColor(0xff, 0x4d, 0x4d));
                format.setBackground(Qt::NoBrush);
                format.setFontWeight(QFont::Bold);
            } else {
                format.setBackground(QGuiApplication::palette().color(QPalette::Highlight));
                format.setForeground(QGuiApplication::palette().color(QPalette::HighlightedText));
            }
            setFormat(segment.start, segment.length, format);
        }
    }

private:
    void refreshAndRehighlight() {
        if (refreshing_ || deferred_) return;
        if (incremental_) {
            if (applyingFormats_) return;
            // Forget only what the edit can have affected: cached spans (cheap to recompute) and the
            // lexer checkpoints at or after the first changed line.
            incrementalCache_.clear();
            if (session_) session_->invalidateFrom(pendingInvalidateLine_ == std::numeric_limits<int>::max()
                                                        ? 0 : static_cast<quint64>(pendingInvalidateLine_));
            pendingInvalidateLine_ = std::numeric_limits<int>::max();
            return;
        }
        refreshing_ = true;
        refreshBackendSpans();
        rehighlight();
        refreshing_ = false;
    }

    void refreshBackendSpans() {
        spans_.clear();
        const QByteArray extension = extension_.toUtf8();
        // Build the UTF-8 buffer block by block. `toPlainText().toUtf8()` made a full QString copy AND
        // a QByteArray copy; this makes only the one buffer the backend API needs.
        QByteArray source;
        source.reserve(static_cast<int>(qMin<qint64>(document()->characterCount() * 2, std::numeric_limits<int>::max() - 16)));
        for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
            source.append(block.text().toUtf8());
            if (block.next().isValid()) source.append('\n');
        }
        const QByteArray sessionId = sessionId_.toUtf8();
        OpenBcHighlight* handle = style_ == Style::VsCodeDark
            ? openbc_highlight_buffer_with_theme(
                reinterpret_cast<const std::uint8_t*>(extension.constData()), extension.size(),
                reinterpret_cast<const std::uint8_t*>(source.constData()), source.size(), 1,
                reinterpret_cast<const std::uint8_t*>(sessionId.constData()), sessionId.size())
            : openbc_highlight_buffer(
                reinterpret_cast<const std::uint8_t*>(extension.constData()), extension.size(),
                reinterpret_cast<const std::uint8_t*>(source.constData()), source.size(),
                reinterpret_cast<const std::uint8_t*>(sessionId.constData()), sessionId.size());
        if (!handle) return;
        const std::size_t count = openbc_highlight_len(handle);
        spans_.reserve(static_cast<int>(count));
        for (std::size_t i = 0; i < count; ++i) {
            const int line = static_cast<int>(openbc_highlight_line(handle, i));
            const QTextBlock block = document()->findBlockByNumber(line);
            if (!block.isValid()) continue;
            const QByteArray lineBytes = block.text().toUtf8();
            const std::size_t byteStart = openbc_highlight_start(handle, i);
            const std::size_t byteLength = openbc_highlight_length(handle, i);
            BackendHighlightSpan span;
            span.line = line;
            span.start = QString::fromUtf8(lineBytes.constData(), static_cast<int>(byteStart)).size();
            span.length = QString::fromUtf8(lineBytes.constData() + byteStart,
                                            static_cast<int>(byteLength)).size();
            span.foreground = backendColor(handle, i, true);
            span.background = backendColor(handle, i, false);
            span.bold = openbc_highlight_bold(handle, i) != 0;
            span.italic = openbc_highlight_italic(handle, i) != 0;
            spans_.push_back(span);
        }
        openbc_highlight_destroy(handle);
        // highlightBlock() binary-searches by line.
        if (!std::is_sorted(spans_.cbegin(), spans_.cend(), [](const BackendHighlightSpan& a, const BackendHighlightSpan& b) { return a.line < b.line; }))
            std::stable_sort(spans_.begin(), spans_.end(), [](const BackendHighlightSpan& a, const BackendHighlightSpan& b) { return a.line < b.line; });
    }

    static std::int64_t provideLine(void* user, std::uint64_t line, std::uint8_t* buf, std::size_t cap) {
        auto* self = static_cast<SyntaxHighlighter*>(user);
        const QTextBlock block = self->document()->findBlockByNumber(static_cast<int>(line));
        if (!block.isValid()) return -1;
        const QByteArray utf8 = block.text().toUtf8();
        const std::size_t n = qMin<std::size_t>(cap, static_cast<std::size_t>(utf8.size()));
        std::memcpy(buf, utf8.constData(), n);
        return static_cast<std::int64_t>(n);
    }

    QColor styledForeground(const QColor& color) const {
        if (!color.isValid() || style_ == Style::Classic) return color;
        QColor result = color.toHsv();
        const int hue = result.hue() < 0 ? 0 : result.hue();
        if (style_ == Style::Vibrant) {
            result.setHsv(hue, qMax(125, result.saturation()),
                          qBound(80, result.value() + 12, 255));
        } else if (style_ == Style::HighContrast) {
            result.setHsv(hue, 255, result.value() < 145 ? 220 : result.value());
        } else {
            const int lightness = color.lightness();
            if (color.saturation() < 35) {
                return QColor(0xd4, 0xd4, 0xd4);
            }
            if (hue < 35 || hue >= 335) return QColor(0xce, 0x91, 0x78);
            if (hue < 85) return QColor(0xd7, 0xba, 0x7d);
            if (hue < 165) return QColor(0x4e, 0xc9, 0xb0);
            if (hue < 245) return QColor(0x56, 0x9c, 0xd6);
            if (lightness < 90) return QColor(0xc5, 0x86, 0xc0);
            return QColor(0xc5, 0x86, 0xc0);
        }
        return result.toRgb();
    }

    static QColor backendColor(const OpenBcHighlight* handle, std::size_t index, bool foreground) {
        const auto channel = [handle, index, foreground](std::uint8_t value) {
            return foreground ? openbc_highlight_foreground(handle, index, value)
                              : openbc_highlight_background(handle, index, value);
        };
        return QColor(channel(0), channel(1), channel(2), channel(3));
    }

    QString sessionId_;
    QString extension_;
    QVector<BackendHighlightSpan> spans_;
    // Bounded: at most kIncrementalCacheLines lines of spans (QCache evicts least recently used).
    static constexpr int kIncrementalCacheLines = 8192;
    static constexpr int kIncrementalChunk = 64;
    QCache<int, QVector<BackendHighlightSpan>> incrementalCache_{kIncrementalCacheLines};
    std::unique_ptr<SyntaxSession> session_;
    QVector<BackendHighlightSpan> lastLineSpans_;
    int pendingInvalidateLine_ = std::numeric_limits<int>::max();
    bool applyingFormats_ = false;
    QVector<BackendHighlightSpan> emptySpans_;
    std::function<QVector<CharSegment>(int)> inlineDiffProvider_;
    Style style_ = Style::VsCodeDark;
    bool enabled_ = true;
    bool deferred_ = false;
    bool incremental_ = false;
    bool refreshing_ = false;
    bool manualRehighlight_ = false;
};

}  // namespace openbc::app
#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QVector>
#include <functional>

#include "backend_api.h"
#include "diff_algorithms.h"

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
    explicit SyntaxHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
        connect(document, &QTextDocument::contentsChanged, this, [this]() {
            refreshAndRehighlight();
        });
    }

    void setLanguage(const QString& extension) {
        if (extension_ == extension) return;
        extension_ = extension;
        refreshAndRehighlight();
    }

    void setInlineDiffProvider(std::function<QVector<CharSegment>(int)> provider) {
        inlineDiffProvider_ = std::move(provider);
    }

protected:
    void highlightBlock(const QString& text) override {
        const int line = currentBlock().blockNumber();
        for (const BackendHighlightSpan& span : spans_) {
            if (span.line != line || span.start >= text.size()) continue;
            QTextCharFormat format;
            format.setForeground(span.foreground);
            if (span.background.alpha() > 0 && span.background != QColor(0, 0, 0, 255)) {
                format.setBackground(span.background);
            }
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
                format.setForeground(QColor(0xff, 0x6b, 0x6b));
                format.setFontWeight(QFont::Bold);
            } else {
                format.setBackground(QColor(0x3a, 0x4a, 0x63));
                format.setForeground(QColor(0x9d, 0xb4, 0xd1));
            }
            setFormat(segment.start, segment.length, format);
        }
    }

private:
    void refreshAndRehighlight() {
        if (refreshing_) return;
        refreshing_ = true;
        refreshBackendSpans();
        rehighlight();
        refreshing_ = false;
    }

    void refreshBackendSpans() {
        spans_.clear();
        const QByteArray extension = extension_.toUtf8();
        const QByteArray source = document()->toPlainText().toUtf8();
        OpenBcHighlight* handle = openbc_highlight_buffer(
            reinterpret_cast<const std::uint8_t*>(extension.constData()), extension.size(),
            reinterpret_cast<const std::uint8_t*>(source.constData()), source.size());
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
    }

    static QColor backendColor(const OpenBcHighlight* handle, std::size_t index, bool foreground) {
        const auto channel = [handle, index, foreground](std::uint8_t value) {
            return foreground ? openbc_highlight_foreground(handle, index, value)
                              : openbc_highlight_background(handle, index, value);
        };
        return QColor(channel(0), channel(1), channel(2), channel(3));
    }

    QString extension_;
    QVector<BackendHighlightSpan> spans_;
    std::function<QVector<CharSegment>(int)> inlineDiffProvider_;
    bool refreshing_ = false;
};

}  // namespace openbc::app

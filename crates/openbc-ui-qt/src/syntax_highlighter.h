#pragma once

#include <QColor>
#include <QFont>
#include <QGuiApplication>
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
    enum class Style { Vibrant, Classic, HighContrast, VsCodeDark };
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

    void setEnabled(bool enabled) {
        if (enabled_ == enabled) return;
        enabled_ = enabled;
        refreshAndRehighlight();
    }

    void setStyle(Style style) {
        if (style_ == style) return;
        style_ = style;
        refreshAndRehighlight();
    }

    void setInlineDiffProvider(std::function<QVector<CharSegment>(int)> provider) {
        inlineDiffProvider_ = std::move(provider);
    }

protected:
    void highlightBlock(const QString& text) override {
        if (!enabled_) return;
        const int line = currentBlock().blockNumber();
        for (const BackendHighlightSpan& span : spans_) {
            if (span.line != line || span.start >= text.size()) continue;
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
                format.setForeground(QGuiApplication::palette().color(QPalette::BrightText));
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
          OpenBcHighlight* handle = style_ == Style::VsCodeDark
            ? openbc_highlight_buffer_with_theme(
                reinterpret_cast<const std::uint8_t*>(extension.constData()), extension.size(),
                reinterpret_cast<const std::uint8_t*>(source.constData()), source.size(), 1)
            : openbc_highlight_buffer(
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

    QString extension_;
    QVector<BackendHighlightSpan> spans_;
    std::function<QVector<CharSegment>(int)> inlineDiffProvider_;
    Style style_ = Style::VsCodeDark;
    bool enabled_ = true;
    bool refreshing_ = false;
};

}  // namespace openbc::app

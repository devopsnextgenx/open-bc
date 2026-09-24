// diff_algorithms.h
// ---------------------------------------------------------------------------
// Qt-side presentation helpers for backend-produced diff rows:
//   - alignTextLines converts openbc-core rows to Qt values.
//   - computeInlineDiff converts backend UTF-8 spans to QString positions.
//   - groupDiffRows keeps copy arrows and minimap sections presentational.
// ---------------------------------------------------------------------------
#pragma once

#include <QChar>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include "backend_api.h"

namespace openbc::app {

struct TextDiffLine {
    QString left;
    QString right;
    int leftNumber = 0;
    int rightNumber = 0;
    bool changed = false;
    bool whitespaceOnly = false;
};

inline QString normalizedTextLine(const QString& line) {
    QString normalized = line;
    normalized.remove(' ');
    normalized.remove('\t');
    return normalized;
}

inline QVector<TextDiffLine> alignTextLines(const QStringList& left, const QStringList& right) {
    const QByteArray leftBytes = left.join('\n').toUtf8();
    const QByteArray rightBytes = right.join('\n').toUtf8();
    OpenBcDiff* handle = openbc_compare_buffers(
        reinterpret_cast<const std::uint8_t*>(leftBytes.constData()), leftBytes.size(),
        reinterpret_cast<const std::uint8_t*>(rightBytes.constData()), rightBytes.size(), 0, 0, 0,
        0.95);
    QVector<TextDiffLine> result;
    if (!handle) return result;
    const std::size_t count = openbc_diff_len(handle);
    result.reserve(static_cast<int>(count));
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t leftLength = 0;
        std::size_t rightLength = 0;
        const auto* leftText = openbc_diff_left_text(handle, i, &leftLength);
        const auto* rightText = openbc_diff_right_text(handle, i, &rightLength);
        const QString leftLine = QString::fromUtf8(reinterpret_cast<const char*>(leftText),
                                                   static_cast<int>(leftLength));
        const QString rightLine = QString::fromUtf8(reinterpret_cast<const char*>(rightText),
                                                     static_cast<int>(rightLength));
        const int leftNumber = static_cast<int>(openbc_diff_left_line(handle, i));
        const int rightNumber = static_cast<int>(openbc_diff_right_line(handle, i));
        const bool changed = openbc_diff_kind(handle, i) != 0;
        const bool whitespaceOnly = leftNumber > 0 && rightNumber > 0 &&
                                    normalizedTextLine(leftLine) == normalizedTextLine(rightLine);
        result.push_back({leftLine, rightLine, leftNumber, rightNumber, changed, whitespaceOnly});
    }
    openbc_diff_destroy(handle);
    return result;
}

// A maximal run of contiguous changed rows that share the same
// whitespace-only-ness. Each group gets exactly one copy arrow instead of
// one per line, and "trivial" (blank/tab-only) runs are always their own
// group so they never merge with a real content change next to them.
struct DiffGroup {
    int start = 0;
    int end = 0;
    bool whitespaceOnly = false;
};

inline QVector<DiffGroup> groupDiffRows(const QVector<TextDiffLine>& rows) {
    QVector<DiffGroup> groups;
    int i = 0;
    while (i < rows.size()) {
        if (!rows[i].changed) {
            ++i;
            continue;
        }
        const int start = i;
        const bool whitespaceOnly = rows[i].whitespaceOnly;
        while (i < rows.size() && rows[i].changed && rows[i].whitespaceOnly == whitespaceOnly) {
            ++i;
        }
        groups.push_back({start, i - 1, whitespaceOnly});
    }
    return groups;
}

// ---------------------------------------------------------------------------
// Char-level diff within one already-paired row, used to render mismatched
// characters in red and mismatched whitespace runs with a tinted background
// (see LineNumberEditor::setInlineDiffs in line_number_editor.h).
// ---------------------------------------------------------------------------

enum class CharSegmentKind { Equal, Mismatch, WhitespaceMismatch };

struct CharSegment {
    int start = 0;
    int length = 0;
    CharSegmentKind kind = CharSegmentKind::Equal;
};

struct InlineDiff {
    QVector<CharSegment> left;
    QVector<CharSegment> right;
};

inline bool isBlankChar(QChar c) { return c == ' ' || c == '\t'; }

// Common-prefix/common-suffix char diff: fast, and - for the typical case of
// a single token changed in an otherwise-identical line - reads the same as
// a full alignment would, without the O(n*m) cost of one. The unmatched
// middle span is classified as a whitespace-only mismatch when everything in
// it, on both sides, is spaces/tabs (e.g. two spaces vs. one tab), otherwise
// as a real content mismatch.
inline InlineDiff computeInlineDiff(const QString& left, const QString& right) {
    const QByteArray leftBytes = left.toUtf8();
    const QByteArray rightBytes = right.toUtf8();
    OpenBcInline* handle = openbc_inline_diff(
        reinterpret_cast<const std::uint8_t*>(leftBytes.constData()), leftBytes.size(),
        reinterpret_cast<const std::uint8_t*>(rightBytes.constData()), rightBytes.size());
    InlineDiff result;
    if (!handle) return result;
    const auto buildSide = [&](std::uint8_t side, const QByteArray& bytes) {
        QVector<CharSegment> segments;
        const std::size_t count = openbc_inline_len(handle, side);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t byteStart = openbc_inline_start(handle, side, i);
            const std::size_t byteLength = openbc_inline_length(handle, side, i);
            const int start = QString::fromUtf8(bytes.constData(), static_cast<int>(byteStart)).size();
            const int length = QString::fromUtf8(bytes.constData() + byteStart,
                                                 static_cast<int>(byteLength)).size();
            const auto kind = openbc_inline_kind(handle, side, i);
            segments.push_back({start, length, kind == 2 ? CharSegmentKind::WhitespaceMismatch
                                                         : kind == 1 ? CharSegmentKind::Mismatch
                                                                     : CharSegmentKind::Equal});
        }
        return segments;
    };
    result.left = buildSide(0, leftBytes);
    result.right = buildSide(1, rightBytes);
    openbc_inline_destroy(handle);
    return result;
}

}  // namespace openbc::app

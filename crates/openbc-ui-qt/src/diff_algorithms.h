// diff_algorithms.h
// ---------------------------------------------------------------------------
// Pure, UI-free line-diff and char-diff algorithms shared by the minimap,
// the line-number gutters and TextCompareView itself:
//   - alignTextLines/groupDiffRows: the existing line-level LCS alignment.
//   - computeInlineDiff: a lightweight char-level diff (common prefix/suffix)
//     used to paint mismatched characters in red and mismatched runs of
//     spaces/tabs with the VS Code-style whitespace markers.
// ---------------------------------------------------------------------------
#pragma once

#include <QChar>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

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
    QVector<TextDiffLine> result;
    const int leftCount = left.size();
    const int rightCount = right.size();
    QVector<QVector<int>> common(leftCount + 1, QVector<int>(rightCount + 1));
    for (int leftIndex = leftCount - 1; leftIndex >= 0; --leftIndex) {
        for (int rightIndex = rightCount - 1; rightIndex >= 0; --rightIndex) {
            common[leftIndex][rightIndex] =
                left[leftIndex] == right[rightIndex]
                    ? common[leftIndex + 1][rightIndex + 1] + 1
                    : qMax(common[leftIndex + 1][rightIndex], common[leftIndex][rightIndex + 1]);
        }
    }

    int leftIndex = 0;
    int rightIndex = 0;
    while (leftIndex < leftCount || rightIndex < rightCount) {
        if (leftIndex < leftCount && rightIndex < rightCount &&
            left[leftIndex] == right[rightIndex]) {
            result.push_back({left[leftIndex], right[rightIndex], leftIndex + 1, rightIndex + 1,
                              false, false});
            ++leftIndex;
            ++rightIndex;
            continue;
        }

        // Find the next exact anchor and align the changed block before it as
        // a pair of editor rows. This keeps insertions from shifting every
        // later matching line onto the wrong row.
        int anchorLeft = leftCount;
        int anchorRight = rightCount;
        for (int candidateLeft = leftIndex; candidateLeft < leftCount; ++candidateLeft) {
            for (int candidateRight = rightIndex; candidateRight < rightCount; ++candidateRight) {
                if (left[candidateLeft] == right[candidateRight] &&
                    common[candidateLeft][candidateRight] == common[leftIndex][rightIndex]) {
                    if (candidateLeft + candidateRight < anchorLeft + anchorRight) {
                        anchorLeft = candidateLeft;
                        anchorRight = candidateRight;
                    }
                }
            }
        }
        const int blockLength = qMax(anchorLeft - leftIndex, anchorRight - rightIndex);
        for (int offset = 0; offset < blockLength; ++offset) {
            const bool hasLeft = leftIndex + offset < anchorLeft;
            const bool hasRight = rightIndex + offset < anchorRight;
            const QString leftText = hasLeft ? left[leftIndex + offset] : QString();
            const QString rightText = hasRight ? right[rightIndex + offset] : QString();
            const bool whitespaceOnly = hasLeft && hasRight &&
                                        normalizedTextLine(leftText) == normalizedTextLine(rightText);
            result.push_back({leftText, rightText, hasLeft ? leftIndex + offset + 1 : 0,
                              hasRight ? rightIndex + offset + 1 : 0, true, whitespaceOnly});
        }
        leftIndex = anchorLeft;
        rightIndex = anchorRight;
    }
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
    InlineDiff diff;
    const int leftLen = left.size();
    const int rightLen = right.size();
    int prefix = 0;
    while (prefix < leftLen && prefix < rightLen && left[prefix] == right[prefix]) {
        ++prefix;
    }
    int suffix = 0;
    while (suffix < leftLen - prefix && suffix < rightLen - prefix &&
           left[leftLen - 1 - suffix] == right[rightLen - 1 - suffix]) {
        ++suffix;
    }
    const int leftMidLen = qMax(0, leftLen - prefix - suffix);
    const int rightMidLen = qMax(0, rightLen - prefix - suffix);
    const auto isBlankRange = [](const QString& text, int start, int len) {
        for (int i = start; i < start + len; ++i) {
            if (!isBlankChar(text[i])) return false;
        }
        return true;
    };
    const bool whitespaceMismatch =
        isBlankRange(left, prefix, leftMidLen) && isBlankRange(right, prefix, rightMidLen);
    const auto buildSide = [&](int midLen) {
        QVector<CharSegment> segments;
        if (prefix > 0) segments.push_back({0, prefix, CharSegmentKind::Equal});
        if (midLen > 0) {
            segments.push_back({prefix, midLen,
                                whitespaceMismatch ? CharSegmentKind::WhitespaceMismatch
                                                    : CharSegmentKind::Mismatch});
        }
        if (suffix > 0) segments.push_back({prefix + midLen, suffix, CharSegmentKind::Equal});
        return segments;
    };
    diff.left = buildSide(leftMidLen);
    diff.right = buildSide(rightMidLen);
    return diff;
}

}  // namespace openbc::app

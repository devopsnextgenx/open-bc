//! The span type shared by every highlighting backend.

/// A syntax-highlighted source span using UTF-8 byte offsets.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HighlightSpan {
    /// Zero-based source line.
    pub line: usize,
    /// UTF-8 byte offset within `line`.
    pub start: usize,
    /// UTF-8 byte length within `line`.
    pub length: usize,
    /// RGBA foreground color.
    pub foreground: [u8; 4],
    /// RGBA background color.
    pub background: [u8; 4],
    /// Whether the theme marks this span bold.
    pub bold: bool,
    /// Whether the theme marks this span italic.
    pub italic: bool,
}

impl HighlightSpan {
    /// True when `next` directly continues this span with an identical foreground and style.
    fn continues_with(&self, next: &HighlightSpan) -> bool {
        self.line == next.line
            && self.start + self.length == next.start
            && self.foreground == next.foreground
            && self.bold == next.bold
            && self.italic == next.italic
    }
}

/// Coalesces neighbouring spans that render identically.
pub(super) fn merge_adjacent_spans(spans: Vec<HighlightSpan>) -> Vec<HighlightSpan> {
    let mut merged: Vec<HighlightSpan> = Vec::with_capacity(spans.len());
    for span in spans {
        if let Some(previous) = merged.last_mut() {
            if previous.continues_with(&span) {
                previous.length += span.length;
                continue;
            }
        }
        merged.push(span);
    }
    merged
}

#[cfg(test)]
mod tests {
    use super::*;

    fn span(line: usize, start: usize, length: usize, fg: u8) -> HighlightSpan {
        HighlightSpan {
            line,
            start,
            length,
            foreground: [fg, 0, 0, 255],
            background: [0; 4],
            bold: false,
            italic: false,
        }
    }

    #[test]
    fn merges_only_adjacent_identical_spans() {
        let merged = merge_adjacent_spans(vec![
            span(0, 0, 2, 1),
            span(0, 2, 3, 1), // adjacent, same style: merged
            span(0, 5, 1, 2), // different colour
            span(0, 7, 1, 2), // gap
            span(1, 8, 1, 2), // new line
        ]);
        assert_eq!(merged, vec![span(0, 0, 5, 1), span(0, 5, 1, 2), span(0, 7, 1, 2), span(1, 8, 1, 2)]);
    }
}

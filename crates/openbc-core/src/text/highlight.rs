//! Backend-owned syntax highlighting based on Tree-like source scopes.

use std::path::Path;

use syntect::easy::HighlightLines;
use syntect::highlighting::{FontStyle, Style, ThemeSet};
use syntect::parsing::SyntaxSet;

/// A color and font treatment produced by the syntax backend.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HighlightStyle {
    /// RGBA foreground color.
    pub foreground: [u8; 4],
    /// RGBA background color, when the backend supplies one.
    pub background: [u8; 4],
    /// Whether the span should use a bold font.
    pub bold: bool,
    /// Whether the span should use an italic font.
    pub italic: bool,
}

/// A byte range in one source line with its backend-computed style.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HighlightSpan {
    /// Zero-based source line number.
    pub line: usize,
    /// Zero-based UTF-8 byte offset within the line.
    pub start: usize,
    /// Span length in UTF-8 bytes.
    pub length: usize,
    /// Style to apply to the span.
    pub style: HighlightStyle,
}

/// Syntax highlighting engine shared by UI frontends.
pub struct HighlightEngine {
    syntax_set: SyntaxSet,
    themes: ThemeSet,
}

impl Default for HighlightEngine {
    fn default() -> Self {
        Self {
            syntax_set: SyntaxSet::load_defaults_newlines(),
            themes: ThemeSet::load_defaults(),
        }
    }
}

impl HighlightEngine {
    /// Highlight a UTF-8 buffer using the syntax selected from `extension`.
    #[must_use]
    pub fn highlight_buffer(&self, extension: &str, source: &str) -> Vec<HighlightSpan> {
        let filename = if extension.starts_with('.') {
            format!("source{extension}")
        } else {
            format!("source.{extension}")
        };
        let Some(syntax) = self
            .syntax_set
            .find_syntax_for_file(Path::new(&filename))
            .ok()
            .flatten()
        else {
            return Vec::new();
        };
        let theme = self
            .themes
            .themes
            .get("base16-ocean.dark")
            .or_else(|| self.themes.themes.values().next());
        let Some(theme) = theme else {
            return Vec::new();
        };

        let mut highlighter = HighlightLines::new(syntax, theme);
        let mut spans = Vec::new();
        for (line_number, line) in source.split_inclusive('\n').enumerate() {
            let content = line.strip_suffix('\n').unwrap_or(line);
            let Ok(ranges) = highlighter.highlight_line(line, &self.syntax_set) else {
                continue;
            };
            let mut offset = 0;
            for (style, text) in ranges {
                let length = text.len().min(content.len().saturating_sub(offset));
                if length > 0 {
                    spans.push(HighlightSpan {
                        line: line_number,
                        start: offset,
                        length,
                        style: style_to_backend(style),
                    });
                }
                offset += text.len();
            }
        }
        if source.is_empty() || !source.ends_with('\n') {
            // split_inclusive already includes the final non-newline line.
        }
        spans
    }
}

fn style_to_backend(style: Style) -> HighlightStyle {
    HighlightStyle {
        foreground: [
            style.foreground.r,
            style.foreground.g,
            style.foreground.b,
            style.foreground.a,
        ],
        background: [
            style.background.r,
            style.background.g,
            style.background.b,
            style.background.a,
        ],
        bold: style.font_style.contains(FontStyle::BOLD),
        italic: style.font_style.contains(FontStyle::ITALIC),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selects_syntax_by_extension_and_returns_spans() {
        let engine = HighlightEngine::default();
        let spans = engine.highlight_buffer("rs", "fn main() { let answer = 42; }\n");
        assert!(!spans.is_empty());
        assert!(spans.iter().any(|span| span.line == 0 && span.length > 0));
    }

    #[test]
    fn unknown_extensions_are_plain() {
        let engine = HighlightEngine::default();
        assert!(engine
            .highlight_buffer("unknown-openbc-format", "plain text")
            .is_empty());
    }
}

//! Backend-owned syntax highlighting based on Tree-like source scopes.

use std::path::Path;
use std::str::FromStr;

use syntect::easy::HighlightLines;
use syntect::highlighting::{
    Color, FontStyle, ScopeSelectors, Style, StyleModifier, Theme, ThemeItem, ThemeSettings,
    ThemeSet,
};
use syntect::parsing::SyntaxSet;

/// Theme used to calculate syntax colors.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HighlightTheme {
    /// The bundled dark theme used by the original highlighter.
    Base16OceanDark,
    /// A VS Code Dark-inspired palette applied to syntax scopes.
    VsCodeDark,
}

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
        self.highlight_buffer_with_theme(extension, source, HighlightTheme::Base16OceanDark)
    }

    /// Highlight a UTF-8 buffer with a selected syntax theme.
    #[must_use]
    pub fn highlight_buffer_with_theme(
        &self,
        extension: &str,
        source: &str,
        theme_kind: HighlightTheme,
    ) -> Vec<HighlightSpan> {
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
        let custom_theme;
        let theme = match theme_kind {
            HighlightTheme::Base16OceanDark => self
                .themes
                .themes
                .get("base16-ocean.dark")
                .or_else(|| self.themes.themes.values().next()),
            HighlightTheme::VsCodeDark => {
                custom_theme = vs_code_dark_theme();
                Some(&custom_theme)
            }
        };
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
        // Syntax spans are foreground-only. Diff backgrounds are owned by Qt.
        background: [0, 0, 0, 0],
        bold: style.font_style.contains(FontStyle::BOLD),
        italic: style.font_style.contains(FontStyle::ITALIC),
    }
}

fn vs_code_dark_theme() -> Theme {
    let foreground = color(0xd4, 0xd4, 0xd4);
    let mut theme = Theme {
        name: Some("VS Code Dark".to_string()),
        settings: ThemeSettings {
            foreground: Some(foreground),
            background: Some(color(0x1e, 0x1e, 0x1e)),
            ..ThemeSettings::default()
        },
        ..Theme::default()
    };

    theme.scopes = [
        ("", foreground, FontStyle::empty()),
        ("comment", color(0x6a, 0x99, 0x55), FontStyle::ITALIC),
        ("string", color(0xce, 0x91, 0x78), FontStyle::empty()),
        ("constant.numeric", color(0xb5, 0xce, 0xa8), FontStyle::empty()),
        (
            "keyword, storage, entity.name.tag",
            color(0x56, 0x9c, 0xd6),
            FontStyle::BOLD,
        ),
        (
            "entity.name.function, support.function",
            color(0xdc, 0xdc, 0xaa),
            FontStyle::empty(),
        ),
        (
            "entity.name.type, support.type, storage.type",
            color(0x4e, 0xc9, 0xb0),
            FontStyle::empty(),
        ),
        ("variable.language", color(0x9c, 0xdc, 0xfe), FontStyle::empty()),
        ("constant.language", color(0x56, 0x9c, 0xd6), FontStyle::empty()),
    ]
    .into_iter()
    .filter_map(|(scope, foreground, font_style)| {
        Some(ThemeItem {
            scope: ScopeSelectors::from_str(scope).ok()?,
            style: StyleModifier {
                foreground: Some(foreground),
                font_style: Some(font_style),
                ..StyleModifier::default()
            },
        })
    })
    .collect();
    theme
}

const fn color(r: u8, g: u8, b: u8) -> Color {
    Color { r, g, b, a: 0xff }
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

    #[test]
    fn syntax_spans_are_foreground_only() {
        let engine = HighlightEngine::default();
        let spans = engine.highlight_buffer("rs", "fn main() {}\n");
        assert!(spans.iter().all(|span| span.style.background == [0, 0, 0, 0]));
    }

    #[test]
    fn vscode_dark_theme_produces_colored_and_strong_spans() {
        let engine = HighlightEngine::default();
        let spans = engine.highlight_buffer_with_theme(
            "rs",
            "fn main() { let answer = 42; }\n",
            HighlightTheme::VsCodeDark,
        );
        assert!(spans.iter().any(|span| span.style.bold));
        assert!(spans
            .iter()
            .any(|span| span.style.foreground != [0xd4, 0xd4, 0xd4, 0xff]));
    }
}

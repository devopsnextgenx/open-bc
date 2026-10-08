//! `syntect` backend (C-family, shell and markup languages).

use std::sync::OnceLock;

use syntect::easy::HighlightLines;
use syntect::highlighting::{FontStyle, Style, Theme, ThemeSet};
use syntect::html::highlighted_html_for_string;
use syntect::parsing::{SyntaxReference, SyntaxSet};
use syntect::util::LinesWithEndings;

use super::html::plain_html;
use super::language;
use super::spans::{merge_adjacent_spans, HighlightSpan};

const DARK_THEME: &str = "base16-ocean.dark";
const LIGHT_THEME: &str = "InspiredGitHub";

fn syntax_set() -> &'static SyntaxSet {
    static SET: OnceLock<SyntaxSet> = OnceLock::new();
    SET.get_or_init(SyntaxSet::load_defaults_newlines)
}

fn theme_set() -> &'static ThemeSet {
    static SET: OnceLock<ThemeSet> = OnceLock::new();
    SET.get_or_init(ThemeSet::load_defaults)
}

/// Syntax definition and theme chosen for one request.
struct Resolved {
    syntax_set: &'static SyntaxSet,
    syntax: &'static SyntaxReference,
    theme: &'static Theme,
}

/// Looks up the syntax (by extension, then by token) and the light/dark theme.
fn resolve(language: &str, is_dark_mode: bool) -> Option<Resolved> {
    let syntax_set = syntax_set();
    let syntax = syntax_set
        .find_syntax_by_extension(&language::normalize(language))
        .or_else(|| syntax_set.find_syntax_by_token(language.trim()))?;
    let theme_name = if is_dark_mode { DARK_THEME } else { LIGHT_THEME };
    let theme = theme_set().themes.get(theme_name)?;
    Some(Resolved { syntax_set, syntax, theme })
}

/// Self-contained HTML; falls back to escaped plain text when the language or theme is missing.
pub(super) fn highlight_html(source: &str, language: &str, is_dark_mode: bool) -> String {
    resolve(language, is_dark_mode)
        .and_then(|r| highlighted_html_for_string(source, r.syntax_set, r.syntax, r.theme).ok())
        .unwrap_or_else(|| plain_html(source))
}

/// Per-token spans; empty when the language or theme is missing.
pub(super) fn highlight_spans(source: &str, language: &str, is_dark_mode: bool) -> Vec<HighlightSpan> {
    let Some(resolved) = resolve(language, is_dark_mode) else {
        return Vec::new();
    };
    let mut highlighter = HighlightLines::new(resolved.syntax, resolved.theme);
    let mut spans = Vec::new();
    for (line, text) in LinesWithEndings::from(source).enumerate() {
        let Ok(ranges) = highlighter.highlight_line(text, resolved.syntax_set) else {
            continue;
        };
        let mut start = 0;
        for (style, token) in ranges {
            let token = token.trim_end_matches(['\n', '\r']);
            if !token.is_empty() {
                spans.push(span_for(line, start, token.len(), style));
            }
            start += token.len();
        }
    }
    merge_adjacent_spans(spans)
}

fn span_for(line: usize, start: usize, length: usize, style: Style) -> HighlightSpan {
    let rgba = |c: syntect::highlighting::Color| [c.r, c.g, c.b, c.a];
    HighlightSpan {
        line,
        start,
        length,
        foreground: rgba(style.foreground),
        background: rgba(style.background),
        bold: style.font_style.contains(FontStyle::BOLD),
        italic: style.font_style.contains(FontStyle::ITALIC),
    }
}

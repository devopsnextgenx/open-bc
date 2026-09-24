//! Backend-owned HTML syntax highlighting using tree-sitter and Helix themes.

use std::path::PathBuf;
use std::sync::OnceLock;
use syntect::easy::HighlightLines;
use syntect::highlighting::{FontStyle, ThemeSet};
use syntect::parsing::SyntaxSet;
use syntect::util::LinesWithEndings;

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

/// Highlight source code as a self-contained HTML fragment.
#[must_use]
pub fn highlight_to_html(source: &str, language: &str, is_dark_mode: bool) -> String {
    let Some((body, css)) = render_source(source, language, is_dark_mode) else {
        return plain_html(source);
    };
    format!("<style>{css}</style><pre class=\"tsc-bg\">{body}</pre>")
}

/// Highlight source code for native editors that apply `QTextCharFormat` spans.
#[must_use]
pub fn highlight_to_spans(source: &str, language: &str, is_dark_mode: bool) -> Vec<HighlightSpan> {
    if uses_syntect(language) {
        return syntect_spans(source, language, is_dark_mode);
    }
    let Some((body, css)) = render_source(source, language, is_dark_mode) else {
        return Vec::new();
    };
    parse_rendered_spans(&body, &css)
}

fn uses_syntect(language: &str) -> bool {
    matches!(
        language.trim().trim_start_matches('.').to_ascii_lowercase().as_str(),
        "c++" | "cpp" | "cc" | "cxx" | "java" | "sh" | "bash" | "shell"
    )
}

fn syntect_spans(source: &str, language: &str, is_dark_mode: bool) -> Vec<HighlightSpan> {
    static SYNTAX_SET: OnceLock<SyntaxSet> = OnceLock::new();
    static THEME_SET: OnceLock<ThemeSet> = OnceLock::new();
    let syntax_set = SYNTAX_SET.get_or_init(SyntaxSet::load_defaults_newlines);
    let normalized = language.trim().trim_start_matches('.').to_ascii_lowercase();
    let Some(syntax) = syntax_set
        .find_syntax_by_extension(&normalized)
        .or_else(|| syntax_set.find_syntax_by_token(language.trim()))
    else {
        return Vec::new();
    };
    let theme_set = THEME_SET.get_or_init(ThemeSet::load_defaults);
    let theme_name = if is_dark_mode {
        "base16-ocean.dark"
    } else {
        "InspiredGitHub"
    };
    let Some(theme) = theme_set.themes.get(theme_name) else {
        return Vec::new();
    };
    let mut highlighter = HighlightLines::new(syntax, theme);
    let mut spans = Vec::new();
    for (line, text) in LinesWithEndings::from(source).enumerate() {
        let Ok(ranges) = highlighter.highlight_line(text, &syntax_set) else {
            continue;
        };
        let mut start = 0;
        for (style, token) in ranges {
            let token = token.trim_end_matches(['\n', '\r']);
            if !token.is_empty() {
                spans.push(HighlightSpan {
                    line,
                    start,
                    length: token.len(),
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
                });
            }
            start += token.len();
        }
    }
    merge_adjacent_spans(spans)
}

fn render_source(source: &str, language: &str, is_dark_mode: bool) -> Option<(String, String)> {
    let lang = language_path(language).and_then(tree_painter::Lang::from)?;
    let theme_source = if is_dark_mode {
        tree_painter::themes::CATPPUCCIN_MOCHA
    } else {
        tree_painter::themes::CATPPUCCIN_LATTE
    };
    let theme = tree_painter::Theme::from_helix(theme_source).ok()?;

    let mut renderer = tree_painter::Renderer::new(theme);
    let lines = renderer.render(&lang, source.as_bytes()).ok()?;
    let body = lines.collect::<String>();
    Some((body, renderer.css()))
}

#[derive(Clone, Copy)]
struct SpanStyle {
    foreground: [u8; 4],
    bold: bool,
    italic: bool,
}

fn parse_rendered_spans(body: &str, css: &str) -> Vec<HighlightSpan> {
    let styles = parse_styles(css);
    let bytes = body.as_bytes();
    let mut spans = Vec::new();
    let mut cursor = 0;
    let mut line = 0;
    let mut line_start = 0;
    let mut active: Option<SpanStyle> = None;

    while cursor < bytes.len() {
        if bytes[cursor] == b'<' {
            if let Some(end) = body[cursor..].find('>') {
                let tag_end = cursor + end + 1;
                let tag = &body[cursor..tag_end];
                if tag.starts_with("<span class=\"tsc-") {
                    let class_start = cursor + "<span class=\"".len();
                    let class_end = body[class_start..].find('"').map(|index| class_start + index);
                    active = class_end.and_then(|end| styles.get(&body[class_start..end]).copied());
                } else if tag == "</span>" {
                    active = None;
                }
                cursor = tag_end;
                continue;
            }
        }

        let (text, consumed) = if bytes[cursor] == b'&' {
            decode_entity(&body[cursor..]).unwrap_or_else(|| ("&".to_string(), 1))
        } else {
            let character = body[cursor..].chars().next().unwrap_or('\0');
            (character.to_string(), character.len_utf8())
        };
        if text == "\n" {
            line += 1;
            line_start = cursor + consumed;
        } else if let Some(style) = active {
            spans.push(HighlightSpan {
                line,
                start: source_byte_offset(body, line_start, cursor),
                length: text.len(),
                foreground: style.foreground,
                background: [0, 0, 0, 0],
                bold: style.bold,
                italic: style.italic,
            });
        }
        cursor += consumed;
    }
    merge_adjacent_spans(spans)
}

fn source_byte_offset(body: &str, line_start: usize, html_offset: usize) -> usize {
    let mut offset = 0;
    let mut cursor = line_start;
    while cursor < html_offset {
        if body.as_bytes()[cursor] == b'<' {
            if let Some(end) = body[cursor..].find('>') {
                cursor += end + 1;
                continue;
            }
        }
        if body.as_bytes()[cursor] == b'&' {
            if let Some((text, consumed)) = decode_entity(&body[cursor..]) {
                offset += text.len();
                cursor += consumed;
                continue;
            }
        }
        let character = body[cursor..].chars().next().unwrap_or('\0');
        offset += character.len_utf8();
        cursor += character.len_utf8();
    }
    offset
}

fn parse_styles(css: &str) -> std::collections::HashMap<String, SpanStyle> {
    let mut styles = std::collections::HashMap::new();
    for rule in css.split('}') {
        let Some((selector, declarations)) = rule.split_once('{') else { continue };
        let Some(class) = selector.trim().strip_prefix('.') else { continue };
        let Some(color) = declarations.split("color:").nth(1).and_then(|value| value.split(';').next())
            .and_then(parse_hex_color)
        else { continue };
        styles.insert(
            class.to_string(),
            SpanStyle {
                foreground: color,
                bold: declarations.contains("font-weight: bold"),
                italic: declarations.contains("font-style: italic"),
            },
        );
    }
    styles
}

fn parse_hex_color(value: &str) -> Option<[u8; 4]> {
    let value = value.trim().strip_prefix('#')?;
    if value.len() != 6 { return None; }
    Some([
        u8::from_str_radix(&value[0..2], 16).ok()?,
        u8::from_str_radix(&value[2..4], 16).ok()?,
        u8::from_str_radix(&value[4..6], 16).ok()?,
        255,
    ])
}

fn decode_entity(value: &str) -> Option<(String, usize)> {
    for (entity, decoded) in [("&amp;", "&"), ("&lt;", "<"), ("&gt;", ">"), ("&quot;", "\""), ("&#39;", "'")] {
        if value.starts_with(entity) { return Some((decoded.to_string(), entity.len())); }
    }
    None
}

fn merge_adjacent_spans(mut spans: Vec<HighlightSpan>) -> Vec<HighlightSpan> {
    let mut merged: Vec<HighlightSpan> = Vec::with_capacity(spans.len());
    for span in spans.drain(..) {
        if let Some(previous) = merged.last_mut() {
            if previous.line == span.line
                && previous.start + previous.length == span.start
                && previous.foreground == span.foreground
                && previous.bold == span.bold
                && previous.italic == span.italic
            {
                previous.length += span.length;
                continue;
            }
        }
        merged.push(span);
    }
    merged
}

fn language_path(language: &str) -> Option<PathBuf> {
    let normalized = language.trim().trim_start_matches('.').to_ascii_lowercase();
    let extension = match normalized.as_str() {
        "rust" => "rs",
        "javascript" => "js",
        "c++" => "cpp",
        "python" => "py",
        value if !value.is_empty() => value,
        _ => return None,
    };
    Some(PathBuf::from(format!("source.{extension}")))
}

fn plain_html(source: &str) -> String {
    format!("<pre>{}</pre>", escape_html(source))
}

fn escape_html(source: &str) -> String {
    source
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

#[cfg(test)]
mod tests {
    use super::{highlight_to_html, highlight_to_spans};

    #[test]
    fn renders_rust_with_a_dark_helix_theme() {
        let html = highlight_to_html("fn main() { let answer = 42; }", "rust", true);
        assert!(html.contains("<style>"));
        assert!(html.contains("tsc-keyword"));
        assert!(html.contains("tsc-bg"));

        let spans = highlight_to_spans("fn main() {}", "rust", true);
        assert!(!spans.is_empty());
        assert_eq!(spans[0].start, 0);
        assert_eq!(spans[0].length, 2);
    }

    #[test]
    fn renders_light_theme_and_escapes_unknown_languages() {
        let html = highlight_to_html("<plain>", "unknown", false);
        assert_eq!(html, "<pre>&lt;plain&gt;</pre>");
    }

    #[test]
    fn renders_cpp_shell_and_java_spans() {
        for (language, source) in [
            ("cpp", "int main() { return 0; }"),
            ("sh", "#!/bin/sh\necho \"hello\""),
            ("java", "class Main { public static void main(String[] args) {} }"),
        ] {
            assert!(!highlight_to_spans(source, language, true).is_empty(), "{language}");
        }
    }
}

//! `tree-painter` backend (tree-sitter grammars with Helix themes).
//!
//! The renderer emits HTML plus a stylesheet; native editors need spans, so
//! [`Rendered::to_spans`] parses that HTML back into byte-offset ranges.

use std::collections::HashMap;

use super::spans::{merge_adjacent_spans, HighlightSpan};

/// Rendered HTML body (`<span class="tsc-...">` per token) and its stylesheet.
pub(super) struct Rendered {
    body: String,
    css: String,
}

/// Renders `source` with the grammar tree-painter associates with `extension` (`rs`, `py`, `js`);
/// `None` when it has no such grammar or the theme fails to load.
pub(super) fn render(source: &str, extension: &str, is_dark_mode: bool) -> Option<Rendered> {
    let lang = tree_painter::Lang::from(format!("source.{extension}"))?;
    let theme_source = if is_dark_mode {
        tree_painter::themes::CATPPUCCIN_MOCHA
    } else {
        tree_painter::themes::CATPPUCCIN_LATTE
    };
    let theme = tree_painter::Theme::from_helix(theme_source).ok()?;

    let mut renderer = tree_painter::Renderer::new(theme);
    let lines = renderer.render(&lang, source.as_bytes()).ok()?;
    let body = lines.collect::<String>();
    Some(Rendered { body, css: renderer.css() })
}

impl Rendered {
    /// Self-contained HTML fragment (stylesheet plus `<pre>`).
    pub(super) fn to_html(&self) -> String {
        format!("<style>{}</style><pre class=\"tsc-bg\">{}</pre>", self.css, self.body)
    }

    /// Foreground-only spans with UTF-8 byte offsets in the *original* source.
    pub(super) fn to_spans(&self) -> Vec<HighlightSpan> {
        let styles = parse_styles(&self.css);
        let body = self.body.as_str();
        let mut spans = Vec::new();
        let mut cursor = 0;
        let (mut line, mut column) = (0, 0);
        let mut active: Option<SpanStyle> = None;

        while cursor < body.len() {
            if body.as_bytes()[cursor] == b'<' {
                if let Some(end) = body[cursor..].find('>') {
                    let tag_end = cursor + end + 1;
                    active = style_after_tag(&body[cursor..tag_end], &styles, active);
                    cursor = tag_end;
                    continue;
                }
            }
            let (character, consumed) = next_character(&body[cursor..]);
            if character == '\n' {
                line += 1;
                column = 0;
            } else {
                let length = character.len_utf8();
                if let Some(style) = active {
                    spans.push(HighlightSpan {
                        line,
                        start: column,
                        length,
                        foreground: style.foreground,
                        background: [0, 0, 0, 0],
                        bold: style.bold,
                        italic: style.italic,
                    });
                }
                column += length;
            }
            cursor += consumed;
        }
        merge_adjacent_spans(spans)
    }
}

#[derive(Clone, Copy)]
struct SpanStyle {
    foreground: [u8; 4],
    bold: bool,
    italic: bool,
}

/// The style in effect after `tag`: opening `tsc-*` spans select one, `</span>` clears it.
fn style_after_tag(
    tag: &str,
    styles: &HashMap<String, SpanStyle>,
    current: Option<SpanStyle>,
) -> Option<SpanStyle> {
    if let Some(rest) = tag.strip_prefix("<span class=\"") {
        if rest.starts_with("tsc-") {
            // An unknown or unterminated class clears the style.
            return rest.find('"').and_then(|end| styles.get(&rest[..end]).copied());
        }
    }
    if tag == "</span>" {
        None
    } else {
        current
    }
}

/// Maps `.class { color: #rrggbb; ... }` rules to span styles.
fn parse_styles(css: &str) -> HashMap<String, SpanStyle> {
    let mut styles = HashMap::new();
    for rule in css.split('}') {
        let Some((selector, declarations)) = rule.split_once('{') else {
            continue;
        };
        let Some(class) = selector.trim().strip_prefix('.') else {
            continue;
        };
        let Some(color) = declarations
            .split("color:")
            .nth(1)
            .and_then(|value| value.split(';').next())
            .and_then(parse_hex_color)
        else {
            continue;
        };
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
    if value.len() != 6 {
        return None;
    }
    Some([
        u8::from_str_radix(&value[0..2], 16).ok()?,
        u8::from_str_radix(&value[2..4], 16).ok()?,
        u8::from_str_radix(&value[4..6], 16).ok()?,
        255,
    ])
}

/// Decodes one of the HTML entities the renderer emits.
fn decode_entity(value: &str) -> Option<(char, usize)> {
    const ENTITIES: [(&str, char); 5] =
        [("&amp;", '&'), ("&lt;", '<'), ("&gt;", '>'), ("&quot;", '"'), ("&#39;", '\'')];
    ENTITIES
        .iter()
        .find(|&&(entity, _)| value.starts_with(entity))
        .map(|&(entity, decoded)| (decoded, entity.len()))
}

/// The next source character at the start of `rest` and how many HTML bytes encode it.
fn next_character(rest: &str) -> (char, usize) {
    decode_entity(rest).unwrap_or_else(|| {
        let character = rest.chars().next().unwrap_or('\0');
        (character, character.len_utf8())
    })
}

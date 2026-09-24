//! Backend-owned HTML syntax highlighting using tree-sitter and Helix themes.

use std::path::PathBuf;

/// Highlight source code as a self-contained HTML fragment.
#[must_use]
pub fn highlight_to_html(source: &str, language: &str, is_dark_mode: bool) -> String {
    let Some(lang) = language_path(language).and_then(tree_painter::Lang::from) else {
        return plain_html(source);
    };

    let theme_source = if is_dark_mode {
        tree_painter::themes::CATPPUCCIN_MOCHA
    } else {
        tree_painter::themes::CATPPUCCIN_LATTE
    };
    let Ok(theme) = tree_painter::Theme::from_helix(theme_source) else {
        return plain_html(source);
    };

    let mut renderer = tree_painter::Renderer::new(theme);
    let Ok(lines) = renderer.render(&lang, source.as_bytes()) else {
        return plain_html(source);
    };
    let body = lines.collect::<String>();
    format!(
        "<style>{}</style><pre class=\"tsc-bg\">{}</pre>",
        renderer.css(),
        body
    )
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
    use super::highlight_to_html;

    #[test]
    fn renders_rust_with_a_dark_helix_theme() {
        let html = highlight_to_html("fn main() { let answer = 42; }", "rust", true);
        assert!(html.contains("<style>"));
        assert!(html.contains("tsc-keyword"));
        assert!(html.contains("tsc-bg"));
    }

    #[test]
    fn renders_light_theme_and_escapes_unknown_languages() {
        let html = highlight_to_html("<plain>", "unknown", false);
        assert_eq!(html, "<pre>&lt;plain&gt;</pre>");
    }
}

//! Backend-owned syntax highlighting as HTML or native-editor spans.
//!
//! Two backends share one front door: `syntect` handles C-family, shell and markup
//! languages, `tree-painter` (tree-sitter grammars with Helix themes) everything else.
//!
//! | Submodule             | Role                                                     |
//! |-----------------------|----------------------------------------------------------|
//! | `language`            | name normalisation and backend routing                   |
//! | `syntect_backend`     | syntect HTML and spans                                   |
//! | `tree_painter_backend`| tree-painter HTML, and spans recovered from its HTML     |
//! | `spans`               | [`HighlightSpan`] and span merging                       |
//! | `html`                | escaped plain-text fallback                              |

mod html;
mod language;
mod spans;
mod syntect_backend;
mod tree_painter_backend;

pub use spans::HighlightSpan;

use std::time::Instant;

use crate::telemetry::{self, Sample};

/// Reports one highlighting call to the observability sink.
fn record(operation: &'static str, started: Instant, source: &str, language: &str) {
    telemetry::record(
        operation,
        started,
        Sample {
            sizes: Some((source.len(), 0)),
            lines: Some((source.lines().count(), 0)),
            detail: Some(language.to_owned()),
            ..Sample::default()
        },
    );
}

/// Highlight source code as a self-contained HTML fragment.
#[must_use]
pub fn highlight_to_html(source: &str, language: &str, is_dark_mode: bool) -> String {
    let started = Instant::now();
    let result = if language::uses_syntect(language) {
        syntect_backend::highlight_html(source, language, is_dark_mode)
    } else {
        match tree_painter_backend::render(source, language, is_dark_mode) {
            Some(rendered) => rendered.to_html(),
            None => return html::plain_html(source),
        }
    };
    record("syntax_highlighting_html", started, source, language);
    result
}

/// Highlight source code for native editors that apply `QTextCharFormat` spans.
#[must_use]
pub fn highlight_to_spans(source: &str, language: &str, is_dark_mode: bool) -> Vec<HighlightSpan> {
    let started = Instant::now();
    let result = if language::uses_syntect(language) {
        syntect_backend::highlight_spans(source, language, is_dark_mode)
    } else {
        match tree_painter_backend::render(source, language, is_dark_mode) {
            Some(rendered) => rendered.to_spans(),
            None => return Vec::new(),
        }
    };
    record("syntax_highlighting_spans", started, source, language);
    result
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
            (
                "java",
                "class Main { public static void main(String[] args) {} }",
            ),
        ] {
            assert!(
                !highlight_to_spans(source, language, true).is_empty(),
                "{language}"
            );
        }
    }

    #[test]
    fn renders_json_yaml_xml_and_html_spans() {
        for (language, source) in [
            ("json", r#"{"name": "OpenBC", "enabled": true}"#),
            ("yaml", "name: OpenBC\nenabled: true"),
            ("yml", "name: OpenBC\nenabled: true"),
            ("xml", "<item enabled=\"true\">OpenBC</item>"),
            ("html", "<main><h1>OpenBC</h1></main>"),
        ] {
            assert!(
                !highlight_to_spans(source, language, true).is_empty(),
                "{language}"
            );
        }
    }

    #[test]
    fn renders_json_yaml_xml_and_html_html() {
        for (language, source) in [
            ("json", r#"{"name": "OpenBC"}"#),
            ("yaml", "name: OpenBC"),
            ("xml", "<item>OpenBC</item>"),
            ("html", "<main>OpenBC</main>"),
        ] {
            let html = highlight_to_html(source, language, true);
            assert!(html.contains("<span"), "{language}");
        }
    }
}

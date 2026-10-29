//! Backend-owned syntax highlighting as HTML or native-editor spans.
//!
//! Two backends share one front door: `tree-painter` (tree-sitter grammars with Helix themes)
//! renders Rust, Python and JavaScript, `syntect` everything else it has a grammar for. Which
//! extension or file name selects which grammar is decided by the `registry` table; users can
//! override it at runtime through [`set_user_mappings`].
//!
//! | Submodule             | Role                                                     |
//! |-----------------------|----------------------------------------------------------|
//! | `registry`            | the language / extension / grammar table                 |
//! | `language`            | routing, user overrides, catalog export                  |
//! | `syntect_backend`     | syntect HTML and spans                                   |
//! | `tree_painter_backend`| tree-painter HTML, and spans recovered from its HTML     |
//! | `spans`               | [`HighlightSpan`] and span merging                       |
//! | `html`                | escaped plain-text fallback                              |

mod html;
mod language;
mod registry;
mod spans;
mod syntect_backend;
mod tree_painter_backend;

pub use language::{
    language_catalog, language_catalog_text, normalize_user_key, plain_text_language, set_user_mappings,
    set_user_mappings_text, LanguageInfo,
};
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
    let result = match language::route(language) {
        language::Route::Syntect(syntax) => syntect_backend::highlight_html(source, syntax, is_dark_mode),
        language::Route::TreePainter(extension) => {
            match tree_painter_backend::render(source, extension, is_dark_mode) {
                Some(rendered) => rendered.to_html(),
                None => return html::plain_html(source),
            }
        }
        language::Route::Plain => return html::plain_html(source),
    };
    record("syntax_highlighting_html", started, source, language);
    result
}

/// Highlight source code for native editors that apply `QTextCharFormat` spans.
#[must_use]
pub fn highlight_to_spans(source: &str, language: &str, is_dark_mode: bool) -> Vec<HighlightSpan> {
    let started = Instant::now();
    let result = match language::route(language) {
        language::Route::Syntect(syntax) => syntect_backend::highlight_spans(source, syntax, is_dark_mode),
        language::Route::TreePainter(extension) => {
            match tree_painter_backend::render(source, extension, is_dark_mode) {
                Some(rendered) => rendered.to_spans(),
                None => return Vec::new(),
            }
        }
        language::Route::Plain => return Vec::new(),
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

    /// Sample source per grammar, used to prove that every configured grammar really highlights.
    fn sample(grammar: &str) -> &'static str {
        match grammar {
            "C" | "C++" | "Objective-C" | "Objective-C++" | "C#" | "Java" | "Go" | "D" | "Scala" | "Groovy" => {
                "// comment\nint main() { return 42; }\nclass A { String s = \"text\"; }"
            }
            "Rust" => "// comment\nfn main() { let x = 42; }",
            "JavaScript" | "JavaScript (Rails)" | "ActionScript" => "// c\nfunction f() { return \"s\" + 42; }",
            "py" | "Python" => "# c\ndef f():\n    return \"s\" + 42",
            "rs" => "fn main() { let x = 42; }",
            "js" => "function f() { return \"s\" + 42; }",
            "Ruby" | "Ruby on Rails" | "Ruby Haml" => "# c\ndef f\n  puts \"s\" + 42.to_s\nend",
            "JSON" => r#"{"a": [1, true, "s"]}"#,
            "YAML" => "# c\nkey: value\nlist:\n  - 1\n",
            "XML" | "HTML" | "HTML (ASP)" | "ASP" | "HTML (Rails)" | "HTML (Erlang)" | "Java Server Page (JSP)" => {
                "<!-- c --><main class=\"a\"><h1>Title</h1></main>"
            }
            "CSS" => "/* c */\nbody { color: #fff; margin: 0 }",
            "Markdown" => "# Title\n\nSome *emphasis* and `code`.\n",
            "reStructuredText" => "Title\n=====\n\nSome *emphasis* and ``code``.\n",
            "Textile" => "h1. Title\n\nSome *strong* text.\n",
            "LaTeX" | "TeX" => "\\documentclass{article}\n% c\n\\begin{document}x\\end{document}\n",
            "BibTeX" => "@article{key, title = {T}, year = 2020}\n",
            "Diff" => "--- a\n+++ b\n@@ -1 +1 @@\n-old\n+new\n",
            "Graphviz (DOT)" => "digraph G { a -> b; }\n",
            "Bourne Again Shell (bash)" => "#!/bin/sh\n# c\necho \"hi $USER\"\n",
            "Batch File" => "@echo off\nREM c\nset X=1\n",
            "Makefile" => "# c\nall: main.o\n\t$(CC) -o app main.o\n",
            "Perl" => "# c\nmy $x = \"s\";\nprint $x;\n",
            "PHP" => "<?php\n// c\n$x = \"s\";\necho $x;\n",
            "Lua" => "-- c\nlocal x = \"s\"\nprint(x)\n",
            "R" => "# c\nx <- c(1, 2)\nprint(\"s\")\n",
            "Rd (R Documentation)" => "% c\n\\name{x}\n\\title{T}\n",
            "MATLAB" => "% c\nx = [1 2 3];\ndisp('s')\n",
            "Tcl" => "# c\nset x \"s\"\nputs $x\n",
            "AppleScript" => "-- c\ntell application \"Finder\" to activate\n",
            "Regular Expression" => "^(a|b)+\\d{2}$",
            "Haskell" | "Literate Haskell" => "-- c\nmain :: IO ()\nmain = putStrLn \"s\"\n",
            "OCaml" | "OCamllex" | "OCamlyacc" => "(* c *)\nlet x = \"s\" in print_string x\n",
            "Erlang" => "% c\n-module(m).\nf() -> \"s\".\n",
            "Clojure" => "; c\n(defn f [x] (str \"s\" x))\n",
            "Lisp" => "; c\n(defun f (x) (list \"s\" x))\n",
            "SQL" | "SQL (Rails)" => "-- c\nSELECT a, 1 FROM t WHERE b = 'x';\n",
            "Pascal" => "{ c }\nprogram P;\nbegin writeln('s'); end.\n",
            "Java Properties" => "# c\nkey=value\nother = 42\n",
            other => panic!("add a sample for grammar {other:?}"),
        }
    }

    #[test]
    fn every_configured_grammar_exists_and_highlights() {
        use super::language::Route;
        use super::registry::{Backend, LANGUAGES};
        let mut checked = std::collections::BTreeSet::new();
        for language in LANGUAGES {
            let grammar = language.grammar;
            if grammar.backend == Backend::Plain {
                assert!(highlight_to_spans("x = 1", language.name, true).is_empty(), "{}", language.name);
                continue;
            }
            // Resolving through the public entry point proves the extension reaches the grammar.
            let ext = language.extensions.first().copied().unwrap_or_else(|| language.filenames[0]);
            assert_eq!(
                super::language::route(ext),
                match grammar.backend {
                    Backend::Syntect => Route::Syntect(grammar.key),
                    _ => Route::TreePainter(grammar.key),
                },
                "{} via {ext}",
                language.name
            );
            if checked.insert((grammar.backend as u8, grammar.key)) {
                let source = sample(grammar.key);
                let spans = highlight_to_spans(source, ext, true);
                assert!(!spans.is_empty(), "grammar {:?} ({}) produced no spans", grammar.key, language.name);
                let html = highlight_to_html(source, ext, false);
                assert!(html.contains("<span"), "grammar {:?} ({}) produced no HTML spans", grammar.key, language.name);
            }
        }
        assert!(checked.len() > 40, "{} distinct grammars checked", checked.len());
    }

    #[test]
    fn registry_covers_every_extension_the_engines_know() {
        use super::registry::LANGUAGES;
        // Deliberately not mapped: the registry assigns these to a different language or drops
        // them (see the conventions in registry.rs). Each entry needs a reason.
        const SKIPPED: &[&str] = &[
            "s",          // assembly, not S-PLUS
            "build",      // NAnt; meson.build is matched by file name instead
            "inc",        // PHP/assembly/Pascal include: too ambiguous
            "cgi", "fcgi", // Ruby CGI scripts: rarely Ruby
            "fasl",       // compiled Lisp, binary
            "script editor", "htc", "mud", "ss", // legacy or already covered by another key
        ];
        // Resolve through the real router. syntect lists some whole file names (`Makefile`) as extensions.
        let known = |ext: &str| {
            super::language::is_known(ext) || super::language::is_known(&format!("x.{ext}"))
        };
        let set = syntect::parsing::SyntaxSet::load_defaults_newlines();
        let mut missing = Vec::new();
        for syntax in set.syntaxes() {
            for ext in &syntax.file_extensions {
                if !known(ext) && !SKIPPED.contains(&ext.as_str()) {
                    missing.push(format!("{} ({})", ext, syntax.name));
                }
            }
        }
        assert!(missing.is_empty(), "syntect knows these extensions the registry does not: {missing:?}");
        // tree-painter's own extension list
        for ext in ["c", "lisp", "lsp", "l", "cl", "cpp", "cc", "cxx", "cu", "js", "py", "rs"] {
            assert!(known(ext), "{ext}");
        }
        // Every syntect syntax name the registry points at must exist.
        for language in LANGUAGES.iter().filter(|l| l.grammar.backend == super::registry::Backend::Syntect) {
            assert!(set.find_syntax_by_name(language.grammar.key).is_some(), "{} -> {}", language.name, language.grammar.key);
        }
        // tree-painter grammars the registry points at must exist.
        for language in LANGUAGES.iter().filter(|l| l.grammar.backend == super::registry::Backend::TreePainter) {
            assert!(tree_painter::Lang::from(format!("source.{}", language.grammar.key)).is_some(), "{}", language.name);
        }
    }

    #[test]
    fn user_mapping_changes_what_a_file_renders_as() {
        let ext = "obc-highlight-test";
        let source = "fn main() { let x = 42; }";
        assert!(highlight_to_spans(source, &format!("a.{ext}"), true).is_empty());
        assert_eq!(super::set_user_mappings([(ext, "Rust")]), 1);
        assert!(!highlight_to_spans(source, &format!("a.{ext}"), true).is_empty());
        assert!(highlight_to_html(source, &format!("a.{ext}"), true).contains("<span"));
        super::set_user_mappings(Vec::<(&str, &str)>::new());
        assert!(highlight_to_spans(source, &format!("a.{ext}"), true).is_empty());
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

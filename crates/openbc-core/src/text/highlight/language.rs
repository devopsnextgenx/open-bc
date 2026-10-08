//! Language-name handling shared by the highlighting backends.

use std::path::PathBuf;

/// Languages rendered by `syntect`; everything else goes to `tree-painter`.
const SYNTECT_LANGUAGES: &[&str] = &[
    "c", "c++", "cpp", "cc", "cxx", "java", "sh", "bash", "shell", "json", "yaml", "yml", "xml",
    "html", "htm",
];

/// Lower-cased language name or extension without surrounding whitespace or leading dot.
pub(super) fn normalize(language: &str) -> String {
    language.trim().trim_start_matches('.').to_ascii_lowercase()
}

/// Whether `language` is handled by the `syntect` backend.
pub(super) fn uses_syntect(language: &str) -> bool {
    SYNTECT_LANGUAGES.contains(&normalize(language).as_str())
}

/// The pseudo file name `tree-painter` uses to pick a grammar (`source.<ext>`).
pub(super) fn tree_painter_path(language: &str) -> Option<PathBuf> {
    let normalized = normalize(language);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn routes_languages_to_backends() {
        assert!(uses_syntect(" .JSON "));
        assert!(uses_syntect("c++"));
        assert!(!uses_syntect("rust"));
        assert_eq!(normalize(" .Rs"), "rs");
    }

    #[test]
    fn maps_names_to_tree_painter_paths() {
        assert_eq!(tree_painter_path("Rust"), Some(PathBuf::from("source.rs")));
        assert_eq!(tree_painter_path(".py"), Some(PathBuf::from("source.py")));
        assert_eq!(tree_painter_path("  "), None);
    }
}

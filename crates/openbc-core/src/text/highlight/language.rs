//! Language routing shared by the highlighting backends.
//!
//! One question is answered here: "which grammar renders this?". The input is whatever the caller
//! has: a file name (`Makefile`, `main.rs`, `view.html.erb`), a bare extension (`rs`, `.py`) or a
//! language name (`rust`, `C++`). The answer is a [`Route`].
//!
//! # Resolution order
//!
//! For a file name the candidates run from the most to the least specific: the whole name
//! (`foo.test.ts`), then each suffix after a dot (`test.ts`, `ts`). The first candidate that
//! matches wins, and at one candidate the user's own mapping beats the built-in table. When no
//! candidate matches, the whole token is tried as a language name or alias.
//!
//! User mappings are process-wide and replaced as a whole ([`set_user_mappings`]); every later
//! highlight call sees them, so open documents only have to ask for a fresh highlight.

use std::collections::HashMap;
use std::sync::{OnceLock, RwLock};

use super::registry::{Backend, Grammar, Language, LANGUAGES, PLAIN_TEXT};

/// Which backend renders a request, and with which grammar.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Route {
    /// syntect, by exact syntax name.
    Syntect(&'static str),
    /// tree-painter, by the extension it recognises.
    TreePainter(&'static str),
    /// No highlighting.
    Plain,
}

impl From<Grammar> for Route {
    fn from(grammar: Grammar) -> Self {
        match grammar.backend {
            Backend::Syntect => Route::Syntect(grammar.key),
            Backend::TreePainter => Route::TreePainter(grammar.key),
            Backend::Plain => Route::Plain,
        }
    }
}

/// Lookup tables over [`LANGUAGES`], built once.
struct Index {
    by_extension: HashMap<&'static str, usize>,
    by_filename: HashMap<&'static str, usize>,
    by_name: HashMap<String, usize>,
}

fn index() -> &'static Index {
    static INDEX: OnceLock<Index> = OnceLock::new();
    INDEX.get_or_init(|| {
        let mut index = Index {
            by_extension: HashMap::new(),
            by_filename: HashMap::new(),
            by_name: HashMap::new(),
        };
        for (i, language) in LANGUAGES.iter().enumerate() {
            for extension in language.extensions {
                index.by_extension.insert(extension, i);
            }
            for filename in language.filenames {
                index.by_filename.insert(filename, i);
            }
            index.by_name.insert(language.name.to_ascii_lowercase(), i);
            for alias in language.aliases {
                index.by_name.insert(alias.to_ascii_lowercase(), i);
            }
        }
        index
    })
}

/// User key (lower-case extension or file name) to an index into [`LANGUAGES`].
type UserMap = HashMap<String, usize>;

fn user_map() -> &'static RwLock<UserMap> {
    static MAP: OnceLock<RwLock<UserMap>> = OnceLock::new();
    MAP.get_or_init(|| RwLock::new(HashMap::new()))
}

/// Candidate keys for a file name, most specific first: `a.test.ts` gives `a.test.ts`, `test.ts`,
/// `ts`. One leading dot is ignored so `.bashrc` is looked up as `bashrc`.
fn candidates(name: &str) -> impl Iterator<Item = &str> {
    let name = name.strip_prefix('.').unwrap_or(name);
    std::iter::once(name)
        .chain(name.match_indices('.').map(move |(i, _)| &name[i + 1..]))
        .filter(|candidate| !candidate.is_empty())
}

/// The language for `token`, consulting `user` before the built-in tables.
fn lookup(user: &UserMap, token: &str) -> Option<usize> {
    let lowered = token.trim().to_ascii_lowercase();
    let name = lowered.rsplit(['/', '\\']).next().unwrap_or_default();
    let index = index();
    for (position, candidate) in candidates(name).enumerate() {
        if let Some(&i) = user.get(candidate) {
            return Some(i);
        }
        // Whole file names only match as the complete name (position 0).
        if position == 0 {
            if let Some(&i) = index.by_filename.get(candidate) {
                return Some(i);
            }
        }
        if let Some(&i) = index.by_extension.get(candidate) {
            return Some(i);
        }
    }
    index.by_name.get(name).copied()
}

/// Resolves `token` to a grammar route; unknown tokens render as plain text.
pub(super) fn route(token: &str) -> Route {
    let user = user_map().read().unwrap_or_else(|poisoned| poisoned.into_inner());
    lookup(&user, token).map_or(Route::Plain, |i| LANGUAGES[i].grammar.into())
}

/// The built-in language a name or alias refers to.
fn language_by_name(name: &str) -> Option<usize> {
    index().by_name.get(name.trim().to_ascii_lowercase().as_str()).copied()
}

/// Whether the built-in tables know `token` at all (test support for the coverage check).
#[cfg(test)]
pub(super) fn is_known(token: &str) -> bool {
    lookup(&UserMap::new(), token).is_some()
}

/// Normalises a user-entered extension or file name: trimmed, lower case, one leading `*.` or `.`
/// removed. `None` when nothing usable is left or when it contains whitespace, a path separator
/// or a wildcard (patterns are not supported).
pub fn normalize_user_key(key: &str) -> Option<String> {
    let key = key.trim().to_ascii_lowercase();
    let key = key.strip_prefix("*.").or_else(|| key.strip_prefix('.')).unwrap_or(&key);
    let valid = !key.is_empty()
        && !key.chars().any(|c| c.is_whitespace() || c.is_control() || "/\\*?:\"<>|".contains(c));
    valid.then(|| key.to_owned())
}

/// Replaces every user mapping. Each pair is (extension or file name, language name or alias).
/// Pairs with an unusable key or an unknown language are skipped. Returns how many were applied.
pub fn set_user_mappings<I, K, L>(pairs: I) -> usize
where
    I: IntoIterator<Item = (K, L)>,
    K: AsRef<str>,
    L: AsRef<str>,
{
    let mut next = UserMap::new();
    for (key, language) in pairs {
        if let (Some(key), Some(i)) = (normalize_user_key(key.as_ref()), language_by_name(language.as_ref())) {
            next.insert(key, i);
        }
    }
    let applied = next.len();
    *user_map().write().unwrap_or_else(|poisoned| poisoned.into_inner()) = next;
    applied
}

/// [`set_user_mappings`] from text: one `key<TAB>language` pair per line.
pub fn set_user_mappings_text(text: &str) -> usize {
    set_user_mappings(text.lines().filter_map(|line| line.split_once('\t')))
}

/// A language as listed in the Options dialog.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LanguageInfo {
    pub name: &'static str,
    pub aliases: &'static [&'static str],
    pub extensions: &'static [&'static str],
    pub filenames: &'static [&'static str],
    /// `syntect`, `tree-painter` or `plain`.
    pub engine: &'static str,
    /// Rendered with the grammar of a related language.
    pub approximate: bool,
}

impl From<&'static Language> for LanguageInfo {
    fn from(language: &'static Language) -> Self {
        Self {
            name: language.name,
            aliases: language.aliases,
            extensions: language.extensions,
            filenames: language.filenames,
            engine: match language.grammar.backend {
                Backend::Syntect => "syntect",
                Backend::TreePainter => "tree-painter",
                Backend::Plain => "plain",
            },
            approximate: language.grammar.approximate,
        }
    }
}

/// Every built-in language, in table order.
#[must_use]
pub fn language_catalog() -> Vec<LanguageInfo> {
    LANGUAGES.iter().map(LanguageInfo::from).collect()
}

/// The catalog as tab-separated lines for the Qt layer:
///
/// ```text
/// L <name> <engine> <exact|approximate> <aliases, comma separated>
/// E <extension> <language name>
/// F <file name> <language name>
/// ```
#[must_use]
pub fn language_catalog_text() -> String {
    let mut out = String::new();
    for info in language_catalog() {
        let fidelity = if info.approximate { "approximate" } else { "exact" };
        out.push_str(&format!("L\t{}\t{}\t{}\t{}\n", info.name, info.engine, fidelity, info.aliases.join(",")));
    }
    for info in language_catalog() {
        for extension in info.extensions {
            out.push_str(&format!("E\t{extension}\t{}\n", info.name));
        }
        for filename in info.filenames {
            out.push_str(&format!("F\t{filename}\t{}\n", info.name));
        }
    }
    out
}

/// Name of the language that switches highlighting off.
#[must_use]
pub fn plain_text_language() -> &'static str {
    PLAIN_TEXT
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn resolve(user: &[(&str, &str)], token: &str) -> Route {
        let map: UserMap = user
            .iter()
            .filter_map(|(k, l)| Some((normalize_user_key(k)?, language_by_name(l)?)))
            .collect();
        lookup(&map, token).map_or(Route::Plain, |i| LANGUAGES[i].grammar.into())
    }

    #[test]
    fn normalizes_extensions() {
        assert_eq!(normalize_user_key(" *.Foo "), Some("foo".into()));
        assert_eq!(normalize_user_key(".d.ts"), Some("d.ts".into()));
        assert_eq!(normalize_user_key("Jenkinsfile"), Some("jenkinsfile".into()));
        for bad in ["", " ", ".", "a b", "dir/file", "*", "a*", "a?b", "c:d"] {
            assert_eq!(normalize_user_key(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn routes_extensions_names_and_file_names() {
        assert_eq!(resolve(&[], "main.rs"), Route::TreePainter("rs"));
        assert_eq!(resolve(&[], "rust"), Route::TreePainter("rs"));
        assert_eq!(resolve(&[], " .JSON "), Route::Syntect("JSON"));
        assert_eq!(resolve(&[], "c++"), Route::Syntect("C++"));
        assert_eq!(resolve(&[], "C#"), Route::Syntect("C#"));
        assert_eq!(resolve(&[], "include/widget.h"), Route::Syntect("C++"));
        assert_eq!(resolve(&[], "Makefile"), Route::Syntect("Makefile"));
        assert_eq!(resolve(&[], "C:\\src\\.bashrc"), Route::Syntect("Bourne Again Shell (bash)"));
        assert_eq!(resolve(&[], "CMakeLists.txt"), Route::Syntect("Bourne Again Shell (bash)"));
        assert_eq!(resolve(&[], "notes.txt"), Route::Plain);
        assert_eq!(resolve(&[], "unknown"), Route::Plain);
        assert_eq!(resolve(&[], ""), Route::Plain);
    }

    #[test]
    fn picks_the_most_specific_suffix() {
        assert_eq!(resolve(&[], "views/index.html.erb"), Route::Syntect("HTML (Rails)"));
        assert_eq!(resolve(&[], "app.js.erb"), Route::Syntect("JavaScript (Rails)"));
        assert_eq!(resolve(&[], "archive.tar.rs"), Route::TreePainter("rs"));
    }

    #[test]
    fn user_mappings_beat_built_ins_and_add_new_extensions() {
        assert_eq!(resolve(&[("h", "C")], "x.h"), Route::Syntect("C"));
        assert_eq!(resolve(&[("*.myext", "python")], "a.MyExt"), Route::TreePainter("py"));
        assert_eq!(resolve(&[("Jenkinsfile", "Groovy"), ("log", "Rust")], "Jenkinsfile"), Route::Syntect("Groovy"));
        assert_eq!(resolve(&[("rs", "Plain Text")], "x.rs"), Route::Plain);
        // an unknown language is ignored, not guessed
        assert_eq!(resolve(&[("h", "NoSuchLanguage")], "x.h"), Route::Syntect("C++"));
        // a more specific user key beats a less specific built-in one
        assert_eq!(resolve(&[("test.rs", "Python")], "a.test.rs"), Route::TreePainter("py"));
    }

    #[test]
    fn global_mappings_are_replaced_as_a_whole() {
        let key = "obc-unit-test-ext";
        assert_eq!(route(key), Route::Plain);
        assert_eq!(set_user_mappings_text(&format!("{key}\tPython\nbad line\n.x y\tGo\nzzz\tNoSuch\n")), 1);
        assert_eq!(route(&format!("f.{key}")), Route::TreePainter("py"));
        assert_eq!(set_user_mappings(Vec::<(String, String)>::new()), 0);
        assert_eq!(route(&format!("f.{key}")), Route::Plain);
    }

    #[test]
    fn table_has_unique_lowercase_keys_and_names() {
        let (mut exts, mut files, mut names): (HashSet<&str>, HashSet<&str>, HashSet<String>) = Default::default();
        for language in LANGUAGES {
            for e in language.extensions {
                assert_eq!(*e, e.to_ascii_lowercase(), "extension {e} must be lower case");
                assert!(!e.starts_with('.') && !e.is_empty(), "extension {e:?}");
                assert!(exts.insert(e), "extension {e} is claimed twice ({})", language.name);
            }
            for f in language.filenames {
                assert_eq!(*f, f.to_ascii_lowercase(), "file name {f} must be lower case");
                assert!(files.insert(f), "file name {f} is claimed twice ({})", language.name);
            }
            for n in std::iter::once(&language.name).chain(language.aliases) {
                assert!(names.insert(n.to_ascii_lowercase()), "language name {n} is used twice");
            }
        }
        assert!(LANGUAGES.len() > 100 && exts.len() > 500, "{} languages, {} extensions", LANGUAGES.len(), exts.len());
        assert!(language_by_name(PLAIN_TEXT).is_some());
    }

    #[test]
    fn catalog_text_lists_every_language_and_key() {
        let text = language_catalog_text();
        assert!(text.lines().any(|l| l == "L\tRust\ttree-painter\texact\trs"));
        assert!(text.lines().any(|l| l == "E\th\tC++"));
        assert!(text.lines().any(|l| l == "F\tmakefile\tMakefile"));
        assert_eq!(text.lines().filter(|l| l.starts_with("L\t")).count(), LANGUAGES.len());
        assert!(text.lines().all(|l| l.split('\t').count() >= 3));
    }
}

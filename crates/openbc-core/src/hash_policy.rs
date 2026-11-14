//! Per-file-type content matching policy for folder compare.
//!
//! A [`MatchPolicy`] answers one question: "how should two files with this name be judged equal?"
//! Either by *size + timestamp* (cheap, ideal for text and source code where a rebuild rarely
//! changes bytes without changing the timestamp) or by a *content hash* with a chosen
//! [`HashAlgorithm`] (ideal for binaries, where timestamps churn on every build).
//!
//! The module is pure: it only classifies. Hashing itself lives in `openbc-compute`.
//!
//! # Serialized form
//!
//! The policy round-trips through one line of text so it can be stored in `preferences.ini` and
//! passed over the C ABI unchanged:
//!
//! ```text
//! default=md5;text:rs,c,h=size;binary:exe,dll,so=md5;iso=sha256
//! ```
//!
//! `default=<method>` is the fallback, every other entry is `[name:]ext1,ext2,...=<method>`.
//! The optional `name:` is the display name of the group shown in Options (it may not contain
//! `; = , :`); without it the name is derived from the built-in groups, or `custom`.
//! `<method>` is `size` (size + timestamp) or an algorithm name (`md5`, `sha1`, `sha256`,
//! `sha512`).

use std::fmt;

/// Hash algorithms selectable for content comparison.
///
/// MD5 and SHA-1 are offered because they are fast and the comparison is not adversarial: two
/// files are being checked for *accidental* difference, not authenticated.
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq, PartialOrd, Ord)]
pub enum HashAlgorithm {
    /// 128-bit MD5. The default: fastest widely available digest.
    #[default]
    Md5,
    /// 160-bit SHA-1.
    Sha1,
    /// 256-bit SHA-256.
    Sha256,
    /// 512-bit SHA-512.
    Sha512,
}

impl HashAlgorithm {
    /// Every algorithm, in display order.
    pub const ALL: [HashAlgorithm; 4] = [Self::Md5, Self::Sha1, Self::Sha256, Self::Sha512];

    /// Lower-case identifier used in the serialized policy.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            Self::Md5 => "md5",
            Self::Sha1 => "sha1",
            Self::Sha256 => "sha256",
            Self::Sha512 => "sha512",
        }
    }

    /// Human-readable label for UI lists.
    #[must_use]
    pub const fn label(self) -> &'static str {
        match self {
            Self::Md5 => "MD5",
            Self::Sha1 => "SHA-1",
            Self::Sha256 => "SHA-256",
            Self::Sha512 => "SHA-512",
        }
    }

    /// Digest length in bytes.
    #[must_use]
    pub const fn digest_len(self) -> usize {
        match self {
            Self::Md5 => 16,
            Self::Sha1 => 20,
            Self::Sha256 => 32,
            Self::Sha512 => 64,
        }
    }

    /// Parse an identifier (case-insensitive, `-` ignored: `SHA-256` == `sha256`).
    #[must_use]
    pub fn parse(text: &str) -> Option<Self> {
        let norm: String =
            text.trim().chars().filter(|c| *c != '-').flat_map(char::to_lowercase).collect();
        Self::ALL.into_iter().find(|a| a.id() == norm)
    }
}

impl fmt::Display for HashAlgorithm {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.label())
    }
}

/// How two same-named files are judged equal.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum MatchMethod {
    /// Compare size and modification time only; no file bytes are read.
    SizeTimestamp,
    /// Compare a content digest computed with the given algorithm.
    Hash(HashAlgorithm),
}

impl Default for MatchMethod {
    fn default() -> Self {
        Self::Hash(HashAlgorithm::Md5)
    }
}

impl MatchMethod {
    /// Serialized identifier: `size` or an algorithm id.
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            Self::SizeTimestamp => "size",
            Self::Hash(a) => a.id(),
        }
    }

    /// Parse `size` / `timestamp` / an algorithm id.
    #[must_use]
    pub fn parse(text: &str) -> Option<Self> {
        match text.trim().to_ascii_lowercase().as_str() {
            "size" | "timestamp" | "size/timestamp" | "size+timestamp" => Some(Self::SizeTimestamp),
            other => HashAlgorithm::parse(other).map(Self::Hash),
        }
    }

    /// `true` when the method needs the file bytes.
    #[must_use]
    pub const fn reads_content(self) -> bool {
        matches!(self, Self::Hash(_))
    }
}

/// One user-editable rule: a named group of extensions and the method used for them.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FileTypeRule {
    /// Display name of the group (`binary`, `text`, ...). Free text, shown in Options.
    pub name: String,
    /// Lower-case extensions without the dot (`exe`, `so`). Matching is case-insensitive.
    pub extensions: Vec<String>,
    /// Method used for files whose extension is listed.
    pub method: MatchMethod,
}

impl FileTypeRule {
    /// Build a rule, normalizing extensions (trim, strip leading `.`/`*.`, lower-case, dedupe).
    #[must_use]
    pub fn new(name: &str, extensions: &[&str], method: MatchMethod) -> Self {
        Self {
            name: name.to_owned(),
            extensions: normalize_extensions(extensions.iter().copied()),
            method,
        }
    }
}

/// Normalize a list of extensions: trim, strip `*.` / `.`, lower-case, drop empties and duplicates.
pub fn normalize_extensions<'a>(items: impl IntoIterator<Item = &'a str>) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for raw in items {
        let ext = raw.trim().trim_start_matches('*').trim_start_matches('.').to_lowercase();
        if !ext.is_empty() && !out.contains(&ext) {
            out.push(ext);
        }
    }
    out
}

/// Group names travel inside the one-line form, so the separators are replaced.
fn sanitize_name(name: &str) -> String {
    let cleaned: String =
        name.chars().map(|c| if matches!(c, ';' | '=' | ',' | ':') { ' ' } else { c }).collect();
    let cleaned = cleaned.trim();
    if cleaned.is_empty() { "custom".to_owned() } else { cleaned.to_owned() }
}

/// Names of the built-in groups, used by the serializer and by Options to recognise them.
pub mod group {
    /// Compiled binaries, libraries and object code.
    pub const BINARY: &str = "binary";
    /// Archives, packages and disk images.
    pub const ARCHIVE: &str = "archive";
    /// Documents and media blobs: bytes matter, timestamps churn.
    pub const MEDIA: &str = "media";
    /// Source code and plain text.
    pub const TEXT: &str = "text";
}

const BINARY_EXTS: &[&str] = &[
    "exe", "dll", "sys", "ocx", "scr", "efi", "so", "dylib", "bundle", "o", "obj", "a", "lib",
    "ko", "elf", "bin", "out", "pyd", "node", "class", "wasm", "pdb", "dbg", "rlib", "rmeta",
];
const ARCHIVE_EXTS: &[&str] = &[
    "zip", "jar", "war", "ear", "apk", "aab", "ipa", "7z", "rar", "tar", "gz", "tgz", "bz2", "xz",
    "zst", "deb", "rpm", "msi", "cab", "iso", "img", "dmg", "whl", "nupkg", "vsix",
];
const MEDIA_EXTS: &[&str] = &[
    "png", "jpg", "jpeg", "gif", "bmp", "webp", "ico", "tif", "tiff", "mp3", "flac", "wav", "ogg",
    "m4a", "mp4", "mkv", "avi", "mov", "webm", "pdf", "ttf", "otf", "woff", "woff2", "psd",
];
const TEXT_EXTS: &[&str] = &[
    "txt", "md", "rst", "log", "csv", "tsv", "json", "jsonc", "yaml", "yml", "toml", "ini", "cfg",
    "conf", "xml", "html", "htm", "css", "scss", "svg", "c", "h", "cc", "cpp", "cxx", "hpp", "hh",
    "hxx", "cs", "java", "kt", "kts", "scala", "go", "rs", "py", "rb", "php", "pl", "lua", "js",
    "mjs", "cjs", "ts", "tsx", "jsx", "vue", "swift", "m", "mm", "sh", "bash", "zsh", "ps1",
    "bat", "cmd", "sql", "cmake", "mk", "gradle", "qml", "qrc", "ui", "in", "lock", "gitignore",
    "reg", "tex",
];

/// The complete matching configuration: ordered rules plus a fallback.
///
/// Lookup is by extension of the *file name only* (the last `.` segment), so `libfoo.so.1` is
/// judged by `1`; callers that care can add `1` to a rule. Names without a dot use the fallback.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MatchPolicy {
    /// Applied when no rule lists the extension. Defaults to MD5.
    pub fallback: MatchMethod,
    /// Rules in priority order; the first one listing the extension wins.
    pub rules: Vec<FileTypeRule>,
}

impl Default for MatchPolicy {
    fn default() -> Self {
        Self::builtin()
    }
}

impl MatchPolicy {
    /// Built-in defaults: binaries/archives/media by MD5, text and source by size + timestamp,
    /// everything unknown by MD5.
    #[must_use]
    pub fn builtin() -> Self {
        let md5 = MatchMethod::Hash(HashAlgorithm::Md5);
        Self {
            fallback: md5,
            rules: vec![
                FileTypeRule::new(group::BINARY, BINARY_EXTS, md5),
                FileTypeRule::new(group::ARCHIVE, ARCHIVE_EXTS, md5),
                FileTypeRule::new(group::MEDIA, MEDIA_EXTS, md5),
                FileTypeRule::new(group::TEXT, TEXT_EXTS, MatchMethod::SizeTimestamp),
            ],
        }
    }

    /// A policy that applies one method to every file (the old global behaviour).
    #[must_use]
    pub fn uniform(method: MatchMethod) -> Self {
        Self { fallback: method, rules: Vec::new() }
    }

    /// Extension (lower-case, no dot) of a file name, if it has one.
    #[must_use]
    pub fn extension_of(file_name: &str) -> Option<String> {
        let base = file_name.rsplit(['/', '\\']).next().unwrap_or(file_name);
        let (stem, ext) = base.rsplit_once('.')?;
        // ".gitignore" has an empty stem: treat the whole dotfile name as the extension.
        let ext = if stem.is_empty() { base.trim_start_matches('.') } else { ext };
        (!ext.is_empty()).then(|| ext.to_lowercase())
    }

    /// Method to use for `file_name`.
    #[must_use]
    pub fn method_for(&self, file_name: &str) -> MatchMethod {
        let Some(ext) = Self::extension_of(file_name) else {
            return self.fallback;
        };
        self.rules
            .iter()
            .find(|rule| rule.extensions.iter().any(|e| *e == ext))
            .map_or(self.fallback, |rule| rule.method)
    }

    /// Algorithm to use when content *must* be hashed for `file_name` (user forced "compare
    /// contents"): the file type's own algorithm if it has one, else the fallback's, else MD5.
    #[must_use]
    pub fn hash_for(&self, file_name: &str) -> HashAlgorithm {
        match self.method_for(file_name) {
            MatchMethod::Hash(a) => a,
            MatchMethod::SizeTimestamp => match self.fallback {
                MatchMethod::Hash(a) => a,
                MatchMethod::SizeTimestamp => HashAlgorithm::Md5,
            },
        }
    }

    /// Serialize to the one-line form documented at module level.
    #[must_use]
    pub fn to_config_string(&self) -> String {
        let mut parts = vec![format!("default={}", self.fallback.id())];
        for rule in &self.rules {
            if rule.extensions.is_empty() {
                continue;
            }
            parts.push(format!("{}:{}={}", sanitize_name(&rule.name), rule.extensions.join(","), rule.method.id()));
        }
        parts.join(";")
    }

    /// Parse the one-line form. Unknown methods are skipped; an unparsable or empty string yields
    /// the built-in policy, so a corrupt preference never breaks folder compare.
    ///
    /// Rules lose their display names when serialized, so they are re-named from the built-in
    /// group whose extension set they overlap most, or `custom`.
    #[must_use]
    pub fn from_config_string(text: &str) -> Self {
        let builtin = Self::builtin();
        let mut fallback = None;
        let mut rules = Vec::new();
        for part in text.split(';').map(str::trim).filter(|p| !p.is_empty()) {
            let Some((key, value)) = part.split_once('=') else { continue };
            let Some(method) = MatchMethod::parse(value) else { continue };
            if key.trim().eq_ignore_ascii_case("default") {
                fallback = Some(method);
                continue;
            }
            let (explicit_name, key) = match key.split_once(':') {
                Some((n, k)) if !n.trim().is_empty() => (Some(sanitize_name(n)), k),
                Some((_, k)) => (None, k),
                None => (None, key),
            };
            let exts = normalize_extensions(key.split(','));
            if exts.is_empty() {
                continue;
            }
            let name = explicit_name.unwrap_or_else(|| builtin
                .rules
                .iter()
                .map(|b| (b, exts.iter().filter(|e| b.extensions.contains(e)).count()))
                .filter(|(_, n)| *n > 0)
                .max_by_key(|(_, n)| *n)
                .map_or_else(|| "custom".to_owned(), |(b, _)| b.name.clone()));
            rules.push(FileTypeRule { name, extensions: exts, method });
        }
        if fallback.is_none() && rules.is_empty() {
            return builtin;
        }
        Self { fallback: fallback.unwrap_or_default(), rules }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_is_md5() {
        assert_eq!(MatchMethod::default(), MatchMethod::Hash(HashAlgorithm::Md5));
        assert_eq!(MatchPolicy::builtin().method_for("README"), MatchMethod::Hash(HashAlgorithm::Md5));
    }

    #[test]
    fn builtin_routes_by_type() {
        let p = MatchPolicy::builtin();
        assert_eq!(p.method_for("app.EXE"), MatchMethod::Hash(HashAlgorithm::Md5));
        assert_eq!(p.method_for("src/main.rs"), MatchMethod::SizeTimestamp);
        assert_eq!(p.method_for("C:\\x\\Foo.CPP"), MatchMethod::SizeTimestamp);
        assert_eq!(p.method_for(".gitignore"), MatchMethod::SizeTimestamp);
        assert_eq!(p.method_for("blob.unknownext"), MatchMethod::Hash(HashAlgorithm::Md5));
    }

    #[test]
    fn algorithm_parse_is_forgiving() {
        assert_eq!(HashAlgorithm::parse("SHA-256"), Some(HashAlgorithm::Sha256));
        assert_eq!(HashAlgorithm::parse(" md5 "), Some(HashAlgorithm::Md5));
        assert_eq!(HashAlgorithm::parse("crc32"), None);
        assert_eq!(MatchMethod::parse("Size"), Some(MatchMethod::SizeTimestamp));
    }

    #[test]
    fn config_string_round_trips() {
        let mut p = MatchPolicy::builtin();
        p.fallback = MatchMethod::Hash(HashAlgorithm::Sha256);
        p.rules.push(FileTypeRule::new("custom", &["*.Foo", ".bar"], MatchMethod::Hash(HashAlgorithm::Sha1)));
        let back = MatchPolicy::from_config_string(&p.to_config_string());
        for name in ["a.exe", "a.rs", "a.foo", "a.bar", "x.zzz", "noext"] {
            assert_eq!(back.method_for(name), p.method_for(name), "{name}");
        }
    }

    #[test]
    fn hash_for_never_returns_size_only() {
        let p = MatchPolicy::builtin();
        assert_eq!(p.hash_for("main.rs"), HashAlgorithm::Md5, "text rule is size-only: use fallback hash");
        let sha = MatchPolicy { fallback: MatchMethod::SizeTimestamp, rules: vec![FileTypeRule::new("x", &["iso"], MatchMethod::Hash(HashAlgorithm::Sha256))] };
        assert_eq!(sha.hash_for("a.iso"), HashAlgorithm::Sha256);
        assert_eq!(sha.hash_for("a.txt"), HashAlgorithm::Md5, "no hash anywhere: MD5");
    }

    #[test]
    fn names_round_trip_and_legacy_lines_still_parse() {
        let p = MatchPolicy::from_config_string(&MatchPolicy::builtin().to_config_string());
        assert_eq!(p, MatchPolicy::builtin(), "names survive a round trip");
        let custom = MatchPolicy::from_config_string("default=md5;My Tools:foo,bar=sha1");
        assert_eq!(custom.rules[0].name, "My Tools");
        let legacy = MatchPolicy::from_config_string("default=md5;exe,dll=sha256");
        assert_eq!(legacy.rules[0].name, "binary", "no name prefix: derived from built-in groups");
        let hostile = FileTypeRule::new("a;b=c,d:e", &["x"], MatchMethod::SizeTimestamp);
        let line = MatchPolicy { fallback: MatchMethod::default(), rules: vec![hostile] }.to_config_string();
        assert_eq!(MatchPolicy::from_config_string(&line).method_for("f.x"), MatchMethod::SizeTimestamp);
    }

    #[test]
    fn corrupt_config_falls_back_to_builtin() {
        assert_eq!(MatchPolicy::from_config_string(""), MatchPolicy::builtin());
        assert_eq!(MatchPolicy::from_config_string("garbage;;=;x=nope"), MatchPolicy::builtin());
    }

    #[test]
    fn first_matching_rule_wins() {
        let p = MatchPolicy {
            fallback: MatchMethod::SizeTimestamp,
            rules: vec![
                FileTypeRule::new("a", &["x"], MatchMethod::Hash(HashAlgorithm::Sha1)),
                FileTypeRule::new("b", &["x"], MatchMethod::Hash(HashAlgorithm::Md5)),
            ],
        };
        assert_eq!(p.method_for("f.x"), MatchMethod::Hash(HashAlgorithm::Sha1));
    }
}

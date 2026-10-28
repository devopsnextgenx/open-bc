//! Build script: reads the usage-tracking endpoint and key at BUILD time and embeds
//! them in the binary. Nothing is read from the environment at run time.
//!
//! Sources, in priority order, for each of `OPENBC_TRACKING_URL` / `OPENBC_TRACKING_KEY`:
//!   1. the build environment variable;
//!   2. a `KEY=VALUE` file: `$OPENBC_TRACKING_FILE`, else the first `tracking.env` found in this
//!      crate's directory or any parent (so a git-ignored `tracking.env` in the repo root works).
//! Empty / missing = that value is absent, and tracking stays off without a URL.
//!
//! The values are written to `$OUT_DIR` as XOR-masked byte arrays, so they never appear as plain
//! text in the binary (`strings`, grep, hex viewers). This is obfuscation, not encryption: whoever
//! can run the program under a debugger can still recover them.

use std::path::{Path, PathBuf};
use std::{env, fs};

const URL_VAR: &str = "OPENBC_TRACKING_URL";
const KEY_VAR: &str = "OPENBC_TRACKING_KEY";
const FILE_VAR: &str = "OPENBC_TRACKING_FILE";
const FILE_NAME: &str = "tracking.env";

fn unquote(value: &str) -> &str {
    let value = value.trim();
    for quote in ['"', '\''] {
        if let Some(inner) = value.strip_prefix(quote).and_then(|v| v.strip_suffix(quote)) {
            return inner;
        }
    }
    value
}

fn from_file(text: &str, name: &str) -> Option<String> {
    text.lines().find_map(|line| {
        let line = line.trim();
        if line.starts_with('#') {
            return None;
        }
        let (key, value) = line.split_once('=')?;
        (key.trim() == name).then(|| unquote(value).trim().to_owned())
    })
}

fn find_file() -> Option<PathBuf> {
    if let Some(path) = env::var_os(FILE_VAR).filter(|value| !value.is_empty()) {
        return Some(PathBuf::from(path));
    }
    let mut dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR")?);
    for _ in 0..5 {
        let candidate = dir.join(FILE_NAME);
        if candidate.is_file() {
            return Some(candidate);
        }
        if !dir.pop() {
            break;
        }
    }
    None
}

fn resolve(name: &str, file_text: Option<&str>) -> String {
    env::var(name)
        .ok()
        .map(|value| unquote(&value).trim().to_owned())
        .filter(|value| !value.is_empty())
        .or_else(|| file_text.and_then(|text| from_file(text, name)))
        .filter(|value| !value.is_empty())
        .unwrap_or_default()
}

/// Must stay identical to `tracking_pad` in the generated code below.
fn pad(index: usize) -> u8 {
    let mut z = (index as u64).wrapping_add(0x9E37_79B9_7F4A_7C15).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z ^= z >> 27;
    z = z.wrapping_mul(0x94D0_49BB_1331_11EB);
    (z >> 32) as u8
}

fn masked(value: &str) -> String {
    let bytes: Vec<String> = value
        .bytes()
        .enumerate()
        .map(|(i, byte)| (byte ^ pad(i)).to_string())
        .collect();
    format!("&[{}]", bytes.join(","))
}

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    for var in [URL_VAR, KEY_VAR, FILE_VAR] {
        println!("cargo:rerun-if-env-changed={var}");
    }
    let file = find_file();
    if let Some(path) = file.as_deref().filter(|path| path.is_file()) {
        println!("cargo:rerun-if-changed={}", path.display());
    }
    let file_text = file.as_deref().and_then(|path: &Path| fs::read_to_string(path).ok());

    let url = resolve(URL_VAR, file_text.as_deref());
    let key = resolve(KEY_VAR, file_text.as_deref());
    // Never print the values; only whether they are present.
    println!(
        "cargo:warning=openbc-observability: usage tracking endpoint {}, key {}",
        if url.is_empty() { "NOT embedded (tracking will be off)" } else { "embedded" },
        if key.is_empty() { "NOT embedded" } else { "embedded" },
    );

    let generated = format!(
        r#"#[allow(dead_code)]
const fn tracking_pad(index: usize) -> u8 {{
    let mut z = (index as u64).wrapping_add(0x9E37_79B9_7F4A_7C15).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z ^= z >> 27;
    z = z.wrapping_mul(0x94D0_49BB_1331_11EB);
    (z >> 32) as u8
}}
#[allow(dead_code)]
const TRACKING_URL_MASKED: &[u8] = {url};
#[allow(dead_code)]
const TRACKING_KEY_MASKED: &[u8] = {key};
"#,
        url = masked(&url),
        key = masked(&key),
    );
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set by cargo"));
    fs::write(out.join("tracking_embedded.rs"), generated).expect("write tracking_embedded.rs");
}

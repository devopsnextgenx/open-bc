//! Tests against real binaries built on the fly (skipped when no C compiler is installed) and
//! against the test executable itself.

use std::path::{Path, PathBuf};
use std::process::Command;

use openbc_core::binary_compare::{diff_binaries, ChangeState, SymbolKind};

use super::{parse_binary, ParseOptions};

fn compile(dir: &Path, name: &str, src: &str) -> Option<PathBuf> {
    let c = dir.join(format!("{name}.c"));
    std::fs::write(&c, src).ok()?;
    let out = dir.join(format!("lib{name}.so"));
    let ok = Command::new("cc")
        .args(["-shared", "-fPIC", "-O0", "-g", "-o"])
        .arg(&out)
        .arg(&c)
        .status()
        .ok()?
        .success();
    ok.then_some(out)
}

const V1: &str = r#"
const char *banner(void) { return "openbc test banner v1"; }
int keep(int x) { return x * 3 + 1; }
int change(int x) { return x + 1; }
int gone(int x) { return x - 1; }
"#;
const V2: &str = r#"
const char *banner(void) { return "openbc test banner v2"; }
int keep(int x) { return x * 3 + 1; }
int change(int x) { return x + 2; }
int fresh(int x) { return x ^ 7; }
"#;

#[cfg(target_os = "linux")]
#[test]
fn elf_pair_is_diffed_by_function_hash() {
    let dir = std::env::temp_dir().join(format!("openbc-bin-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let (Some(a), Some(b)) = (compile(&dir, "v1", V1), compile(&dir, "v2", V2)) else {
        eprintln!("no C compiler: skipped");
        return;
    };
    let opts = ParseOptions::default();
    let m1 = parse_binary(&a, &opts).unwrap();
    let m2 = parse_binary(&b, &opts).unwrap();
    assert_eq!(m1.format, "ELF");
    assert!(m1.meta("architecture").is_some());
    assert!(m1.sections.iter().any(|s| s.name == ".text"));
    assert!(m1.symbols.iter().any(|s| s.kind == SymbolKind::Exported && s.name == "keep"));
    assert!(m1.strings.iter().any(|s| s.contains("openbc test banner v1")));

    let d = diff_binaries(&m1, &m2);
    let f = |n: &str| d.functions.iter().find(|e| e.key == n).map(|e| e.state);
    assert_eq!(d.file_state, ChangeState::Modified);
    assert_eq!(f("change"), Some(ChangeState::Modified));
    assert_eq!(f("gone"), Some(ChangeState::Removed));
    assert_eq!(f("fresh"), Some(ChangeState::Added));
    assert!(matches!(f("keep"), Some(ChangeState::Unchanged | ChangeState::Modified)));
    let strings = d.strings.iter().find(|e| e.key.contains("banner v1")).unwrap();
    assert_eq!(strings.state, ChangeState::Modified);
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn parses_the_running_test_executable() {
    let exe = std::env::current_exe().unwrap();
    let m = parse_binary(&exe, &ParseOptions::default()).unwrap();
    assert!(["ELF", "PE", "Mach-O"].contains(&m.format.as_str()));
    assert!(!m.sections.is_empty());
    assert_eq!(m.file_sha256.len(), 64);
    // identical file against itself: nothing may differ
    let d = diff_binaries(&m, &m);
    assert_eq!(d.totals().changed(), 0);
}

#[test]
fn non_executable_is_rejected_cleanly() {
    let p = std::env::temp_dir().join(format!("openbc-notbin-{}.txt", std::process::id()));
    std::fs::write(&p, b"just text, definitely not an executable").unwrap();
    let err = parse_binary(&p, &ParseOptions::default()).unwrap_err();
    assert!(err.to_string().contains("not a supported executable"));
    std::fs::remove_file(&p).ok();
}

#[test]
fn size_limit_is_enforced() {
    let exe = std::env::current_exe().unwrap();
    let opts = ParseOptions { max_file_bytes: 10, ..ParseOptions::default() };
    assert!(matches!(parse_binary(&exe, &opts), Err(super::BinaryError::TooLarge { .. })));
}

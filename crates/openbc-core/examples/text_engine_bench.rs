//! Benchmarks for the text comparison engine.
//!
//! `cargo run --release -p openbc-core --example text_engine_bench -- [--sizes 3,50,100,500] [--dir /tmp]`
//!
//! Generates deterministic files, then measures per scenario: index build (both files), diff,
//! time-to-first-rows, a scroll sweep, syntax-highlighting cost and memory (peak RSS from
//! `/proc/self/status` on Linux, index/diff bytes from the model's own accounting).

use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use openbc_core::text::engine::*;
use openbc_core::text::{HighlightSession, SessionLimits};

fn rss_peak_mb() -> f64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM")).map(str::to_owned))
        .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse::<f64>().ok()))
        .map_or(0.0, |kb| kb / 1024.0)
}

fn rss_anon_mb() -> f64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("RssAnon")).map(str::to_owned))
        .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse::<f64>().ok()))
        .map_or(0.0, |kb| kb / 1024.0)
}

/// Reset the kernel's peak-RSS high-water mark so each scenario reports its own peak.
fn reset_peak() {
    let _ = std::fs::write("/proc/self/clear_refs", "5");
}

fn rss_now_mb() -> f64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmRSS")).map(str::to_owned))
        .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse::<f64>().ok()))
        .map_or(0.0, |kb| kb / 1024.0)
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

#[derive(Clone, Copy)]
enum Shape {
    /// ~40-byte source-code-like lines.
    Normal,
    /// Few extremely long lines.
    LongLines,
    /// Lines drawn from a tiny vocabulary (very many duplicates).
    Repetitive,
    /// Short lines (~8 bytes): millions of them.
    Short,
}

fn line_for(shape: Shape, n: u64) -> String {
    match shape {
        Shape::Normal => format!("    let value_{n} = compute({n}, \"item {}\"); // note\n", n % 97),
        Shape::LongLines => format!("{n}:{}\n", "abcdefghij".repeat(100_000)),
        Shape::Repetitive => format!("common line {}\n", n % 5),
        Shape::Short => format!("l{n}\n"),
    }
}

/// Write a file of about `bytes` bytes. `edits`: (position fraction, inserted lines, deleted lines).
fn generate(path: &Path, shape: Shape, bytes: u64, edits: &[(f64, u64, u64)]) -> u64 {
    let sample = line_for(shape, 12345).len() as u64;
    let lines = (bytes / sample).max(1);
    let mut plan: Vec<(u64, u64, u64)> = edits.iter().map(|&(f, i, d)| ((f * lines as f64) as u64, i, d)).collect();
    plan.sort();
    let mut w = BufWriter::with_capacity(1 << 20, File::create(path).unwrap());
    let mut n = 0u64;
    let mut written = 0u64;
    let mut rng = Rng(0x9E3779B97F4A7C15);
    let mut p = 0;
    while n < lines {
        if p < plan.len() && n == plan[p].0 {
            for k in 0..plan[p].1 {
                let s = format!("INSERTED {} {}\n", k, rng.next() % 1000);
                written += s.len() as u64;
                w.write_all(s.as_bytes()).unwrap();
            }
            n += plan[p].2; // skip (delete) source lines
            p += 1;
            continue;
        }
        let s = line_for(shape, n);
        written += s.len() as u64;
        w.write_all(s.as_bytes()).unwrap();
        n += 1;
    }
    w.flush().unwrap();
    written
}

struct Row {
    name: String,
    size_mb: f64,
    lines: u64,
    index_ms: f64,
    diff_ms: f64,
    first_ms: f64,
    scroll_us_per_row: f64,
    syntax_ms_per_100: f64,
    index_mb: f64,
    diff_kb: f64,
    peak_mb: f64,
    rss_mb: f64,
    anon_mb: f64,
}

fn run(name: &str, l: &Path, r: &Path) -> Row {
    reset_peak();
    let cancel = CancelToken::new();
    let cfg = CompareConfig::default();
    // Phase timing: index each side separately, then diff, so the numbers are attributable.
    let t = Instant::now();
    let ls = Arc::new(MappedTextFile::open(l).unwrap());
    let rs = Arc::new(MappedTextFile::open(r).unwrap());
    let li = LineIndex::build(ls.as_ref(), &cfg.index, &cancel, &mut |_| {}).unwrap();
    let ri = LineIndex::build(rs.as_ref(), &cfg.index, &cancel, &mut |_| {}).unwrap();
    let index_ms = t.elapsed().as_secs_f64() * 1e3;
    let t = Instant::now();
    let diff = DiffEngine::new()
        .diff(Side { source: ls.as_ref(), index: &li }, Side { source: rs.as_ref(), index: &ri }, &cancel, &mut |_| {})
        .unwrap();
    let diff_ms = t.elapsed().as_secs_f64() * 1e3;
    drop((li, ri, diff));

    // End-to-end as the UI does it: open -> first visible rows.
    let t = Instant::now();
    let model = TextCompareModel::open(Some(l), Some(r), cfg, &cancel, &mut |_| {}).unwrap();
    for row in 0..60.min(model.row_count()) {
        if let Some(d) = model.row(row) {
            if let Some(x) = d.left_line {
                let _ = model.line_window(SideId::Left, x, 0, 400).unwrap();
            }
            if let Some(x) = d.right_line {
                let _ = model.line_window(SideId::Right, x, 0, 400).unwrap();
            }
        }
    }
    let first_ms = t.elapsed().as_secs_f64() * 1e3;

    // Scroll sweep: 2000 evenly spread 50-row screens.
    let rows = model.row_count().max(1);
    let t = Instant::now();
    let mut touched = 0u64;
    for k in 0..2000u64 {
        let start = (rows / 2000).max(1) * k % rows;
        for row in start..(start + 50).min(rows) {
            let d = model.row(row).unwrap();
            if let Some(x) = d.left_line {
                let _ = model.line_window(SideId::Left, x, 0, 400).unwrap();
            }
            if d.kind == DiffType::Replace {
                let _ = model.inline_diff(row);
            }
            touched += 1;
        }
    }
    let scroll_us = t.elapsed().as_secs_f64() * 1e6 / touched.max(1) as f64;

    // Syntax: highlight 100 lines from the middle (cold session) as C-like code.
    let mut sess = HighlightSession::new("c", true, SessionLimits::default());
    let mid = model.line_count(SideId::Left) / 2;
    let t = Instant::now();
    let _ = sess.highlight_range(mid, 100, &mut |n| model.line_window(SideId::Left, n, 0, 4000).ok().map(|w| w.text));
    let syntax_ms = t.elapsed().as_secs_f64() * 1e3;

    let mem = model.memory_report();
    Row {
        name: name.to_owned(),
        size_mb: (model.source_len(SideId::Left) + model.source_len(SideId::Right)) as f64 / 2.0 / 1048576.0,
        lines: model.line_count(SideId::Left),
        index_ms,
        diff_ms,
        first_ms,
        scroll_us_per_row: scroll_us,
        syntax_ms_per_100: syntax_ms,
        index_mb: mem.index_bytes as f64 / 1048576.0,
        diff_kb: mem.diff_bytes as f64 / 1024.0,
        peak_mb: rss_peak_mb(),
        rss_mb: rss_now_mb(),
        anon_mb: rss_anon_mb(),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let arg = |k: &str, d: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).cloned().unwrap_or_else(|| d.to_owned());
    let dir = PathBuf::from(arg("--dir", "/tmp"));
    let sizes: Vec<u64> = arg("--sizes", "3,50,100").split(',').map(|s| s.parse().unwrap()).collect();
    let mut rows = Vec::new();
    let mb = 1 << 20;
    let (l, r) = (dir.join("bench_l.txt"), dir.join("bench_r.txt"));
    for &s in &sizes {
        let edits = [(0.02, 40, 10), (0.5, 400, 300), (0.97, 25, 25)];
        generate(&l, Shape::Normal, s * mb, &[]);
        generate(&r, Shape::Normal, s * mb, &edits);
        rows.push(run(&format!("code {s} MB, edits start/mid/end"), &l, &r));
    }
    let big = *sizes.iter().max().unwrap();
    generate(&l, Shape::Normal, big * mb, &[]);
    generate(&r, Shape::Normal, big * mb, &[]);
    rows.push(run(&format!("code {big} MB, identical"), &l, &r));
    generate(&l, Shape::Short, 100 * mb, &[]);
    generate(&r, Shape::Short, 100 * mb, &[(0.3, 1000, 1000), (0.8, 5, 0)]);
    rows.push(run("100 MB short lines (~13M)", &l, &r));
    generate(&l, Shape::Repetitive, 50 * mb, &[]);
    generate(&r, Shape::Repetitive, 50 * mb, &[(0.5, 10, 7)]);
    rows.push(run("50 MB repetitive (5 distinct lines)", &l, &r));
    generate(&l, Shape::LongLines, 200 * mb, &[]);
    generate(&r, Shape::LongLines, 200 * mb, &[(0.5, 0, 1)]);
    rows.push(run("200 MB, 1 MB-long lines", &l, &r));
    let _ = (std::fs::remove_file(&l), std::fs::remove_file(&r));

    println!("| scenario | size/side MB | lines | index ms | diff ms | first rows ms | scroll µs/row | syntax ms/100 lines | index MB | diff KB | heap now MB | RSS now MB | peak RSS MB |");
    println!("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|");
    for r in rows {
        println!(
            "| {} | {:.0} | {} | {:.0} | {:.0} | {:.1} | {:.1} | {:.1} | {:.1} | {:.1} | {:.0} | {:.0} | {:.0} |",
            r.name, r.size_mb, r.lines, r.index_ms, r.diff_ms, r.first_ms, r.scroll_us_per_row, r.syntax_ms_per_100, r.index_mb, r.diff_kb, r.anon_mb, r.rss_mb, r.peak_mb
        );
    }
}

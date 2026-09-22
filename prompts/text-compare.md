Here is the crate layout breakdown, architectural interface design, and the GitHub Copilot prompt to implement the text and log comparison module into your existing **OpenBC** workspace.

---

### 1. Crate Layout & Responsibility Breakdown

To maintain strict modularity, the text comparison logic should be split across **`openbc-core`** (for pure algorithms and normalization rules) and **`openbc-engine`** (for async file streaming and processing execution).

```
openbc-core (No UI, Pure Domain Logic)
├── text/
│   ├── diff.rs           <-- Wrapper around `similar` / `imara-diff`
│   ├── normalize.rs      <-- Regex & Rule-based log line normalizer
│   ├── fuzzy.rs          <-- Fuzzy line matching using `strsim` (>95% threshold)
│   └── models.rs         <-- Output diff models (DiffChunk, LineChangeType)
└── lib.rs

openbc-engine (Async Execution & Pipeline Orchestration)
├── text_runner.rs       <-- Async streaming reader, chunck processing with Rayon
└── lib.rs

```

#### Detailed Crate Responsibilities:

1. **`openbc-core` (Domain & Algorithmic Core):**
* **`DiffEngine` / `TextDiff` Interface:** Pure, synchronous functions that take two string slices or line buffers and return structured diff results.
* **`LogNormalizer`:** Configurable rule pipeline (built on `regex`) to strip/mask ISO timestamps, RFC dates, container/pod IDs, and UUIDs.
* **`FuzzyMatcher`:** Evaluates line similarity scores using `strsim` (e.g., Jaro-Winkler) when exact line matches fail due to residual noise.


2. **`openbc-engine` (Orchestration & Parallel Execution):**
* **`AsyncTextCompareTask`:** Reads files or VFS streams asynchronously via Tokio, breaks large files into line chunks, dispatches chunk comparisons to Rayon CPU worker threads, and streams diff updates back to the Qt UI via `tokio::sync::mpsc` channels.



---

### 2. Core Interfaces & Helper Types (`openbc-core`)

#### `openbc-core/src/text/models.rs`

```rust
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChangeKind {
    Unchanged,
    Added,
    Deleted,
    Modified { similarity_score: u8 }, // Score 0-100 for fuzzy log matches
}

#[derive(Debug, Clone)]
pub struct LineDiff {
    pub left_line_num: Option<usize>,
    pub right_line_num: Option<usize>,
    pub left_text: String,
    pub right_text: String,
    pub kind: ChangeKind,
}

#[derive(Debug, Clone)]
pub struct CompareOptions {
    pub ignore_whitespace: bool,
    pub ignore_timestamps: bool,
    pub ignore_container_ids: bool,
    pub fuzzy_threshold: f64, // e.g., 0.95 (95% match)
}

```

#### `openbc-core/src/text/normalize.rs`

```rust
use regex::Regex;
use std::sync::LazyLock;

static TIMESTAMP_REGEX: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?Z?)|(\d{2}:\d{2}:\d{2}\.\d+)").unwrap()
});

static CONTAINER_ID_REGEX: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)(pod-[a-z0-9-]+)|([a-f0-9]{12,64})").unwrap()
});

pub struct LogNormalizer;

impl LogNormalizer {
    pub fn normalize(line: &str, options: &CompareOptions) -> String {
        let mut result = line.to_string();
        if options.ignore_timestamps {
            result = TIMESTAMP_REGEX.replace_all(&result, "<TS>").to_string();
        }
        if options.ignore_container_ids {
            result = CONTAINER_ID_REGEX.replace_all(&result, "<CONTAINER>").to_string();
        }
        result
    }
}

```

#### `openbc-core/src/text/diff.rs`

```rust
use similar::{ChangeTag, TextDiff};
use strsim::jaro_winkler;
use crate::text::models::{CompareOptions, LineDiff, ChangeKind};
use crate::text::normalize::LogNormalizer;

pub struct TextCompareEngine;

impl TextCompareEngine {
    pub fn compare_buffers(
        left: &str,
        right: &str,
        options: &CompareOptions,
    ) -> Vec<LineDiff> {
        let norm_left: Vec<String> = left.lines().map(|l| LogNormalizer::normalize(l, options)).collect();
        let norm_right: Vec<String> = right.lines().map(|l| LogNormalizer::normalize(l, options)).collect();

        let norm_left_joined = norm_left.join("\n");
        let norm_right_joined = norm_right.join("\n");

        let diff = TextDiff::from_lines(&norm_left_joined, &norm_right_joined);
        let left_raw: Vec<&str> = left.lines().collect();
        let right_raw: Vec<&str> = right.lines().collect();

        let mut results = Vec::new();
        let mut idx_left = 0;
        let mut idx_right = 0;

        for change in diff.iter_all_changes() {
            match change.tag() {
                ChangeTag::Equal => {
                    results.push(LineDiff {
                        left_line_num: Some(idx_left + 1),
                        right_line_num: Some(idx_right + 1),
                        left_text: left_raw.get(idx_left).unwrap_or(&"").to_string(),
                        right_text: right_raw.get(idx_right).unwrap_or(&"").to_string(),
                        kind: ChangeKind::Unchanged,
                    });
                    idx_left += 1;
                    idx_right += 1;
                }
                ChangeTag::Delete => {
                    results.push(LineDiff {
                        left_line_num: Some(idx_left + 1),
                        right_line_num: None,
                        left_text: left_raw.get(idx_left).unwrap_or(&"").to_string(),
                        right_text: String::new(),
                        kind: ChangeKind::Deleted,
                    });
                    idx_left += 1;
                }
                ChangeTag::Insert => {
                    // Check if previous deleted line can be fuzzy matched with this inserted line
                    if let Some(last) = results.last_mut() {
                        if last.kind == ChangeKind::Deleted && last.right_line_num.is_none() {
                            let sim = jaro_winkler(&last.left_text, right_raw.get(idx_right).unwrap_or(&""));
                            if sim >= options.fuzzy_threshold {
                                last.right_line_num = Some(idx_right + 1);
                                last.right_text = right_raw.get(idx_right).unwrap_or(&"").to_string();
                                last.kind = ChangeKind::Modified { similarity_score: (sim * 100.0) as u8 };
                                idx_right += 1;
                                continue;
                            }
                        }
                    }

                    results.push(LineDiff {
                        left_line_num: None,
                        right_line_num: Some(idx_right + 1),
                        left_text: String::new(),
                        right_text: right_raw.get(idx_right).unwrap_or(&"").to_string(),
                        kind: ChangeKind::Added,
                    });
                    idx_right += 1;
                }
            }
        }
        results
    }
}

```

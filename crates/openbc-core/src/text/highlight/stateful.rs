//! Stateful, checkpointed line highlighting shared by both editors.
//!
//! The old per-line path highlighted each line in isolation, so multi-line comments and strings
//! lost their state, and it cached every line it ever saw. A [`HighlightSession`] instead
//!
//! * carries a [`LexerState`] from line to line (syntect's parse + highlight state),
//! * snapshots that state every `checkpoint_interval` lines so any line can be reached by replaying
//!   at most one interval from the nearest checkpoint,
//! * keeps checkpoints and span results in bounded structures (checkpoints are thinned, spans live
//!   in a [`BoundedCache`]),
//! * never replays more than `lookback_limit` lines: a far jump into a huge file restarts from a
//!   fresh state a little earlier (`approximate` results; documented trade-off).
//!
//! Languages are resolved through the same registry as the buffer API, so extension mappings and
//! user overrides apply. tree-sitter languages (Rust/Python/JS) have no incremental line API, so
//! sessions use the matching syntect grammar for them.

use std::collections::BTreeMap;
use std::sync::Arc;

use syntect::highlighting::{FontStyle, HighlightIterator, HighlightState, Highlighter, Style, Theme};
use syntect::parsing::{ParseState, ScopeStack, SyntaxReference, SyntaxSet};

use super::language::{route, Route};
use super::syntect_backend::{syntax_set, theme_set, DARK_THEME, LIGHT_THEME};
use crate::text::engine::cache::BoundedCache;

/// Visual attributes of a style id.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct StyleInfo {
    /// RGBA foreground.
    pub foreground: [u8; 4],
    /// RGBA background.
    pub background: [u8; 4],
    /// Bold.
    pub bold: bool,
    /// Italic.
    pub italic: bool,
}

/// A highlighted range of one line. Offsets are UTF-8 bytes within the line text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StyleSpan {
    /// Start byte.
    pub start: u32,
    /// Length in bytes.
    pub length: u32,
    /// Index into the session's style table.
    pub style_id: u32,
}

/// Lexer state carried between lines.
#[derive(Clone)]
pub enum LexerState {
    /// No highlighting.
    Plain,
    /// syntect parse and highlight state.
    Syntect(Box<(ParseState, HighlightState)>),
}

/// A line-at-a-time highlighter with explicit state.
pub trait SyntaxEngine: Send {
    /// State before the first line.
    fn initial_state(&self) -> LexerState;
    /// Highlight `line` (no terminator), updating `state` to the state after it.
    fn highlight_line(&mut self, line: &str, state: &mut LexerState, out: &mut Vec<StyleSpan>);
    /// Style table entry.
    fn style(&self, id: u32) -> Option<StyleInfo>;
}

struct PlainEngine;
impl SyntaxEngine for PlainEngine {
    fn initial_state(&self) -> LexerState {
        LexerState::Plain
    }
    fn highlight_line(&mut self, _: &str, _: &mut LexerState, _: &mut Vec<StyleSpan>) {}
    fn style(&self, _: u32) -> Option<StyleInfo> {
        None
    }
}

struct SyntectEngine {
    set: &'static SyntaxSet,
    syntax: &'static SyntaxReference,
    theme: &'static Theme,
    styles: Vec<StyleInfo>,
    lookup: std::collections::HashMap<StyleInfo, u32>,
    buf: String,
}

impl SyntectEngine {
    fn intern(&mut self, style: Style) -> u32 {
        let c = |c: syntect::highlighting::Color| [c.r, c.g, c.b, c.a];
        let info = StyleInfo {
            foreground: c(style.foreground),
            background: c(style.background),
            bold: style.font_style.contains(FontStyle::BOLD),
            italic: style.font_style.contains(FontStyle::ITALIC),
        };
        if let Some(&id) = self.lookup.get(&info) {
            return id;
        }
        let id = self.styles.len() as u32;
        self.styles.push(info);
        self.lookup.insert(info, id);
        id
    }
}

impl SyntaxEngine for SyntectEngine {
    fn initial_state(&self) -> LexerState {
        let highlighter = Highlighter::new(self.theme);
        LexerState::Syntect(Box::new((
            ParseState::new(self.syntax),
            HighlightState::new(&highlighter, ScopeStack::new()),
        )))
    }

    fn highlight_line(&mut self, line: &str, state: &mut LexerState, out: &mut Vec<StyleSpan>) {
        let LexerState::Syntect(inner) = state else { return };
        let (parse, hl) = &mut **inner;
        // The newline-aware syntax set expects a terminator on every line.
        self.buf.clear();
        self.buf.push_str(line);
        self.buf.push('\n');
        let Ok(ops) = parse.parse_line(&self.buf, self.set) else { return };
        let highlighter = Highlighter::new(self.theme);
        let mut start = 0usize;
        let mut last: Option<StyleSpan> = None;
        let pieces: Vec<(Style, usize)> = HighlightIterator::new(hl, &ops, &self.buf, &highlighter)
            .map(|(style, text)| (style, text.len()))
            .collect();
        for (style, len) in pieces {
            let end = (start + len).min(line.len());
            if end > start {
                let id = self.intern(style);
                match last.as_mut() {
                    Some(prev) if prev.style_id == id && (prev.start + prev.length) as usize == start => {
                        prev.length += (end - start) as u32;
                    }
                    _ => {
                        if let Some(prev) = last.take() {
                            out.push(prev);
                        }
                        last = Some(StyleSpan { start: start as u32, length: (end - start) as u32, style_id: id });
                    }
                }
            }
            start += len;
        }
        if let Some(prev) = last {
            out.push(prev);
        }
    }

    fn style(&self, id: u32) -> Option<StyleInfo> {
        self.styles.get(id as usize).copied()
    }
}

/// Limits for a session. All memory held by a session is bounded by these.
#[derive(Clone, Copy, Debug)]
pub struct SessionLimits {
    /// Snapshot the lexer state every this many lines.
    pub checkpoint_interval: u64,
    /// Maximum number of checkpoints kept (older ones are thinned out).
    pub max_checkpoints: usize,
    /// Maximum cached lines of spans.
    pub span_cache_lines: usize,
    /// Never replay more than this many lines for one request.
    pub lookback_limit: u64,
    /// Lines of warm-up before an approximate restart.
    pub warmup_lines: u64,
}

impl Default for SessionLimits {
    fn default() -> Self {
        Self {
            checkpoint_interval: 256,
            max_checkpoints: 4096,
            span_cache_lines: 4096,
            lookback_limit: 50_000,
            warmup_lines: 500,
        }
    }
}

/// Provides the text of a line (no terminator), or `None` past the end.
pub type LineProvider<'a> = dyn FnMut(u64) -> Option<String> + 'a;

/// Highlighting of one file (one side) with bounded memory.
pub struct HighlightSession {
    engine: Box<dyn SyntaxEngine>,
    limits: SessionLimits,
    checkpoints: BTreeMap<u64, LexerState>,
    spans: BoundedCache<u64, Arc<[StyleSpan]>>,
    approximate: bool,
}

impl HighlightSession {
    /// Session for `language` (file name, extension or language name, as for the buffer API).
    #[must_use]
    pub fn new(language: &str, dark: bool, limits: SessionLimits) -> Self {
        let engine: Box<dyn SyntaxEngine> = match resolve(language, dark) {
            Some(engine) => Box::new(engine),
            None => Box::new(PlainEngine),
        };
        Self::with_engine(engine, limits)
    }

    /// Session over a custom engine.
    #[must_use]
    pub fn with_engine(engine: Box<dyn SyntaxEngine>, limits: SessionLimits) -> Self {
        let mut checkpoints = BTreeMap::new();
        checkpoints.insert(0, engine.initial_state());
        Self {
            spans: BoundedCache::new(limits.span_cache_lines.max(2)),
            engine,
            limits,
            checkpoints,
            approximate: false,
        }
    }

    /// True when a far jump forced a restart from a fresh state (results may differ from a
    /// top-to-bottom pass inside a multi-line construct that began before the restart).
    #[must_use]
    pub fn is_approximate(&self) -> bool {
        self.approximate
    }

    /// Style table lookup.
    #[must_use]
    pub fn style(&self, id: u32) -> Option<StyleInfo> {
        self.engine.style(id)
    }

    /// Number of stored checkpoints (bounded by `max_checkpoints`).
    #[must_use]
    pub fn checkpoint_count(&self) -> usize {
        self.checkpoints.len()
    }

    /// Number of cached lines (bounded).
    #[must_use]
    pub fn cached_lines(&self) -> usize {
        self.spans.len()
    }

    /// Forget everything at or after `line` (file changed there).
    pub fn invalidate_from(&mut self, line: u64) {
        let keep_to = line.max(1);
        self.checkpoints.split_off(&keep_to);
        if line == 0 {
            self.checkpoints.insert(0, self.engine.initial_state());
        }
        self.spans.clear();
        self.approximate = false;
    }

    /// Spans for lines `first..first+count`, in order. Lines past the end yield nothing.
    pub fn highlight_range(
        &mut self,
        first: u64,
        count: u64,
        provider: &mut LineProvider<'_>,
    ) -> Vec<(u64, Arc<[StyleSpan]>)> {
        let last = first.saturating_add(count).saturating_sub(1);
        let mut out = Vec::with_capacity(count as usize);
        if count == 0 {
            return out;
        }
        // All cached? (peek without disturbing order guarantees)
        let mut cached = Vec::with_capacity(count as usize);
        for line in first..=last {
            match self.spans.get(&line) {
                Some(s) => cached.push((line, s)),
                None => break,
            }
        }
        if cached.len() as u64 == count {
            return cached;
        }

        let interval = self.limits.checkpoint_interval.max(1);
        let (mut line, mut state) = match self.checkpoints.range(..=first).next_back() {
            Some((&l, st)) if first - l <= self.limits.lookback_limit => (l, st.clone()),
            _ => {
                // Too far from any checkpoint: restart from a fresh state a little earlier.
                let start = first.saturating_sub(self.limits.warmup_lines) / interval * interval;
                if start > 0 {
                    self.approximate = true;
                }
                let state = self.engine.initial_state();
                self.checkpoints.entry(start).or_insert_with(|| state.clone());
                (start, state)
            }
        };
        let mut scratch = Vec::new();
        while line <= last {
            let Some(text) = provider(line) else { break };
            if line % interval == 0 && !self.checkpoints.contains_key(&line) {
                self.checkpoints.insert(line, state.clone());
                self.thin_checkpoints();
            }
            scratch.clear();
            self.engine.highlight_line(&text, &mut state, &mut scratch);
            if line >= first {
                let spans: Arc<[StyleSpan]> = scratch.as_slice().into();
                self.spans.insert(line, spans.clone());
                out.push((line, spans));
            }
            line += 1;
        }
        out
    }

    fn thin_checkpoints(&mut self) {
        if self.checkpoints.len() <= self.limits.max_checkpoints {
            return;
        }
        // Drop every second checkpoint (keeping line 0): halves memory, doubles worst-case replay.
        let keys: Vec<u64> = self.checkpoints.keys().copied().collect();
        for key in keys.into_iter().skip(1).step_by(2) {
            self.checkpoints.remove(&key);
        }
    }
}

fn resolve(language: &str, dark: bool) -> Option<SyntectEngine> {
    let set = syntax_set();
    let syntax = match route(language) {
        Route::Syntect(name) => set.find_syntax_by_name(name)?,
        Route::TreePainter(ext) => set.find_syntax_by_extension(ext)?,
        Route::Plain => return None,
    };
    let theme = theme_set().themes.get(if dark { DARK_THEME } else { LIGHT_THEME })?;
    Some(SyntectEngine { set, syntax, theme, styles: Vec::new(), lookup: Default::default(), buf: String::new() })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn session(lang: &str, limits: SessionLimits) -> HighlightSession {
        HighlightSession::new(lang, true, limits)
    }

    fn provider(lines: &[String]) -> impl FnMut(u64) -> Option<String> + '_ {
        move |n| lines.get(n as usize).cloned()
    }

    fn styled(s: &HighlightSession, spans: &[StyleSpan]) -> Vec<(u32, u32, [u8; 4])> {
        spans.iter().map(|sp| (sp.start, sp.length, s.style(sp.style_id).unwrap().foreground)).collect()
    }

    #[test]
    fn matches_the_whole_buffer_highlighter_including_multiline_comments() {
        let src = "/* start\n   still comment\n*/ int x = 1; // tail\nchar *s = \"a\\\nb\";\n";
        let lines: Vec<String> = src.lines().map(str::to_owned).collect();
        let mut s = session("c", SessionLimits::default());
        let got = s.highlight_range(0, lines.len() as u64, &mut provider(&lines));
        let reference = crate::text::highlight_to_spans(src, "c", true);
        for (line, spans) in got {
            let want: Vec<(u32, u32, [u8; 4])> = reference
                .iter()
                .filter(|r| r.line == line as usize)
                .map(|r| (r.start as u32, r.length as u32, r.foreground))
                .collect();
            assert_eq!(styled(&s, &spans), want, "line {line}");
        }
    }

    #[test]
    fn jumping_into_a_comment_uses_carried_state_not_a_fresh_line() {
        let lines: Vec<String> = ["/* open", "inside comment", "end */ int y;"].map(String::from).to_vec();
        let mut s = session("c", SessionLimits::default());
        // Ask for line 1 directly: the session must replay line 0 first.
        let direct = s.highlight_range(1, 1, &mut provider(&lines));
        let mut fresh = session("c", SessionLimits::default());
        let isolated = fresh.highlight_range(0, 1, &mut provider(&["inside comment".to_owned()]));
        assert_ne!(styled(&s, &direct[0].1), styled(&fresh, &isolated[0].1), "state must matter");
        assert!(!s.is_approximate());
    }

    #[test]
    fn checkpoints_and_caches_stay_bounded_on_huge_inputs() {
        let limits = SessionLimits { checkpoint_interval: 16, max_checkpoints: 32, span_cache_lines: 64, lookback_limit: 1_000_000, warmup_lines: 10 };
        let lines: Vec<String> = (0..20_000).map(|i| format!("int v{i} = {i}; /* c")).collect();
        let mut s = session("c", limits);
        for chunk in (0..20_000u64).step_by(500) {
            s.highlight_range(chunk, 40, &mut provider(&lines));
            assert!(s.checkpoint_count() <= 33);
            assert!(s.cached_lines() <= 64);
        }
    }

    #[test]
    fn far_jumps_replay_a_bounded_number_of_lines() {
        let limits = SessionLimits { lookback_limit: 200, warmup_lines: 50, checkpoint_interval: 32, ..Default::default() };
        let lines: Vec<String> = (0..10_000).map(|i| format!("x = {i}")).collect();
        let mut s = session("py", limits);
        let mut calls = 0u64;
        let mut p = |n: u64| {
            calls += 1;
            lines.get(n as usize).cloned()
        };
        s.highlight_range(9_000, 10, &mut p);
        assert!(calls < 300, "replayed {calls} lines");
        assert!(s.is_approximate());
    }

    #[test]
    fn plain_and_unknown_languages_produce_no_spans_and_invalidate_works() {
        let lines = vec!["hello".to_owned()];
        let mut s = session("notes.txt", SessionLimits::default());
        assert!(s.highlight_range(0, 1, &mut provider(&lines))[0].1.is_empty());
        let mut s = session("c", SessionLimits::default());
        s.highlight_range(0, 1, &mut provider(&lines));
        assert!(s.cached_lines() > 0);
        s.invalidate_from(0);
        assert_eq!(s.cached_lines(), 0);
        assert_eq!(s.checkpoint_count(), 1);
    }
}

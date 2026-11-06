//! Allocation-free, streaming line normalisation used for hashing and equality checks.
//!
//! Normalisation is described by [`NormalizeOptions`]. The streaming [`LineNormalizer`] can be fed
//! a line in arbitrary chunks (lines may span I/O blocks) and produces the same bytes as the
//! one-shot path. Whitespace means ASCII space, tab, VT and FF. Case folding is Unicode simple
//! lower-casing of valid UTF-8 sequences; invalid bytes pass through unchanged.

/// Which differences are ignored when matching lines.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NormalizeOptions {
    /// Ignore leading and trailing whitespace.
    pub ignore_leading_trailing_whitespace: bool,
    /// Ignore all whitespace (implies the previous option).
    pub ignore_all_whitespace: bool,
    /// Ignore letter case.
    pub ignore_case: bool,
    /// Ignore the kind of line ending (LF / CRLF / CR) and a missing final newline.
    pub ignore_line_endings: bool,
}

impl Default for NormalizeOptions {
    /// Exact comparison of line content, except that line-ending kind is ignored (matching the
    /// existing editor, which splits with `str::lines`).
    fn default() -> Self {
        Self {
            ignore_leading_trailing_whitespace: false,
            ignore_all_whitespace: false,
            ignore_case: false,
            ignore_line_endings: true,
        }
    }
}

impl NormalizeOptions {
    /// True when line text is compared byte-for-byte (no whitespace/case folding).
    #[must_use]
    pub fn is_identity_text(&self) -> bool {
        !self.ignore_leading_trailing_whitespace && !self.ignore_all_whitespace && !self.ignore_case
    }
}

#[inline]
fn is_ws(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | 0x0B | 0x0C)
}

/// Whitespace runs longer than this are treated as content rather than buffered (bounds memory for
/// pathological lines; the rule is deterministic so both sides still agree).
const MAX_PENDING_WS: usize = 64 * 1024;
const OUT_CHUNK: usize = 4096;

/// Streaming normaliser. Reuse one instance per scan; call [`reset`](Self::reset) between lines.
#[derive(Debug)]
pub struct LineNormalizer {
    opts: NormalizeOptions,
    seen_content: bool,
    pending_ws: Vec<u8>,
    carry: [u8; 4],
    carry_len: usize,
}

impl LineNormalizer {
    /// Create a normaliser for `opts`.
    #[must_use]
    pub fn new(opts: NormalizeOptions) -> Self {
        Self { opts, seen_content: false, pending_ws: Vec::new(), carry: [0; 4], carry_len: 0 }
    }

    /// Start a new line.
    pub fn reset(&mut self) {
        self.seen_content = false;
        self.pending_ws.clear();
        self.carry_len = 0;
    }

    /// Feed `chunk`; normalised output is delivered to `out` in pieces.
    pub fn feed(&mut self, chunk: &[u8], out: &mut dyn FnMut(&[u8])) {
        let mut buf = [0u8; OUT_CHUNK + 8];
        let mut n = 0usize;
        let mut i = 0usize;
        macro_rules! put {
            ($slice:expr) => {{
                let s: &[u8] = $slice;
                if n + s.len() > OUT_CHUNK {
                    out(&buf[..n]);
                    n = 0;
                }
                if s.len() > OUT_CHUNK {
                    out(s);
                } else {
                    buf[n..n + s.len()].copy_from_slice(s);
                    n += s.len();
                }
            }};
        }
        // Finish a multi-byte sequence carried over from the previous chunk.
        if self.carry_len > 0 {
            let need = utf8_len(self.carry[0]) - self.carry_len;
            let take = need.min(chunk.len());
            self.carry[self.carry_len..self.carry_len + take].copy_from_slice(&chunk[..take]);
            self.carry_len += take;
            i = take;
            if self.carry_len < utf8_len(self.carry[0]) {
                return; // still incomplete
            }
            let seq = self.carry;
            let len = self.carry_len;
            self.carry_len = 0;
            self.emit_char(&seq[..len], &mut buf, &mut n, out);
        }
        while i < chunk.len() {
            let b = chunk[i];
            if b < 0x80 {
                i += 1;
                if is_ws(b) {
                    if self.opts.ignore_all_whitespace {
                        continue;
                    }
                    if self.opts.ignore_leading_trailing_whitespace {
                        if !self.seen_content {
                            continue;
                        }
                        if self.pending_ws.len() < MAX_PENDING_WS {
                            self.pending_ws.push(b);
                            continue;
                        }
                    }
                }
                self.flush_pending(&mut buf, &mut n, out);
                self.seen_content = true;
                let byte = if self.opts.ignore_case { b.to_ascii_lowercase() } else { b };
                put!(&[byte]);
            } else if self.opts.ignore_case {
                let len = utf8_len(b);
                if i + len > chunk.len() {
                    let have = chunk.len() - i;
                    self.carry[..have].copy_from_slice(&chunk[i..]);
                    self.carry_len = have;
                    i = chunk.len();
                } else {
                    let mut seq = [0u8; 4];
                    seq[..len].copy_from_slice(&chunk[i..i + len]);
                    i += len;
                    self.emit_char(&seq[..len], &mut buf, &mut n, out);
                }
            } else {
                self.flush_pending(&mut buf, &mut n, out);
                self.seen_content = true;
                put!(&[b]);
                i += 1;
            }
        }
        if n > 0 {
            out(&buf[..n]);
        }
    }

    /// End of line: flush carried bytes (trailing whitespace is dropped when trimming).
    pub fn finish(&mut self, out: &mut dyn FnMut(&[u8])) {
        if self.carry_len > 0 {
            let seq = self.carry;
            let len = self.carry_len;
            self.carry_len = 0;
            out(&seq[..len]);
        }
        if !self.opts.ignore_leading_trailing_whitespace && !self.opts.ignore_all_whitespace {
            if !self.pending_ws.is_empty() {
                out(&self.pending_ws);
            }
        }
        self.pending_ws.clear();
        self.seen_content = false;
    }

    fn flush_pending(&mut self, buf: &mut [u8], n: &mut usize, out: &mut dyn FnMut(&[u8])) {
        if self.pending_ws.is_empty() {
            return;
        }
        if *n > 0 {
            out(&buf[..*n]);
            *n = 0;
        }
        out(&self.pending_ws);
        self.pending_ws.clear();
    }

    /// Emit one (possibly invalid) UTF-8 sequence, lower-casing it when valid.
    fn emit_char(&mut self, seq: &[u8], buf: &mut [u8], n: &mut usize, out: &mut dyn FnMut(&[u8])) {
        self.flush_pending(buf, n, out);
        self.seen_content = true;
        let mut tmp = [0u8; 16];
        let encoded: &[u8] = match std::str::from_utf8(seq) {
            Ok(text) => {
                let mut len = 0;
                for c in text.chars().flat_map(char::to_lowercase) {
                    let mut one = [0u8; 4];
                    let s = c.encode_utf8(&mut one);
                    if len + s.len() > tmp.len() {
                        break;
                    }
                    tmp[len..len + s.len()].copy_from_slice(s.as_bytes());
                    len += s.len();
                }
                &tmp[..len]
            }
            Err(_) => seq,
        };
        if *n + encoded.len() > OUT_CHUNK {
            out(&buf[..*n]);
            *n = 0;
        }
        buf[*n..*n + encoded.len()].copy_from_slice(encoded);
        *n += encoded.len();
    }
}

#[inline]
fn utf8_len(lead: u8) -> usize {
    match lead {
        0xC0..=0xDF => 2,
        0xE0..=0xEF => 3,
        0xF0..=0xF7 => 4,
        _ => 1,
    }
}

/// Normalise a whole line into `out` (cleared first), reusing its allocation.
pub fn normalize_into(line: &[u8], opts: NormalizeOptions, out: &mut Vec<u8>) {
    out.clear();
    if opts.is_identity_text() {
        out.extend_from_slice(line);
        return;
    }
    let mut norm = LineNormalizer::new(opts);
    norm.feed(line, &mut |chunk| out.extend_from_slice(chunk));
    norm.finish(&mut |chunk| out.extend_from_slice(chunk));
}

/// Scratch space for [`normalized_eq`]; keep one per thread/scan to avoid per-line allocation.
#[derive(Debug, Default)]
pub struct EqScratch {
    a: Vec<u8>,
    b: Vec<u8>,
}

/// Whether two raw lines are equal after normalisation.
pub fn normalized_eq(a: &[u8], b: &[u8], opts: NormalizeOptions, scratch: &mut EqScratch) -> bool {
    if opts.is_identity_text() {
        return a == b;
    }
    normalize_into(a, opts, &mut scratch.a);
    normalize_into(b, opts, &mut scratch.b);
    scratch.a == scratch.b
}

#[cfg(test)]
mod tests {
    use super::*;

    fn norm(line: &[u8], opts: NormalizeOptions) -> Vec<u8> {
        let mut out = Vec::new();
        normalize_into(line, opts, &mut out);
        out
    }

    fn trim() -> NormalizeOptions {
        NormalizeOptions { ignore_leading_trailing_whitespace: true, ..Default::default() }
    }

    #[test]
    fn trims_and_ignores_whitespace() {
        assert_eq!(norm(b"  a b \t", trim()), b"a b");
        let all = NormalizeOptions { ignore_all_whitespace: true, ..Default::default() };
        assert_eq!(norm(b" a \t b ", all), b"ab");
        assert_eq!(norm(b"   ", trim()), b"");
    }

    #[test]
    fn folds_case_including_unicode() {
        let opts = NormalizeOptions { ignore_case: true, ..Default::default() };
        assert_eq!(norm("ÄÖ Straße".as_bytes(), opts), "äö straße".as_bytes());
    }

    #[test]
    fn chunked_feeding_matches_one_shot() {
        let opts = NormalizeOptions {
            ignore_leading_trailing_whitespace: true,
            ignore_case: true,
            ..Default::default()
        };
        let line = "   ÄÖÜ  hello   wörld \t ".as_bytes();
        let expected = norm(line, opts);
        for step in 1..=line.len() {
            let mut n = LineNormalizer::new(opts);
            let mut out = Vec::new();
            for c in line.chunks(step) {
                n.feed(c, &mut |x| out.extend_from_slice(x));
            }
            n.finish(&mut |x| out.extend_from_slice(x));
            assert_eq!(out, expected, "step {step}");
        }
    }

    #[test]
    fn invalid_utf8_passes_through() {
        let opts = NormalizeOptions { ignore_case: true, ..Default::default() };
        assert_eq!(norm(b"A\xFFB", opts), b"a\xFFb");
    }
}

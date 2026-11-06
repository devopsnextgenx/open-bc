//! Encoding detection and lazy line decoding.
//!
//! Line boundaries are always found on the raw bytes of the file (so byte offsets stay exact);
//! only the lines that are actually displayed or compared character-wise are decoded.
//!
//! Supported: UTF-8 (with or without BOM; invalid sequences decode to U+FFFD per line, never
//! corrupting line boundaries), UTF-16 LE/BE with BOM, and BOM-less UTF-16 when the first KiB is
//! clearly NUL-interleaved ASCII. Other legacy encodings are read as UTF-8 with replacement
//! characters; a future decoder only needs a new [`TextEncoding`] variant.

use std::borrow::Cow;

/// Text encoding of a file.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TextEncoding {
    /// UTF-8 (also used for ASCII and, lossily, for unknown 8-bit text).
    Utf8,
    /// UTF-16, little endian.
    Utf16Le,
    /// UTF-16, big endian.
    Utf16Be,
}

impl TextEncoding {
    /// Short stable name for UI and logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Self::Utf8 => "UTF-8",
            Self::Utf16Le => "UTF-16 LE",
            Self::Utf16Be => "UTF-16 BE",
        }
    }
}

/// Result of [`detect`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Detected {
    /// Chosen encoding.
    pub encoding: TextEncoding,
    /// Number of leading BOM bytes that are not part of the text.
    pub bom_len: u8,
    /// True when the sample contains NUL bytes that are not explained by UTF-16 (likely binary).
    pub looks_binary: bool,
}

/// Detect the encoding from the first bytes of a file.
#[must_use]
pub fn detect(sample: &[u8]) -> Detected {
    let utf8 = |bom_len, looks_binary| Detected { encoding: TextEncoding::Utf8, bom_len, looks_binary };
    if sample.starts_with(&[0xEF, 0xBB, 0xBF]) {
        return utf8(3, false);
    }
    // UTF-32 LE BOM (FF FE 00 00) is not supported; do not mistake it for UTF-16.
    if sample.starts_with(&[0xFF, 0xFE, 0x00, 0x00]) {
        return utf8(0, true);
    }
    if sample.starts_with(&[0xFF, 0xFE]) {
        return Detected { encoding: TextEncoding::Utf16Le, bom_len: 2, looks_binary: false };
    }
    if sample.starts_with(&[0xFE, 0xFF]) {
        return Detected { encoding: TextEncoding::Utf16Be, bom_len: 2, looks_binary: false };
    }
    let window = &sample[..sample.len().min(1024) & !1];
    // Tiny samples are too ambiguous to call UTF-16 (a short binary blob would qualify).
    if window.len() >= 8 {
        let pairs = window.len() / 2;
        let (mut even_nul, mut odd_nul) = (0usize, 0usize);
        for pair in window.chunks_exact(2) {
            even_nul += usize::from(pair[0] == 0);
            odd_nul += usize::from(pair[1] == 0);
        }
        if odd_nul * 10 >= pairs * 3 && even_nul == 0 {
            return Detected { encoding: TextEncoding::Utf16Le, bom_len: 0, looks_binary: false };
        }
        if even_nul * 10 >= pairs * 3 && odd_nul == 0 {
            return Detected { encoding: TextEncoding::Utf16Be, bom_len: 0, looks_binary: false };
        }
    }
    utf8(0, sample.contains(&0))
}

/// Decode one line's raw bytes (without terminator) into text; invalid input becomes U+FFFD.
#[must_use]
pub fn decode_line(bytes: &[u8], encoding: TextEncoding) -> Cow<'_, str> {
    match encoding {
        TextEncoding::Utf8 => String::from_utf8_lossy(bytes),
        TextEncoding::Utf16Le | TextEncoding::Utf16Be => {
            let big = encoding == TextEncoding::Utf16Be;
            let units = bytes.chunks_exact(2).map(|p| {
                if big { u16::from_be_bytes([p[0], p[1]]) } else { u16::from_le_bytes([p[0], p[1]]) }
            });
            Cow::Owned(
                char::decode_utf16(units)
                    .map(|r| r.unwrap_or(char::REPLACEMENT_CHARACTER))
                    .collect(),
            )
        }
    }
}

/// Transcode into `out` (cleared first) as UTF-8, reusing its allocation.
pub fn decode_line_into(bytes: &[u8], encoding: TextEncoding, out: &mut Vec<u8>) {
    out.clear();
    match decode_line(bytes, encoding) {
        Cow::Borrowed(text) => out.extend_from_slice(text.as_bytes()),
        Cow::Owned(text) => out.extend_from_slice(text.as_bytes()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_boms_and_heuristics() {
        assert_eq!(detect(b"\xEF\xBB\xBFhi").bom_len, 3);
        assert_eq!(detect(b"\xFF\xFEh\0i\0").encoding, TextEncoding::Utf16Le);
        assert_eq!(detect(b"\xFE\xFF\0h\0i").encoding, TextEncoding::Utf16Be);
        assert_eq!(detect(b"h\0e\0l\0l\0o\0").encoding, TextEncoding::Utf16Le);
        assert_eq!(detect(b"\0h\0e\0l\0l\0o").encoding, TextEncoding::Utf16Be);
        assert!(detect(b"ab\0cd").looks_binary);
        assert_eq!(detect(b"plain").encoding, TextEncoding::Utf8);
    }

    #[test]
    fn decodes_lossily() {
        assert_eq!(decode_line(b"a\xFFb", TextEncoding::Utf8), "a\u{FFFD}b");
        assert_eq!(decode_line(&[0x3D, 0xD8, 0x00, 0xDE], TextEncoding::Utf16Le), "😀");
        assert_eq!(decode_line(&[0x00, 0x41, 0xD8], TextEncoding::Utf16Be), "A");
    }
}

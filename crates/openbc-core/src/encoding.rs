//! Byte/text codecs shared by the CSV and registry parsers.

/// Decodes UTF-16 bytes (lossy). A trailing odd byte is ignored.
pub(crate) fn utf16_from_bytes(bytes: &[u8], big_endian: bool) -> String {
    let units: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|c| {
            if big_endian {
                u16::from_be_bytes([c[0], c[1]])
            } else {
                u16::from_le_bytes([c[0], c[1]])
            }
        })
        .collect();
    String::from_utf16_lossy(&units)
}

/// Encodes `text` as UTF-16LE preceded by a byte-order mark.
pub(crate) fn utf16le_with_bom(text: &str) -> Vec<u8> {
    let mut out = vec![0xFF, 0xFE];
    out.extend(text.encode_utf16().flat_map(u16::to_le_bytes));
    out
}

/// Decodes bytes to text: honours UTF-8 / UTF-16 BOMs, otherwise lossy UTF-8.
///
/// ```
/// assert_eq!(openbc_core::table_compare::csv::decode_text(b"a,b"), "a,b");
/// ```
pub fn decode_text(bytes: &[u8]) -> String {
    match bytes {
        [0xEF, 0xBB, 0xBF, rest @ ..] => String::from_utf8_lossy(rest).into_owned(),
        [0xFF, 0xFE, rest @ ..] => utf16_from_bytes(rest, false),
        [0xFE, 0xFF, rest @ ..] => utf16_from_bytes(rest, true),
        _ => String::from_utf8_lossy(bytes).into_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn utf16_round_trips_through_decode_text() {
        assert_eq!(decode_text(&utf16le_with_bom("héllo")), "héllo");
    }

    #[test]
    fn honours_utf8_bom_and_falls_back_to_lossy() {
        assert_eq!(decode_text(&[0xEF, 0xBB, 0xBF, b'x']), "x");
        assert_eq!(decode_text(&[b'a', 0xFF]), "a\u{FFFD}");
    }
}

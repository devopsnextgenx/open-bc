//! CSV/TSV text decoding, parsing and writing (RFC 4180 flavour). Pure, no I/O.

pub use crate::encoding::decode_text;

/// Picks the delimiter (`,` `;` tab `|`) that occurs most in the first non-empty line,
/// ignoring quoted text. Falls back to `,`.
pub fn sniff_delimiter(text: &str) -> u8 {
    let line = text.lines().find(|l| !l.trim().is_empty()).unwrap_or("");
    let mut counts = [(b',', 0usize), (b';', 0), (b'\t', 0), (b'|', 0)];
    let mut quoted = false;
    for c in line.bytes() {
        if c == b'"' {
            quoted = !quoted;
        } else if !quoted {
            if let Some(slot) = counts.iter_mut().find(|(d, _)| *d == c) {
                slot.1 += 1;
            }
        }
    }
    counts.iter().max_by_key(|(_, n)| *n).filter(|(_, n)| *n > 0).map_or(b',', |(d, _)| *d)
}

/// Parses delimited text into rows. Blank lines are kept as one empty cell so row
/// numbers keep matching the source file.
pub fn parse_csv(text: &str, delimiter: u8) -> Vec<Vec<String>> {
    let delim = delimiter as char;
    let (mut rows, mut row, mut field) = (Vec::new(), Vec::new(), String::new());
    let (mut quoted, mut pending) = (false, false);
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        if quoted {
            if c == '"' {
                if chars.peek() == Some(&'"') {
                    field.push('"');
                    chars.next();
                } else {
                    quoted = false;
                }
            } else {
                field.push(c);
            }
            continue;
        }
        match c {
            '"' if field.is_empty() => {
                quoted = true;
                pending = true;
            }
            c if c == delim => {
                row.push(std::mem::take(&mut field));
                pending = true;
            }
            '\r' | '\n' => {
                if c == '\r' && chars.peek() == Some(&'\n') {
                    chars.next();
                }
                row.push(std::mem::take(&mut field));
                rows.push(std::mem::take(&mut row));
                pending = false;
            }
            _ => {
                field.push(c);
                pending = true;
            }
        }
    }
    if pending || !field.is_empty() || !row.is_empty() {
        row.push(field);
        rows.push(row);
    }
    rows
}

/// Serialises rows as UTF-8 with CRLF line ends, quoting only where required.
pub fn write_csv(rows: &[Vec<String>], delimiter: u8) -> Vec<u8> {
    let delim = delimiter as char;
    let mut out = String::new();
    for row in rows {
        for (i, cell) in row.iter().enumerate() {
            if i > 0 {
                out.push(delim);
            }
            if cell.contains(delim) || cell.contains(['"', '\r', '\n']) {
                out.push('"');
                out.push_str(&cell.replace('"', "\"\""));
                out.push('"');
            } else {
                out.push_str(cell);
            }
        }
        out.push_str("\r\n");
    }
    out.into_bytes()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_quotes_and_newlines() {
        let rows = vec![vec!["a,b".to_string(), "say \"hi\"".into(), "x\ny".into()], vec!["1".into(), "".into(), "3".into()]];
        let text = String::from_utf8(write_csv(&rows, b',')).unwrap();
        assert_eq!(parse_csv(&text, b','), rows);
    }

    #[test]
    fn sniffs_semicolon() {
        assert_eq!(sniff_delimiter("a;b;c\n1;2;3"), b';');
    }
}

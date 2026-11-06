/// A compact, line-oriented index for a UTF-8-like byte stream.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LineIndex {
    starts: Vec<u64>,
    ends: Vec<u64>,
    count: u64,
}

impl LineIndex {
    /// Build an index by scanning the byte stream once and recording each line start and end.
    pub fn from_bytes(input: &[u8]) -> Self {
        if input.is_empty() {
            return Self { starts: Vec::new(), ends: Vec::new(), count: 0 };
        }

        let mut starts = Vec::new();
        let mut ends = Vec::new();
        let mut cursor = 0usize;
        starts.push(0);

        while cursor < input.len() {
            let byte = input[cursor];
            if byte == b'\n' {
                ends.push(cursor as u64);
                let next = cursor + 1;
                if next < input.len() {
                    starts.push(next as u64);
                }
                cursor += 1;
            } else if byte == b'\r' {
                ends.push(cursor as u64);
                let next = cursor + 1;
                if next < input.len() && input[next] == b'\n' {
                    cursor += 2;
                } else {
                    cursor += 1;
                }
                if cursor < input.len() {
                    starts.push(cursor as u64);
                }
            } else {
                cursor += 1;
            }
        }

        if input[input.len() - 1] != b'\n' && input[input.len() - 1] != b'\r' {
            ends.push(input.len() as u64);
        }

        let count = starts.len().min(ends.len()) as u64;
        Self { starts, ends, count }
    }

    #[must_use]
    pub fn line_count(&self) -> u64 {
        self.count
    }

    /// Returns `(start_offset, byte_length)` for a line in the source.
    #[must_use]
    pub fn line_range(&self, line: u64) -> (u64, u32) {
        if self.count == 0 {
            return (0, 0);
        }
        let index = line.min(self.count - 1) as usize;
        let start = self.starts.get(index).copied().unwrap_or(0);
        let end = self.ends.get(index).copied().unwrap_or(start);
        (start, end.saturating_sub(start) as u32)
    }

    /// Resolve the containing line for a byte offset (binary search).
    #[must_use]
    pub fn line_for_offset(&self, offset: u64) -> Option<u64> {
        if self.count == 0 {
            return None;
        }
        let after = self.starts.partition_point(|&start| start <= offset);
        Some(after.saturating_sub(1) as u64)
    }
}

#[cfg(test)]
mod tests {
    use super::LineIndex;

    #[test]
    fn index_tracks_basic_newlines() {
        let index = LineIndex::from_bytes(b"alpha\nbeta\r\ngamma");
        assert_eq!(index.line_count(), 3);
        assert_eq!(index.line_range(0), (0, 5));
        assert_eq!(index.line_range(1), (6, 4));
        assert_eq!(index.line_for_offset(7), Some(1));
    }

    #[test]
    fn empty_input_has_no_lines() {
        let index = LineIndex::from_bytes(b"");
        assert_eq!(index.line_count(), 0);
        assert_eq!(index.line_for_offset(0), None);
    }
}

//! Bounds-checked fixed-width reads shared by the container walkers. Out-of-range reads
//! yield 0 so a truncated header degrades to "missing value" instead of panicking.

fn array_at<const N: usize>(b: &[u8], at: usize) -> Option<[u8; N]> {
    b.get(at..at.checked_add(N)?)?.try_into().ok()
}

pub(super) fn be32(b: &[u8], at: usize) -> u32 {
    array_at::<4>(b, at).map_or(0, u32::from_be_bytes)
}

pub(super) fn be64(b: &[u8], at: usize) -> u64 {
    array_at::<8>(b, at).map_or(0, u64::from_be_bytes)
}

pub(super) fn le32(b: &[u8], at: usize) -> u32 {
    array_at::<4>(b, at).map_or(0, u32::from_le_bytes)
}

pub(super) fn le16(b: &[u8], at: usize) -> u16 {
    array_at::<2>(b, at).map_or(0, u16::from_le_bytes)
}

/// Printable-ASCII view of a four-character code (non-printables become `.`).
pub(super) fn fourcc(b: &[u8]) -> String {
    b.iter()
        .map(|&c| if (32..127).contains(&c) { c as char } else { '.' })
        .collect::<String>()
        .trim()
        .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_both_endiannesses() {
        let b = [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08];
        assert_eq!(be32(&b, 0), 0x0102_0304);
        assert_eq!(le32(&b, 0), 0x0403_0201);
        assert_eq!(le16(&b, 2), 0x0403);
        assert_eq!(be64(&b, 0), 0x0102_0304_0506_0708);
    }

    #[test]
    fn out_of_range_reads_are_zero() {
        let b = [1u8, 2, 3];
        assert_eq!((be32(&b, 0), be64(&b, 0), le32(&b, 1), le16(&b, 2)), (0, 0, 0, 0));
        assert_eq!(be32(&b, usize::MAX), 0);
    }

    #[test]
    fn fourcc_masks_non_printables() {
        assert_eq!(fourcc(b"avc1"), "avc1");
        assert_eq!(fourcc(&[b'a', 0, b'b', b' ']), "a.b");
    }
}

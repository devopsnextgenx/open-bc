//! 64-bit SimHash over byte 4-grams: a cheap fuzzy hash for function bodies.
//!
//! Two byte sequences that share most of their 4-grams produce hashes with a small Hamming
//! distance, so [`similarity`] tells "one instruction changed" (~0.95+) from "rewritten"
//! (~0.5, the value for unrelated data).

use xxhash_rust::xxh3::xxh3_64;

/// SimHash of `bytes`. Sequences shorter than 4 bytes hash as a single gram.
#[must_use]
pub fn simhash(bytes: &[u8]) -> u64 {
    let mut acc = [0i32; 64];
    let mut add = |gram: &[u8]| {
        let h = xxh3_64(gram);
        for (bit, slot) in acc.iter_mut().enumerate() {
            *slot += if (h >> bit) & 1 == 1 { 1 } else { -1 };
        }
    };
    if bytes.len() < 4 {
        add(bytes);
    } else {
        for gram in bytes.windows(4) {
            add(gram);
        }
    }
    acc.iter().enumerate().fold(0u64, |out, (bit, v)| if *v > 0 { out | (1 << bit) } else { out })
}

/// Similarity in `0.0..=1.0` derived from the Hamming distance of two SimHashes.
#[must_use]
pub fn similarity(a: u64, b: u64) -> f64 {
    1.0 - f64::from((a ^ b).count_ones()) / 64.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identical_is_one_and_small_edit_is_close() {
        let base: Vec<u8> = (0..400u32).map(|i| (i.wrapping_mul(2654435761) >> 13) as u8).collect();
        let mut edited = base.clone();
        edited[200] ^= 0xFF;
        assert!((similarity(simhash(&base), simhash(&base)) - 1.0).abs() < f64::EPSILON);
        assert!(similarity(simhash(&base), simhash(&edited)) > 0.85);
        let other: Vec<u8> = (0..400u32).map(|i| (i.wrapping_mul(40503) >> 7) as u8 ^ 0x5A).collect();
        assert!(similarity(simhash(&base), simhash(&other)) < 0.8);
    }

    #[test]
    fn tiny_inputs_do_not_panic() {
        let _ = simhash(&[]);
        let _ = simhash(&[1]);
        let _ = simhash(&[1, 2, 3]);
    }
}

//! Line hashing. XXH3-64 is the default; the trait lets tests inject a colliding hasher.
//!
//! A hash is only an *accelerator*: equal hashes are always verified against the actual (normalised)
//! bytes before two lines are treated as equal. XXH3 is not cryptographic and does not need to be.

use xxhash_rust::xxh3::{xxh3_64, Xxh3};

/// Incremental hashing state for one line.
pub trait HashStream {
    /// Feed more bytes.
    fn update(&mut self, bytes: &[u8]);
    /// Return the digest and reset for the next line.
    fn finish_reset(&mut self) -> u64;
}

/// A line hash function. One-shot and streaming results **must** agree for the same bytes.
pub trait LineHasher: Send + Sync {
    /// Hash a complete byte string.
    fn hash_bytes(&self, bytes: &[u8]) -> u64;
    /// Create a streaming state.
    fn new_stream(&self) -> Box<dyn HashStream>;
}

/// XXH3 64-bit.
#[derive(Clone, Copy, Debug, Default)]
pub struct Xxh3Hasher;

struct Xxh3Stream(Xxh3);

impl HashStream for Xxh3Stream {
    fn update(&mut self, bytes: &[u8]) {
        self.0.update(bytes);
    }
    fn finish_reset(&mut self) -> u64 {
        let digest = self.0.digest();
        self.0.reset();
        digest
    }
}

impl LineHasher for Xxh3Hasher {
    fn hash_bytes(&self, bytes: &[u8]) -> u64 {
        xxh3_64(bytes)
    }
    fn new_stream(&self) -> Box<dyn HashStream> {
        Box::new(Xxh3Stream(Xxh3::new()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn streaming_matches_one_shot() {
        let h = Xxh3Hasher;
        let data: Vec<u8> = (0..10_000u32).map(|i| (i % 251) as u8).collect();
        let mut s = h.new_stream();
        for chunk in data.chunks(37) {
            s.update(chunk);
        }
        assert_eq!(s.finish_reset(), h.hash_bytes(&data));
        // reset really resets
        s.update(b"abc");
        assert_eq!(s.finish_reset(), h.hash_bytes(b"abc"));
    }
}

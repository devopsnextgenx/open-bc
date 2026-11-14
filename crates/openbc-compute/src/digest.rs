//! Content digests for every [`HashAlgorithm`] the folder-compare policy can select.
//!
//! The functions are CPU-only and allocation-light. [`crate::ComputeDispatcher::hash_bytes_with`]
//! runs them on the shared Rayon pool; they can also be called directly (the binary extractor
//! does, for per-section and per-function hashes).

use md5::Md5;
use openbc_core::HashAlgorithm;
use sha1::Sha1;
use sha2::{Digest, Sha256, Sha512};

/// Digest of `bytes` with `algorithm`.
#[must_use]
pub fn digest_bytes(algorithm: HashAlgorithm, bytes: &[u8]) -> Vec<u8> {
    match algorithm {
        HashAlgorithm::Md5 => Md5::digest(bytes).to_vec(),
        HashAlgorithm::Sha1 => Sha1::digest(bytes).to_vec(),
        HashAlgorithm::Sha256 => Sha256::digest(bytes).to_vec(),
        HashAlgorithm::Sha512 => Sha512::digest(bytes).to_vec(),
    }
}

/// Lower-case hexadecimal rendering of a digest.
#[must_use]
pub fn to_hex(digest: &[u8]) -> String {
    use std::fmt::Write;
    let mut out = String::with_capacity(digest.len() * 2);
    for b in digest {
        let _ = write!(out, "{b:02x}");
    }
    out
}

/// Hex digest of `bytes` with `algorithm`.
#[must_use]
pub fn hex_digest(algorithm: HashAlgorithm, bytes: &[u8]) -> String {
    to_hex(&digest_bytes(algorithm, bytes))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_vectors_for_abc() {
        assert_eq!(hex_digest(HashAlgorithm::Md5, b"abc"), "900150983cd24fb0d6963f7d28e17f72");
        assert_eq!(hex_digest(HashAlgorithm::Sha1, b"abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
        assert_eq!(
            hex_digest(HashAlgorithm::Sha256, b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert!(hex_digest(HashAlgorithm::Sha512, b"abc").starts_with("ddaf35a193617aba"));
    }

    #[test]
    fn lengths_match_algorithm() {
        for a in HashAlgorithm::ALL {
            assert_eq!(digest_bytes(a, b"x").len(), a.digest_len());
        }
    }
}

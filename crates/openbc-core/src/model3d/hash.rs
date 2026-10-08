//! Cheap multiplicative hasher for integer keys (vertex welding, edge maps).

use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};

/// Fx-style hasher; not DoS-resistant, intended for trusted integer keys only.
#[derive(Default)]
pub struct FxHasher(u64);

impl Hasher for FxHasher {
    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_u64(b as u64);
        }
    }

    fn write_u64(&mut self, i: u64) {
        self.0 = (self.0.rotate_left(5) ^ i).wrapping_mul(0x517c_c1b7_2722_0a95);
    }

    fn write_i64(&mut self, i: i64) {
        self.write_u64(i as u64);
    }

    fn finish(&self) -> u64 {
        self.0
    }
}

/// `HashMap` keyed with [`FxHasher`].
pub type FxMap<K, V> = HashMap<K, V, BuildHasherDefault<FxHasher>>;

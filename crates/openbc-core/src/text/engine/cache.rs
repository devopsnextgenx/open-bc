//! A small bounded cache with O(1) operations and a hard entry limit.
//!
//! Two generations ("clock-pro lite"): inserts go to the young map; when it fills up the old map is
//! discarded and the young one becomes old. A hit in the old map promotes the entry. The total
//! number of entries never exceeds `capacity`, so memory is bounded without per-entry bookkeeping.

use std::collections::HashMap;
use std::hash::Hash;

/// Bounded recently-used cache.
#[derive(Debug)]
pub struct BoundedCache<K, V> {
    young: HashMap<K, V>,
    old: HashMap<K, V>,
    half: usize,
}

impl<K: Eq + Hash + Clone, V: Clone> BoundedCache<K, V> {
    /// Create a cache that holds at most `capacity` entries (minimum 2).
    #[must_use]
    pub fn new(capacity: usize) -> Self {
        Self { young: HashMap::new(), old: HashMap::new(), half: (capacity / 2).max(1) }
    }

    /// Look up a value, promoting it when it was in the old generation.
    pub fn get(&mut self, key: &K) -> Option<V> {
        if let Some(value) = self.young.get(key) {
            return Some(value.clone());
        }
        if let Some(value) = self.old.remove(key) {
            self.insert(key.clone(), value.clone());
            return Some(value);
        }
        None
    }

    /// Insert or replace an entry.
    pub fn insert(&mut self, key: K, value: V) {
        if self.young.len() >= self.half && !self.young.contains_key(&key) {
            self.old = std::mem::take(&mut self.young);
        }
        self.young.insert(key, value);
    }

    /// Number of cached entries.
    #[must_use]
    pub fn len(&self) -> usize {
        self.young.len() + self.old.len()
    }

    /// True when empty.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Hard upper bound on [`len`](Self::len).
    #[must_use]
    pub fn capacity(&self) -> usize {
        self.half * 2
    }

    /// Drop every entry.
    pub fn clear(&mut self) {
        self.young.clear();
        self.old.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::BoundedCache;

    #[test]
    fn never_exceeds_capacity_and_keeps_hot_entries() {
        let mut cache = BoundedCache::new(8);
        for i in 0..1000u32 {
            cache.insert(i, i);
            assert!(cache.len() <= cache.capacity());
            // Keep entry 0 hot.
            if i % 2 == 0 {
                cache.insert(0, 0);
            }
        }
        assert_eq!(cache.get(&0), Some(0));
        assert_eq!(cache.get(&1), None);
    }
}

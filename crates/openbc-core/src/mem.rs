#![warn(dead_code, unused)]

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

struct CountingAllocator;

static CURRENT_BYTES: AtomicUsize = AtomicUsize::new(0);
static PEAK_BYTES: AtomicUsize = AtomicUsize::new(0);

#[global_allocator]
static GLOBAL: CountingAllocator = CountingAllocator;

// Feature gating keeps measurement overhead out of normal builds.
// SAFETY: Each operation delegates the caller-provided valid layout and pointer to `System`;
// counters observe sizes only and do not alter allocation ownership or lifetimes.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // SAFETY: Delegating the original layout unchanged to the system allocator.
        let allocation = unsafe { System.alloc(layout) };
        if !allocation.is_null() {
            record_allocation(layout.size());
        }
        allocation
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        // SAFETY: The pointer and layout are passed through unchanged from GlobalAlloc's caller.
        unsafe { System.dealloc(pointer, layout) };
        CURRENT_BYTES.fetch_sub(layout.size(), Ordering::Relaxed);
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        // SAFETY: The pointer and original layout are passed through unchanged from the caller.
        let allocation = unsafe { System.realloc(pointer, layout, new_size) };
        if !allocation.is_null() {
            let current = if new_size >= layout.size() {
                CURRENT_BYTES.fetch_add(new_size - layout.size(), Ordering::Relaxed)
                    + (new_size - layout.size())
            } else {
                CURRENT_BYTES.fetch_sub(layout.size() - new_size, Ordering::Relaxed)
                    - (layout.size() - new_size)
            };
            record_peak(current);
        }
        allocation
    }
}

fn record_allocation(size: usize) {
    let current = CURRENT_BYTES.fetch_add(size, Ordering::Relaxed) + size;
    record_peak(current);
}

fn record_peak(current: usize) {
    let mut peak = PEAK_BYTES.load(Ordering::Relaxed);
    while current > peak {
        match PEAK_BYTES.compare_exchange_weak(peak, current, Ordering::Relaxed, Ordering::Relaxed)
        {
            Ok(_) => break,
            Err(observed) => peak = observed,
        }
    }
}

/// Return live Rust allocations observed by the feature-gated global allocator.
#[must_use]
pub fn current_bytes() -> u64 {
    CURRENT_BYTES.load(Ordering::Relaxed) as u64
}

/// Return the high-water mark of Rust allocations since process start.
#[must_use]
pub fn peak_bytes() -> u64 {
    PEAK_BYTES.load(Ordering::Relaxed) as u64
}

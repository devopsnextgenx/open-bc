#![warn(dead_code, unused)]

/// Return current Rust allocator usage when the `mem-stats` feature is enabled.
#[no_mangle]
pub extern "C" fn openbc_rust_allocated_bytes() -> u64 {
    openbc_core::mem::current_bytes()
}

/// Return peak Rust allocator usage when the `mem-stats` feature is enabled.
#[no_mangle]
pub extern "C" fn openbc_rust_peak_allocated_bytes() -> u64 {
    openbc_core::mem::peak_bytes()
}

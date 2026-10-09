//! Shared plumbing for the per-tab compare actors (table, registry, hex, media, ...).
//!
//! Every actor follows the image-compare shape: one actor per tab, commands in over a
//! small bounded channel, `*Event`s out over a bounded `tokio::sync::mpsc` channel (so a
//! slow UI back-pressures the actor), and all CPU-bound work on Rayon.
//!
//! * [`runtime`]: channel capacities, [`CommandHandle`], the [`BatchedActor`] loop and the
//!   Rayon bridges.
//! * [`source`]: [`ByteSource`] (where compare inputs come from) and its local-FS impl.
//! * [`util`]: [`Side`], error-context helpers and the load/save helpers the actors share.
//!
//! Everything is re-exported here, so `openbc_engine::common::X` paths are stable
//! regardless of which file `X` lives in.

mod runtime;
mod source;
mod util;

pub use runtime::*;
pub use source::*;
pub use util::*;

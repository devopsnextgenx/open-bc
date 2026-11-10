//! Qt-independent text comparison engine.
//!
//! ```text
//!   TextSource (mmap / buffered)  ->  LineIndex (24-byte records, xxh3)  ->  DiffEngine
//!                                                                              |
//!                                    TextCompareModel  <-  DiffIndex (run-length segments)
//! ```
//!
//! Nothing here depends on Qt or any GUI type: both the `QPlainTextEdit` editor and the virtual
//! editor (and a future CLI) are presentation layers over [`TextCompareModel`].

pub mod cache;
pub mod diff;
pub mod diff_index;
pub mod encoding;
pub mod error;
pub mod hasher;
pub mod instrument;
pub mod line_index;
pub mod line_norm;
pub mod model;
pub mod rules;
pub mod source;

pub use cache::BoundedCache;
pub use diff::{DiffConfig, DiffEngine, Side};
pub use diff_index::{ChangeBlock, DiffIndex, DiffRow, DiffStats, DiffType, Segment};
pub use encoding::TextEncoding;
pub use error::{CancelToken, EngineError, Phase, Progress};
pub use hasher::{HashStream, LineHasher, Xxh3Hasher};
pub use instrument::{enter_session, record_failed_operation, record_model_operation, SessionGuard};
pub use line_index::{flags, IndexConfig, LineIndex, LineRecord, MAX_LINES};
pub use rules::{LineRules, MAX_RULE_LINE_BYTES};
pub use model::{CompareConfig, EditOutcome, FindOptions, LineWindow, Match, MemoryReport, SaveReport, SideId, TextCompareModel};
pub use line_norm::{normalized_eq, EqScratch, LineNormalizer, NormalizeOptions};
pub use source::{EditableSource, MapPolicy, MappedTextFile, MemorySource, TextSource};
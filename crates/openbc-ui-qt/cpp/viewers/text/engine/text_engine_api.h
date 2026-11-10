// text_engine_api.h
// C ABI of the Qt-independent text comparison engine (Rust: src/ffi/text_engine.rs).
// Layouts must stay byte-for-byte in sync with the #[repr(C)] structs there.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

struct OpenBcTmModel;
struct OpenBcTmCancel;
struct OpenBcTmSyntax;

struct OpenBcTmOptions {
    std::uint8_t ignoreWhitespaceEdges;
    std::uint8_t ignoreAllWhitespace;
    std::uint8_t ignoreCase;
    std::uint8_t ignoreLineEndings;
    std::uint8_t bufferedIo;
    std::uint8_t reserved[3];
    std::uint64_t ramRecordLimit;        // 0 = default
    std::uint32_t inlineCacheEntries;    // 0 = default
    std::uint32_t inlineMaxLineBytes;    // 0 = default
    const char* sessionId;               // UTF-8, NUL-terminated id of the owning tab (instrumentation); may be null
};

struct OpenBcTmRow {
    std::int64_t left;    // -1: no left line
    std::int64_t right;   // -1: no right line
    std::uint8_t kind;    // 0 equal, 1 insert, 2 delete, 3 replace
};

struct OpenBcTmWindow {
    std::uint32_t written;
    std::uint32_t lineBytes;
    std::uint64_t startChar;
    std::uint8_t moreAfter;
    std::uint8_t bufferTruncated;
};

struct OpenBcTmRange {
    std::uint32_t start;   // UTF-8 byte offset in the decoded line
    std::uint32_t len;
    std::uint8_t kind;     // 0 equal, 1 mismatch, 2 whitespace-only
};

struct OpenBcTmStats {
    std::uint64_t rows, leftLines, rightLines, equalRows, insertedRows, deletedRows, replacedRows, blocks;
    std::uint8_t degraded;
};

struct OpenBcTmMatch {
    std::uint64_t line, start, len;
};

struct OpenBcTmMemory {
    std::uint64_t indexBytes, indexResidentBytes, diffBytes, inlineCacheEntries, editBytes;
};

struct OpenBcTmEdit {
    std::uint64_t firstRow, oldRows, newRows, firstLine, insertedLines;
    std::uint8_t side;
};

struct OpenBcTmSpan {
    std::uint64_t line;
    std::uint32_t start;
    std::uint32_t length;
    std::uint32_t foreground;   // 0xRRGGBBAA
    std::uint32_t background;
    std::uint8_t flags;         // bit0 bold, bit1 italic
};

using OpenBcTmCallback = void (*)(void* user, std::int32_t kind, const char* message, void* handle);
using OpenBcTmLineProvider = std::int64_t (*)(void* user, std::uint64_t line, std::uint8_t* buf, std::size_t cap);

constexpr std::int32_t kOpenBcTmEvResult = 1;
constexpr std::int32_t kOpenBcTmEvError = 2;
constexpr std::int32_t kOpenBcTmEvProgress = 5;

OpenBcTmCancel* openbc_tm_cancel_new();
void openbc_tm_cancel(const OpenBcTmCancel*);
void openbc_tm_cancel_destroy(OpenBcTmCancel*);

void openbc_tm_open_async(const char* left, const char* right, const OpenBcTmOptions* opts,
                          const OpenBcTmCancel* cancel, OpenBcTmCallback cb, void* user);
OpenBcTmModel* openbc_tm_open_sync(const char* left, const char* right, const OpenBcTmOptions* opts);
const std::uint8_t* openbc_tm_last_error(std::size_t* len);
void openbc_tm_destroy(OpenBcTmModel*);

std::uint64_t openbc_tm_row_count(const OpenBcTmModel*);
std::uint8_t openbc_tm_row(const OpenBcTmModel*, std::uint64_t row, OpenBcTmRow* out);
std::uint64_t openbc_tm_line_count(const OpenBcTmModel*, std::uint8_t side);
std::int64_t openbc_tm_line_bytes(const OpenBcTmModel*, std::uint8_t side, std::uint64_t line);
std::uint8_t openbc_tm_line_window(const OpenBcTmModel*, std::uint8_t side, std::uint64_t line,
                                   std::uint64_t startChar, std::uint32_t maxChars, std::uint8_t* buf,
                                   std::size_t cap, OpenBcTmWindow* out);
std::int32_t openbc_tm_inline(const OpenBcTmModel*, std::uint64_t row, std::uint8_t side,
                              OpenBcTmRange* out, std::size_t cap);
std::uint8_t openbc_tm_next_diff(const OpenBcTmModel*, std::uint64_t row, std::uint64_t* out);
std::uint8_t openbc_tm_prev_diff(const OpenBcTmModel*, std::uint64_t row, std::uint64_t* out);
std::uint8_t openbc_tm_row_for_line(const OpenBcTmModel*, std::uint8_t side, std::uint64_t line, std::uint64_t* out);
void openbc_tm_stats(const OpenBcTmModel*, OpenBcTmStats* out);
void openbc_tm_overview(const OpenBcTmModel*, std::size_t buckets, std::uint8_t* out);
void openbc_tm_memory(const OpenBcTmModel*, OpenBcTmMemory* out);
std::uint8_t openbc_tm_encoding(const OpenBcTmModel*, std::uint8_t side);
std::uint8_t openbc_tm_verify(const OpenBcTmModel*);
std::int32_t openbc_tm_find(const OpenBcTmModel*, std::uint8_t side, const std::uint8_t* needle,
                            std::size_t needleLen, std::uint8_t caseSensitive, std::int64_t startLine,
                            std::uint64_t startCol, std::uint8_t forward, const OpenBcTmCancel* cancel,
                            OpenBcTmMatch* out);

// ---- editing -----------------------------------------------------------------------------------
// Return 1 = done, 0 = nothing to do, -1 = error (openbc_tm_last_error).
std::int32_t openbc_tm_replace_lines(const OpenBcTmModel*, std::uint8_t side, std::uint64_t firstLine,
                                     std::uint64_t remove, const std::uint8_t* text, std::size_t len,
                                     std::uint8_t coalesce, OpenBcTmEdit* out);
std::int32_t openbc_tm_undo(const OpenBcTmModel*, std::uint8_t side, OpenBcTmEdit* out);
std::int32_t openbc_tm_redo(const OpenBcTmModel*, std::uint8_t side, OpenBcTmEdit* out);
std::uint8_t openbc_tm_can_undo(const OpenBcTmModel*, std::uint8_t side);
std::uint8_t openbc_tm_can_redo(const OpenBcTmModel*, std::uint8_t side);
void openbc_tm_seal_undo(const OpenBcTmModel*, std::uint8_t side);
std::uint64_t openbc_tm_edit_count(const OpenBcTmModel*, std::uint8_t side);
std::uint8_t openbc_tm_is_dirty(const OpenBcTmModel*, std::uint8_t side);
std::int32_t openbc_tm_copy_block(const OpenBcTmModel*, std::uint8_t fromSide, std::uint64_t row, OpenBcTmEdit* out);
std::uint8_t openbc_tm_block_at(const OpenBcTmModel*, std::uint64_t row, std::uint64_t* start, std::uint64_t* len);
std::size_t openbc_tm_path(const OpenBcTmModel*, std::uint8_t side, std::uint8_t* buf, std::size_t cap);
std::size_t openbc_tm_rules_text(const OpenBcTmModel*, std::uint8_t* buf, std::size_t cap);
std::uint8_t openbc_tm_line_cursors(const OpenBcTmModel*, std::uint64_t row, std::uint64_t* left, std::uint64_t* right);

constexpr std::uint8_t kOpenBcTmTaskSetRules = 0;
constexpr std::uint8_t kOpenBcTmTaskRediff = 1;
constexpr std::uint8_t kOpenBcTmTaskSave = 2;
constexpr std::uint8_t kOpenBcTmTaskSaveAs = 3;
// Long operations on a worker thread; events like openbc_tm_open_async (EV_RESULT carries the
// written path for saves). The model is kept alive by the task.
void openbc_tm_task_async(const OpenBcTmModel*, std::uint8_t task, std::uint8_t side, const std::uint8_t* text,
                          std::size_t len, const OpenBcTmCancel* cancel, OpenBcTmCallback cb, void* user);

OpenBcTmSyntax* openbc_syn_new(const std::uint8_t* language, std::size_t len, std::uint8_t dark,
                               std::uint32_t spanCacheLines);
std::size_t openbc_syn_language_name(const std::uint8_t* name, std::size_t len, std::uint8_t* buf, std::size_t cap);
void openbc_syn_destroy(OpenBcTmSyntax*);
void openbc_syn_invalidate(const OpenBcTmSyntax*, std::uint64_t line);
std::int64_t openbc_syn_range(const OpenBcTmSyntax*, const OpenBcTmModel*, std::uint8_t side,
                              std::uint64_t first, std::uint64_t count, OpenBcTmSpan* out, std::size_t cap);
std::int64_t openbc_syn_range_cb(const OpenBcTmSyntax*, std::uint64_t first, std::uint64_t count,
                                 OpenBcTmLineProvider provider, void* user, OpenBcTmSpan* out, std::size_t cap);

}  // extern "C"

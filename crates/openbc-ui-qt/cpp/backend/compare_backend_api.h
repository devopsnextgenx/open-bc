// compare_backend_api.h
// ---------------------------------------------------------------------------
// C ABI of openbc-ui-qt for the Table / Registry / Hex / Media compare tabs
// (table_compare_ffi.rs, registry_compare_ffi.rs, hex_compare_ffi.rs,
// media_compare_ffi.rs). Pure declarations: every file is loaded, parsed and
// compared by openbc-engine / openbc-compute / openbc-core. Strings are
// (pointer, length) pairs, not NUL-terminated, valid while their handle lives.
// Contract: see ffi_common.rs.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// Event kinds delivered through OpenBcEventCallback.
enum OpenBcEventKind {
    OPENBC_EV_STATUS = 0,    // message
    OPENBC_EV_RESULT = 1,    // handle: view / summary (owned by receiver)
    OPENBC_EV_ERROR = 2,     // message
    OPENBC_EV_SAVED = 3,     // message: "<side 0|1>|<path>"
    OPENBC_EV_WINDOW = 4,    // handle: hex window (owned by receiver)
    OPENBC_EV_PROGRESS = 5,  // message: "<done>|<total>"
};

// Runs on a Rust worker thread. `message` is only valid during the call.
typedef void (*OpenBcEventCallback)(void* user, int32_t kind, const char* message, void* handle);

// ------------------------------------------------------------------ table
struct OpenBcTableSession;
struct OpenBcTableView;
OpenBcTableSession* openbc_table_session_create(const char* left, const char* right,
                                                OpenBcEventCallback cb, void* user);
void openbc_table_session_destroy(OpenBcTableSession*);
uint8_t openbc_table_set_options(const OpenBcTableSession*, uint8_t ignoreCase, uint8_t trim,
                                 double tolerance, const uint32_t* keys, size_t keyCount);
uint8_t openbc_table_select_sheets(const OpenBcTableSession*, int32_t left, int32_t right);
uint8_t openbc_table_reload(const OpenBcTableSession*);
uint8_t openbc_table_copy(const OpenBcTableSession*, size_t start, size_t end, uint8_t toRight);
uint8_t openbc_table_save(const OpenBcTableSession*, uint8_t left, const char* pathOrNull);
void openbc_table_view_destroy(OpenBcTableView*);
size_t openbc_table_view_rows(const OpenBcTableView*);
size_t openbc_table_view_columns(const OpenBcTableView*);
uint8_t openbc_table_row_kind(const OpenBcTableView*, size_t row);  // 0 eq 1 chg 2 left 3 right
uint32_t openbc_table_row_number(const OpenBcTableView*, size_t row, uint8_t side);
const uint8_t* openbc_table_cell(const OpenBcTableView*, size_t row, uint8_t side, size_t col,
                                 size_t* len);
uint8_t openbc_table_cell_changed(const OpenBcTableView*, size_t row, size_t col);
size_t openbc_table_group_count(const OpenBcTableView*);
size_t openbc_table_group_start(const OpenBcTableView*, size_t group);
size_t openbc_table_group_end(const OpenBcTableView*, size_t group);
size_t openbc_table_sheet_count(const OpenBcTableView*, uint8_t side);
const uint8_t* openbc_table_sheet_name(const OpenBcTableView*, uint8_t side, size_t index,
                                       size_t* len);
int32_t openbc_table_selected_sheet(const OpenBcTableView*, uint8_t side);
uint32_t openbc_table_side_flags(const OpenBcTableView*, uint8_t side);  // 1 loaded 2 editable 4 dirty
const uint8_t* openbc_table_side_error(const OpenBcTableView*, uint8_t side, size_t* len);
/* Replace one cell of `side` (0 left / 1 right). `row` = view row index, `col` = column.
   Returns 1 if the command was accepted, 0 if the session is busy (same contract as openbc_table_copy). */
uint8_t openbc_table_set_cell(OpenBcTableSession*, uint8_t side, size_t row, size_t col, const char* utf8_text);
/* Non-zero when this side's row was changed since load / last save (typed edit OR copied in). */
uint8_t openbc_table_row_edited(const OpenBcTableView*, size_t row, uint8_t side);

// --------------------------------------------------------------- registry
struct OpenBcRegistrySession;
struct OpenBcRegistryView;
OpenBcRegistrySession* openbc_registry_session_create(const char* left, const char* right,
                                                      OpenBcEventCallback cb, void* user);
void openbc_registry_session_destroy(OpenBcRegistrySession*);
uint8_t openbc_registry_set_options(const OpenBcRegistrySession*, uint8_t ignoreDataCase);
uint8_t openbc_registry_reload(const OpenBcRegistrySession*);
uint8_t openbc_registry_copy(const OpenBcRegistrySession*, size_t start, size_t end,
                             uint8_t toRight);
uint8_t openbc_registry_save(const OpenBcRegistrySession*, uint8_t left, const char* pathOrNull);
void openbc_registry_view_destroy(OpenBcRegistryView*);
size_t openbc_registry_view_rows(const OpenBcRegistryView*);
uint8_t openbc_registry_row_kind(const OpenBcRegistryView*, size_t row);
const uint8_t* openbc_registry_row_key(const OpenBcRegistryView*, size_t row, size_t* len);
const uint8_t* openbc_registry_row_name(const OpenBcRegistryView*, size_t row, size_t* len);  // null = key row
const uint8_t* openbc_registry_row_type(const OpenBcRegistryView*, size_t row, uint8_t side,
                                        size_t* len);  // null = absent
const uint8_t* openbc_registry_row_data(const OpenBcRegistryView*, size_t row, uint8_t side,
                                        size_t* len);
size_t openbc_registry_group_count(const OpenBcRegistryView*);
size_t openbc_registry_group_start(const OpenBcRegistryView*, size_t group);
size_t openbc_registry_group_end(const OpenBcRegistryView*, size_t group);
uint32_t openbc_registry_side_flags(const OpenBcRegistryView*, uint8_t side);
const uint8_t* openbc_registry_side_error(const OpenBcRegistryView*, uint8_t side, size_t* len);
uint8_t openbc_registry_set_value(OpenBcRegistrySession*, size_t row, uint8_t side, const char* utf8_data);
uint8_t openbc_registry_row_edited(const OpenBcRegistryView*, size_t row, uint8_t side);

// -------------------------------------------------------------------- hex
struct OpenBcHexSession;
struct OpenBcHexSummary;
struct OpenBcHexWindow;
OpenBcHexSession* openbc_hex_session_create(const char* left, const char* right,
                                            OpenBcEventCallback cb, void* user);
void openbc_hex_session_destroy(OpenBcHexSession*);
uint8_t openbc_hex_reload(const OpenBcHexSession*);
uint8_t openbc_hex_request_window(const OpenBcHexSession*, uint64_t id, uint64_t offset,
                                  size_t len);
void openbc_hex_summary_destroy(OpenBcHexSummary*);
uint64_t openbc_hex_summary_size(const OpenBcHexSummary*, uint8_t side);
size_t openbc_hex_summary_run_count(const OpenBcHexSummary*);
uint64_t openbc_hex_summary_run_offset(const OpenBcHexSummary*, size_t i);
uint64_t openbc_hex_summary_run_len(const OpenBcHexSummary*, size_t i);
int64_t openbc_hex_summary_run_at(const OpenBcHexSummary*, uint64_t offset);    // -1 none
int64_t openbc_hex_summary_next_run(const OpenBcHexSummary*, uint64_t offset);  // -1 none
int64_t openbc_hex_summary_prev_run(const OpenBcHexSummary*, uint64_t offset);  // -1 none
uint8_t openbc_hex_summary_truncated(const OpenBcHexSummary*);
uint64_t openbc_hex_summary_diff_bytes(const OpenBcHexSummary*);
const uint8_t* openbc_hex_summary_error(const OpenBcHexSummary*, uint8_t side, size_t* len);
void openbc_hex_window_destroy(OpenBcHexWindow*);
uint64_t openbc_hex_window_id(const OpenBcHexWindow*);
uint64_t openbc_hex_window_offset(const OpenBcHexWindow*);
const uint8_t* openbc_hex_window_bytes(const OpenBcHexWindow*, uint8_t side, size_t* len);

// ------------------------------------------------------------------ media
struct OpenBcMediaSession;
struct OpenBcMediaView;
struct OpenBcAudioPeak {
    float minimum;
    float maximum;
};
OpenBcMediaSession* openbc_media_session_create(const char* left, const char* right,
                                                OpenBcEventCallback cb, void* user);
void openbc_media_session_destroy(OpenBcMediaSession*);
uint8_t openbc_media_reload(const OpenBcMediaSession*);
void openbc_media_view_destroy(OpenBcMediaView*);
size_t openbc_media_view_rows(const OpenBcMediaView*);
// field: 0 group, 1 key, 2 left value, 3 right value (null = absent on that side)
const uint8_t* openbc_media_row_text(const OpenBcMediaView*, size_t row, uint8_t field,
                                     size_t* len);
uint32_t openbc_media_row_flags(const OpenBcMediaView*, size_t row);  // 1 differs 2 informational
const uint8_t* openbc_media_side_error(const OpenBcMediaView*, uint8_t side, size_t* len);
uint8_t openbc_media_side_present(const OpenBcMediaView*, uint8_t side);
const uint8_t* openbc_media_side_cover(const OpenBcMediaView* v, uint8_t side, size_t* len);
uint8_t openbc_media_audio_present(const OpenBcMediaView*);
const uint8_t* openbc_media_audio_error(const OpenBcMediaView*, size_t* len);
// Audio waveform side: 0 left, 1 right, 2 absolute difference.
uint64_t openbc_media_audio_frame_count(const OpenBcMediaView*, uint8_t side);
uint32_t openbc_media_audio_sample_rate(const OpenBcMediaView*, uint8_t side);
size_t openbc_media_audio_level_count(const OpenBcMediaView*, uint8_t side);
uint64_t openbc_media_audio_level_frames_per_bin(const OpenBcMediaView*, uint8_t side,
                                                 size_t level);
size_t openbc_media_audio_bin_count(const OpenBcMediaView*, uint8_t side, size_t level);
const OpenBcAudioPeak* openbc_media_audio_level_data(const OpenBcMediaView*, uint8_t side,
                                                     size_t level);
// Add next to the other openbc_media_* declarations in compare_backend_api.h
// (omit if that header is generated by cbindgen from media_compare_ffi.rs).
extern "C" std::uint8_t openbc_media_save(OpenBcMediaSession* session, std::uint8_t side, std::size_t count,
                                          const char* const* groups, const char* const* keys,
                                          const char* const* values /* null entry = remove tag */,
                                          std::uint8_t cover_mode /* 0 keep, 1 replace, 2 remove */,
                                          const std::uint8_t* cover, std::size_t cover_len);

}  // extern "C"
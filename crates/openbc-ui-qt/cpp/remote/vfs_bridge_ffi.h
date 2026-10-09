// vfs_bridge_ffi.h
// ---------------------------------------------------------------------------
// Hand-written declarations for the `extern "C"` functions exported by
// vfs_bridge.rs. Kept in lockstep with that file by hand rather than by
// bindgen, since it's a small, stable surface (connect/list/stat/read/
// disconnect + two destructors) - if vfs_bridge.rs's signatures change,
// this header and remote_vfs_bridge.h's callers need to change with it.
//
// Link against whatever staticlib/cdylib target vfs_bridge.rs builds into
// (the openbc-vfs-ffi crate, presumably) - not shown here since the build
// wiring wasn't part of the files this header was built from.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// Connect using a JSON profile (see vfs_detail::profileToFfiJson in
// remote_vfs_bridge.h for the exact shape expected). On success writes a
// session id to *session_id and returns nullptr; on failure returns an
// allocated error string that must be freed with openbc_vfs_string_destroy.
char* openbc_vfs_connect(const unsigned char* json, size_t json_length, int32_t* session_id);

// Drop a session created by openbc_vfs_connect.
void openbc_vfs_disconnect(int32_t session_id);

// List one directory level. On success writes an allocated JSON array
// (array of {name, isDir, size, mtimeMs}) to *listing and returns nullptr;
// on failure returns an allocated error string. Both need
// openbc_vfs_string_destroy.
char* openbc_vfs_list(int32_t session_id, const unsigned char* path, size_t path_length, char** listing);

// Stat a path. Returns 1 for a directory, 0 for a file, -1 on error (with
// *error set to an allocated string needing openbc_vfs_string_destroy).
int32_t openbc_vfs_stat(int32_t session_id, const unsigned char* path, size_t path_length, char** error);

// Read a file into an allocated buffer. Returns nullptr on error (with
// *error set). The returned buffer must be freed with
// openbc_vfs_buffer_destroy using the *out_length written here.
unsigned char* openbc_vfs_read(int32_t session_id, const unsigned char* path, size_t path_length,
                                size_t* out_length, char** error);

// Replace file contents or create one directory through openbc-engine.
char* openbc_engine_write_file(int32_t session_id, const unsigned char* path, size_t path_length,
                               const unsigned char* bytes, size_t bytes_length);
char* openbc_engine_create_dir(int32_t session_id, const unsigned char* path, size_t path_length);
unsigned char* openbc_engine_read_file(int32_t session_id, const unsigned char* path,
                                       size_t path_length, size_t* out_length, char** error);
char* openbc_engine_rename(int32_t session_id, const unsigned char* from, size_t from_length,
                           const unsigned char* to, size_t to_length);
char* openbc_engine_copy_entry(int32_t source_session_id, const unsigned char* source_path,
                               size_t source_path_length, int32_t destination_session_id,
                               const unsigned char* destination_path, size_t destination_path_length);
char* openbc_engine_mirror_entry(int32_t source_session_id, const unsigned char* source_path,
                                 size_t source_path_length, int32_t destination_session_id,
                                 const unsigned char* destination_path, size_t destination_path_length);
char* openbc_engine_delete_entry(int32_t session_id, const unsigned char* path, size_t path_length);

// Compare one directory level through openbc-engine/openbc-core. On success
// writes an allocated JSON array to *result and returns nullptr.
char* openbc_engine_compare_folder_level(
    int32_t left_session_id, const unsigned char* left_path, size_t left_path_length,
    int32_t right_session_id, const unsigned char* right_path, size_t right_path_length,
    uint8_t left_present, uint8_t right_present,
    uint8_t check_content, uint8_t ignore_timestamps, char** result);
char* openbc_engine_compare_files(
    int32_t left_session_id, const unsigned char* left_path, size_t left_path_length,
    int32_t right_session_id, const unsigned char* right_path, size_t right_path_length,
    uint8_t* equal);

// Free a string returned by any of the functions above.
void openbc_vfs_string_destroy(char* value);

// Free a buffer returned by openbc_vfs_read.
void openbc_vfs_buffer_destroy(unsigned char* value, size_t length);

}  // extern "C"
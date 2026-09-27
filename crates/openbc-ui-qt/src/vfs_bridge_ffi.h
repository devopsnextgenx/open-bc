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

// Free a string returned by any of the functions above.
void openbc_vfs_string_destroy(char* value);

// Free a buffer returned by openbc_vfs_read.
void openbc_vfs_buffer_destroy(unsigned char* value, size_t length);

}  // extern "C"
#pragma once

#include <cstddef>
#include <cstdint>

struct OpenBcDiff;
struct OpenBcInline;
struct OpenBcHighlight;

extern "C" {
int openbc_initialize_observability();
void openbc_log_message(const std::uint8_t*, std::size_t);
void openbc_register_instrumentation_session(const std::uint8_t*, std::size_t,
                                             const std::uint8_t*, std::size_t);
char* openbc_instrumentation_session_report(const std::uint8_t*, std::size_t, std::size_t*);
void openbc_instrumentation_report_destroy(char*);

// Anonymous usage tracking + first-run popup (all best effort and silent).
void openbc_usage_start(const std::uint8_t* version, std::size_t versionLength);
void openbc_usage_shutdown();
void openbc_usage_register_tab(const std::uint8_t* id, std::size_t idLength,
                               const std::uint8_t* kind, std::size_t kindLength);
void openbc_usage_close_tab(const std::uint8_t* id, std::size_t idLength);
int openbc_first_run_prompt_pending();
void openbc_first_run_prompt_mark_shown();
// Both return a masked address (free with openbc_instrumentation_report_destroy) or nullptr if invalid.
char* openbc_mask_email(const std::uint8_t* email, std::size_t emailLength, std::size_t* maskedLength);
char* openbc_first_run_submit_email(const std::uint8_t* email, std::size_t emailLength,
                                    std::size_t* maskedLength);
OpenBcDiff* openbc_compare_buffers(const std::uint8_t*, std::size_t, const std::uint8_t*,
                                   std::size_t, std::uint8_t, std::uint8_t, std::uint8_t,
                                   double, const std::uint8_t*, std::size_t, const std::uint8_t*,
                                   std::size_t);
void openbc_diff_destroy(OpenBcDiff*);
std::size_t openbc_diff_len(const OpenBcDiff*);
std::uint8_t openbc_diff_kind(const OpenBcDiff*, std::size_t);
std::size_t openbc_diff_left_line(const OpenBcDiff*, std::size_t);
std::size_t openbc_diff_right_line(const OpenBcDiff*, std::size_t);
const std::uint8_t* openbc_diff_left_text(const OpenBcDiff*, std::size_t, std::size_t*);
const std::uint8_t* openbc_diff_right_text(const OpenBcDiff*, std::size_t, std::size_t*);

OpenBcInline* openbc_inline_diff(const std::uint8_t*, std::size_t, const std::uint8_t*,
                                 std::size_t, const std::uint8_t*, std::size_t);
void openbc_inline_destroy(OpenBcInline*);
std::size_t openbc_inline_len(const OpenBcInline*, std::uint8_t);
std::size_t openbc_inline_start(const OpenBcInline*, std::uint8_t, std::size_t);
std::size_t openbc_inline_length(const OpenBcInline*, std::uint8_t, std::size_t);
std::uint8_t openbc_inline_kind(const OpenBcInline*, std::uint8_t, std::size_t);

OpenBcHighlight* openbc_highlight_buffer(const std::uint8_t*, std::size_t, const std::uint8_t*,
                                         std::size_t, const std::uint8_t*, std::size_t);
OpenBcHighlight* openbc_highlight_buffer_with_theme(const std::uint8_t*, std::size_t,
                                                    const std::uint8_t*, std::size_t,
                                                    std::uint8_t, const std::uint8_t*,
                                                    std::size_t);
void openbc_highlight_destroy(OpenBcHighlight*);
std::size_t openbc_highlight_len(const OpenBcHighlight*);
std::size_t openbc_highlight_line(const OpenBcHighlight*, std::size_t);
std::size_t openbc_highlight_start(const OpenBcHighlight*, std::size_t);
std::size_t openbc_highlight_length(const OpenBcHighlight*, std::size_t);
std::uint8_t openbc_highlight_foreground(const OpenBcHighlight*, std::size_t, std::uint8_t);
std::uint8_t openbc_highlight_background(const OpenBcHighlight*, std::size_t, std::uint8_t);
std::uint8_t openbc_highlight_bold(const OpenBcHighlight*, std::size_t);
std::uint8_t openbc_highlight_italic(const OpenBcHighlight*, std::size_t);
char* openbc_highlight_to_html(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t,
                               std::uint8_t);
void openbc_highlight_html_destroy(char*);

char* openbc_vfs_connect(const std::uint8_t*, std::size_t, std::int32_t*);
void openbc_vfs_disconnect(std::int32_t);
char* openbc_vfs_list(std::int32_t, const std::uint8_t*, std::size_t, char**);
std::int32_t openbc_vfs_stat(std::int32_t, const std::uint8_t*, std::size_t, char**);
std::uint8_t* openbc_vfs_read(std::int32_t, const std::uint8_t*, std::size_t, std::size_t*, char**);
void openbc_vfs_string_destroy(char*);
void openbc_vfs_buffer_destroy(std::uint8_t*, std::size_t);
}

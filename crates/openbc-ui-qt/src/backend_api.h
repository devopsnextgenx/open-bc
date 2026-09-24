#pragma once

#include <cstddef>
#include <cstdint>

struct OpenBcDiff;
struct OpenBcInline;
struct OpenBcHighlight;

extern "C" {
OpenBcDiff* openbc_compare_buffers(const std::uint8_t*, std::size_t, const std::uint8_t*,
                                   std::size_t, std::uint8_t, std::uint8_t, std::uint8_t,
                                   double);
void openbc_diff_destroy(OpenBcDiff*);
std::size_t openbc_diff_len(const OpenBcDiff*);
std::uint8_t openbc_diff_kind(const OpenBcDiff*, std::size_t);
std::size_t openbc_diff_left_line(const OpenBcDiff*, std::size_t);
std::size_t openbc_diff_right_line(const OpenBcDiff*, std::size_t);
const std::uint8_t* openbc_diff_left_text(const OpenBcDiff*, std::size_t, std::size_t*);
const std::uint8_t* openbc_diff_right_text(const OpenBcDiff*, std::size_t, std::size_t*);

OpenBcInline* openbc_inline_diff(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t);
void openbc_inline_destroy(OpenBcInline*);
std::size_t openbc_inline_len(const OpenBcInline*, std::uint8_t);
std::size_t openbc_inline_start(const OpenBcInline*, std::uint8_t, std::size_t);
std::size_t openbc_inline_length(const OpenBcInline*, std::uint8_t, std::size_t);
std::uint8_t openbc_inline_kind(const OpenBcInline*, std::uint8_t, std::size_t);

OpenBcHighlight* openbc_highlight_buffer(const std::uint8_t*, std::size_t, const std::uint8_t*,
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
}

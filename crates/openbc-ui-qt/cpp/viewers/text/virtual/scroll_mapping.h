// scroll_mapping.h
// ---------------------------------------------------------------------------
// Maps a logical uint64 "top row" to the int range of a QScrollBar and back. Qt-free so it can be
// unit-tested (cpp/tools/virtual_editor_test).
//
//   * While the number of scrollable rows fits the int range the mapping is the identity: one
//     scrollbar unit == one row, exactly.
//   * Beyond that the scrollbar is scaled proportionally; the ends are exact (value 0 == first
//     row, value max == last possible top row) and the mapping is monotonic, so the user can
//     reach every part of the file. Keyboard and wheel navigation never go through the scrollbar
//     (they set the row directly), so scaling never costs precision where it matters.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdint>

namespace openbc::app {

struct ScrollMapper {
    static constexpr std::uint64_t kMaxScrollbar = 0x7FFFFFFFULL;

    std::uint64_t totalRows = 0;
    std::uint64_t visibleRows = 1;

    // Largest row that may be the first visible one (the last page stays full).
    std::uint64_t maxTop() const {
        return totalRows > visibleRows ? totalRows - visibleRows : 0;
    }

    bool scaled() const { return maxTop() > kMaxScrollbar; }

    int scrollbarMax() const {
        return static_cast<int>(scaled() ? kMaxScrollbar : maxTop());
    }

    int toScrollbar(std::uint64_t top) const {
        top = std::min(top, maxTop());
        if (!scaled()) return static_cast<int>(top);
        if (top == maxTop()) return static_cast<int>(kMaxScrollbar);
        const long double ratio = static_cast<long double>(top) / static_cast<long double>(maxTop());
        return static_cast<int>(ratio * static_cast<long double>(kMaxScrollbar));
    }

    std::uint64_t toTop(int value) const {
        if (value <= 0) return 0;
        if (!scaled()) return std::min<std::uint64_t>(static_cast<std::uint64_t>(value), maxTop());
        if (static_cast<std::uint64_t>(value) >= kMaxScrollbar) return maxTop();
        const long double ratio = static_cast<long double>(value) / static_cast<long double>(kMaxScrollbar);
        return std::min<std::uint64_t>(static_cast<std::uint64_t>(ratio * static_cast<long double>(maxTop())), maxTop());
    }

    // Rows a scrollbar page step represents in the unscaled case (the bar's pageStep is in bar units).
    int pageStep() const {
        if (!scaled()) return static_cast<int>(std::max<std::uint64_t>(1, visibleRows > 1 ? visibleRows - 1 : 1));
        const long double per = static_cast<long double>(maxTop()) / static_cast<long double>(kMaxScrollbar);
        return static_cast<int>(std::max<long double>(1.0L, static_cast<long double>(visibleRows) / per));
    }
};

}  // namespace openbc::app

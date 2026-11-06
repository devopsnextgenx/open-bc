// Unit tests for options/text_editor_selection.h (Qt-free).
// Build and run (one line): g++ -std=c++17 -I crates/openbc-ui-qt/cpp
// crates/openbc-ui-qt/cpp/tools/text_editor_select_test/text_editor_select_test.cpp -o /tmp/tes && /tmp/tes
#include <cstdio>
#include <limits>

#include "options/text_editor_selection.h"

using namespace openbc::app::text_editor;

static int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

int main() {
    constexpr std::int64_t MB = 1024 * 1024;
    Settings d;  // defaults
    CHECK(d.mode == Mode::Hybrid);
    CHECK(d.thresholdValue == 3 && d.thresholdUnit == Unit::MB);
    CHECK(thresholdBytes(d) == 3ULL * 1024 * 1024);

    // Hybrid switches exactly at the threshold: below -> Existing, equal or above -> Virtual.
    CHECK(chooseEditor(d, 3 * MB - 1, 10) == Editor::Existing);
    CHECK(chooseEditor(d, 3 * MB, 10) == Editor::Virtual);
    CHECK(chooseEditor(d, 10, 3 * MB) == Editor::Virtual);   // either side
    CHECK(chooseEditor(d, 0, 0) == Editor::Existing);        // two empty files / missing sides
    CHECK(chooseEditor(d, kUnknownSize, 10) == Editor::Virtual);   // cannot size -> safe fallback
    CHECK(chooseEditor(d, 10, kUnknownSize) == Editor::Virtual);

    // Explicit modes override Hybrid logic.
    Settings e; e.mode = Mode::Existing;
    CHECK(chooseEditor(e, 900 * MB, 900 * MB) == Editor::Existing);
    CHECK(chooseEditor(e, kUnknownSize, 1) == Editor::Existing);
    Settings v; v.mode = Mode::Virtual;
    CHECK(chooseEditor(v, 1, 1) == Editor::Virtual);

    // Units: 512 KB, 3 MB, 1 GB.
    Settings s; s.thresholdValue = 512; s.thresholdUnit = Unit::KB;
    CHECK(thresholdBytes(s) == 512 * 1024);
    CHECK(chooseEditor(s, 512 * 1024 - 1, 0) == Editor::Existing);
    CHECK(chooseEditor(s, 512 * 1024, 0) == Editor::Virtual);
    s.thresholdValue = 1; s.thresholdUnit = Unit::GB;
    CHECK(thresholdBytes(s) == 1024ULL * 1024 * 1024);
    CHECK(chooseEditor(s, 1023 * MB, 0) == Editor::Existing);

    // Validation: zero, overflow, unreasonable, below minimum.
    std::uint64_t out = 0; std::string err;
    CHECK(!thresholdToBytes(0, Unit::MB, &out, &err) && !err.empty());
    CHECK(!thresholdToBytes(std::numeric_limits<std::uint64_t>::max(), Unit::GB, &out, &err));
    CHECK(!thresholdToBytes(std::numeric_limits<std::uint64_t>::max() / 1024 + 1, Unit::KB, &out, &err) ||
          out <= kMaxThresholdBytes);
    CHECK(!thresholdToBytes(65, Unit::GB, &out, &err));      // > 64 GB
    CHECK(thresholdToBytes(64, Unit::GB, &out, &err) && out == kMaxThresholdBytes);
    CHECK(thresholdToBytes(1, Unit::KB, &out, &err) && out == 1024);
    CHECK(!thresholdToBytes(1ULL << 40, Unit::MB, &out, &err));  // 2^40 MB = 1 EB

    // Sanitizing hand-edited / corrupt settings falls back to the defaults.
    Settings bad; bad.thresholdValue = 0; bad.mode = static_cast<Mode>(99); bad.thresholdUnit = static_cast<Unit>(7);
    const Settings fixed = sanitized(bad);
    CHECK(fixed.mode == Mode::Hybrid && fixed.thresholdValue == 3 && fixed.thresholdUnit == Unit::MB);
    Settings good; good.mode = Mode::Virtual; good.thresholdValue = 10; good.thresholdUnit = Unit::GB;
    const Settings kept = sanitized(good);
    CHECK(kept.mode == Mode::Virtual && kept.thresholdValue == 10 && kept.thresholdUnit == Unit::GB);

    std::printf(failures ? "%d FAILURE(S)\n" : "text_editor_select_test: all passed\n", failures);
    return failures ? 1 : 0;
}

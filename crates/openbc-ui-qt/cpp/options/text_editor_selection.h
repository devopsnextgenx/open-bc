// text_editor_selection.h
// ---------------------------------------------------------------------------
// Tools > Options > Text Compare: which editor implementation a new text comparison uses.
//
//   Existing : QPlainTextEdit / QTextDocument editor (always).
//   Virtual  : VirtualTextEditor, a QAbstractScrollArea over the Qt-independent engine (always).
//   Hybrid   : decided per comparison from the file sizes and a user-configurable threshold.
//
// Qt-free on purpose (like options_status.h / drop_router.h) so the decision logic is unit-tested
// without a GUI: see cpp/tools/text_editor_select_test.
//
// UNITS: KB/MB/GB are binary (1 KB = 1024 B, 1 MB = 1024 KB, 1 GB = 1024 MB). Internally the
// threshold is always bytes (uint64_t). The accepted range is 1 KB .. 64 GB.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

namespace openbc::app::text_editor {

enum class Mode : int { Existing = 0, Virtual = 1, Hybrid = 2 };
enum class Unit : int { KB = 0, MB = 1, GB = 2 };
enum class Editor { Existing, Virtual };

inline constexpr std::uint64_t kKiB = 1024ULL;
inline constexpr std::uint64_t kMinThresholdBytes = 1 * kKiB;
inline constexpr std::uint64_t kMaxThresholdBytes = 64ULL * kKiB * kKiB * kKiB;
inline constexpr std::uint64_t kDefaultThresholdValue = 3;
inline constexpr Unit kDefaultThresholdUnit = Unit::MB;
inline constexpr std::int64_t kUnknownSize = -1;

inline constexpr std::uint64_t unitBytes(Unit unit) {
    switch (unit) {
    case Unit::KB: return kKiB;
    case Unit::MB: return kKiB * kKiB;
    case Unit::GB: return kKiB * kKiB * kKiB;
    }
    return kKiB * kKiB;
}

inline const char* unitName(Unit unit) {
    switch (unit) {
    case Unit::KB: return "KB";
    case Unit::MB: return "MB";
    case Unit::GB: return "GB";
    }
    return "MB";
}

struct Settings {
    Mode mode = Mode::Hybrid;
    std::uint64_t thresholdValue = kDefaultThresholdValue;
    Unit thresholdUnit = kDefaultThresholdUnit;
};

// Overflow-safe conversion. Returns false (and a message) for zero, overflow or out-of-range
// values; `bytes` is only written on success.
inline bool thresholdToBytes(std::uint64_t value, Unit unit, std::uint64_t* bytes,
                             std::string* error = nullptr) {
    const std::uint64_t scale = unitBytes(unit);
    if (value == 0) {
        if (error) *error = "The threshold must be greater than zero.";
        return false;
    }
    if (value > kMaxThresholdBytes / scale) {  // also guards value * scale overflow
        if (error) *error = "The threshold is too large (maximum 64 GB).";
        return false;
    }
    const std::uint64_t result = value * scale;
    if (result < kMinThresholdBytes) {
        if (error) *error = "The threshold must be at least 1 KB.";
        return false;
    }
    if (bytes) *bytes = result;
    return true;
}

inline bool isValid(const Settings& s, std::string* error = nullptr) {
    std::uint64_t ignored = 0;
    return thresholdToBytes(s.thresholdValue, s.thresholdUnit, &ignored, error);
}

// Replaces anything invalid (hand-edited preferences.ini, older versions) with the defaults.
inline Settings sanitized(Settings s) {
    const int mode = static_cast<int>(s.mode);
    if (mode < 0 || mode > 2) s.mode = Mode::Hybrid;
    const int unit = static_cast<int>(s.thresholdUnit);
    if (unit < 0 || unit > 2) s.thresholdUnit = kDefaultThresholdUnit;
    if (!isValid(s)) {
        s.thresholdValue = kDefaultThresholdValue;
        s.thresholdUnit = kDefaultThresholdUnit;
    }
    return s;
}

inline std::uint64_t thresholdBytes(const Settings& s) {
    std::uint64_t bytes = 0;
    const Settings ok = sanitized(s);
    thresholdToBytes(ok.thresholdValue, ok.thresholdUnit, &bytes);
    return bytes;
}

// The decision, made BEFORE any file content is loaded.
//   Explicit modes override Hybrid.
//   Hybrid: either side >= threshold -> Virtual; either side of unknown size -> Virtual;
//           otherwise Existing.
// A side with no file at all (empty path) is passed as size 0.
inline Editor chooseEditor(const Settings& settings, std::int64_t leftBytes, std::int64_t rightBytes) {
    switch (settings.mode) {
    case Mode::Existing: return Editor::Existing;
    case Mode::Virtual: return Editor::Virtual;
    case Mode::Hybrid: break;
    }
    if (leftBytes < 0 || rightBytes < 0) return Editor::Virtual;
    const std::uint64_t limit = thresholdBytes(settings);
    if (static_cast<std::uint64_t>(leftBytes) >= limit || static_cast<std::uint64_t>(rightBytes) >= limit)
        return Editor::Virtual;
    return Editor::Existing;
}

}  // namespace openbc::app::text_editor

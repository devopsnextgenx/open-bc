// content_sniff.h
// ---------------------------------------------------------------------------
// Header sniffing for dropped files, so type detection is not extension-only.
// Std-only (no Qt): unit-tested by tools/drop_router_test.
//
// Precedence implemented by refineKind(), which drop_payload.h applies ONLY to
// files whose extension is unknown or ambiguous (".obj"); a recognised extension
// is trusted without any I/O so a drop never blocks the GUI thread on a slow mount:
//   definite signature (PNG, JPEG, RIFF/WAVE, STEP, "Windows Registry Editor"...)
//     >  extension (classifyFilePath in file_kinds.h)
//     >  text/binary heuristic
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstring>

#include "common/viewer_kind.h"

namespace openbc::app::sniff {

enum class Sniffed { Unknown, Text, Binary, Picture, Model3d, Media, Registry };

namespace detail {

inline bool startsWith(const unsigned char* d, std::size_t n, const char* magic, std::size_t at = 0) {
    const std::size_t len = std::strlen(magic);
    return n >= at + len && std::memcmp(d + at, magic, len) == 0;
}

// UTF-16LE text with a BOM: compare every second byte against ASCII `magic`.
inline bool startsWithUtf16le(const unsigned char* d, std::size_t n, const char* magic) {
    if (n < 2 || d[0] != 0xFF || d[1] != 0xFE) return false;
    const std::size_t len = std::strlen(magic);
    if (n < 2 + len * 2) return false;
    for (std::size_t i = 0; i < len; ++i) {
        if (d[2 + i * 2] != static_cast<unsigned char>(magic[i]) || d[3 + i * 2] != 0) return false;
    }
    return true;
}

}  // namespace detail

// Classifies the first bytes of a file. Never reads past `n`.
inline Sniffed sniffBytes(const unsigned char* d, std::size_t n) {
    using detail::startsWith;
    if (!d || n == 0) return Sniffed::Unknown;

    // Registry exports (ANSI or UTF-16LE with BOM).
    if (startsWith(d, n, "Windows Registry Editor Version") || startsWith(d, n, "REGEDIT4") ||
        detail::startsWithUtf16le(d, n, "Windows Registry Editor Version"))
        return Sniffed::Registry;

    // Pictures.
    static const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= sizeof png && std::memcmp(d, png, sizeof png) == 0) return Sniffed::Picture;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return Sniffed::Picture;           // JPEG
    if (startsWith(d, n, "GIF87a") || startsWith(d, n, "GIF89a")) return Sniffed::Picture;
    if (startsWith(d, n, "RIFF") && startsWith(d, n, "WEBP", 8)) return Sniffed::Picture;
    if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 0x2A && d[3] == 0) ||
                   (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 0x2A)))
        return Sniffed::Picture;                                                                   // TIFF

    // Media containers.
    if (startsWith(d, n, "RIFF") && (startsWith(d, n, "WAVE", 8) || startsWith(d, n, "AVI ", 8)))
        return Sniffed::Media;
    if (startsWith(d, n, "ID3") || startsWith(d, n, "fLaC") || startsWith(d, n, "OggS")) return Sniffed::Media;
    if (startsWith(d, n, "ftyp", 4)) return Sniffed::Media;                                         // MP4 / M4A / MOV
    if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) return Sniffed::Media;  // Matroska / WebM

    // 3D models with unambiguous headers.
    if (startsWith(d, n, "glTF") || startsWith(d, n, "ISO-10303-21") || startsWith(d, n, "ply\n") ||
        startsWith(d, n, "ply\r\n"))
        return Sniffed::Model3d;

    // Executables / archives are binary regardless of NULs.
    if (startsWith(d, n, "\x7f" "ELF") || startsWith(d, n, "MZ") || startsWith(d, n, "PK\x03\x04"))
        return Sniffed::Binary;

    // UTF-16/UTF-8 BOMs are text.
    if (n >= 2 && ((d[0] == 0xFF && d[1] == 0xFE) || (d[0] == 0xFE && d[1] == 0xFF))) return Sniffed::Text;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) return Sniffed::Text;

    // Heuristic: a NUL byte or many control characters means binary.
    std::size_t control = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = d[i];
        if (c == 0) return Sniffed::Binary;
        if (c < 0x09 || (c > 0x0D && c < 0x20 && c != 0x1B)) ++control;
    }
    return control * 10 > n * 3 ? Sniffed::Binary : Sniffed::Text;
}

// Combines the extension-based kind with what the header says (see precedence above).
inline ViewerKind refineKind(ViewerKind byExtension, Sniffed s) {
    switch (s) {
    case Sniffed::Picture: return ViewerKind::Picture;
    case Sniffed::Media: return ViewerKind::Media;
    case Sniffed::Model3d: return ViewerKind::Model3d;
    case Sniffed::Registry: return ViewerKind::Registry;
    case Sniffed::Binary:
        // Extension-less / unknown-extension binary data has no better viewer than hex.
        return byExtension == ViewerKind::Text ? ViewerKind::Hex : byExtension;
    case Sniffed::Text:
    case Sniffed::Unknown:
        break;
    }
    return byExtension;
}

}  // namespace openbc::app::sniff

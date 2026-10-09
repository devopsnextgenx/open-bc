// picture_exif.h
// ---------------------------------------------------------------------------
// Minimal, dependency-free EXIF reader used by the picture-compare metadata
// panel. Understands the TIFF structure inside a JPEG APP1 segment (and plain
// TIFF files): IFD0, the Exif sub-IFD and the GPS sub-IFD. Everything is
// bounds-checked, so truncated or corrupt files simply yield fewer entries.
//
// Standard library only (no Qt) so it can be unit-tested on its own.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace openbc::app::exif {

struct Entry {
    std::string group;  // "EXIF" or "GPS"
    std::string key;
    std::string value;
};

namespace detail {

struct Reader {
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;
    bool little = true;

    bool has(std::size_t off, std::size_t n) const { return off <= size && n <= size - off; }
    std::uint16_t u16(std::size_t o) const {
        return little ? std::uint16_t(base[o] | (base[o + 1] << 8))
                      : std::uint16_t((base[o] << 8) | base[o + 1]);
    }
    std::uint32_t u32(std::size_t o) const {
        return little ? (std::uint32_t(base[o]) | std::uint32_t(base[o + 1]) << 8 |
                         std::uint32_t(base[o + 2]) << 16 | std::uint32_t(base[o + 3]) << 24)
                      : (std::uint32_t(base[o]) << 24 | std::uint32_t(base[o + 1]) << 16 |
                         std::uint32_t(base[o + 2]) << 8 | std::uint32_t(base[o + 3]));
    }
};

struct Field {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::size_t dataOff = 0;
};

inline std::size_t typeSize(std::uint16_t type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

template <class Fn>
inline void forEachField(const Reader& r, std::size_t ifdOff, Fn&& fn) {
    if (!r.has(ifdOff, 2)) return;
    const std::size_t n = std::min<std::size_t>(r.u16(ifdOff), 1024);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t e = ifdOff + 2 + i * 12;
        if (!r.has(e, 12)) break;
        Field f;
        f.tag = r.u16(e);
        f.type = r.u16(e + 2);
        f.count = r.u32(e + 4);
        const std::size_t unit = typeSize(f.type);
        if (unit == 0 || f.count == 0) continue;
        const std::uint64_t total = std::uint64_t(unit) * f.count;
        f.dataOff = total <= 4 ? e + 8 : std::size_t(r.u32(e + 8));
        if (!r.has(f.dataOff, std::size_t(std::min<std::uint64_t>(total, r.size)))) continue;
        if (total > r.size) continue;
        fn(f);
    }
}

inline std::string ascii(const Reader& r, const Field& f) {
    if (f.type != 2 && f.type != 7) return {};
    std::string s;
    for (std::uint32_t i = 0; i < f.count && r.has(f.dataOff + i, 1); ++i) {
        const char c = char(r.base[f.dataOff + i]);
        if (c == '\0') break;
        s.push_back(c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

inline std::vector<double> numbers(const Reader& r, const Field& f, std::uint32_t maxCount = 8) {
    std::vector<double> out;
    const std::size_t unit = typeSize(f.type);
    const std::uint32_t n = std::min(f.count, maxCount);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t o = f.dataOff + i * unit;
        if (!r.has(o, unit)) break;
        switch (f.type) {
            case 1: case 7: out.push_back(r.base[o]); break;
            case 6: out.push_back(std::int8_t(r.base[o])); break;
            case 3: out.push_back(r.u16(o)); break;
            case 8: out.push_back(std::int16_t(r.u16(o))); break;
            case 4: out.push_back(r.u32(o)); break;
            case 9: out.push_back(std::int32_t(r.u32(o))); break;
            case 5: {
                const double d = r.u32(o + 4);
                if (d != 0) out.push_back(r.u32(o) / d);
                break;
            }
            case 10: {
                const double d = std::int32_t(r.u32(o + 4));
                if (d != 0) out.push_back(std::int32_t(r.u32(o)) / d);
                break;
            }
            default: break;
        }
    }
    return out;
}

template <class... A>
inline std::string sfmt(const char* format, A... args) {
    char buf[160];
    std::snprintf(buf, sizeof buf, format, args...);
    return buf;
}

inline void addEntry(std::vector<Entry>& out, const char* group, const char* key, std::string value) {
    if (!value.empty()) out.push_back({group, key, std::move(value)});
}

// "2024:05:17 09:30:00" -> "2024-05-17 09:30:00"
inline std::string prettyDate(std::string s) {
    if (s.size() >= 10 && s[4] == ':' && s[7] == ':') {
        s[4] = '-';
        s[7] = '-';
    }
    return s;
}

inline std::string orientationName(int v) {
    static const char* names[] = {"Normal", "Mirror horizontal", "Rotate 180", "Mirror vertical",
                                  "Mirror horizontal, rotate 270 CW", "Rotate 90 CW",
                                  "Mirror horizontal, rotate 90 CW", "Rotate 270 CW"};
    return v >= 1 && v <= 8 ? std::string(names[v - 1]) + " (" + std::to_string(v) + ")"
                            : std::to_string(v);
}

inline double gpsDegrees(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double deg = v[0];
    if (v.size() > 1) deg += v[1] / 60.0;
    if (v.size() > 2) deg += v[2] / 3600.0;
    return deg;
}

inline void parseExifIfd(const Reader& r, std::size_t off, std::vector<Entry>& out) {
    std::uint32_t pixelX = 0, pixelY = 0;
    forEachField(r, off, [&](const Field& f) {
        const auto n = [&]() { return numbers(r, f, 1); };
        switch (f.tag) {
            case 0x829A: {
                const auto v = n();
                if (v.empty() || v[0] <= 0) break;
                addEntry(out, "EXIF", "Exposure time",
                     v[0] < 1.0 ? sfmt("1/%d s", int(std::lround(1.0 / v[0]))) : sfmt("%g s", v[0]));
                break;
            }
            case 0x829D: {
                const auto v = n();
                if (!v.empty()) addEntry(out, "EXIF", "Aperture", sfmt("f/%.3g", v[0]));
                break;
            }
            case 0x8827: {
                const auto v = n();
                if (!v.empty()) addEntry(out, "EXIF", "ISO", sfmt("%d", int(v[0])));
                break;
            }
            case 0x9003: addEntry(out, "EXIF", "Date taken", prettyDate(ascii(r, f))); break;
            case 0x9004: addEntry(out, "EXIF", "Date digitized", prettyDate(ascii(r, f))); break;
            case 0x9204: {
                const auto v = n();
                if (!v.empty()) addEntry(out, "EXIF", "Exposure bias", sfmt("%+.2f EV", v[0]));
                break;
            }
            case 0x9209: {
                const auto v = n();
                if (!v.empty()) addEntry(out, "EXIF", "Flash", (int(v[0]) & 1) ? "Fired" : "Did not fire");
                break;
            }
            case 0x920A: {
                const auto v = n();
                if (!v.empty()) addEntry(out, "EXIF", "Focal length", sfmt("%.3g mm", v[0]));
                break;
            }
            case 0xA001: {
                const auto v = n();
                if (v.empty()) break;
                const int cs = int(v[0]);
                addEntry(out, "EXIF", "Color space",
                     cs == 1 ? "sRGB" : cs == 0xFFFF ? "Uncalibrated" : sfmt("%d", cs));
                break;
            }
            case 0xA002: {
                const auto v = n();
                if (!v.empty()) pixelX = std::uint32_t(v[0]);
                break;
            }
            case 0xA003: {
                const auto v = n();
                if (!v.empty()) pixelY = std::uint32_t(v[0]);
                break;
            }
            case 0xA405: {
                const auto v = n();
                if (!v.empty() && v[0] > 0) addEntry(out, "EXIF", "Focal length (35mm)", sfmt("%d mm", int(v[0])));
                break;
            }
            case 0xA434: addEntry(out, "EXIF", "Lens model", ascii(r, f)); break;
            default: break;
        }
    });
    if (pixelX && pixelY) {
        addEntry(out, "EXIF", "Pixel dimensions (EXIF)", sfmt("%u x %u", pixelX, pixelY));
    }
}

inline void parseGpsIfd(const Reader& r, std::size_t off, std::vector<Entry>& out) {
    std::vector<double> lat, lon;
    std::string latRef, lonRef;
    double altitude = 0;
    bool hasAltitude = false;
    int altitudeRef = 0;
    forEachField(r, off, [&](const Field& f) {
        switch (f.tag) {
            case 0x0001: latRef = ascii(r, f); break;
            case 0x0002: lat = numbers(r, f, 3); break;
            case 0x0003: lonRef = ascii(r, f); break;
            case 0x0004: lon = numbers(r, f, 3); break;
            case 0x0005: {
                const auto v = numbers(r, f, 1);
                if (!v.empty()) altitudeRef = int(v[0]);
                break;
            }
            case 0x0006: {
                const auto v = numbers(r, f, 1);
                if (!v.empty()) {
                    altitude = v[0];
                    hasAltitude = true;
                }
                break;
            }
            default: break;
        }
    });
    if (!lat.empty() && !lon.empty()) {
        double la = gpsDegrees(lat), lo = gpsDegrees(lon);
        if (!latRef.empty() && (latRef[0] == 'S' || latRef[0] == 's')) la = -la;
        if (!lonRef.empty() && (lonRef[0] == 'W' || lonRef[0] == 'w')) lo = -lo;
        addEntry(out, "GPS", "Position", sfmt("%.6f, %.6f", la, lo));
    }
    if (hasAltitude) addEntry(out, "GPS", "Altitude", sfmt("%.1f m", altitudeRef == 1 ? -altitude : altitude));
}

}  // namespace detail

// `tiff` points at the TIFF header ("II*\0" or "MM\0*").
inline std::vector<Entry> parseTiff(const std::uint8_t* tiff, std::size_t size) {
    std::vector<Entry> out;
    if (!tiff || size < 8) return out;
    detail::Reader r;
    r.base = tiff;
    r.size = size;
    if (tiff[0] == 'I' && tiff[1] == 'I') r.little = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M') r.little = false;
    else return out;
    if (r.u16(2) != 42) return out;

    std::size_t exifOff = 0, gpsOff = 0;
    double xres = 0, yres = 0;
    int unit = 2;
    detail::forEachField(r, r.u32(4), [&](const detail::Field& f) {
        const auto first = [&]() { return detail::numbers(r, f, 1); };
        switch (f.tag) {
            case 0x010E: detail::addEntry(out, "EXIF", "Description", detail::ascii(r, f)); break;
            case 0x010F: detail::addEntry(out, "EXIF", "Camera make", detail::ascii(r, f)); break;
            case 0x0110: detail::addEntry(out, "EXIF", "Camera model", detail::ascii(r, f)); break;
            case 0x0112: {
                const auto v = first();
                if (!v.empty()) detail::addEntry(out, "EXIF", "Orientation", detail::orientationName(int(v[0])));
                break;
            }
            case 0x011A: { const auto v = first(); if (!v.empty()) xres = v[0]; break; }
            case 0x011B: { const auto v = first(); if (!v.empty()) yres = v[0]; break; }
            case 0x0128: { const auto v = first(); if (!v.empty()) unit = int(v[0]); break; }
            case 0x0131: detail::addEntry(out, "EXIF", "Software", detail::ascii(r, f)); break;
            case 0x0132: detail::addEntry(out, "EXIF", "Date modified (EXIF)", detail::prettyDate(detail::ascii(r, f))); break;
            case 0x013B: detail::addEntry(out, "EXIF", "Artist", detail::ascii(r, f)); break;
            case 0x8298: detail::addEntry(out, "EXIF", "Copyright", detail::ascii(r, f)); break;
            case 0x8769: { const auto v = first(); if (!v.empty()) exifOff = std::size_t(v[0]); break; }
            case 0x8825: { const auto v = first(); if (!v.empty()) gpsOff = std::size_t(v[0]); break; }
            default: break;
        }
    });
    if (xres > 0 && yres > 0 && (unit == 2 || unit == 3)) {
        detail::addEntry(out, "EXIF", "Resolution",
                     detail::sfmt(unit == 2 ? "%g x %g dpi" : "%g x %g dpcm", xres, yres));
    }
    if (exifOff) detail::parseExifIfd(r, exifOff, out);
    if (gpsOff) detail::parseGpsIfd(r, gpsOff, out);
    return out;
}

// Accepts the start of a JPEG file (only the first couple of hundred KB are
// needed - the Exif segment sits before the image data) or of a TIFF file.
inline std::vector<Entry> parse(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 8) return {};
    if (data[0] == 0xFF && data[1] == 0xD8) {
        std::size_t pos = 2;
        while (pos + 4 <= size) {
            if (data[pos] != 0xFF) break;
            const std::uint8_t marker = data[pos + 1];
            if (marker == 0xFF) { ++pos; continue; }                      // fill byte
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) { pos += 2; continue; }  // no payload
            if (marker == 0xDA || marker == 0xD9) break;                  // image data / EOI
            const std::size_t len = (std::size_t(data[pos + 2]) << 8) | data[pos + 3];
            if (len < 2) break;
            const std::size_t seg = pos + 4;
            const std::size_t segLen = std::min(len - 2, size - seg);
            if (marker == 0xE1 && segLen >= 6 && std::memcmp(data + seg, "Exif\0\0", 6) == 0) {
                return parseTiff(data + seg + 6, segLen - 6);
            }
            pos += 2 + len;
        }
        return {};
    }
    if ((data[0] == 'I' && data[1] == 'I' && data[2] == 42 && data[3] == 0) ||
        (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && data[3] == 42)) {
        return parseTiff(data, size);
    }
    return {};
}

}  // namespace openbc::app::exif
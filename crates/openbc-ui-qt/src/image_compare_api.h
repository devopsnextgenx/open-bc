// image_compare_api.h
// C ABI exported by the Rust openbc-ui-qt crate (image_compare_ffi.rs).
// Layouts must stay byte-for-byte in sync with the #[repr(C)] structs there.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

struct OpenbcImageCompareOptions {
    std::uint8_t mode;           // 0 tolerance, 1 range, 2 blend
    std::uint8_t tolerance;
    std::uint8_t rangeMin;
    std::uint8_t rangeMax;
    std::uint8_t blendPercent;
    std::uint8_t resize;         // 0 auto, 1 native, 2 fit-left, 3 fit-right, 4 fit-larger, 5 fit-smaller
    std::uint8_t compareAlpha;
    std::uint8_t reserved;
    std::int32_t offsetX;
    std::int32_t offsetY;
};

struct OpenbcStr {
    const std::uint8_t* ptr;
    std::size_t len;
};

struct OpenbcImageInfo {
    OpenbcStr path;
    OpenbcStr format;
    std::uint64_t sizeBytes;
    std::int64_t modifiedUnixMs;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bitsPerPixel;
    std::uint8_t hasModified;
    std::uint8_t reserved[3];
};

enum OpenbcImageEventKind : std::uint32_t {
    OPENBC_IMAGE_LOADING = 1,
    OPENBC_IMAGE_LOADED = 2,
    OPENBC_IMAGE_RESULT = 3,
    OPENBC_IMAGE_PATH_ERROR = 4,
    OPENBC_IMAGE_FAILED = 5,
};

struct OpenbcImageCompareEvent {
    std::uint32_t kind;
    std::uint32_t side;  // 0 left, 1 right
    std::uint64_t seq;

    OpenbcImageInfo left;
    OpenbcImageInfo right;
    const std::uint8_t* leftRgba;
    const std::uint8_t* rightRgba;

    OpenbcStr path;
    OpenbcStr message;

    OpenbcImageCompareOptions options;
    std::uint32_t verdict;  // 0 binary-same, 1 pixels-same, 2 similar, 3 different
    std::uint32_t backend;  // 0 cpu, 1 gpu
    std::uint8_t binaryIdentical;
    std::uint8_t maxDelta;
    std::uint8_t hasOverlap;
    std::uint8_t reserved;
    std::uint32_t overlayWidth;
    std::uint32_t overlayHeight;
    const std::uint8_t* overlayRgba;
    std::uint32_t overlapX0, overlapY0, overlapX1, overlapY1;
    std::uint32_t rightWidth;   // right image size after the resize policy
    std::uint32_t rightHeight;
    std::uint64_t comparedPixels;
    std::uint64_t identicalPixels;
    std::uint64_t similarPixels;
    std::uint64_t differentPixels;
    std::uint64_t sumDelta;
};

struct OpenbcPixelProbe {
    std::int32_t x;
    std::int32_t y;
    std::uint8_t leftPresent[81];
    std::uint8_t rightPresent[81];
    std::uint8_t leftRgba[81 * 4];
    std::uint8_t rightRgba[81 * 4];
};

static_assert(sizeof(void*) == 8, "image compare ABI assumes a 64-bit target");
static_assert(sizeof(OpenbcImageCompareOptions) == 16, "ABI drift vs Rust OpenbcImageCompareOptions");
static_assert(sizeof(OpenbcImageInfo) == 64, "ABI drift vs Rust OpenbcImageInfo");
static_assert(sizeof(OpenbcImageCompareEvent) == 304, "ABI drift vs Rust OpenbcImageCompareEvent");
static_assert(sizeof(OpenbcPixelProbe) == 820, "ABI drift vs Rust OpenbcPixelProbe");

struct OpenbcImageCompare;  // opaque

// Called on a Rust worker thread. Copy what you need and return; do not call back into the API.
typedef void (*OpenbcImageCompareCallback)(void* user, const OpenbcImageCompareEvent* event);

OpenbcImageCompare* openbc_image_compare_create(OpenbcImageCompareCallback callback, void* user,
                                                const OpenbcImageCompareOptions* options);
// After this returns the callback is not running and never will again.
void openbc_image_compare_destroy(OpenbcImageCompare* handle);

// Return 0 ok, 1 queue full, 2 session ended.
int openbc_image_compare_open(OpenbcImageCompare* handle, const std::uint8_t* left, std::size_t leftLen,
                              const std::uint8_t* right, std::size_t rightLen);
int openbc_image_compare_reload(OpenbcImageCompare* handle);
int openbc_image_compare_swap(OpenbcImageCompare* handle);
void openbc_image_compare_set_options(OpenbcImageCompare* handle, const OpenbcImageCompareOptions* options);
// Returns 1 and fills `out` when a compare result exists, else 0.
int openbc_image_compare_probe(OpenbcImageCompare* handle, std::int32_t x, std::int32_t y,
                               OpenbcPixelProbe* out);

}  // extern "C"

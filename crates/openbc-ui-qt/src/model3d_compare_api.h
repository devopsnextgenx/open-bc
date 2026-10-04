// model3d_compare_api.h - C ABI of the 3D Compare backend (crates/openbc-ui-qt/src/model3d_compare_ffi.rs).
// Keep in sync with the #[repr(C)] structs there. Pointers inside an event are valid only for the
// duration of the callback; the callback runs on a Rust thread.
#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {

struct Openbc3dStr { const std::uint8_t* ptr; std::size_t len; };

struct Openbc3dOptions {
    float tolerance;       // mm
    float range;           // heat-map +/- range, mm
    float offset[3];       // extra offset applied to B, mm
    std::uint8_t alignment;        // 0 original, 1 centre at origin, 2 bbox corners, 3 centroids
    std::uint8_t preferGpu;
    std::uint8_t computeChanges;
    std::uint8_t pad;
};

struct Openbc3dMesh {
    const float* positions; std::size_t vertexCount;   // xyz triples
    const std::uint32_t* indices; std::size_t indexCount;
    const std::uint32_t* edges; std::size_t edgeCount; // index pairs (wireframe)
};
struct Openbc3dFileInfo { Openbc3dStr path, format; std::uint64_t sizeBytes; std::int64_t modifiedUnixMs; std::uint8_t hasModified; };
struct Openbc3dStats {
    std::uint64_t triangles, vertices; double area, volume; std::uint64_t sizeBytes;
    float bmin[3], bmax[3]; std::uint8_t watertight;
};
struct Openbc3dSide { Openbc3dFileInfo info; Openbc3dStats stats; Openbc3dMesh mesh; };
struct Openbc3dRegion { std::uint8_t side; std::uint32_t triangles; double area; float maxDeviation; float bmin[3], bmax[3]; };

enum { OPENBC_3D_LOADING = 0, OPENBC_3D_LOADED = 1, OPENBC_3D_RESULT = 2, OPENBC_3D_PATH_ERROR = 3,
       OPENBC_3D_FAILED = 4, OPENBC_3D_NOTICE = 5 };

struct Openbc3dEvent {
    std::uint32_t kind, side;
    Openbc3dStr path, message;
    std::uint8_t leftPresent, rightPresent; Openbc3dSide left, right;                   // LOADED
    Openbc3dOptions options; std::uint32_t backend; float displayShift[3];              // RESULT (0 CPU, 1 GPU)
    const float* bPositions; std::size_t bVertexCount;                                  // null when unchanged
    const float* devA; std::size_t devACount; const float* devB; std::size_t devBCount; std::uint64_t devToken;
    float maxDeviation, meanDeviation, surfaceChanged, tolerance, range;
    const float* histogram; std::size_t histogramCount;
    const Openbc3dRegion* regions; std::size_t regionCount; std::uint32_t regionsTotal;
    std::uint8_t hasChanges, changesTruncated; std::uint64_t changesToken;
    double removedMm3, addedMm3; float cellSize; Openbc3dMesh removed, added;
    float alignMs, deviationMs, analysisMs, changesMs;
};
struct Openbc3dProbe { float point[3]; float deviation; std::uint8_t side; std::uint32_t triangle; };

struct Openbc3dCompare;
typedef void (*Openbc3dCallback)(void* user, const Openbc3dEvent* ev);

Openbc3dCompare* openbc_model3d_create(Openbc3dCallback cb, void* user, const Openbc3dOptions* options);
void openbc_model3d_destroy(Openbc3dCompare* h);   // blocks; no callback runs afterwards
void openbc_model3d_set_options(Openbc3dCompare* h, const Openbc3dOptions* o);   // latest wins
void openbc_model3d_open(Openbc3dCompare* h, const std::uint8_t* l, std::size_t ln, const std::uint8_t* r, std::size_t rn);
void openbc_model3d_reload(Openbc3dCompare* h);
void openbc_model3d_swap(Openbc3dCompare* h);
void openbc_model3d_align(Openbc3dCompare* h, std::uint32_t cmd);   // 0 ICP, 1 parts, 2 reset
std::uint8_t openbc_model3d_probe(Openbc3dCompare* h, std::uint8_t side, const float* origin, const float* dir, Openbc3dProbe* out);
std::size_t openbc_model3d_gpu_name(Openbc3dCompare* h, std::uint8_t* buf, std::size_t cap);
std::size_t openbc_model3d_extensions(Openbc3dCompare* h, std::uint8_t* buf, std::size_t cap);

}  // extern "C"

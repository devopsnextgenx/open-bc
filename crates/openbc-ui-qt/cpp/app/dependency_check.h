// dependency_check.h
// ---------------------------------------------------------------------------
// Qt-free runtime dependency verification, shared by:
//   * `openbc --dependency-check`      (cpp/app/cli_options.cpp)
//   * `openbc-depcheck <exe>`          (cpp/tools/depcheck/depcheck_main.cpp), a tiny console
//     program that still runs when openbc itself cannot start because a Qt/MinGW DLL is missing.
//
// Windows: parses the PE import table of the executable and, recursively, of every DLL found next
//          to it or on PATH. System DLLs (api-ms-*, anything in %SystemRoot%\System32) count as
//          present. Also checks the Qt platform plugin (platforms/qwindows.dll).
// Linux:   runs `ldd` and reports "not found" entries; checks the Qt platform plugin directory
//          only when it sits next to the executable.
//
// IMPORTANT: no Qt includes here, ever - this code must work when Qt cannot be loaded.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace openbc::depcheck {

struct Report {
    std::string executable;
    std::vector<std::string> found;    // "name -> resolved path"
    std::vector<std::string> missing;  // "name (needed by X)"
    std::vector<std::string> notes;    // non-fatal observations
    bool ok() const { return missing.empty(); }
};

// Direct imports of a PE file (DLL names as written in the import table). Empty on parse failure;
// `error` (optional) receives the reason. Exposed for tests.
std::vector<std::string> peImports(const std::string& path, std::string* error = nullptr);

// Verifies `executable`. `extraDirs` are searched before PATH (Windows only).
Report check(const std::string& executable, const std::vector<std::string>& extraDirs = {});

// Human-readable multi-line text, ending with a "MISSING" section when relevant.
std::string format(const Report& report);

}  // namespace openbc::depcheck

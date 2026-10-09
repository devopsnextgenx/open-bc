// dependency_check.cpp - see dependency_check.h. Qt-free on purpose.
#include "app/dependency_check.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace openbc::depcheck {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::uint32_t rd32(const std::vector<unsigned char>& b, size_t o) {
    if (o + 4 > b.size()) return 0;
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (std::uint32_t(b[o + 3]) << 24);
}
std::uint16_t rd16(const std::vector<unsigned char>& b, size_t o) {
    if (o + 2 > b.size()) return 0;
    return std::uint16_t(b[o] | (b[o + 1] << 8));
}

std::vector<std::string> splitPath(const char* value) {
    std::vector<std::string> out;
    if (!value) return out;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string cur;
    for (const char* p = value; *p; ++p) {
        if (*p == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Case-insensitive file lookup in `dir` (Windows names are case-insensitive; the check is also
// run on Linux against Windows trees in tests).
std::string findInDir(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    const fs::path direct = dir / name;
    if (fs::exists(direct, ec)) return direct.string();
    const std::string want = lower(name);
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (lower(it->path().filename().string()) == want) return it->path().string();
    }
    return {};
}

bool isWindowsSystemName(const std::string& lname) {
    if (startsWith(lname, "api-ms-win-") || startsWith(lname, "ext-ms-")) return true;
#ifndef _WIN32
    // Checking a Windows binary from a non-Windows host: there is no System32 to look in.
    static const std::set<std::string> known = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
        "shell32.dll", "ole32.dll", "oleaut32.dll", "ws2_32.dll", "ntdll.dll", "msvcrt.dll", "comdlg32.dll",
        "shlwapi.dll", "userenv.dll", "crypt32.dll", "bcrypt.dll", "winmm.dll", "imm32.dll", "version.dll",
        "dwmapi.dll", "uxtheme.dll", "setupapi.dll", "winspool.drv", "netapi32.dll", "iphlpapi.dll",
        "d3d11.dll", "dxgi.dll", "opengl32.dll", "secur32.dll", "rpcrt4.dll", "mpr.dll", "psapi.dll"};
    return known.count(lname) > 0;
#else
    return false;
#endif
}

Report checkPe(const std::string& exe, const std::vector<std::string>& extraDirs) {
    Report report;
    report.executable = exe;
    std::vector<fs::path> dirs;
    std::error_code ec;
    dirs.push_back(fs::absolute(exe, ec).parent_path());
    for (const auto& d : extraDirs) dirs.emplace_back(d);
    for (const auto& d : splitPath(std::getenv("PATH"))) dirs.emplace_back(d);
    const char* sysRoot = std::getenv("SystemRoot");
    fs::path system32 = sysRoot ? fs::path(sysRoot) / "System32" : fs::path();

    std::set<std::string> seen;
    std::vector<std::pair<std::string, std::string>> queue{{exe, fs::path(exe).filename().string()}};
    while (!queue.empty()) {
        auto [file, owner] = queue.back();
        queue.pop_back();
        std::string err;
        const auto imports = peImports(file, &err);
        if (!err.empty()) {
            report.notes.push_back("cannot parse " + fs::path(file).filename().string() + ": " + err);
            continue;
        }
        for (const auto& dll : imports) {
            const std::string key = lower(dll);
            if (!seen.insert(key).second) continue;
            if (isWindowsSystemName(key)) continue;
            std::string resolved;
            for (const auto& d : dirs) {
                resolved = findInDir(d, dll);
                if (!resolved.empty()) break;
            }
            if (resolved.empty() && !system32.empty()) resolved = findInDir(system32, dll);
            if (resolved.empty()) {
                report.missing.push_back(dll + " (needed by " + owner + ")");
                continue;
            }
            report.found.push_back(dll + " -> " + resolved);
            const bool inWindowsDir = !system32.empty() && fs::path(resolved).parent_path() == system32;
            if (!inWindowsDir) queue.emplace_back(resolved, dll);
        }
    }
    // Qt cannot start a GUI without its platform plugin; this is the classic "works in git bash,
    // silent in cmd" failure, so call it out explicitly.
    const fs::path exeDir = fs::absolute(exe, ec).parent_path();
    const bool hasQt = std::any_of(report.found.begin(), report.found.end(), [](const std::string& s) {
        return lower(s).find("qt6core") != std::string::npos;
    });
    if (hasQt && findInDir(exeDir / "platforms", "qwindows.dll").empty()) {
        report.missing.push_back("platforms/qwindows.dll (Qt platform plugin; run windeployqt)");
    }
    return report;
}

#ifndef _WIN32
Report checkElf(const std::string& exe) {
    Report report;
    report.executable = exe;
    const std::string cmd = "ldd \"" + exe + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        report.notes.push_back("could not run ldd");
        return report;
    }
    char line[1024];
    while (std::fgets(line, sizeof line, pipe)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const auto arrow = s.find("=>");
        if (s.find("not found") != std::string::npos) {
            const auto start = s.find_first_not_of(" \t");
            const std::string name = s.substr(start, s.find(' ', start) - start);
            report.missing.push_back(name + " (needed by " + fs::path(exe).filename().string() + ")");
        } else if (arrow != std::string::npos) {
            const auto start = s.find_first_not_of(" \t");
            report.found.push_back(s.substr(start));
        } else if (s.find("not a dynamic executable") != std::string::npos) {
            report.notes.push_back("not a dynamic executable");
        }
    }
    pclose(pipe);
    return report;
}
#endif

}  // namespace

std::vector<std::string> peImports(const std::string& path, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::vector<std::string>{};
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("cannot open file");
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return fail("not a PE file");
    const size_t pe = rd32(b, 0x3C);
    if (pe + 24 > b.size() || b[pe] != 'P' || b[pe + 1] != 'E') return fail("bad PE signature");
    const size_t coff = pe + 4;
    const unsigned sections = rd16(b, coff + 2);
    const size_t optSize = rd16(b, coff + 16);
    const size_t opt = coff + 20;
    const unsigned magic = rd16(b, opt);
    const size_t dirs = opt + (magic == 0x20B ? 112 : 96);
    const std::uint32_t importRva = rd32(b, dirs + 8);  // data directory #1 (imports)
    if (importRva == 0) return {};                       // no imports at all
    const size_t secTable = opt + optSize;
    auto rvaToOff = [&](std::uint32_t rva) -> size_t {
        for (unsigned i = 0; i < sections; ++i) {
            const size_t s = secTable + i * 40;
            const std::uint32_t va = rd32(b, s + 12), vsize = rd32(b, s + 8), raw = rd32(b, s + 20),
                                rawSize = rd32(b, s + 16);
            if (rva >= va && rva < va + std::max(vsize, rawSize)) return raw + (rva - va);
        }
        return 0;
    };
    std::vector<std::string> out;
    size_t d = rvaToOff(importRva);
    if (d == 0) return fail("bad import table");
    for (; d + 20 <= b.size(); d += 20) {
        const std::uint32_t nameRva = rd32(b, d + 12);
        if (nameRva == 0) break;
        const size_t n = rvaToOff(nameRva);
        if (n == 0 || n >= b.size()) break;
        std::string name;
        for (size_t i = n; i < b.size() && b[i]; ++i) name += char(b[i]);
        out.push_back(name);
    }
    return out;
}

Report check(const std::string& executable, const std::vector<std::string>& extraDirs) {
    std::string err;
    const bool isPe = [&] {
        std::ifstream in(executable, std::ios::binary);
        char m[2] = {0, 0};
        in.read(m, 2);
        return in && m[0] == 'M' && m[1] == 'Z';
    }();
    if (isPe) return checkPe(executable, extraDirs);
#ifndef _WIN32
    return checkElf(executable);
#else
    Report r;
    r.executable = executable;
    r.notes.push_back("not a PE file");
    return r;
#endif
}

std::string format(const Report& r) {
    std::ostringstream o;
    o << "OpenBC dependency check: " << r.executable << "\n";
    o << "Resolved (" << r.found.size() << "):\n";
    for (const auto& f : r.found) o << "  ok       " << f << "\n";
    for (const auto& n : r.notes) o << "  note     " << n << "\n";
    if (r.missing.empty()) {
        o << "RESULT: all dependencies resolved.\n";
    } else {
        o << "MISSING (" << r.missing.size() << "):\n";
        for (const auto& m : r.missing) o << "  MISSING  " << m << "\n";
        o << "RESULT: " << r.missing.size() << " missing. Bundle these next to the executable when packaging.\n";
    }
    return o.str();
}

}  // namespace openbc::depcheck

// openbc-depcheck: console tool, no Qt. Usage: openbc-depcheck [path/to/openbc[.exe]] [--dir EXTRA]...
// Exit code 0 = everything resolved, 2 = something missing, 1 = usage/IO error.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/dependency_check.h"

int main(int argc, char** argv) {
    std::string target;
    std::vector<std::string> dirs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dirs.emplace_back(argv[++i]);
        else if (a == "-h" || a == "--help") {
            std::puts("usage: openbc-depcheck [openbc executable] [--dir EXTRA_SEARCH_DIR]...");
            return 0;
        } else target = a;
    }
    if (target.empty()) {
        // default: the openbc executable sitting next to this tool
        std::filesystem::path self = std::filesystem::absolute(argv[0]);
#ifdef _WIN32
        target = (self.parent_path() / "openbc.exe").string();
#else
        target = (self.parent_path() / "openbc").string();
#endif
    }
    if (!std::filesystem::exists(target)) {
        std::fprintf(stderr, "openbc-depcheck: file not found: %s\n", target.c_str());
        return 1;
    }
    const auto report = openbc::depcheck::check(target, dirs);
    std::fputs(openbc::depcheck::format(report).c_str(), stdout);
    return report.ok() ? 0 : 2;
}

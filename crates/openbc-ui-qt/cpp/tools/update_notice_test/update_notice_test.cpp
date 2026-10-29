// update_notice_test.cpp
// Standalone unit test for home/release_logic.h (Qt-free):
//
//   g++ -std=c++17 -Wall -Wextra -I cpp cpp/tools/update_notice_test/update_notice_test.cpp -o update_notice_test
//   ./update_notice_test
//
// Never compiled into the GUI (build.rs skips cpp/tools).
#include <cstdio>

#include "home/release_logic.h"

using namespace openbc::app::updates;

static int g_failed = 0, g_run = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_run;                                                        \
        if (!(cond)) {                                                  \
            ++g_failed;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static Release rel(const char* tag) { return Release{tag, "", "notes " + std::string(tag), "", ""}; }

int main() {
    CHECK((parseVersion("v1.0.23") == std::vector<int>{1, 0, 23}));
    CHECK((parseVersion("1.2") == std::vector<int>{1, 2}));
    CHECK((parseVersion("v2.0.0-beta.1") == std::vector<int>{2, 0, 0}));
    CHECK(parseVersion("nightly").empty());
    CHECK(parseVersion("").empty());
    CHECK(parseVersion("1..2").size() == 1);

    CHECK(compareVersions("1.0.10", "1.0.9") > 0);   // numeric, not lexical
    CHECK(compareVersions("v1.0.22", "1.0.22") == 0);
    CHECK(compareVersions("1.0", "1.0.0") == 0);
    CHECK(compareVersions("1.0.22", "1.1.0") < 0);

    // Up to date, change log already seen: nothing to show.
    {
        Plan p = plan({rel("v1.0.22"), rel("v1.0.21")}, "1.0.22", "1.0.22", "");
        CHECK(!p.showWhatsNew && !p.showUpdate);
    }
    // Just installed 1.0.22 (last seen 1.0.21): what's new for 1.0.22 only.
    {
        Plan p = plan({rel("v1.0.21"), rel("v1.0.22")}, "1.0.22", "1.0.21", "");
        CHECK(p.showWhatsNew && p.whatsNew.tag == "v1.0.22" && !p.showUpdate);
    }
    // Fresh install (never seen anything): shown once too.
    {
        Plan p = plan({rel("v1.0.22")}, "1.0.22", "", "");
        CHECK(p.showWhatsNew);
    }
    // Newer releases: whole log newest first.
    {
        Plan p = plan({rel("v1.0.23"), rel("v1.0.25"), rel("v1.0.22"), rel("v1.0.24")}, "1.0.22", "1.0.22", "");
        CHECK(p.showUpdate && p.newer.size() == 3);
        CHECK(p.newer[0].tag == "v1.0.25" && p.newer[2].tag == "v1.0.23");
    }
    // "Don't notify for this release" hides exactly that release...
    {
        Plan p = plan({rel("v1.0.23")}, "1.0.22", "1.0.22", "v1.0.23");
        CHECK(!p.showUpdate);
    }
    // ...and a still newer one notifies again.
    {
        Plan p = plan({rel("v1.0.24"), rel("v1.0.23")}, "1.0.22", "1.0.22", "v1.0.23");
        CHECK(p.showUpdate && p.newer.front().tag == "v1.0.24");
    }
    // Junk tags are ignored; a running version newer than any release shows nothing.
    {
        Plan p = plan({rel("nightly"), rel("v1.0.20")}, "1.0.22", "1.0.22", "");
        CHECK(!p.showUpdate && !p.showWhatsNew);
    }

    std::printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}

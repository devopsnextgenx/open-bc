// drop_router_test.cpp
// Standalone unit test for the drop-routing policy (common/drop_router.h) and the
// content sniffer (common/content_sniff.h). Qt-free by design, like tools/depcheck:
//
//   g++ -std=c++17 -Wall -Wextra -I cpp cpp/tools/drop_router_test/drop_router_test.cpp -o drop_router_test
//   ./drop_router_test
//
// Never compiled into the GUI (build.rs skips cpp/tools).
#include <cstdio>
#include <string>
#include <vector>

#include "common/content_sniff.h"
#include "common/drop_router.h"

using namespace openbc::app;
using namespace openbc::app::drop;

static int g_failed = 0, g_run = 0;
#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_run;                                                                             \
        if (!(cond)) {                                                                       \
            ++g_failed;                                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                    \
    } while (0)

static Item file(const char* p, ViewerKind k) { return {p, false, k}; }
static Item dir(const char* p) { return {p, true, ViewerKind::Text}; }
static TabContext tab(TabKind k, Occupancy o = Occupancy::Empty, bool l = false, bool r = false) {
    TabContext c;
    c.tab = k;
    c.occupancy = o;
    c.leftSet = l;
    c.rightSet = r;
    return c;
}

static void testRejects() {
    Decision d = route(tab(TabKind::Home), {});
    CHECK(d.action == Action::Reject && !d.message.empty());
    d = route(tab(TabKind::Home), {file("a", ViewerKind::Text), file("b", ViewerKind::Text), file("c", ViewerKind::Text)});
    CHECK(d.action == Action::Reject);
    CHECK(d.message.find("3") != std::string::npos);  // says how many were dropped
    d = route(tab(TabKind::Folder), {dir("/x"), file("/y.txt", ViewerKind::Text)});
    CHECK(d.action == Action::Reject);
}

static void testHome() {
    const std::vector<std::pair<ViewerKind, TabKind>> cases = {
        {ViewerKind::Picture, TabKind::Picture}, {ViewerKind::Text, TabKind::Text},
        {ViewerKind::Table, TabKind::Table},     {ViewerKind::Registry, TabKind::Registry},
        {ViewerKind::Hex, TabKind::Hex},         {ViewerKind::Media, TabKind::Media},
        {ViewerKind::Model3d, TabKind::Model3d}, {ViewerKind::Version, TabKind::Hex}};
    for (const auto& c : cases) {
        Decision d = route(tab(TabKind::Home), {file("/a", c.first), file("/b", c.first)});
        CHECK(d.action == Action::OpenNewTab);
        CHECK(d.target == c.second);
        CHECK(d.assign == Assign::Both && d.left == "/a" && d.right == "/b" && d.run);
        CHECK(d.message.empty());
    }
    Decision d = route(tab(TabKind::Home), {dir("/l"), dir("/r")});
    CHECK(d.action == Action::OpenNewTab && d.target == TabKind::Folder && d.run);
    // single file: opened with only the left side, no comparison yet
    d = route(tab(TabKind::Home), {file("/a.png", ViewerKind::Picture)});
    CHECK(d.action == Action::OpenNewTab && d.target == TabKind::Picture);
    CHECK(d.assign == Assign::Left && d.left == "/a.png" && d.right.empty() && !d.run);
    // single folder must not fill both sides
    d = route(tab(TabKind::Home), {dir("/only")});
    CHECK(d.target == TabKind::Folder && d.assign == Assign::Left && d.right.empty() && !d.run);
}

static void testMixedTypes() {
    Decision d = route(tab(TabKind::Home), {file("/a.png", ViewerKind::Picture), file("/b.txt", ViewerKind::Text)});
    CHECK(d.action == Action::OpenNewTab && d.target == TabKind::Hex);
    CHECK(d.message.find("Hex Compare") != std::string::npos);
    CHECK(d.left == "/a.png" && d.right == "/b.txt");  // both preserved
}

static void testFolderTab() {
    // Two folders onto an empty Folder Compare tab: reuse, both sides, run. No new tab.
    Decision d = route(tab(TabKind::Folder), {dir("/l"), dir("/r")});
    CHECK(d.action == Action::ApplyToTab && d.target == TabKind::Folder);
    CHECK(d.assign == Assign::Both && d.left == "/l" && d.right == "/r" && d.run);
    // One folder: fills left, right stays untouched (empty), does not run.
    d = route(tab(TabKind::Folder), {dir("/l")});
    CHECK(d.assign == Assign::Left && d.left == "/l" && d.right.empty() && !d.run);
    // Left already set, one folder dropped -> fills right and runs.
    d = route(tab(TabKind::Folder, Occupancy::Populated, true, false), {dir("/r")});
    CHECK(d.assign == Assign::Right && d.right == "/r" && d.left.empty() && d.run);
    // Both set, side hint decides; left is preserved when the hint says right.
    TabContext c = tab(TabKind::Folder, Occupancy::Populated, true, true);
    c.sideHint = Side::Right;
    d = route(c, {dir("/n")});
    CHECK(d.assign == Assign::Right && d.right == "/n" && d.left.empty() && d.run);
    c.sideHint = Side::Left;
    d = route(c, {dir("/n")});
    CHECK(d.assign == Assign::Left && d.left == "/n" && d.right.empty() && d.run);
    c.sideHint.reset();
    d = route(c, {dir("/n")});
    CHECK(d.assign == Assign::Left);  // documented default
    // The side under the cursor always wins, even when the other side is the empty one.
    TabContext e = tab(TabKind::Folder, Occupancy::Populated, false, false);
    e.sideHint = Side::Right;
    d = route(e, {dir("/r")});
    CHECK(d.assign == Assign::Right && d.right == "/r" && d.left.empty() && !d.run);
    e.leftSet = true;  // left already set, dropped on the left half -> replaces left (not "first empty")
    e.sideHint = Side::Left;
    d = route(e, {dir("/l2")});
    CHECK(d.assign == Assign::Left && d.left == "/l2" && d.right.empty() && !d.run);
}

static void testIncompatible() {
    const std::vector<Item> twoImages = {file("/a.png", ViewerKind::Picture), file("/b.png", ViewerKind::Picture)};
    // Empty Text tab + two images -> converted (viewer swapped into the same slot).
    Decision d = route(tab(TabKind::Text, Occupancy::Empty), twoImages);
    CHECK(d.action == Action::ReplaceEmptyTab && d.target == TabKind::Picture && d.run);
    // Active Text tab -> preserved, new Picture tab.
    d = route(tab(TabKind::Text, Occupancy::Populated, true, true), twoImages);
    CHECK(d.action == Action::OpenNewTab && d.target == TabKind::Picture);
    CHECK(d.message.find("kept") != std::string::npos);
    // Text tab with unsaved edits is never replaced.
    d = route(tab(TabKind::Text, Occupancy::Dirty, true, true), twoImages);
    CHECK(d.action == Action::OpenNewTab);
    // Hex + two CSV -> Table.
    d = route(tab(TabKind::Hex, Occupancy::Populated, true, true),
              {file("/a.csv", ViewerKind::Table), file("/b.csv", ViewerKind::Table)});
    CHECK(d.action == Action::OpenNewTab && d.target == TabKind::Table);
    // Folder tab (empty) + two files -> replaced by the file viewer.
    d = route(tab(TabKind::Folder), {file("/a.txt", ViewerKind::Text), file("/b.txt", ViewerKind::Text)});
    CHECK(d.action == Action::ReplaceEmptyTab && d.target == TabKind::Text);
    // Image tab + two text files, empty -> converted.
    d = route(tab(TabKind::Picture), {file("/a.txt", ViewerKind::Text), file("/b.txt", ViewerKind::Text)});
    CHECK(d.action == Action::ReplaceEmptyTab && d.target == TabKind::Text);
    // Single incompatible item keeps the unaffected nothing: left only, no run.
    d = route(tab(TabKind::Text, Occupancy::Populated, true, true), {file("/a.png", ViewerKind::Picture)});
    CHECK(d.action == Action::OpenNewTab && d.assign == Assign::Left && !d.run && d.right.empty());
}

static void testCompatibleReuse() {
    Decision d = route(tab(TabKind::Picture, Occupancy::Populated, true, true),
                       {file("/a.png", ViewerKind::Picture), file("/b.jpg", ViewerKind::Picture)});
    CHECK(d.action == Action::ApplyToTab && !d.confirmDiscard);
    // Dirty table tab, compatible drop: reuse but the viewer must confirm discarding edits.
    d = route(tab(TabKind::Table, Occupancy::Dirty, true, true),
              {file("/a.csv", ViewerKind::Table), file("/b.csv", ViewerKind::Table)});
    CHECK(d.action == Action::ApplyToTab && d.confirmDiscard);
    // Version files land on a Hex tab (reuse).
    d = route(tab(TabKind::Hex), {file("/a.exe", ViewerKind::Version), file("/b.dll", ViewerKind::Version)});
    CHECK(d.action == Action::ApplyToTab && d.target == TabKind::Hex);
}

static void testDeterminism() {
    const std::vector<Item> items = {file("/x.png", ViewerKind::Picture), file("/y.png", ViewerKind::Picture)};
    const Decision a = route(tab(TabKind::Home), items), b = route(tab(TabKind::Home), items);
    CHECK(a.action == b.action && a.target == b.target && a.left == b.left && a.right == b.right);
    // Paths with spaces / non-ASCII pass through byte-for-byte.
    const std::string odd = "/tmp/mi carpeta/caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.png";
    Decision d = route(tab(TabKind::Home), {Item{odd, false, ViewerKind::Picture}});
    CHECK(d.left == odd);
}


static void testSniff() {
    using namespace sniff;
    auto S = [](const std::string& s) { return sniffBytes(reinterpret_cast<const unsigned char*>(s.data()), s.size()); };
    CHECK(S(std::string("\x89PNG\r\n\x1a\n", 8) + "xxxx") == Sniffed::Picture);
    CHECK(S("\xFF\xD8\xFF\xE0....") == Sniffed::Picture);
    CHECK(S("GIF89a....") == Sniffed::Picture);
    CHECK(S(std::string("RIFF\0\0\0\0WEBPVP8 ", 16)) == Sniffed::Picture);
    CHECK(S(std::string("RIFF\0\0\0\0WAVEfmt ", 16)) == Sniffed::Media);
    CHECK(S("ID3\x04....") == Sniffed::Media);
    CHECK(S(std::string("\0\0\0\x18" "ftypmp42", 12)) == Sniffed::Media);
    CHECK(S("glTF\x02\0\0\0") == Sniffed::Model3d);
    CHECK(S("ISO-10303-21;\nHEADER;") == Sniffed::Model3d);
    CHECK(S("Windows Registry Editor Version 5.00\r\n") == Sniffed::Registry);
    CHECK(S("REGEDIT4\r\n") == Sniffed::Registry);
    {   // UTF-16LE registry export
        std::string u = "\xFF\xFE";
        for (char c : std::string("Windows Registry Editor Version 5.00")) { u += c; u += '\0'; }
        CHECK(S(u) == Sniffed::Registry);
    }
    CHECK(S("\x7f" "ELF\x02\x01\x01") == Sniffed::Binary);
    CHECK(S(std::string("abc\0def", 7)) == Sniffed::Binary);
    CHECK(S("plain,csv,text\n1,2,3\n") == Sniffed::Text);
    CHECK(S("\xEF\xBB\xBF" "hello") == Sniffed::Text);
    CHECK(sniffBytes(nullptr, 0) == Sniffed::Unknown);
    CHECK(sniffBytes(reinterpret_cast<const unsigned char*>("ID"), 2) == Sniffed::Text);  // too short for ID3; no crash

    // Precedence: signature > extension > heuristic.
    CHECK(refineKind(ViewerKind::Text, Sniffed::Picture) == ViewerKind::Picture);   // "photo.dat" that is a JPEG
    CHECK(refineKind(ViewerKind::Table, Sniffed::Text) == ViewerKind::Table);       // .csv stays Table
    CHECK(refineKind(ViewerKind::Text, Sniffed::Binary) == ViewerKind::Hex);        // unknown-ext binary
    CHECK(refineKind(ViewerKind::Version, Sniffed::Binary) == ViewerKind::Version); // recognised ext wins
    CHECK(refineKind(ViewerKind::Text, Sniffed::Registry) == ViewerKind::Registry);
}

int main() {
    testRejects();
    testHome();
    testMixedTypes();
    testFolderTab();
    testIncompatible();
    testCompatibleReuse();
    testDeterminism();
    testSniff();
    std::printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}

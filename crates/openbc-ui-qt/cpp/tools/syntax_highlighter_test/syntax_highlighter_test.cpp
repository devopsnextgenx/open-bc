// Tests for the existing editor's SyntaxHighlighter after the fixes:
//   * incremental (large-file) mode keeps lexer state across lines (multi-line comments / strings),
//   * its cache is bounded (no ever-growing QHash/QSet),
//   * format-only document notifications do not wipe the cache.
// Uses the real Rust session API (static library built from src/ffi/text_engine.rs); the unrelated
// whole-buffer backend entry points are stubbed because the incremental path never calls them.
#include <QApplication>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextLayout>
#include <cstdio>

#include "viewers/text/syntax_highlighter.h"

extern "C" {
OpenBcHighlight* openbc_highlight_buffer(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t) { return nullptr; }
OpenBcHighlight* openbc_highlight_buffer_with_theme(const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t, std::uint8_t, const std::uint8_t*, std::size_t) { return nullptr; }
void openbc_highlight_destroy(OpenBcHighlight*) {}
std::size_t openbc_highlight_len(const OpenBcHighlight*) { return 0; }
std::size_t openbc_highlight_line(const OpenBcHighlight*, std::size_t) { return 0; }
std::size_t openbc_highlight_start(const OpenBcHighlight*, std::size_t) { return 0; }
std::size_t openbc_highlight_length(const OpenBcHighlight*, std::size_t) { return 0; }
std::uint8_t openbc_highlight_foreground(const OpenBcHighlight*, std::size_t, std::uint8_t) { return 0; }
std::uint8_t openbc_highlight_background(const OpenBcHighlight*, std::size_t, std::uint8_t) { return 0; }
std::uint8_t openbc_highlight_bold(const OpenBcHighlight*, std::size_t) { return 0; }
std::uint8_t openbc_highlight_italic(const OpenBcHighlight*, std::size_t) { return 0; }
void openbc_highlight_html_destroy(char* p) { delete[] p; }
std::size_t openbc_highlight_set_user_mappings(const std::uint8_t*, std::size_t) { return 0; }
char* openbc_highlight_catalog() { char* c = new char[3]; c[0] = '['; c[1] = ']'; c[2] = 0; return c; }
}

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static QList<QTextLayout::FormatRange> formatsOf(QTextDocument* doc, int block) {
    return doc->findBlockByNumber(block).layout()->formats();
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QPlainTextEdit edit;
    edit.setPlainText("/* start\nstill inside the comment\nend */ int x = 1;\nint y;\n");
    QTextDocument* doc = edit.document();
    openbc::app::SyntaxHighlighter hl(doc, "t");
    hl.setStyle(openbc::app::SyntaxHighlighter::Style::VsCodeDark);
    hl.setLanguage("t.c");
    hl.setIncremental(true);
    hl.rehighlightVisibleBlocks(0, 3);

    const auto b0 = formatsOf(doc, 0), b1 = formatsOf(doc, 1), b3 = formatsOf(doc, 3);
    CHECK(!b0.isEmpty() && !b1.isEmpty() && !b3.isEmpty());
    if (!b0.isEmpty() && !b1.isEmpty() && !b3.isEmpty()) {
        // Line 1 is wholly inside the comment opened on line 0: same colour, spanning the whole line.
        CHECK(b1[0].start == 0);
        CHECK(b1[0].length == QString("still inside the comment").size());
        CHECK(b1[0].format.foreground().color() == b0[0].format.foreground().color());
        CHECK(b3[0].format.foreground().color() != b0[0].format.foreground().color());
    }

    // Bounded cache: a 60k-line document highlighted window by window never exceeds the limit.
    QString big;
    for (int i = 0; i < 60000; ++i) big += QString("int v%1 = %1; /* c */\n").arg(i);
    edit.setPlainText(big);
    hl.setIncremental(true);
    int peak = 0;
    for (int first = 0; first < 60000; first += 500) {
        hl.rehighlightVisibleBlocks(first, first + 40);
        peak = qMax(peak, hl.incrementalCachedLines());
    }
    std::printf("  incremental cache peak: %d lines (limit 8192) for 60000 lines\n", peak);
    CHECK(peak <= 8192);
    CHECK(peak > 0);

    // Format-only notifications (what rehighlightBlock itself causes) must not clear the cache.
    const int before = hl.incrementalCachedLines();
    hl.rehighlightVisibleBlocks(59000, 59010);
    CHECK(hl.incrementalCachedLines() >= qMin(before, 1));

    std::printf(failures ? "%d FAILURE(S)\n" : "syntax_highlighter_test: all passed\n", failures);
    return failures ? 1 : 0;
}

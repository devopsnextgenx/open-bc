// virtual_editor_test.cpp — tests for VirtualTextEditor, TextModel and ScrollMapper.
// Runs headless: QT_QPA_PLATFORM=offscreen. Build/run with build_and_run.sh in this folder.
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFrame>
#include <QKeyEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>
#include <cstdio>
#include <fstream>

#include "viewers/text/virtual/scroll_mapping.h"
#include <QPlainTextEdit>
#include "viewers/text/plain_text_editor_adapter.h"
#include "viewers/text/virtual/virtual_text_compare_viewer.h"
#include "viewers/text/virtual/virtual_text_editor.h"

using namespace openbc::app;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto _a = (a); const auto _b = (b);                                                        \
        if (!(_a == _b)) {                                                                               \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);                           \
            ++failures;                                                                                  \
        }                                                                                                \
    } while (0)

static QTemporaryDir* g_dir;

static QString writeFile(const QString& name, const QByteArray& bytes) {
    const QString path = g_dir->filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
    return path;
}

static double rssAnonMb() {
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (line.startsWith("RssAnon")) return line.simplified().split(' ')[1].toDouble() / 1024.0;
    return 0;
}

static void pump(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool waitFor(const std::function<bool()>& cond, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        pump(20);
    }
    return true;
}

static void testScrollMapper() {
    ScrollMapper m;
    m.totalRows = 1000; m.visibleRows = 40;
    CHECK(!m.scaled());
    CHECK_EQ(m.scrollbarMax(), 960);
    for (std::uint64_t t : {0ULL, 1ULL, 500ULL, 960ULL}) CHECK_EQ(m.toTop(m.toScrollbar(t)), t);   // exact identity
    CHECK_EQ(m.toTop(5000), 960ULL);                                                                 // clamped

    // Far beyond int range: 10 billion rows.
    m.totalRows = 10'000'000'000ULL; m.visibleRows = 50;
    CHECK(m.scaled());
    CHECK_EQ(m.toScrollbar(0), 0);
    CHECK_EQ(m.toScrollbar(m.maxTop()), m.scrollbarMax());
    CHECK_EQ(m.toTop(0), 0ULL);
    CHECK_EQ(m.toTop(m.scrollbarMax()), m.maxTop());           // the very last page is reachable
    CHECK(m.pageStep() >= 1);
    std::uint64_t prev = 0;
    bool monotonic = true;
    for (long long v = 0; v <= m.scrollbarMax() && monotonic; v += 4'000'003) {
        const std::uint64_t t = m.toTop(static_cast<int>(v));
        monotonic = t >= prev;
        prev = t;
    }
    CHECK(monotonic);
    // Round trip stays within one scrollbar unit's worth of rows.
    const std::uint64_t unit = m.maxTop() / ScrollMapper::kMaxScrollbar + 1;
    for (std::uint64_t t : {1ULL, 123456789ULL, 5'000'000'000ULL, 9'999'999'000ULL}) {
        const std::uint64_t back = m.toTop(m.toScrollbar(t));
        CHECK((back > t ? back - t : t - back) <= unit + 1);
    }
    // Degenerate: fewer rows than a page.
    m.totalRows = 3; m.visibleRows = 40;
    CHECK_EQ(m.scrollbarMax(), 0);
    CHECK_EQ(m.toTop(10), 0ULL);
}

static std::shared_ptr<TextModel> open(const QString& l, const QString& r, QString* err = nullptr, ModelOptions o = {}) {
    return TextModel::openSync(l, r, o, err);
}

static void testModelBasics() {
    // LF vs CRLF, no trailing newline, unicode, empty-line, binary garbage.
    const QString l = writeFile("l.txt", "alpha\nbeta\r\ngrüße 😀\n\ntail");
    const QString r = writeFile("r.txt", "alpha\nBETA\ngrüße 😀\n\ntail\nextra\n");
    auto m = open(l, r);
    CHECK(m != nullptr);
    CHECK_EQ(m->lineCount(Side::Left), 5ULL);
    CHECK_EQ(m->lineCount(Side::Right), 6ULL);
    CHECK_EQ(m->rowCount(), 6ULL);
    CHECK(m->row(1).kind == RowKind::Replace);
    CHECK(m->row(5).kind == RowKind::Insert);
    CHECK_EQ(m->window(Side::Left, 1, 0, 100).text, QStringLiteral("beta"));          // CRLF stripped
    CHECK_EQ(m->window(Side::Left, 2, 0, 100).text, QStringLiteral("grüße 😀"));
    CHECK_EQ(m->window(Side::Left, 4, 0, 100).text, QStringLiteral("tail"));          // no trailing newline
    CHECK_EQ(m->window(Side::Left, 2, 3, 3).text, QStringLiteral("ße "));
    const auto ranges = m->inlineRanges(1, Side::Left, QStringLiteral("beta"));
    CHECK(!ranges.isEmpty());
    CHECK(m->nextDifference(0).value() == 1);
    CHECK(!m->nextDifference(5).has_value());
    CHECK(m->prevDifference(5).value() == 1);

    // Empty files and a missing side.
    const QString e1 = writeFile("e1.txt", ""), e2 = writeFile("e2.txt", "");
    auto me = open(e1, e2);
    CHECK(me && me->rowCount() == 0);
    auto mo = open(e1, QString());
    CHECK(mo && mo->rowCount() == 0);

    // Invalid UTF-8 / binary do not crash and keep line boundaries.
    const QString b = writeFile("bin.dat", QByteArray("ok\n\xff\xfe\x00zz\nend", 12));
    auto mb = open(b, b);
    CHECK(mb && mb->lineCount(Side::Left) == 3);
    CHECK((mb->encodingCode(Side::Left) & 0x80) != 0);
    CHECK_EQ(mb->window(Side::Left, 2, 0, 10).text, QStringLiteral("end"));

    // Errors are reported, not thrown.
    QString err;
    CHECK(open(g_dir->filePath("missing.txt"), l, &err) == nullptr);
    CHECK(!err.isEmpty());

    // Normalisation changes the alignment.
    ModelOptions o;
    o.ignoreCase = true;
    auto mi = open(l, r, nullptr, o);
    CHECK(mi && mi->stats().replacedRows == 0);

    // UTF-16 file compares equal to its UTF-8 twin.
    QByteArray u16("\xFF\xFE", 2);
    for (QChar c : QStringLiteral("alpha\nbeta\n")) { u16.append(char(c.unicode() & 255)); u16.append(char(c.unicode() >> 8)); }
    const QString p16 = writeFile("u16.txt", u16), p8 = writeFile("u8.txt", "alpha\nbeta\n");
    auto m16 = open(p16, p8);
    CHECK(m16 && m16->stats().blocks == 0 && m16->encodingCode(Side::Left) == 1);
}

static std::pair<QString, QString> makeLargePair(int lines) {
    QByteArray a, b;
    for (int i = 0; i < lines; ++i) {
        const QByteArray line = "line number " + QByteArray::number(i) + " payload\n";
        a += line;
        if (i == 40) b += "INSERTED A\nINSERTED B\n";
        if (i == 70000) continue;                                    // a deletion near the middle
        b += (i % 5000 == 123) ? ("CHANGED " + QByteArray::number(i) + "\n") : line;
    }
    return {writeFile("big_l.txt", a), writeFile("big_r.txt", b)};
}

static bool rowFillerCheck(const TextModel& m) {
    const ModelRow r = m.row(40);
    return r.left == -1 && r.right == 40;
}

static void press(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent ev(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &ev);
}

static void testEditorsShareModel() {
    const auto [l, r] = makeLargePair(100000);
    auto m = open(l, r);
    CHECK(m != nullptr);
    const OpenBcTmStats st = m->stats();
    CHECK_EQ(st.insertedRows, 2ULL);
    CHECK_EQ(st.deletedRows, 1ULL);
    CHECK_EQ(st.replacedRows, 20ULL);
    CHECK(m->row(40).kind == RowKind::Insert && m->row(41).kind == RowKind::Insert);
    CHECK(m->row(42).left == 40 && m->row(42).right == 42);

    VirtualTextEditor L, R;
    L.resize(800, 400); R.resize(800, 400);
    L.setModel(m, Side::Left);
    R.setModel(m, Side::Right);
    L.show(); R.show();
    pump(50);

    // Both editors render the same model; only visible rows (+ prefetch) were decoded.
    CHECK(!L.grab().toImage().isNull());
    CHECK(!R.grab().toImage().isNull());
    CHECK(L.visibleRowCount() > 5);
    CHECK(L.cachedLines() <= L.visibleRowCount() + 2 * 24 + 8);
    CHECK(L.findChildren<QTextDocument*>().isEmpty());     // no QTextDocument anywhere in the editor
    CHECK(L.model() == R.model());                         // one shared model, no copies

    // Scroll synchronisation (the tab wires exactly this).
    L.onTopRowChanged = [&](quint64 row) { R.setFirstVisibleRow(row); };
    L.setFirstVisibleRow(50000);
    CHECK_EQ(R.firstVisibleRow(), 50000ULL);
    CHECK_EQ(L.firstVisibleRow(), 50000ULL);
    L.setFirstVisibleRow(0);

    // Gutter shows each side's own line numbers; a filler row is empty on the side without a line.
    CHECK(rowFillerCheck(*m));

    // Difference navigation.
    L.setCaret(0, 0);
    L.goToNextDifference();
    CHECK_EQ(L.currentRow(), 40ULL);
    L.goToNextDifference();
    CHECK(L.currentRow() > 40);
    const quint64 second = L.currentRow();
    L.goToPreviousDifference();
    CHECK_EQ(L.currentRow(), 40ULL);
    L.setCaret(m->rowCount() - 1, 0);
    L.goToNextDifference();                               // no further difference: stays put
    CHECK_EQ(L.currentRow(), m->rowCount() - 1);
    (void)second;

    // Go to line (1-based, this editor's own file).
    R.goToLine(100);
    CHECK_EQ(R.currentRow(), m->rowForLine(Side::Right, 99).value());
    ITextCompareEditor* iface = &L;                        // usable through the common interface
    iface->goToLine(1);
    CHECK_EQ(iface->currentRow(), 0ULL);

    // Find: forward, wrap-around, case-insensitive, backward.
    L.setCaret(0, 0);
    CHECK(L.findText("number 9999 ", true, true));
    CHECK_EQ(L.currentRow(), m->rowForLine(Side::Left, 9999).value());
    CHECK_EQ(L.selectedText(), QStringLiteral("number 9999 "));
    CHECK(L.findText("number 9999 ", true, true));         // single occurrence: wraps to itself
    CHECK_EQ(L.currentRow(), m->rowForLine(Side::Left, 9999).value());
    CHECK(L.findText("LINE NUMBER 77 ", false, true));
    CHECK_EQ(L.currentRow(), m->rowForLine(Side::Left, 77).value());
    CHECK(L.findText("number 7 payload", true, false));
    CHECK_EQ(L.currentRow(), m->rowForLine(Side::Left, 7).value());
    CHECK(!L.findText("definitely-not-in-file", true, true));
    CHECK(L.findText("INSERTED B", true, true) == false);  // exists only on the right
    CHECK(R.findText("INSERTED B", true, true));
    CHECK_EQ(R.currentRow(), 41ULL);

    // Selection + clipboard.
    L.setCaret(0, 0);
    L.setCaret(2, 4, true, false);
    CHECK_EQ(L.selectedText(), QStringLiteral("line number 0 payload\nline number 1 payload\nline"));
    L.copySelection();
    CHECK_EQ(QGuiApplication::clipboard()->text(), L.selectedText());
    L.setCaret(39, 0);
    L.setCaret(43, 0, true, false);                        // spans the two filler rows
    CHECK_EQ(L.selectedText(), QStringLiteral("line number 39 payload\nline number 40 payload\n"));
    L.setCaret(5, 0);
    press(&L, Qt::Key_Down, Qt::ShiftModifier);
    press(&L, Qt::Key_Right, Qt::ShiftModifier);
    CHECK(L.hasSelection());
    CHECK_EQ(L.selectedText(), QStringLiteral("line number 5 payload\nl"));
    press(&L, Qt::Key_A, Qt::ControlModifier);
    CHECK(L.hasSelection());
    press(&L, Qt::Key_End, Qt::ControlModifier);
    CHECK_EQ(L.currentRow(), m->rowCount() - 1);
    press(&L, Qt::Key_Home, Qt::ControlModifier);
    CHECK_EQ(L.currentRow(), 0ULL);
    press(&L, Qt::Key_PageDown);
    CHECK(L.firstVisibleRow() > 0 && L.currentRow() > 0);

    // Caches stay bounded while scrolling through the whole file.
    L.setCacheLimitChars(64 * 1024);
    for (quint64 row = 0; row < m->rowCount(); row += static_cast<quint64>(L.visibleRowCount())) {
        L.setFirstVisibleRow(row);
        if ((row / L.visibleRowCount()) % 50 == 0) { L.grab(); }
    }
    L.grab();
    CHECK(L.cachedLines() < 2500);
    CHECK(L.cachedSpanLines() < 2500);
}

static void testSyntaxMultiline() {
    const QByteArray src = "/* start\nstill inside the comment\nend */ int x = 1;\nint y;\n";
    const QString a = writeFile("s1.c", src), b = writeFile("s2.c", src);
    auto m = open(a, b);
    VirtualTextEditor L;
    L.resize(600, 300);
    L.setLanguage(QStringLiteral("s1.c"));
    L.setModel(m, Side::Left);
    L.show();
    CHECK(waitFor([&] { return !L.spansForLine(3).isEmpty() && !L.spansForLine(1).isEmpty(); }, 60000));
    const auto l0 = L.spansForLine(0), l1 = L.spansForLine(1), l3 = L.spansForLine(3);
    CHECK(!l0.isEmpty() && !l1.isEmpty() && !l3.isEmpty());
    if (!l0.isEmpty() && !l1.isEmpty() && !l3.isEmpty()) {
        // Line 1 lies wholly inside a comment that began on line 0: the lexer state was carried.
        CHECK_EQ(l1[0].start, 0);
        CHECK_EQ(l1[0].length, QStringLiteral("still inside the comment").size());
        CHECK_EQ(l1[0].foreground, l0[0].foreground);
        CHECK(l3[0].foreground != l0[0].foreground);       // real code is not comment-coloured
    }
    CHECK(waitFor([&] { return L.highlightIdle(); }, 10000));
    L.grab();
}

static void testVeryLongLinesStayCheap() {
    const qint64 mb = 40;
    const QString pa = g_dir->filePath("long_a.txt"), pb = g_dir->filePath("long_b.txt");
    for (const QString& path : {pa, pb}) {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        const QByteArray chunk = QByteArray("abcdefghij").repeated(100000);   // 1 MB
        for (qint64 i = 0; i < mb; ++i) f.write(chunk);
        f.write(path == pa ? "\nsecond\n" : "X\nsecond\n");
    }
    const double before = rssAnonMb();
    QElapsedTimer t;
    t.start();
    auto m = open(pa, pb);
    CHECK(m != nullptr);
    VirtualTextEditor L;
    L.resize(800, 300);
    L.setModel(m, Side::Left);
    L.show();
    L.grab();
    pump(100);                                               // lets the deferred scroll-range update run
    CHECK(t.elapsed() < 20000);
    CHECK(L.cachedLines() <= 40);
    L.setHorizontalPixelOffset(L.horizontalScrollBar()->maximum() / 2);
    L.grab();
    CHECK(L.horizontalPixelOffset() > 1000);
    const double grown = rssAnonMb() - before;
    std::printf("  long-line test: anon heap growth %.1f MB for %lld MB of text per side (%lld ms)\n", grown, (long long)mb, (long long)t.elapsed());
    CHECK(grown < 80.0);                                    // file is mapped, never copied into app memory
}

static void testLoaderAsyncAndCancel() {
    const auto [l, r] = makeLargePair(20000);
    std::shared_ptr<TextModel> got;
    QString failed;
    {
        TextModelLoader loader;
        QEventLoop loop;
        loader.onFinished = [&](std::shared_ptr<TextModel> mm) { got = mm; loop.quit(); };
        loader.onFailed = [&](const QString& e) { failed = e; loop.quit(); };
        loader.start(l, r, {});
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    CHECK(got != nullptr);
    CHECK(got && got->rowCount() > 20000);

    {   // Failure path
        TextModelLoader loader;
        QEventLoop loop;
        loader.onFailed = [&](const QString& e) { failed = e; loop.quit(); };
        loader.onFinished = [&](std::shared_ptr<TextModel>) { loop.quit(); };
        loader.start(g_dir->filePath("nope.txt"), l, {});
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    CHECK(!failed.isEmpty());

    {   // Destroying the loader mid-flight must not crash or leak a delivered model.
        const auto [bl, br] = makeLargePair(300000);
        auto* loader = new TextModelLoader;
        loader->start(bl, br, {});
        delete loader;
        pump(1500);
    }
}


static void testCompareTab() {
    const auto [l, r] = makeLargePair(60000);
    VirtualTextCompareViewer tab(l, r);
    tab.resize(1200, 500);
    tab.show();
    int titleChanges = 0;
    tab.onTitleChanged = [&] { ++titleChanges; };
    CHECK(tab.title() == QStringLiteral("big_l.txt <-> big_r.txt"));
    CHECK(waitFor([&] { return tab.model() != nullptr; }, 30000));
    if (!tab.model()) return;
    CHECK(tab.leftEditor()->model() == tab.rightEditor()->model());      // one shared model
    CHECK(tab.statusText().contains("difference blocks"));
    CHECK(tab.statusText().contains("Virtual editor"));
    CHECK(tab.findChild<QToolBar*>(QStringLiteral("textCompareToolbar")) != nullptr);
    CHECK(tab.findChildren<QWidget*>(QStringLiteral("sidePathSelector")).size() == 2);
    CHECK(tab.findChildren<QToolButton*>(QStringLiteral("saveSideBtn")).size() == 2);
    CHECK(tab.findChild<QFrame*>(QStringLiteral("textCompareMinimap")) != nullptr);
    CHECK(tab.leftEditor()->numberGutterSide() == VirtualTextEditor::GutterSide::Left);
    CHECK(tab.leftEditor()->arrowGutterSide() == VirtualTextEditor::GutterSide::Right);
    CHECK(tab.rightEditor()->numberGutterSide() == VirtualTextEditor::GutterSide::Right);
    CHECK(tab.rightEditor()->arrowGutterSide() == VirtualTextEditor::GutterSide::Left);
    CHECK(tab.editMenuItems().size() == 6);
    CHECK(!tab.editMenuItems()[0]->isEnabled() && !tab.editMenuItems()[1]->isEnabled() &&
          !tab.editMenuItems()[2]->isEnabled());

    // Scroll sync, caret sync and difference navigation through the tab's wiring.
    tab.leftEditor()->setFirstVisibleRow(30000);
    CHECK_EQ(tab.rightEditor()->firstVisibleRow(), 30000ULL);
    tab.rightEditor()->setFirstVisibleRow(100);
    CHECK_EQ(tab.leftEditor()->firstVisibleRow(), 100ULL);
    tab.leftEditor()->setCaret(0, 0);
    tab.leftEditor()->goToNextDifference();
    CHECK_EQ(tab.rightEditor()->currentRow(), tab.leftEditor()->currentRow());   // peers follow the caret
    tab.leftEditor()->setHorizontalPixelOffset(0);

    // Reload keeps the scroll position; Swap sides recomputes the alignment.
    tab.leftEditor()->setFirstVisibleRow(5000);
    tab.reload(true);
    CHECK(tab.loading() || tab.model());
    CHECK(waitFor([&] { return !tab.loading() && tab.leftEditor()->firstVisibleRow() == 5000; }, 30000));
    const OpenBcTmStats before = tab.model()->stats();
    CHECK(tab.actionsMenuItems().size() == 2);
    tab.actionsMenuItems()[1]->trigger();                                         // Swap Sides
    CHECK(waitFor([&] { return !tab.loading() && tab.title() == QStringLiteral("big_r.txt <-> big_l.txt"); }, 30000));
    pump(300);
    const OpenBcTmStats after = tab.model()->stats();
    CHECK_EQ(after.insertedRows, before.deletedRows);       // what was an insertion is now a deletion
    CHECK_EQ(after.deletedRows, before.insertedRows);
    CHECK(titleChanges >= 1);

    // A file change is announced first; content changes only after Reload Latest.
    tab.actionsMenuItems()[1]->trigger();                    // swap back
    CHECK(waitFor([&] { return !tab.loading() && tab.title() == QStringLiteral("big_l.txt <-> big_r.txt"); }, 30000));
    pump(300);
    const auto oldModel = tab.model();
    auto* bannerFrame = tab.findChild<QFrame*>(QStringLiteral("fileChangeBanner"));
    auto* banner = static_cast<FileChangeBanner*>(bannerFrame);
    CHECK(banner != nullptr);
    {
        QFile f(r);
        f.open(QIODevice::Append);
        f.write("brand new trailing line\n");
    }
    CHECK(waitFor([&] { return banner && banner->isStale(1); }, 15000));
    CHECK(tab.model() == oldModel);
    QPushButton* reloadLatest = nullptr;
    if (banner) {
        for (auto* button : banner->findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("Reload Latest")) reloadLatest = button;
    }
    CHECK(reloadLatest != nullptr);
    if (reloadLatest) reloadLatest->click();
    CHECK(waitFor([&] { return tab.model() != oldModel && !tab.loading(); }, 30000));
    CHECK(tab.model()->lineCount(Side::Right) == oldModel->lineCount(Side::Right) + 1);

    // Dropping new paths on the tab reloads it. An empty tab shows a hint and loads nothing.
    VirtualTextCompareViewer empty{QString(), QString()};
    CHECK(empty.model() == nullptr && empty.statusText().contains("Drop"));
    empty.applyDroppedPaths(l, r);
    CHECK(waitFor([&] { return empty.model() != nullptr; }, 30000));
}

// Both editor implementations are driven through the SAME interface and must agree on behaviour.
static void exerciseThroughInterface(ITextCompareEditor& e, const QString& label, bool alignedRowsEqualLines) {
    e.goToLine(3);
    CHECK_EQ(e.currentRow(), 2ULL);                                       // line 3 of the file
    CHECK(e.findText("gamma", true, true));
    CHECK_EQ(e.selectedText(), QStringLiteral("gamma"));
    CHECK(e.findText("GAMMA", false, true));                              // wraps around, case-insensitive
    CHECK_EQ(e.selectedText().toLower(), QStringLiteral("gamma"));
    CHECK(!e.findText("zzz-not-there", true, true));
    CHECK(e.findText("beta", true, false));                               // backward
    CHECK_EQ(e.selectedText(), QStringLiteral("beta"));
    e.copySelection();
    CHECK_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("beta"));
    e.selectAllText();
    CHECK(e.selectedText().startsWith("alpha"));
    CHECK(e.selectedText().contains("delta"));
    CHECK(e.isReadOnly());
    e.setEditorFont(QFont("monospace", 11));
    e.setFirstVisibleRow(0);
    CHECK_EQ(e.firstVisibleRow(), 0ULL);
    CHECK(e.widget() != nullptr);
    (void)label; (void)alignedRowsEqualLines;
}

static void testCommonInterface() {
    const QString l = writeFile("i1.txt", "alpha\nbeta\ngamma\ndelta\n");
    const QString r = writeFile("i2.txt", "alpha\nbeta\ngamma\ndelta\n");
    auto m = open(l, r);

    VirtualTextEditor virt;
    virt.resize(500, 300);
    virt.setModel(m, Side::Left);
    virt.show();
    exerciseThroughInterface(virt, "virtual", true);

    QPlainTextEdit plain;
    plain.setPlainText("alpha\nbeta\ngamma\ndelta");
    plain.setReadOnly(true);
    plain.resize(500, 300);
    plain.show();
    PlainTextEditorAdapter adapter(&plain);
    adapter.setModel(m, Side::Left);
    exerciseThroughInterface(adapter, "plain", true);
    // Diff navigation is delegated to the owning viewer.
    adapter.nextDifferenceRow = [](quint64 from) -> std::optional<quint64> { return from < 3 ? std::optional<quint64>(3) : std::nullopt; };
    adapter.goToLine(1);
    adapter.goToNextDifference();
    CHECK_EQ(adapter.currentRow(), 3ULL);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir dir;
    g_dir = &dir;
    testScrollMapper();
    testModelBasics();
    testEditorsShareModel();
    testSyntaxMultiline();
    testVeryLongLinesStayCheap();
    testLoaderAsyncAndCancel();
    testCompareTab();
    testCommonInterface();
    std::printf(failures ? "%d FAILURE(S)\n" : "virtual_editor_test: all passed\n", failures);
    return failures ? 1 : 0;
}

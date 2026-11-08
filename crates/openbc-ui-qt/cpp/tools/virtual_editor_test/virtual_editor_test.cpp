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
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
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
    // Edit menu: Undo, Redo, Save, Save As, separator, Copy, Select All. While locked, Undo/Redo stay
    // enabled (using them flashes the lock); Save is off because nothing is unsaved.
    CHECK(tab.editMenuItems().size() == 7);
    CHECK(tab.editMenuItems()[0]->isEnabled() && tab.editMenuItems()[1]->isEnabled());
    CHECK(!tab.editMenuItems()[2]->isEnabled());
    CHECK(!tab.writeEnabled());

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

// ======================================================================================================
// Write mode: lock icon, flashing, editing, copy block, undo/redo, save, unsaved-changes, rules
// ======================================================================================================

static const QColor kRed(0xe5, 0x39, 0x35), kGreen(0x2e, 0xa0, 0x43), kBlue(0x1e, 0x88, 0xe5), kYellow(0xfd, 0xd8, 0x35);

static std::unique_ptr<VirtualTextCompareViewer> makeTab(const QString& l, const QString& r) {
    auto tab = std::make_unique<VirtualTextCompareViewer>(l, r);
    tab->resize(1200, 500);
    tab->show();
    waitFor([&] { return tab->model() != nullptr && !tab->loading(); }, 30000);
    return tab;
}

static void typeText(QWidget* w, const QString& text) {
    for (const char32_t cp : text.toUcs4()) {              // one key event per CODE POINT (emoji = 2 UTF-16 units)
        QKeyEvent ev(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QString::fromUcs4(&cp, 1));
        QApplication::sendEvent(w, &ev);
    }
}

static void key(QWidget* w, int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
    QKeyEvent ev(QEvent::KeyPress, k, m);
    QApplication::sendEvent(w, &ev);
}

static QString lineOf(const std::shared_ptr<TextModel>& m, Side s, quint64 line) { return m->window(s, line, 0, 1000).text; }

static QStringList linesOf(const std::shared_ptr<TextModel>& m, Side s) {
    QStringList out;
    for (quint64 i = 0; i < m->lineCount(s); ++i) out << lineOf(m, s, i);
    return out;
}

// The alignment must still cover every line of both sides exactly once.
static bool alignmentValid(const std::shared_ptr<TextModel>& m) {
    quint64 nl = 0, nr = 0;
    for (quint64 r = 0; r < m->rowCount(); ++r) {
        const ModelRow row = m->row(r);
        if (row.left >= 0) { if (static_cast<quint64>(row.left) != nl) return false; ++nl; }
        if (row.right >= 0) { if (static_cast<quint64>(row.right) != nr) return false; ++nr; }
    }
    return nl == m->lineCount(Side::Left) && nr == m->lineCount(Side::Right);
}

// Presses a button of the (modal) unsaved-changes dialog as soon as it appears.
static void answerDialog(const QStringList& buttons) {
    auto* queue = new QStringList(buttons);
    auto* timer = new QTimer;
    timer->setInterval(40);
    auto* elapsed = new QElapsedTimer;
    elapsed->start();
    QObject::connect(timer, &QTimer::timeout, [timer, queue, elapsed]() {
        if (queue->isEmpty() || elapsed->elapsed() > 15000) {
            timer->stop();
            timer->deleteLater();
            delete queue;
            delete elapsed;
            return;
        }
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (w->objectName() != QLatin1String("unsavedChangesDialog") || !w->isVisible()) continue;
            for (auto* b : w->findChildren<QPushButton*>())
                if (b->isEnabled() && b->text() == queue->first()) {
                    queue->removeFirst();
                    b->click();
                    return;
                }
        }
    });
    timer->start();
}

static bool dialogVisible() {
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->objectName() == QLatin1String("unsavedChangesDialog") && w->isVisible()) return true;
    return false;
}

static void clickArrow(VirtualTextEditor* e, quint64 row, bool arrowOnRight) {
    int y = -1;
    for (int probe = 0; probe < e->viewport()->height(); ++probe)
        if (e->rowAt(probe) == static_cast<qint64>(row)) { y = probe + 2; break; }
    const int x = arrowOnRight ? e->viewport()->width() - 10 : 10;
    const QPoint pos(x, y);
    QMouseEvent press(QEvent::MouseButtonPress, pos, e->viewport()->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, e->viewport()->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(e->viewport(), &press);
    QApplication::sendEvent(e->viewport(), &release);
}

static QString readAll(const QString& path) {
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return QString::fromUtf8(f.readAll());
}

static void focusEditor(VirtualTextEditor* e) {
    e->setFocus();
    if (e->onFocused) e->onFocused();
}

static void testLockFlashing() {
    const QString l = writeFile("w1_l.txt", "alpha\nbeta\ngamma\ndelta\n");
    const QString r = writeFile("w1_r.txt", "alpha\nBETA\ngamma\ndelta\nextra\n");
    auto tab = makeTab(l, r);
    auto* le = tab->leftEditor();
    CHECK(!tab->writeEnabled());
    CHECK(tab->lockColor() == kRed);                       // read-only = red lock
    CHECK(le->isReadOnly() && tab->rightEditor()->isReadOnly());
    CHECK(tab->lockButton()->toolTip().contains("Read-only"));
    le->setCaret(0, 5);

    // One blocked edit: the text is untouched and the lock flashes red-blue-yellow-red, three times, in 1.2 s.
    QElapsedTimer clock;
    clock.start();
    typeText(le, "x");
    CHECK(tab->lockFlashing());
    CHECK_EQ(tab->lockFlashStep(), 0);
    CHECK_EQ(lineOf(tab->model(), Side::Left, 0), QStringLiteral("alpha"));
    QVector<QColor> sequence{tab->lockColor()};
    qint64 flashMs = 0;
    while (tab->lockFlashing() && clock.elapsed() < 4000) {
        pump(5);
        if (tab->lockColor() != sequence.last()) sequence.push_back(tab->lockColor());
        flashMs = clock.elapsed();
    }
    const QVector<QColor> expected{kRed, kBlue, kYellow, kRed, kBlue, kYellow, kRed, kBlue, kYellow, kRed};
    CHECK(sequence == expected);
    std::printf("  lock flash: %lld ms, %d colour changes\n", static_cast<long long>(flashMs), static_cast<int>(sequence.size()));
    CHECK(flashMs >= 1100 && flashMs <= 1700);
    CHECK(!tab->lockFlashing());
    CHECK(tab->lockColor() == kRed);                       // back to the steady red lock

    // EVERY attempt restarts the flash: Backspace, Delete, Enter, Tab, paste, cut, undo, redo, copy arrow...
    QGuiApplication::clipboard()->setText("clip");
    le->setCaret(0, 2);
    le->setCaret(0, 4, true, false);                       // a selection, so Cut has something to cut
    struct Attempt { const char* name; std::function<void()> run; };
    const QVector<Attempt> attempts{
        {"backspace", [&] { key(le, Qt::Key_Backspace); }},
        {"delete", [&] { key(le, Qt::Key_Delete); }},
        {"enter", [&] { key(le, Qt::Key_Return); }},
        {"paste", [&] { key(le, Qt::Key_V, Qt::ControlModifier); }},
        {"cut", [&] { key(le, Qt::Key_X, Qt::ControlModifier); }},
        {"undo shortcut", [&] { key(le, Qt::Key_Z, Qt::ControlModifier); }},
        {"undo action", [&] { tab->editMenuItems()[0]->trigger(); }},
        {"redo action", [&] { tab->editMenuItems()[1]->trigger(); }},
        {"copy arrow", [&] { clickArrow(le, 1, true); }},
        {"copy button", [&] { tab->copyDiffAction()->trigger(); }},
        {"input method", [&] { QInputMethodEvent ev; ev.setCommitString("é"); QApplication::sendEvent(le, &ev); }},
    };
    for (const Attempt& a : attempts) {
        pump(220);                                         // let the previous flash advance
        CHECK(tab->lockFlashing() ? tab->lockFlashStep() > 0 : true);
        a.run();
        if (!(tab->lockFlashing() && tab->lockFlashStep() == 0)) std::printf("  attempt '%s' did not restart the flash\n", a.name);
        CHECK(tab->lockFlashing() && tab->lockFlashStep() == 0);
    }
    CHECK_EQ(linesOf(tab->model(), Side::Left), (QStringList{"alpha", "beta", "gamma", "delta"}));   // nothing was edited
    CHECK(!tab->hasUnsavedChanges());
    CHECK_EQ(tab->model()->stats().blocks, 2ULL);                                                      // nothing was copied

    // Tab keeps normal focus navigation while read-only, so it is not an edit attempt.
    waitFor([&] { return !tab->lockFlashing(); }, 4000);
    key(le, Qt::Key_Tab);
    CHECK(!tab->lockFlashing());

    // A restart extends the flash: at 1.4 s after the first attempt a second one made at 0.5 s still runs.
    waitFor([&] { return !tab->lockFlashing(); }, 4000);
    typeText(le, "x");
    pump(500);
    typeText(le, "x");
    pump(900);
    CHECK(tab->lockFlashing());
    CHECK(waitFor([&] { return !tab->lockFlashing(); }, 3000));
}

static void testUnlockAndTyping() {
    const QString l = writeFile("w2_l.txt", "alpha\nbeta\ngamma\ndelta\n");
    const QString r = writeFile("w2_r.txt", "alpha\nBETA\ngamma\ndelta\nextra\n");
    auto tab = makeTab(l, r);
    auto* le = tab->leftEditor();
    auto* re = tab->rightEditor();
    auto model = tab->model();

    tab->lockButton()->click();                            // click the lock: write mode on, icon green
    CHECK(tab->writeEnabled());
    CHECK(tab->lockColor() == kGreen);
    CHECK(!tab->lockFlashing());
    CHECK(tab->lockButton()->toolTip().contains("Editing"));
    CHECK(!le->isReadOnly() && !re->isReadOnly());
    CHECK(tab->statusText().contains("editing"));

    // Typing on the right side edits the right file only.
    focusEditor(re);
    re->setCaret(1, 4);
    typeText(re, "!??");
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("BETA!??"));
    CHECK(model->isDirty(Side::Right) && !model->isDirty(Side::Left));
    CHECK(tab->title().endsWith(" *"));
    CHECK(tab->saveButton(false)->isEnabled() && !tab->saveButton(true)->isEnabled());
    CHECK(tab->statusText().contains("unsaved"));
    CHECK_EQ(re->caret().second, 7);
    CHECK(alignmentValid(model));
    CHECK(!tab->lockFlashing());                           // editing while unlocked never flashes

    // Typing is ONE undo step; redo brings it back; undo to the saved state clears "modified".
    tab->editMenuItems()[0]->trigger();
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("BETA"));
    CHECK(!model->isDirty(Side::Right));
    CHECK(!tab->title().endsWith(" *"));
    tab->editMenuItems()[1]->trigger();
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("BETA!??"));
    CHECK(model->isDirty(Side::Right));
    tab->editMenuItems()[0]->trigger();

    // Typing on the left makes the row equal again, so the block disappears.
    focusEditor(le);
    const quint64 blocksBefore = model->stats().blocks;
    le->setCaret(1, 4);
    key(le, Qt::Key_Backspace);                            // "beta" -> "bet"
    typeText(le, "A");                                     // "betA"
    CHECK_EQ(lineOf(model, Side::Left, 1), QStringLiteral("betA"));
    for (int i = 0; i < 3; ++i) key(le, Qt::Key_Backspace);
    key(le, Qt::Key_Backspace);
    CHECK_EQ(lineOf(model, Side::Left, 1), QStringLiteral(""));
    typeText(le, "BETA");
    CHECK_EQ(lineOf(model, Side::Left, 1), QStringLiteral("BETA"));
    CHECK(model->row(1).kind == RowKind::Equal);
    CHECK_EQ(model->stats().blocks, blocksBefore - 1);
    CHECK(alignmentValid(model));

    // Both editors repaint with the new state (no crash, caches in step).
    CHECK(!le->grab().toImage().isNull() && !re->grab().toImage().isNull());

    // Typing on a FILLER row creates a line.
    const quint64 lastRow = model->rowCount() - 1;
    CHECK(model->row(lastRow).kind == RowKind::Insert);
    le->setCaret(lastRow, 0);
    typeText(le, "Q");
    CHECK_EQ(linesOf(model, Side::Left).last(), QStringLiteral("Q"));
    CHECK(model->row(lastRow).kind == RowKind::Replace);
    CHECK(alignmentValid(model));
    tab->editMenuItems()[0]->trigger();                    // undo the typed line
    CHECK_EQ(model->lineCount(Side::Left), 4ULL);
}

static void testLineEditing() {
    const QString l = writeFile("w3_l.txt", "alpha\n    indented\nbeta\n");
    const QString r = writeFile("w3_r.txt", "alpha\n    indented\nbeta\n");
    auto tab = makeTab(l, r);
    tab->setWriteMode(true);
    auto* le = tab->leftEditor();
    auto model = tab->model();
    focusEditor(le);

    le->setCaret(0, 5);
    key(le, Qt::Key_Tab);                                  // write mode: Tab inserts a tab character
    CHECK_EQ(lineOf(model, Side::Left, 0), QStringLiteral("alpha\t"));
    key(le, Qt::Key_Backspace);
    CHECK_EQ(lineOf(model, Side::Left, 0), QStringLiteral("alpha"));
    le->setCaret(1, 12);
    key(le, Qt::Key_Return);                               // auto-indent copies the leading whitespace
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha", "    indented", "    ", "beta"}));
    CHECK(le->caret() == (std::pair<quint64, int>{2, 4}));
    typeText(le, "x");
    key(le, Qt::Key_Left);
    key(le, Qt::Key_Home);
    key(le, Qt::Key_Backspace);                            // at column 0: joins with the previous line
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha", "    indented    x", "beta"}));
    le->setCaret(0, 5);
    key(le, Qt::Key_Delete);                               // at the end of a line: joins the next one
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha    indented    x", "beta"}));
    CHECK(alignmentValid(model));

    // Select everything and type: replaces all lines.
    key(le, Qt::Key_A, Qt::ControlModifier);
    typeText(le, "Z");
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"Z"}));
    CHECK(alignmentValid(model));
    tab->editMenuItems()[0]->trigger();
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha    indented    x", "beta"}));

    // Paste multi-line text (mixed line endings), then cut a selection. Rows are not lines here: the
    // right file has more lines, so the left pane shows filler rows; address lines through their rows.
    const auto rowOf = [&](quint64 line) { return model->rowForLine(Side::Left, line).value(); };
    QGuiApplication::clipboard()->setText("p1\np2\r\np3");
    le->setCaret(rowOf(1), 4);
    key(le, Qt::Key_V, Qt::ControlModifier);
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha    indented    x", "betap1", "p2", "p3"}));
    CHECK(le->caret() == (std::pair<quint64, int>{rowOf(3), 2}));
    le->setCaret(rowOf(0), 0);
    le->setCaret(rowOf(0), 5, true, false);
    key(le, Qt::Key_X, Qt::ControlModifier);
    CHECK_EQ(QGuiApplication::clipboard()->text(), QStringLiteral("alpha"));
    CHECK_EQ(lineOf(model, Side::Left, 0), QStringLiteral("    indented    x"));

    // A selection spanning several rows is deleted as one edit.
    le->setCaret(rowOf(0), 4);
    le->setCaret(rowOf(2), 1, true, false);
    key(le, Qt::Key_Backspace);
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"    2", "p3"}));
    CHECK(alignmentValid(model));

    // Undo everything: back to the original file content, and clean.
    while (le->canUndo()) CHECK(le->undo());
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha", "    indented", "beta"}));
    CHECK(!tab->hasUnsavedChanges());
    CHECK_EQ(model->stats().blocks, 0ULL);

    // Unicode: emoji are deleted as a whole.
    le->setCaret(0, 5);
    typeText(le, QString::fromUtf8("😀"));
    CHECK_EQ(lineOf(model, Side::Left, 0), QString::fromUtf8("alpha😀"));
    key(le, Qt::Key_Backspace);
    CHECK_EQ(lineOf(model, Side::Left, 0), QStringLiteral("alpha"));
}

static void testCopyBlocks() {
    const QString l = writeFile("w4_l.txt", "alpha\nbeta\ngamma\ndelta\n");
    const QString r = writeFile("w4_r.txt", "alpha\nBETA\ngamma\ndelta\nextra\n");
    auto tab = makeTab(l, r);
    auto* le = tab->leftEditor();
    auto* re = tab->rightEditor();
    auto model = tab->model();
    CHECK_EQ(model->stats().blocks, 2ULL);

    clickArrow(le, 1, true);                               // locked: flashes, copies nothing
    CHECK(tab->lockFlashing());
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("BETA"));
    tab->setWriteMode(true);

    clickArrow(le, 0, true);                               // row 0 is not a difference: no arrow there
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("BETA"));
    clickArrow(le, 1, true);                               // left -> right
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("beta"));
    CHECK_EQ(model->stats().blocks, 1ULL);
    CHECK(model->isDirty(Side::Right) && !model->isDirty(Side::Left));

    clickArrow(re, 4, false);                              // the right editor's arrow points left: right -> left
    CHECK_EQ(linesOf(model, Side::Left), (QStringList{"alpha", "beta", "gamma", "delta", "extra"}));
    CHECK_EQ(model->stats().blocks, 0ULL);
    CHECK(model->isDirty(Side::Left));
    CHECK(alignmentValid(model));

    // Each copy is one undo step on the side that received it.
    focusEditor(le);
    tab->editMenuItems()[0]->trigger();
    CHECK_EQ(model->lineCount(Side::Left), 4ULL);
    focusEditor(re);
    tab->editMenuItems()[0]->trigger();
    CHECK_EQ(lineOf(model, Side::Right, 1), QStringLiteral("BETA"));
    CHECK(!tab->hasUnsavedChanges());

    // The toolbar Copy button copies the difference at the caret, in the direction of the active pane.
    focusEditor(re);
    re->setCaret(1, 0);
    tab->copyDiffAction()->trigger();                      // right -> left
    CHECK_EQ(lineOf(model, Side::Left, 1), QStringLiteral("BETA"));
    re->setCaret(0, 0);                                    // not inside a difference
    const quint64 undoDepth = model->editCount(Side::Left);
    tab->copyDiffAction()->trigger();
    CHECK_EQ(model->editCount(Side::Left), undoDepth);
    CHECK(tab->statusText().contains("difference"));
}

static void testSaving() {
    const QString l = writeFile("w5_l.txt", "alpha\nbeta\ngamma\n");
    const QString r = writeFile("w5_r.txt", "alpha\r\nBETA\r\ngamma");    // CRLF, no trailing newline
    auto tab = makeTab(l, r);
    tab->setWriteMode(true);
    auto* le = tab->leftEditor();
    auto* re = tab->rightEditor();
    auto model = tab->model();
    focusEditor(re);
    re->setCaret(1, 4);
    typeText(re, "!");
    CHECK(tab->saveButton(false)->isEnabled());
    tab->saveButton(false)->click();                       // header Save button of the right side
    CHECK(waitFor([&] { return !model->isDirty(Side::Right) && !tab->taskBusy(); }, 15000));
    CHECK_EQ(readAll(r), QStringLiteral("alpha\r\nBETA!\r\ngamma"));     // CRLF and the missing final newline kept
    CHECK_EQ(readAll(l), QStringLiteral("alpha\nbeta\ngamma\n"));          // the other file is untouched
    CHECK(!tab->saveButton(false)->isEnabled());
    CHECK(!tab->title().endsWith(" *"));
    // Our own write must not look like an external change.
    pump(600);
    auto* banner = static_cast<FileChangeBanner*>(tab->findChild<QFrame*>(QStringLiteral("fileChangeBanner")));
    CHECK(banner != nullptr && !banner->isStale(0) && !banner->isStale(1));

    // Ctrl+S saves the active pane; undo after saving makes it dirty again (differs from disk).
    focusEditor(le);
    le->setCaret(2, 5);
    typeText(le, "?");
    CHECK(tab->editMenuItems()[2]->isEnabled());
    tab->editMenuItems()[2]->trigger();                    // Save
    CHECK(waitFor([&] { return !model->isDirty(Side::Left) && !tab->taskBusy(); }, 15000));
    CHECK_EQ(readAll(l), QStringLiteral("alpha\nbeta\ngamma?\n"));
    tab->editMenuItems()[0]->trigger();
    CHECK(model->isDirty(Side::Left));
    CHECK_EQ(readAll(l), QStringLiteral("alpha\nbeta\ngamma?\n"));         // nothing written until saved again
    // Locking again keeps the edits and still allows saving.
    tab->lockButton()->click();
    CHECK(!tab->writeEnabled() && tab->lockColor() == kRed);
    CHECK(model->isDirty(Side::Left));
    typeText(le, "z");
    CHECK_EQ(lineOf(model, Side::Left, 2), QStringLiteral("gamma"));
    CHECK(tab->lockFlashing());
    tab->saveButton(true)->click();
    CHECK(waitFor([&] { return !model->isDirty(Side::Left) && !tab->taskBusy(); }, 15000));
    CHECK_EQ(readAll(l), QStringLiteral("alpha\nbeta\ngamma\n"));
}

static void testUnsavedChangesProtocol() {
    const QString l = writeFile("w6_l.txt", "one\ntwo\n");
    const QString r = writeFile("w6_r.txt", "one\ntwo\n");
    auto tab = makeTab(l, r);
    CHECK(tab->confirmClose());                            // nothing to lose: no dialog at all
    tab->setWriteMode(true);
    auto* le = tab->leftEditor();
    auto* re = tab->rightEditor();
    focusEditor(le);
    le->setCaret(0, 3);
    typeText(le, "L");
    focusEditor(re);
    re->setCaret(1, 3);
    typeText(re, "R");
    CHECK(tab->hasUnsavedChanges());

    answerDialog({"Cancel"});
    CHECK(!tab->confirmClose());                           // Cancel keeps the tab (and the edits)
    CHECK(tab->hasUnsavedChanges());
    answerDialog({"Continue"});
    CHECK(tab->confirmClose());                            // Continue discards
    answerDialog({"Save Left", "Save Right"});
    CHECK(tab->confirmClose());                            // both saved from the dialog; it closes itself
    CHECK(!tab->hasUnsavedChanges());
    CHECK_EQ(readAll(l), QStringLiteral("oneL\ntwo\n"));
    CHECK_EQ(readAll(r), QStringLiteral("one\ntwoR\n"));
    CHECK(!dialogVisible());

    // Replacing the content asks first: Swap sides, Reload, dropped paths.
    focusEditor(le);
    le->setCaret(0, 4);
    typeText(le, "!");
    answerDialog({"Cancel"});
    tab->actionsMenuItems()[1]->trigger();                 // Swap Sides -> cancelled
    CHECK_EQ(tab->leftText(), l);
    CHECK(tab->hasUnsavedChanges());
    answerDialog({"Cancel"});
    tab->actionsMenuItems()[0]->trigger();                 // Reload -> cancelled
    CHECK(tab->hasUnsavedChanges());
    answerDialog({"Cancel"});
    tab->applyDroppedPaths(r, l);
    CHECK_EQ(tab->leftText(), l);
    CHECK(tab->hasUnsavedChanges());
    answerDialog({"Continue"});
    tab->actionsMenuItems()[1]->trigger();                 // Swap -> continue: edits are dropped, sides swap
    CHECK(waitFor([&] { return !tab->loading() && tab->leftText() == r && tab->model() && !tab->hasUnsavedChanges(); }, 15000));
    CHECK_EQ(readAll(l), QStringLiteral("oneL\ntwo\n"));   // the discarded edit never reached the disk
}

static void testRules() {
    const QString l = writeFile("w7_l.txt", "id=1 ok\n# c1\nkeep\n");
    const QString r = writeFile("w7_r.txt", "id=2 ok\n# c2\nkeep\n");
    auto tab = makeTab(l, r);
    CHECK_EQ(tab->model()->stats().blocks, 1ULL);
    CHECK(!tab->rulesBarVisible());
    tab->rulesAction()->setChecked(true);                  // the toolbar "Rules" button shows the bar
    CHECK(tab->rulesBarVisible());

    tab->rulesEdit()->setText("/^#.*$/,1");                // Enter in the bar applies
    QMetaObject::invokeMethod(tab->rulesEdit(), "returnPressed");
    CHECK(waitFor([&] { return tab->rulesText() == QStringLiteral("/^#.*$/,1") && !tab->taskBusy(); }, 15000));
    CHECK_EQ(tab->model()->stats().blocks, 0ULL);          // masked parts count as matched
    CHECK(tab->statusText().contains("rules active"));
    CHECK(alignmentValid(tab->model()));

    // Rules apply to edits made afterwards, and to the other side's lines.
    tab->setWriteMode(true);
    auto* re = tab->rightEditor();
    focusEditor(re);
    re->setCaret(0, 4);
    key(re, Qt::Key_Backspace);
    typeText(re, "9");                                     // id=9 ok  (only the masked digit differs)
    CHECK_EQ(lineOf(tab->model(), Side::Right, 0), QStringLiteral("id=9 ok"));
    CHECK_EQ(tab->model()->stats().blocks, 0ULL);
    typeText(re, "X");                                     // "id=9X ok"... unmasked difference
    CHECK_EQ(tab->model()->stats().blocks, 1ULL);

    // Rules survive a reload (and the edit is discarded by it).
    answerDialog({"Continue"});                            // arm the answer BEFORE the modal dialog opens
    tab->actionsMenuItems()[0]->trigger();
    CHECK(waitFor([&] { return !tab->loading() && !tab->hasUnsavedChanges() && tab->model() && tab->model()->stats().blocks == 0; }, 15000));
    CHECK_EQ(tab->rulesText(), QStringLiteral("/^#.*$/,1"));

    // An invalid regular expression is reported and changes nothing.
    tab->applyRules("/(bad/");
    CHECK(waitFor([&] { return !tab->taskBusy(); }, 15000));
    CHECK_EQ(tab->rulesText(), QStringLiteral("/^#.*$/,1"));
    CHECK(tab->rulesStatusLabel()->text().contains("regular expression"));
    CHECK_EQ(tab->model()->stats().blocks, 0ULL);

    // Clearing the rules restores the plain comparison.
    tab->applyRules(QString());
    CHECK(waitFor([&] { return tab->rulesText().isEmpty() && !tab->taskBusy(); }, 15000));
    CHECK_EQ(tab->model()->stats().blocks, 1ULL);
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // progress is visible even if a test hangs
    QApplication app(argc, argv);
    QTemporaryDir dir;
    g_dir = &dir;
    std::printf("[ testScrollMapper ]\n");
    testScrollMapper();
    std::printf("[ testModelBasics ]\n");
    testModelBasics();
    std::printf("[ testEditorsShareModel ]\n");
    testEditorsShareModel();
    std::printf("[ testSyntaxMultiline ]\n");
    testSyntaxMultiline();
    std::printf("[ testVeryLongLinesStayCheap ]\n");
    testVeryLongLinesStayCheap();
    std::printf("[ testLoaderAsyncAndCancel ]\n");
    testLoaderAsyncAndCancel();
    std::printf("[ testCompareTab ]\n");
    testCompareTab();
    std::printf("[ testCommonInterface ]\n");
    testCommonInterface();
    std::printf("[ testLockFlashing ]\n");
    testLockFlashing();
    std::printf("[ testUnlockAndTyping ]\n");
    testUnlockAndTyping();
    std::printf("[ testLineEditing ]\n");
    testLineEditing();
    std::printf("[ testCopyBlocks ]\n");
    testCopyBlocks();
    std::printf("[ testSaving ]\n");
    testSaving();
    std::printf("[ testUnsavedChangesProtocol ]\n");
    testUnsavedChangesProtocol();
    std::printf("[ testRules ]\n");
    testRules();
    std::printf(failures ? "%d FAILURE(S)\n" : "virtual_editor_test: all passed\n", failures);
    return failures ? 1 : 0;
}

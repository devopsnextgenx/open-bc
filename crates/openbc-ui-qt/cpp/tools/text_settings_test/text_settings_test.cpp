// Persistence + selection tests for Tools > Options > Text Compare.
// Uses the real OptionsStore (QSettings ini) and the real Hybrid decision with real files.
// Build: see build_and_run.sh next to this file.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

#include "options/app_options.h"
#include "viewers/text/text_editor_choice.h"

// Link stubs for the Rust highlight bridge. OptionsStore references the syntax-mapping store, which
// this test does not exercise (it only checks the Text Compare keys).
extern "C" {
void openbc_highlight_html_destroy(char* p) { delete[] p; }
std::size_t openbc_highlight_set_user_mappings(const std::uint8_t*, std::size_t) { return 0; }
char* openbc_highlight_catalog() { char* c = new char[3]; c[0] = '['; c[1] = ']'; c[2] = 0; return c; }
}

using namespace openbc::app;
using namespace openbc::app::text_editor;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static QString sparseFile(const QTemporaryDir& d, const QString& name, qint64 bytes) {
    const QString path = d.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.resize(bytes);        // sparse: cheap even for GB sizes
    return path;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString ini = dir.filePath("preferences.ini");

    {   // Defaults on a fresh profile.
        QSettings s(ini, QSettings::IniFormat);
        OptionsStore store(&s);
        const Settings t = store.options().textEditor;
        CHECK(t.mode == Mode::Hybrid && t.thresholdValue == 3 && t.thresholdUnit == Unit::MB);
    }
    {   // Persist, then restore after a "restart" (fresh QSettings + store).
        QSettings s(ini, QSettings::IniFormat);
        OptionsStore store(&s);
        AppOptions next = store.options();
        next.textEditor = {Mode::Virtual, 512, Unit::KB};
        store.commit(next);
        s.sync();
    }
    {
        QSettings s(ini, QSettings::IniFormat);
        OptionsStore store(&s);
        const Settings t = store.options().textEditor;
        CHECK(t.mode == Mode::Virtual && t.thresholdValue == 512 && t.thresholdUnit == Unit::KB);
        AppOptions next = store.options();
        next.textEditor = {Mode::Hybrid, 1, Unit::GB};
        store.commit(next);
        s.sync();
    }
    {
        QSettings s(ini, QSettings::IniFormat);
        OptionsStore store(&s);
        const Settings t = store.options().textEditor;
        CHECK(t.mode == Mode::Hybrid && t.thresholdValue == 1 && t.thresholdUnit == Unit::GB);
    }
    {   // Corrupt / hand-edited values never break startup: they fall back to the defaults.
        QSettings s(ini, QSettings::IniFormat);
        s.setValue("textCompare/editorMode", 42);
        s.setValue("textCompare/thresholdValue", 0);
        s.setValue("textCompare/thresholdUnit", 9);
        s.sync();
        OptionsStore store(&s);
        const Settings t = store.options().textEditor;
        CHECK(t.mode == Mode::Hybrid && t.thresholdValue == 3 && t.thresholdUnit == Unit::MB);
        s.setValue("textCompare/thresholdValue", "banana");
        s.setValue("textCompare/editorMode", 1);
        s.setValue("textCompare/thresholdUnit", 1);
        s.sync();
        OptionsStore again(&s);
        CHECK(again.options().textEditor.mode == Mode::Virtual);
        CHECK(again.options().textEditor.thresholdValue == 3);
        s.setValue("textCompare/thresholdValue", "99999999999999999999");   // overflows 64-bit
        OptionsStore big(&s);
        CHECK(big.options().textEditor.thresholdValue == 3);
        s.setValue("textCompare/thresholdValue", 100000);                    // 100000 MB > 64 GB
        OptionsStore huge(&s);
        CHECK(huge.options().textEditor.thresholdValue == 3 && huge.options().textEditor.thresholdUnit == Unit::MB);
    }

    // Hybrid decision with real files, before any content is read.
    const Settings hybrid;   // 3 MB default
    const QString small = sparseFile(dir, "small.txt", 3 * 1024 * 1024 - 1);
    const QString exact = sparseFile(dir, "exact.txt", 3 * 1024 * 1024);
    const QString tiny = sparseFile(dir, "tiny.txt", 10);
    CHECK(chooseTextEditor(hybrid, tiny, small, {}, {}).editor == Editor::Existing);
    CHECK(chooseTextEditor(hybrid, tiny, exact, {}, {}).editor == Editor::Virtual);   // >= threshold
    CHECK(chooseTextEditor(hybrid, exact, tiny, {}, {}).editor == Editor::Virtual);   // either side
    CHECK(chooseTextEditor(hybrid, dir.filePath("new-file.txt"), tiny, {}, {}).editor == Editor::Existing);  // missing = empty
    Settings kb{Mode::Hybrid, 512, Unit::KB};
    CHECK(chooseTextEditor(kb, tiny, small, {}, {}).editor == Editor::Virtual);
    Settings gb{Mode::Hybrid, 1, Unit::GB};
    CHECK(chooseTextEditor(gb, tiny, exact, {}, {}).editor == Editor::Existing);
    // Explicit modes override Hybrid.
    CHECK(chooseTextEditor({Mode::Existing, 3, Unit::MB}, exact, exact, {}, {}).editor == Editor::Existing);
    CHECK(chooseTextEditor({Mode::Virtual, 3, Unit::MB}, tiny, tiny, {}, {}).editor == Editor::Virtual);
    // A directory / non-regular path cannot be sized -> safe fallback to Virtual in Hybrid.
    CHECK(chooseTextEditor(hybrid, dir.path(), tiny, {}, {}).editor == Editor::Virtual);
    // Remote or in-memory text can only be shown by the Existing editor, even if Virtual was requested.
    const EditorChoice remote = chooseTextEditor({Mode::Virtual, 3, Unit::MB}, "sftp://host/a.txt", tiny, "hello", {});
    CHECK(remote.editor == Editor::Existing && !remote.note.isEmpty());
    const EditorChoice clip = chooseTextEditor(hybrid, QString(), tiny, "clipboard text", {});
    CHECK(clip.editor == Editor::Existing);

    std::printf(failures ? "%d FAILURE(S)\n" : "text_settings_test: all passed\n", failures);
    return failures ? 1 : 0;
}

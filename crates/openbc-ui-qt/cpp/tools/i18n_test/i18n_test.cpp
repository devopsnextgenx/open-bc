// Standalone checks for i18n/i18n.h (no GUI). Build/run: ./build_and_run.sh
//   ./build_and_run.sh --dump            print messages.json generated from messages_defaults.h
//   ./build_and_run.sh --check <dir>     report keys missing from / unknown to each messages*.json in <dir>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>

#include "i18n/i18n.h"

using namespace openbc::i18n;

static int g_failed = 0;
#define CHECK_EQ(actual, expected)                                                                         \
    do {                                                                                                   \
        const QString a_ = (actual), e_ = (expected);                                                      \
        if (a_ != e_) {                                                                                    \
            ++g_failed;                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n   got:      %s\n   expected: %s\n", __FILE__, __LINE__,  \
                         #actual, qPrintable(a_), qPrintable(e_));                                         \
        }                                                                                                  \
    } while (0)

static void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(data);
}

static int check(const QString& dir) {
    int problems = 0;
    const auto& builtin = Catalog::builtin();
    for (const QString& name : QDir(dir).entryList({"messages*.json"}, QDir::Files, QDir::Name)) {
        QHash<QString, QString> keys;
        QString error;
        if (!detail::loadFile(dir + "/" + name, keys, &error)) {
            std::printf("%s: UNREADABLE (%s)\n", qPrintable(name), qPrintable(error));
            ++problems;
            continue;
        }
        for (auto it = builtin.constBegin(); it != builtin.constEnd(); ++it) {
            if (!keys.contains(it.key())) {
                std::printf("%s: missing  %s\n", qPrintable(name), qPrintable(it.key()));
                if (name == "messages.json") ++problems;   // only the default file must be complete
            }
        }
        for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
            if (!builtin.contains(it.key())) std::printf("%s: unknown  %s\n", qPrintable(name), qPrintable(it.key()));
        }
    }
    return problems ? 1 : 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc >= 2 && QString(argv[1]) == "--dump") {
        std::fwrite(Catalog::builtinAsJson().constData(), 1, Catalog::builtinAsJson().size(), stdout);
        return 0;
    }
    if (argc >= 3 && QString(argv[1]) == "--check") return check(argv[2]);

    QTemporaryDir tmp;
    const QString root = tmp.path() + "/prefs";
    const QString res = root + "/resources";
    const QString bundled = tmp.path() + "/bundled";

    // 0. before init(): built-in table answers, unknown key is shown as itself
    CHECK_EQ(t("menu.session"), "&Session");
    CHECK_EQ(t("no.such.key"), "no.such.key");

    // 1. deploy + fallback chain
    write(bundled + "/messages.json", R"({"a":"A-en","b":"B-en","only.en":"EN","fmt":"%1 of %2"})");
    write(bundled + "/messages_fr.json", R"({"a":"A-fr","fmt":"%2 sur %1"})");
    Catalog::instance().init(root, "fr", {bundled});
    CHECK_EQ(t("a"), "A-fr");                 // language file
    CHECK_EQ(t("b"), "B-en");                 // falls back to messages.json
    CHECK_EQ(t("menu.tools"), "&Tools");      // not in any JSON -> built-in
    CHECK_EQ(t("totally.unknown"), "totally.unknown");
    CHECK_EQ(t("fmt", 3, 7), "7 sur 3");
    CHECK_EQ(t("only.en", 1, 2), "EN");       // args on a placeholder-less text are harmless
    CHECK_EQ(Catalog::instance().availableLanguages().join(","), "en,fr");

    // 2. language variants
    CHECK_EQ(detail::candidates("pt-BR").join(","), "pt_BR,pt");
    CHECK_EQ(detail::candidates("zh_CN").join(","), "zh_CN,cn,zh");
    CHECK_EQ(detail::candidates("cn").join(","), "cn");
    CHECK_EQ(detail::candidates("fr_FR.UTF-8").join(","), "fr_FR,fr");
    CHECK_EQ(detail::candidates("C").join(","), "");

    // 3. user deletes keys and breaks a file -> still fine
    write(res + "/messages_fr.json", "{ this is not json");
    write(res + "/messages.json", R"({"b":"B-user", "bad": 42, "nested": {"x": "NX"}})");
    Catalog::instance().reload();
    CHECK_EQ(t("a"), "a");                    // 'a' was deleted everywhere and is not built-in
    CHECK_EQ(t("b"), "B-user");
    CHECK_EQ(t("bad"), "bad");                // non-string value ignored
    CHECK_EQ(t("nested.x"), "NX");
    CHECK_EQ(t("menu.session"), "&Session");  // safety net still works with a gutted messages.json
    CHECK_EQ(Catalog::instance().activeFiles().join(","), "messages.json");
    write(res + "/messages.json", "");        // empty file
    Catalog::instance().reload();
    CHECK_EQ(t("menu.edit"), "&Edit");

    // 4. runtime language switch + listener
    int notified = 0;
    const int id = Catalog::instance().addListener([&] { ++notified; });
    write(res + "/messages_fr.json", R"({"a":"A-fr2"})");
    Catalog::instance().setLanguage("fr");
    CHECK_EQ(t("a"), "A-fr2");
    Catalog::instance().setLanguage("de");    // no messages_de.json
    CHECK_EQ(t("menu.help"), "&Help");
    CHECK_EQ(QString::number(notified), "2");
    Catalog::instance().removeListener(id);

    // 5. deployment never clobbers user edits, but upgrades untouched files
    const QString d = tmp.path() + "/deploy";
    write(bundled + "/messages_de.json", R"({"v":"1"})");
    deployBundled(d, {bundled});
    write(bundled + "/messages_de.json", R"({"v":"2"})");
    deployBundled(d, {bundled});              // untouched copy -> upgraded
    QHash<QString, QString> h;
    detail::loadFile(d + "/messages_de.json", h);
    CHECK_EQ(h.value("v"), "2");
    write(d + "/messages_de.json", R"({"v":"user"})");
    write(bundled + "/messages_de.json", R"({"v":"3"})");
    deployBundled(d, {bundled});              // user edited -> kept
    h.clear();
    detail::loadFile(d + "/messages_de.json", h);
    CHECK_EQ(h.value("v"), "user");
    QFile::remove(d + "/messages_de.json");
    deployBundled(d, {bundled});              // deleted -> reset
    h.clear();
    detail::loadFile(d + "/messages_de.json", h);
    CHECK_EQ(h.value("v"), "3");

    std::puts(g_failed ? "i18n_test: FAILED" : "i18n_test: all checks passed");
    return g_failed ? 1 : 0;
}

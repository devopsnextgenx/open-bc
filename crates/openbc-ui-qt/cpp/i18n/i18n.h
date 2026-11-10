// ---------------------------------------------------------------------------
// i18n.h - JSON message catalogs with layered fallback.
//
// Lookup order for t("some.key"):
//   1. messages_<lang>.json   (e.g. messages_fr.json, messages_cn.json,
//                              messages_pt_BR.json, then messages_pt.json)
//   2. messages.json          (default language)
//   3. built-in table         (messages_defaults.h, compiled into the binary)
//   4. the key text itself
//
// The JSON files live in the per-user preferences folder:
//     <SessionHistory::rootPath()>/resources/      (~/.config/open-bc/resources
//                                                   on Linux AND Windows)
// Defaults shipped inside the binary (Qt resource ":/i18n") are deployed there
// on startup without ever overwriting a file the user edited.
//
// Robustness rules: a missing / unreadable / malformed file is skipped, a
// non-string value is skipped, a missing key falls through to the next layer.
// Nothing in here throws or asserts, so users can hand-edit the files freely.
//
// Header-only, no Q_OBJECT (no moc needed). Thread-safe.
// ---------------------------------------------------------------------------
#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QReadWriteLock>
#include <QSaveFile>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "i18n/messages_defaults.h"

namespace openbc::i18n {

namespace detail {

// "a": {"b": "x"} is accepted as key "a.b"; anything that is not a string/object is ignored.
inline void flatten(const QJsonObject& object, const QString& prefix, QHash<QString, QString>& out) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString key = prefix.isEmpty() ? it.key() : prefix + QLatin1Char('.') + it.key();
        if (it.value().isString()) {
            out.insert(key, it.value().toString());
        } else if (it.value().isObject()) {
            flatten(it.value().toObject(), key, out);
        }
    }
}

// Parses one catalog file. Returns false (and leaves `out` untouched) if it is absent or not a JSON object.
inline bool loadFile(const QString& path, QHash<QString, QString>& out, QString* error = nullptr) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QByteArray data = file.readAll();
    if (data.startsWith("\xEF\xBB\xBF")) data.remove(0, 3);   // UTF-8 BOM from Windows editors
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = parse.error != QJsonParseError::NoError ? parse.errorString()
                                                              : QStringLiteral("root is not an object");
        }
        return false;
    }
    QHash<QString, QString> loaded;
    flatten(doc.object(), QString(), loaded);
    out = std::move(loaded);
    return true;
}

// Language setting -> file-name suffixes to try, most specific first.
//   "auto"  -> system locale        "fr_FR" -> fr_FR, fr
//   "pt-BR" -> pt_BR, pt            "zh_CN" / "zh" -> zh_CN, cn, zh
//   "cn"    -> cn
inline QStringList candidates(QString tag) {
    tag = tag.trimmed();
    tag.replace(QLatin1Char('-'), QLatin1Char('_'));
    if (tag.isEmpty() || tag.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
        tag = QLocale::system().name();   // "fr_FR", "C" ...
    }
    tag = tag.section(QLatin1Char('.'), 0, 0).section(QLatin1Char('@'), 0, 0);   // drop ".UTF-8" / "@euro"
    if (tag.isEmpty() || tag == QLatin1String("C") || tag == QLatin1String("POSIX")) return {};

    const QString base = tag.section(QLatin1Char('_'), 0, 0);
    QStringList out;
    out << tag;
    if (base.compare(QLatin1String("zh"), Qt::CaseInsensitive) == 0) {
        const QString region = tag.section(QLatin1Char('_'), 1);
        if (region.isEmpty() || region.compare(QLatin1String("CN"), Qt::CaseInsensitive) == 0 ||
            region.compare(QLatin1String("SG"), Qt::CaseInsensitive) == 0 ||
            region.compare(QLatin1String("Hans"), Qt::CaseInsensitive) == 0) {
            out << QStringLiteral("cn");   // project convention: messages_cn.json = Simplified Chinese
        }
    }
    if (base != tag) out << base;
    out.removeDuplicates();
    return out;
}

inline QByteArray sha256(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

inline QByteArray sha256OfFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? sha256(file.readAll()) : QByteArray();
}

}  // namespace detail

// Where bundled defaults come from; the first source that has a given file name wins.
//   :/i18n                           embedded Qt resource (works in deb/rpm/AppImage/Flatpak/MSI alike)
//   <exe>/../share/openbc/resources  optional installed copy (Linux layouts)
//   <exe>/resources                  optional installed copy (Windows layout)
inline QStringList defaultBundledDirs() {
    const QString app = QCoreApplication::applicationDirPath();
    return {QStringLiteral(":/i18n"), app + QStringLiteral("/../share/openbc/resources"),
            app + QStringLiteral("/resources")};
}

// Copies bundled messages*.json into `destDir`, safely:
//   - missing in destDir                      -> copied
//   - present, byte-identical                 -> nothing
//   - present, still exactly what WE deployed -> upgraded (user never touched it)
//   - present and edited by the user          -> left alone
// What we deployed is remembered as SHA-256 in destDir/.deployed.json.
// Deleting a file from destDir resets it to the bundled version on next start.
inline void deployBundled(const QString& destDir, const QStringList& sourceDirs) {
    if (!QDir().mkpath(destDir)) return;
    const QString manifestPath = destDir + QStringLiteral("/.deployed.json");
    QJsonObject manifest;
    {
        QFile manifestFile(manifestPath);
        if (manifestFile.open(QIODevice::ReadOnly)) {
            manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        }
    }
    bool dirty = false;
    QSet<QString> seen;
    for (const QString& sourceDir : sourceDirs) {
        const QDir dir(sourceDir);
        if (!dir.exists()) continue;
        const QStringList names = dir.entryList({QStringLiteral("messages*.json")}, QDir::Files);
        for (const QString& name : names) {
            if (seen.contains(name)) continue;
            seen.insert(name);
            QFile in(dir.filePath(name));
            if (!in.open(QIODevice::ReadOnly)) continue;
            const QByteArray bundled = in.readAll();
            in.close();
            const QString bundledHash = QString::fromLatin1(detail::sha256(bundled));
            const QString destPath = destDir + QLatin1Char('/') + name;

            bool write = false;
            if (!QFileInfo::exists(destPath)) {
                write = true;
            } else {
                const QString current = QString::fromLatin1(detail::sha256OfFile(destPath));
                const QString recorded = manifest.value(name).toString();
                if (current == bundledHash) {
                    if (recorded != bundledHash) {
                        manifest[name] = bundledHash;
                        dirty = true;
                    }
                } else if (!recorded.isEmpty() && current == recorded) {
                    write = true;   // pristine copy of an older release -> upgrade it
                }
            }
            if (write) {
                QSaveFile out(destPath);
                if (out.open(QIODevice::WriteOnly) && out.write(bundled) == bundled.size() && out.commit()) {
                    manifest[name] = bundledHash;
                    dirty = true;
                } else {
                    qWarning().noquote() << "[i18n] cannot deploy" << destPath;
                }
            }
        }
    }
    if (dirty) {
        QSaveFile out(manifestPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
            out.commit();
        }
    }
}

class Catalog {
public:
    static Catalog& instance() {
        static Catalog catalog;
        return catalog;
    }

    // Call once at startup, after QApplication exists and before menus are built.
    //   preferencesRoot: SessionHistory::rootPath()  (~/.config/open-bc)
    //   language:        "auto", "fr", "cn", "pt_BR", ... (persist it in preferences.ini)
    void init(const QString& preferencesRoot, const QString& language = QStringLiteral("auto"),
              const QStringList& bundledDirs = defaultBundledDirs()) {
        {
            QWriteLocker locker(&lock_);
            resourcesDir_ = preferencesRoot + QStringLiteral("/resources");
            language_ = language;
        }
        deployBundled(resourcesDir(), bundledDirs);
        reload();
    }

    // Switch language at runtime; fires listeners (rebuild menus / retranslate there).
    void setLanguage(const QString& language) {
        {
            QWriteLocker locker(&lock_);
            language_ = language;
        }
        reload();
    }

    // Re-read the JSON files (after the user edited them, or after setLanguage).
    void reload() {
        QString dir, language;
        {
            QReadLocker locker(&lock_);
            dir = resourcesDir_;
            language = language_;
        }
        std::vector<QHash<QString, QString>> layers;
        QStringList names;
        if (!dir.isEmpty()) {
            for (const QString& suffix : detail::candidates(language)) {
                const QString name = QStringLiteral("messages_") + suffix + QStringLiteral(".json");
                loadLayer(dir + QLatin1Char('/') + name, name, layers, names);
            }
            loadLayer(dir + QStringLiteral("/messages.json"), QStringLiteral("messages.json"), layers, names);
        }
        {
            QWriteLocker locker(&lock_);
            layers_ = std::move(layers);
            layerNames_ = names;
        }
        {
            QMutexLocker locker(&reportMutex_);
            reported_.clear();
        }
        notify();
    }

    QString text(const QString& key) const {
        {
            QReadLocker locker(&lock_);
            for (const auto& layer : layers_) {
                const auto it = layer.constFind(key);
                if (it != layer.constEnd()) return *it;
            }
        }
        const auto& table = builtin();
        const auto it = table.constFind(key);
        if (it != table.constEnd()) {
            report(key, false);
            return *it;
        }
        report(key, true);
        return key;
    }

    QString resourcesDir() const {
        QReadLocker locker(&lock_);
        return resourcesDir_;
    }
    QString language() const {
        QReadLocker locker(&lock_);
        return language_;
    }
    // Files that are actually in use right now, most specific first (for diagnostics / About).
    QStringList activeFiles() const {
        QReadLocker locker(&lock_);
        return layerNames_;
    }

    // Language codes found in the resources folder: "en" (messages.json) plus every messages_<code>.json.
    QStringList availableLanguages() const {
        QStringList codes{QStringLiteral("en")};
        const QDir dir(resourcesDir());
        for (const QString& name : dir.entryList({QStringLiteral("messages_*.json")}, QDir::Files, QDir::Name)) {
            codes << name.mid(9, name.size() - 9 - 5);   // strip "messages_" and ".json"
        }
        codes.removeDuplicates();
        return codes;
    }

    // Listeners run after every (re)load. Returns an id for removeListener().
    int addListener(std::function<void()> listener) {
        QMutexLocker locker(&listenerMutex_);
        listeners_.emplace_back(nextListenerId_, std::move(listener));
        return nextListenerId_++;
    }
    void removeListener(int id) {
        QMutexLocker locker(&listenerMutex_);
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                        [id](const auto& entry) { return entry.first == id; }),
                         listeners_.end());
    }

    // The compiled-in safety net, as a map.
    static const QHash<QString, QString>& builtin() {
        static const QHash<QString, QString> table = [] {
            QHash<QString, QString> map;
            for (const BuiltinMessage& message : kBuiltinMessages) {
                map.insert(QString::fromUtf8(message.key), QString::fromUtf8(message.text));
            }
            return map;
        }();
        return table;
    }

    // messages.json content generated from the built-in table (sorted keys, indented).
    static QByteArray builtinAsJson() {
        QJsonObject object;
        for (auto it = builtin().constBegin(); it != builtin().constEnd(); ++it) object.insert(it.key(), it.value());
        return QJsonDocument(object).toJson(QJsonDocument::Indented);   // QJsonObject keeps keys sorted
    }

private:
    Catalog() = default;

    void loadLayer(const QString& path, const QString& name, std::vector<QHash<QString, QString>>& layers,
                   QStringList& names) {
        if (!QFileInfo::exists(path)) return;
        QHash<QString, QString> layer;
        QString error;
        if (detail::loadFile(path, layer, &error)) {
            layers.push_back(std::move(layer));
            names << name;
        } else {
            qWarning().noquote() << "[i18n] ignoring" << path << "-" << error;
        }
    }

    // Log each problem key once per load: key missing everywhere is a bug (warn); key only in the
    // built-in table means a JSON file lost it (the safety net worked; debug level).
    void report(const QString& key, bool missingEverywhere) const {
        QMutexLocker locker(&reportMutex_);
        if (reported_.contains(key)) return;
        reported_.insert(key);
        if (missingEverywhere) {
            qWarning().noquote() << "[i18n] unknown message key" << key << "- showing the key";
        } else {
            qDebug().noquote() << "[i18n] key" << key << "not in JSON files - using built-in text";
        }
    }

    void notify() {
        std::vector<std::function<void()>> copy;
        {
            QMutexLocker locker(&listenerMutex_);
            for (const auto& entry : listeners_) copy.push_back(entry.second);
        }
        for (const auto& listener : copy) listener();
    }

    mutable QReadWriteLock lock_;
    QString resourcesDir_;
    QString language_ = QStringLiteral("auto");
    std::vector<QHash<QString, QString>> layers_;   // language layers first, messages.json last
    QStringList layerNames_;

    mutable QMutex reportMutex_;
    mutable QSet<QString> reported_;

    QMutex listenerMutex_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    int nextListenerId_ = 1;
};

// ----- call-site API ---------------------------------------------------------
//   t("menu.session")                    -> "&Session" / "&Session" in French ...
//   t("window.title.text", fileName)     -> "%1" placeholders filled via QString::arg()
//   tq(dynamicKey)                       -> key built at runtime
// Safe to call before Catalog::init(): the built-in table answers.

template <class... Args>
inline QString tq(const QString& key, const Args&... args) {
    QString message = Catalog::instance().text(key);
    if constexpr (sizeof...(Args) > 0) {
        if (message.contains(QLatin1Char('%'))) {
            int unused[] = {0, ((message = message.arg(args)), 0)...};
            (void)unused;
        }
    }
    return message;
}

template <class... Args>
inline QString t(const char* key, const Args&... args) {
    return tq(QString::fromUtf8(key), args...);
}

}  // namespace openbc::i18n

// syntax_mappings.h
// ---------------------------------------------------------------------------
// The user's "this extension uses that highlighting language" mappings, and the read-only catalog
// of built-in languages they refer to.
//
//   SyntaxCatalog       the languages, extensions and file names the backend ships (parsed once
//                       from openbc_highlight_catalog()). Rust owns the table; this is a view.
//   SyntaxMappingStore  the user's overrides. One process-wide instance that
//                         * loads them from preferences.ini (profile) the first time it is used,
//                         * hands them to the backend (openbc_highlight_set_user_mappings), so
//                           every highlight call honours them, and
//                         * tells live highlighters to re-highlight when they change, so open
//                           comparisons follow without being reopened.
//
// Persisted layout (same preferences.ini as window geometry and remote connections):
//   [syntaxHighlighting]
//   mappings\size=2
//   mappings\1\extension=h        mappings\1\language=C
//   mappings\2\extension=jenkinsfile   mappings\2\language=Groovy
//
// Header-only and free of Q_OBJECT like the rest of the app: change notification uses
// std::function listeners tied to a QObject's lifetime (QPointer), not signals.
// ---------------------------------------------------------------------------
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "backend/backend_api.h"
#include "home/session_history.h"

namespace openbc::app {

// One user rule. `key` is an extension ("cpp") or an exact file name ("makefile"): lower case,
// no leading dot. `language` is the canonical name from the catalog.
struct SyntaxMapping {
    QString key;
    QString language;
    bool operator==(const SyntaxMapping& other) const { return key == other.key && language == other.language; }
};

struct SyntaxLanguage {
    QString name;
    QString engine;            // "syntect" | "tree-painter" | "plain"
    bool approximate = false;  // rendered with the grammar of a related language
    QStringList aliases;
};

// One built-in rule: an extension (`fileName == false`) or a whole file name.
struct SyntaxCatalogEntry {
    QString key;
    QString language;
    bool fileName = false;
};

class SyntaxCatalog {
public:
    static const SyntaxCatalog& get() {
        static const SyntaxCatalog catalog = fromBackend();
        return catalog;
    }

    const QList<SyntaxLanguage>& languages() const { return languages_; }
    const QList<SyntaxCatalogEntry>& entries() const { return entries_; }

    // Case-insensitive match on the language name or one of its aliases; null when unknown.
    const SyntaxLanguage* findLanguage(const QString& nameOrAlias) const {
        const QString wanted = nameOrAlias.trimmed();
        if (wanted.isEmpty()) return nullptr;
        for (const SyntaxLanguage& language : languages_) {
            if (language.name.compare(wanted, Qt::CaseInsensitive) == 0) return &language;
        }
        for (const SyntaxLanguage& language : languages_) {
            for (const QString& alias : language.aliases)
                if (alias.compare(wanted, Qt::CaseInsensitive) == 0) return &language;
        }
        return nullptr;
    }

    // Built-in language for a normalized key, or an empty string.
    QString builtInLanguageFor(const QString& key) const {
        const auto it = builtIn_.constFind(key);
        return it == builtIn_.constEnd() ? QString() : it.value();
    }

    // What the user typed ("*.CPP", " .h ", "Jenkinsfile") as a key; empty when unusable. Same
    // rules as the backend (normalize_user_key): ASCII lower case, one leading "*." or ".", and no
    // whitespace, path separators or wildcards.
    static QString normalizeKey(const QString& raw) {
        QString key = raw.trimmed();
        for (QChar& c : key)
            if (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) c = QChar(c.unicode() + 32);
        if (key.startsWith(QLatin1String("*."))) key.remove(0, 2);
        else if (key.startsWith(QLatin1Char('.'))) key.remove(0, 1);
        if (key.isEmpty()) return {};
        static const QString forbidden = QStringLiteral("/\\*?:\"<>|");
        for (const QChar c : key)
            if (c.isSpace() || c.unicode() < 0x20 || c.unicode() == 0x7f || forbidden.contains(c)) return {};
        return key;
    }

private:
    static SyntaxCatalog fromBackend() {
        SyntaxCatalog catalog;
        char* raw = openbc_highlight_catalog();
        if (!raw) return catalog;
        const QString text = QString::fromUtf8(raw);
        openbc_highlight_html_destroy(raw);
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const QStringList f = line.split(QLatin1Char('\t'));
            if (f.size() >= 4 && f[0] == QLatin1String("L")) {
                SyntaxLanguage language;
                language.name = f[1];
                language.engine = f[2];
                language.approximate = f[3] == QLatin1String("approximate");
                if (f.size() >= 5) language.aliases = f[4].split(QLatin1Char(','), Qt::SkipEmptyParts);
                catalog.languages_.append(language);
            } else if (f.size() >= 3 && (f[0] == QLatin1String("E") || f[0] == QLatin1String("F"))) {
                catalog.entries_.append({f[1], f[2], f[0] == QLatin1String("F")});
                catalog.builtIn_.insert(f[1], f[2]);
            }
        }
        return catalog;
    }

    QList<SyntaxLanguage> languages_;
    QList<SyntaxCatalogEntry> entries_;
    QHash<QString, QString> builtIn_;
};

class SyntaxMappingStore {
public:
    static SyntaxMappingStore& instance() {
        static SyntaxMappingStore store;
        return store;
    }

    // preferences.ini, the same file as the window geometry. Tests may point it elsewhere before
    // the first call to instance().
    static QString& settingsFileOverride() {
        static QString path;
        return path;
    }
    static QString settingsFilePath() {
        return settingsFileOverride().isEmpty() ? SessionHistory::rootPath() + "/preferences.ini"
                                                : settingsFileOverride();
    }

    const QList<SyntaxMapping>& mappings() const { return mappings_; }

    // Replaces the user's mappings: unusable entries are dropped, a key given twice keeps the last
    // language. When something changed it is saved to the profile, handed to the backend, and
    // listeners run (open editors re-highlight).
    void setMappings(const QList<SyntaxMapping>& requested) {
        const QList<SyntaxMapping> clean = sanitize(requested);
        if (clean == mappings_) return;
        mappings_ = clean;
        persist();
        pushToBackend();
        notify();
    }

    // `fn` runs after every change while `context` is alive. Registering is cheap; a destroyed
    // context is forgotten automatically.
    void addListener(QObject* context, std::function<void()> fn) {
        listeners_.push_back({QPointer<QObject>(context), std::move(fn)});
    }

private:
    struct Listener {
        QPointer<QObject> context;
        std::function<void()> fn;
    };

    SyntaxMappingStore() {
        load();
        pushToBackend();  // before any editor highlights its first document
    }

    static QList<SyntaxMapping> sanitize(const QList<SyntaxMapping>& in) {
        const SyntaxCatalog& catalog = SyntaxCatalog::get();
        QList<SyntaxMapping> out;
        QHash<QString, int> position;
        for (const SyntaxMapping& mapping : in) {
            const QString key = SyntaxCatalog::normalizeKey(mapping.key);
            const SyntaxLanguage* language = catalog.findLanguage(mapping.language);
            if (key.isEmpty() || !language) continue;
            const SyntaxMapping clean{key, language->name};
            const auto it = position.constFind(key);
            if (it != position.constEnd()) {
                out[it.value()] = clean;
            } else {
                position.insert(key, static_cast<int>(out.size()));
                out.append(clean);
            }
        }
        return out;
    }

    void load() {
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        QList<SyntaxMapping> stored;
        settings.beginGroup(QStringLiteral("syntaxHighlighting"));
        const int count = settings.beginReadArray(QStringLiteral("mappings"));
        for (int i = 0; i < count; ++i) {
            settings.setArrayIndex(i);
            stored.append({settings.value(QStringLiteral("extension")).toString(),
                           settings.value(QStringLiteral("language")).toString()});
        }
        settings.endArray();
        settings.endGroup();
        mappings_ = sanitize(stored);
    }

    void persist() const {
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        settings.remove(QStringLiteral("syntaxHighlighting"));
        settings.beginGroup(QStringLiteral("syntaxHighlighting"));
        settings.beginWriteArray(QStringLiteral("mappings"), static_cast<int>(mappings_.size()));
        for (int i = 0; i < mappings_.size(); ++i) {
            settings.setArrayIndex(i);
            settings.setValue(QStringLiteral("extension"), mappings_[i].key);
            settings.setValue(QStringLiteral("language"), mappings_[i].language);
        }
        settings.endArray();
        settings.endGroup();
        settings.sync();
    }

    void pushToBackend() const {
        QByteArray text;
        for (const SyntaxMapping& mapping : mappings_)
            text += mapping.key.toUtf8() + '\t' + mapping.language.toUtf8() + '\n';
        openbc_highlight_set_user_mappings(reinterpret_cast<const std::uint8_t*>(text.constData()),
                                           static_cast<std::size_t>(text.size()));
    }

    void notify() {
        const std::vector<Listener> snapshot = listeners_;  // a listener may register another
        for (const Listener& listener : snapshot)
            if (listener.context && listener.fn) listener.fn();
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
                                        [](const Listener& l) { return l.context.isNull(); }),
                         listeners_.end());
    }

    QList<SyntaxMapping> mappings_;
    std::vector<Listener> listeners_;
};

}  // namespace openbc::app

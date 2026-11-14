// match_policy.h
// ---------------------------------------------------------------------------
// Folder Compare file matching: which files are compared by size + timestamp and which by a
// content hash (and with which algorithm). This header is the UI side of the policy that lives in
// openbc-core (hash_policy.rs): it only edits and ships ONE text line. The rules themselves are
// evaluated in Rust, and so are the built-in defaults (openbc_match_policy_builtin), so nothing
// here duplicates an extension table.
//
// Line format (see hash_policy.rs):  default=md5;text:rs,c,h=size;binary:exe,dll,so=md5
// ---------------------------------------------------------------------------
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "backend/compare_backend_api.h"

namespace openbc::app::matchpolicy {

struct Rule {
    QString name;             // display name of the file-type group ("binary", "text", ...)
    QStringList extensions;   // lower-case, no dot
    QString method;           // one of methodIds()
};

struct Config {
    QString fallback = QStringLiteral("md5");  // method for extensions no rule lists
    QList<Rule> rules;
};

// Method ids, in display order. "size" = size + modification time (reads no file bytes).
inline const QStringList& methodIds() {
    static const QStringList ids{"size", "md5", "sha1", "sha256", "sha512"};
    return ids;
}

inline QString methodLabel(const QString& id) {
    if (id == "size") return QStringLiteral("Size / timestamp");
    if (id == "md5") return QStringLiteral("MD5");
    if (id == "sha1") return QStringLiteral("SHA-1");
    if (id == "sha256") return QStringLiteral("SHA-256");
    if (id == "sha512") return QStringLiteral("SHA-512");
    return id;
}

inline bool isMethod(const QString& id) { return methodIds().contains(id); }

// Trim, strip "*." / ".", lower-case, drop empties and duplicates.
inline QStringList normalizeExtensions(const QStringList& raw) {
    QStringList out;
    for (QString e : raw) {
        e = e.trimmed().toLower();
        while (e.startsWith('*') || e.startsWith('.')) e.remove(0, 1);
        if (!e.isEmpty() && !out.contains(e)) out << e;
    }
    return out;
}

inline QString builtinLine() {
    char* raw = openbc_match_policy_builtin();
    const QString line = raw ? QString::fromUtf8(raw) : QStringLiteral("default=md5");
    if (raw) openbc_match_policy_free(raw);
    return line;
}

inline Config parse(const QString& line) {
    Config config;
    for (const QString& partRaw : line.split(';', Qt::SkipEmptyParts)) {
        const QString part = partRaw.trimmed();
        const int eq = part.indexOf('=');
        if (eq <= 0) continue;
        QString key = part.left(eq).trimmed();
        const QString method = part.mid(eq + 1).trimmed().toLower();
        if (!isMethod(method)) continue;
        if (key.compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0) {
            config.fallback = method;
            continue;
        }
        QString name;
        if (const int colon = key.indexOf(':'); colon >= 0) {
            name = key.left(colon).trimmed();
            key = key.mid(colon + 1);
        }
        const QStringList exts = normalizeExtensions(key.split(','));
        if (exts.isEmpty()) continue;
        config.rules.push_back({name.isEmpty() ? QStringLiteral("custom") : name, exts, method});
    }
    return config;
}

// An empty stored value means "the built-in defaults".
inline Config effective(const QString& stored) { return parse(stored.isEmpty() ? builtinLine() : stored); }

inline QString serialize(const Config& config) {
    const auto clean = [](QString name) {
        for (const QChar c : QStringLiteral(";=,:")) name.replace(c, ' ');
        name = name.trimmed();
        return name.isEmpty() ? QStringLiteral("custom") : name;
    };
    QStringList parts{QStringLiteral("default=") + config.fallback};
    for (const Rule& rule : config.rules) {
        const QStringList exts = normalizeExtensions(rule.extensions);
        if (exts.isEmpty() || !isMethod(rule.method)) continue;
        parts << clean(rule.name) + ':' + exts.join(',') + '=' + rule.method;
    }
    return parts.join(';');
}

// True when the backend currently applies a per-file-type policy.
inline bool enabled() { return openbc_match_policy_enabled() != 0; }

// Method id the installed policy (or the built-in one) uses for `fileName`.
inline QString methodFor(const QString& fileName) {
    const QByteArray name = fileName.toUtf8();
    char* raw = openbc_match_policy_method_for(name.constData());
    const QString id = raw ? QString::fromUtf8(raw) : QStringLiteral("md5");
    if (raw) openbc_match_policy_free(raw);
    return id;
}

// Installs (or, when `enabled` is false, removes) the process-wide policy in the backend.
inline void apply(bool enabled, const QString& stored) {
    const QByteArray line = (stored.isEmpty() ? builtinLine() : stored).toUtf8();
    openbc_match_policy_set(line.constData(), enabled ? 1 : 0);
}

}  // namespace openbc::app::matchpolicy

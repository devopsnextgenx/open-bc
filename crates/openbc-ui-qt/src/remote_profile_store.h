// remote_profile_store.h
// ---------------------------------------------------------------------------
// RemoteProfileStore: loads/saves the RemoteProfileNode tree to disk via
// QSettings, the same storage mechanism the rest of the app already uses.
// Shaped after SessionHistory's saved-node API (loadSavedNodes/createNode/
// renameNode/removeNode/colorForNode) so RemoteConnectionsDialog can be
// written the same way HomeView's history tree already is.
//
// A word on "safely" for passwords: this is a desktop app with no server
// component, so there's no perfect place to hide a secret the app itself
// must be able to read back unattended. What's here is a meaningful step up
// from plaintext - passwords are only ever persisted when the user ticks
// "Save password", and even then they're XOR-obfuscated against a key
// derived from this machine's ID, so the settings file isn't readable
// plaintext and doesn't travel usefully if copied to another machine. It is
// NOT a substitute for a real OS secret store. If/when this app links
// QtKeychain (or an equivalent), swap `obfuscate`/`deobfuscate` below for
// calls into it and nothing else in this file has to change.
// ---------------------------------------------------------------------------
#pragma once

#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <functional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>

#include "remote_profile.h"
#include "session_history.h"

namespace openbc::app {

namespace detail {

inline QByteArray machineKey() {
    return QCryptographicHash::hash(QSysInfo::machineUniqueId(), QCryptographicHash::Sha256);
}

inline QString obfuscate(const QString& plain) {
    if (plain.isEmpty()) return {};
    QByteArray key = machineKey();
    QByteArray data = plain.toUtf8();
    for (int i = 0; i < data.size(); ++i) data[i] = static_cast<char>(data[i] ^ key[i % key.size()]);
    return QString::fromLatin1(data.toBase64());
}

inline QString deobfuscate(const QString& encoded) {
    if (encoded.isEmpty()) return {};
    QByteArray key = machineKey();
    QByteArray data = QByteArray::fromBase64(encoded.toLatin1());
    for (int i = 0; i < data.size(); ++i) data[i] = static_cast<char>(data[i] ^ key[i % key.size()]);
    return QString::fromUtf8(data);
}

inline QString protocolToKey(RemoteProtocol protocol) {
    switch (protocol) {
    case RemoteProtocol::Sftp: return "sftp";
    case RemoteProtocol::Ftp: return "ftp";
    case RemoteProtocol::FtpsExplicit: return "ftps-explicit";
    case RemoteProtocol::FtpsImplicit: return "ftps-implicit";
    case RemoteProtocol::Smb: return "smb";
    case RemoteProtocol::NetworkDrive: return "network-drive";
    }
    return "sftp";
}

inline RemoteProtocol protocolFromKey(const QString& key) {
    if (key == "ftp") return RemoteProtocol::Ftp;
    if (key == "ftps-explicit") return RemoteProtocol::FtpsExplicit;
    if (key == "ftps-implicit") return RemoteProtocol::FtpsImplicit;
    if (key == "smb") return RemoteProtocol::Smb;
    if (key == "network-drive") return RemoteProtocol::NetworkDrive;
    return RemoteProtocol::Sftp;
}

inline QJsonObject profileToJson(const RemoteProfile& profile) {
    QJsonObject obj;
    obj["name"] = profile.name;
    obj["protocol"] = protocolToKey(profile.protocol);
    obj["host"] = profile.host;
    obj["port"] = profile.port;
    obj["username"] = profile.username;
    obj["savePassword"] = profile.savePassword;
    obj["password"] = profile.savePassword ? obfuscate(profile.password) : QString();
    obj["keyFile"] = profile.keyFile;
    obj["passphrase"] = profile.savePassword ? obfuscate(profile.passphrase) : QString();
    obj["certFile"] = profile.certFile;
    obj["shareName"] = profile.shareName;
    obj["domain"] = profile.domain;
    obj["mountPath"] = profile.mountPath;
    obj["remoteRoot"] = profile.remoteRoot;
    obj["passiveMode"] = profile.passiveMode;
    obj["description"] = profile.description;
    return obj;
}

inline RemoteProfile profileFromJson(const QJsonObject& obj) {
    RemoteProfile profile;
    profile.name = obj["name"].toString();
    profile.protocol = protocolFromKey(obj["protocol"].toString());
    profile.host = obj["host"].toString();
    profile.port = obj["port"].toInt(defaultPortFor(profile.protocol));
    profile.username = obj["username"].toString();
    profile.savePassword = obj["savePassword"].toBool(true);
    profile.password = deobfuscate(obj["password"].toString());
    profile.keyFile = obj["keyFile"].toString();
    profile.passphrase = deobfuscate(obj["passphrase"].toString());
    profile.certFile = obj["certFile"].toString();
    profile.shareName = obj["shareName"].toString();
    profile.domain = obj["domain"].toString();
    profile.mountPath = obj["mountPath"].toString();
    profile.remoteRoot = obj["remoteRoot"].toString(QStringLiteral("/"));
    profile.passiveMode = obj["passiveMode"].toBool(true);
    profile.description = obj["description"].toString();
    return profile;
}

inline QJsonObject nodeToJson(const RemoteProfileNode& node) {
    QJsonObject obj;
    obj["name"] = node.name;
    obj["path"] = node.path;
    QJsonArray profiles;
    for (const auto& profile : node.profiles) profiles.append(profileToJson(profile));
    obj["profiles"] = profiles;
    QJsonArray children;
    for (const auto& child : node.children) children.append(nodeToJson(child));
    obj["children"] = children;
    return obj;
}

inline RemoteProfileNode nodeFromJson(const QJsonObject& obj) {
    RemoteProfileNode node;
    node.name = obj["name"].toString();
    node.path = obj["path"].toString();
    for (const auto value : obj["profiles"].toArray()) node.profiles.append(profileFromJson(value.toObject()));
    for (const auto value : obj["children"].toArray()) node.children.append(nodeFromJson(value.toObject()));
    return node;
}

// Finds a node by its slash-separated path, mutably, so callers can splice
// profiles/children in and out in place before the whole tree is
// re-serialized. Pass an empty path for "the implicit root list".
inline RemoteProfileNode* findNode(QList<RemoteProfileNode>& nodes, const QString& path) {
    for (auto& node : nodes) {
        if (node.path == path) return &node;
        if (auto* found = findNode(node.children, path)) return found;
    }
    return nullptr;
}

}  // namespace detail

class RemoteProfileStore {
public:
    // Same preferences.ini PersistedMainWindow already writes window
    // geometry to (see main_window.h). Deliberately NOT a default-
    // constructed `QSettings settings;` - that overload stores under
    // QCoreApplication::organizationName()/applicationName(), which this
    // app never sets, so it silently fails to persist (or persists to a
    // location that changes across launches) instead of surfacing an
    // error. Using the same explicit, already-proven-working file every
    // other preference in the app relies on makes saved connections
    // survive a restart like everything else does.
    static QString settingsFilePath() { return SessionHistory::rootPath() + "/preferences.ini"; }

    static QList<RemoteProfileNode> loadTree() {
        auto& cache = treeCache();
        if (!loaded()) {
            QSettings settings(settingsFilePath(), QSettings::IniFormat);
            const QByteArray json = settings.value("remoteConnections/tree").toByteArray();
            if (!json.isEmpty()) {
                const auto doc = QJsonDocument::fromJson(json);
                for (const auto value : doc.array()) cache.append(detail::nodeFromJson(value.toObject()));
            }
            if (cache.isEmpty()) {
                // Same convention as SessionHistory's saved sessions: start
                // the user off with one obvious place to put things.
                RemoteProfileNode personal;
                personal.name = "Personal";
                personal.path = "Personal";
                cache.append(personal);
            }
            loaded() = true;
        }
        return cache;
    }

    static bool createNode(const QString& parentPath, const QString& name) {
        if (name.trimmed().isEmpty()) return false;
        loadTree();
        auto& roots = treeCache();
        RemoteProfileNode node;
        node.name = name.trimmed();
        if (parentPath.isEmpty()) {
            node.path = node.name;
            roots.append(node);
        } else {
            auto* parent = detail::findNode(roots, parentPath);
            if (!parent) return false;
            node.path = parentPath + "/" + node.name;
            parent->children.append(node);
        }
        persist();
        return true;
    }

    static bool renameNode(const QString& path, const QString& newName) {
        if (newName.trimmed().isEmpty()) return false;
        loadTree();
        auto* node = detail::findNode(treeCache(), path);
        if (!node) return false;
        const QString parentPath = path.contains('/') ? path.left(path.lastIndexOf('/')) : QString();
        node->name = newName.trimmed();
        node->path = parentPath.isEmpty() ? node->name : parentPath + "/" + node->name;
        rewriteDescendantPaths(*node);
        persist();
        return true;
    }

    static bool removeNode(const QString& path) {
        loadTree();
        return removeNodeFrom(treeCache(), path) && (persist(), true);
    }

    // index < 0 appends a new profile; otherwise updates the profile
    // currently at that index within nodePath.
    static bool saveProfile(const QString& nodePath, int index, const RemoteProfile& profile) {
        loadTree();
        auto* node = detail::findNode(treeCache(), nodePath);
        if (!node) return false;
        if (index >= 0 && index < node->profiles.size()) {
            node->profiles[index] = profile;
        } else {
            node->profiles.append(profile);
        }
        persist();
        return true;
    }

    static bool removeProfile(const QString& nodePath, int index) {
        loadTree();
        auto* node = detail::findNode(treeCache(), nodePath);
        if (!node || index < 0 || index >= node->profiles.size()) return false;
        node->profiles.removeAt(index);
        persist();
        return true;
    }

    static bool moveProfile(const QString& sourceNodePath, int index, const QString& targetNodePath) {
        loadTree();
        auto& roots = treeCache();
        auto* source = detail::findNode(roots, sourceNodePath);
        auto* target = detail::findNode(roots, targetNodePath);
        if (!source || !target || index < 0 || index >= source->profiles.size()) return false;
        target->profiles.append(source->profiles.takeAt(index));
        persist();
        return true;
    }

    static bool findProfileByName(const QString& nodePath, const QString& name, RemoteProfile* out) {
        std::function<const RemoteProfileNode*(const QList<RemoteProfileNode>&)> search =
            [&](const QList<RemoteProfileNode>& nodes) -> const RemoteProfileNode* {
            for (const auto& node : nodes) {
                if (node.path == nodePath) return &node;
                if (const auto* found = search(node.children)) return found;
            }
            return nullptr;
        };
        const auto* node = search(loadTree());
        if (!node) return false;
        for (const auto& profile : node->profiles) {
            if (profile.name == name) {
                if (out) *out = profile;
                return true;
            }
        }
        return false;
    }

    // Match a displayed compare path (sftp://user@host/path, or a network
    // drive mount) back to the saved profile that produced it.
    static bool findProfileForPath(const QString& path, RemoteProfile* out) {
        std::function<bool(const RemoteProfileNode&)> walk = [&](const RemoteProfileNode& node) -> bool {
            for (const auto& profile : node.profiles) {
                if (profile.protocol == RemoteProtocol::NetworkDrive) {
                    if (!profile.mountPath.isEmpty() &&
                        (path == profile.mountPath || path.startsWith(profile.mountPath + "/") ||
                         path.startsWith(profile.mountPath + "\\"))) {
                        if (out) *out = profile;
                        return true;
                    }
                } else {
                    const QString prefix = profile.displayAddress();
                    if (!prefix.isEmpty() && (path == prefix || path.startsWith(prefix + "/"))) {
                        if (out) *out = profile;
                        return true;
                    }
                }
            }
            for (const auto& child : node.children) {
                if (walk(child)) return true;
            }
            return false;
        };
        for (const auto& node : loadTree()) {
            if (walk(node)) return true;
        }
        return false;
    }

    // Deterministic per-node colour, same trick SessionHistory uses for its
    // saved-session folders, so the two trees read consistently.
    static QColor colorForNode(const QString& path) {
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        const QString key = "remoteConnections/colors/" + QString(path).replace('/', "_");
        if (settings.contains(key)) return settings.value(key).value<QColor>();
        static const QColor palette[] = {
            QColor(0x4f, 0x9d, 0xde), QColor(0xe5, 0x7a, 0x44), QColor(0x68, 0xc0, 0x9a),
            QColor(0xe8, 0xc5, 0x47), QColor(0xb0, 0x7a, 0xe0), QColor(0xe0, 0x7a, 0xa0),
        };
        const QColor chosen = palette[QRandomGenerator::global()->bounded(int(std::size(palette)))];
        settings.setValue(key, chosen);
        return chosen;
    }

private:
    static QList<RemoteProfileNode>& treeCache() {
        static QList<RemoteProfileNode> cache;
        return cache;
    }
    static bool& loaded() {
        static bool flag = false;
        return flag;
    }

    static void persist() {
        QJsonArray array;
        for (const auto& node : treeCache()) array.append(detail::nodeToJson(node));
        QSettings settings(settingsFilePath(), QSettings::IniFormat);
        settings.setValue("remoteConnections/tree", QJsonDocument(array).toJson(QJsonDocument::Compact));
    }

    static void rewriteDescendantPaths(RemoteProfileNode& node) {
        for (auto& child : node.children) {
            child.path = node.path + "/" + child.name;
            rewriteDescendantPaths(child);
        }
    }

    static bool removeNodeFrom(QList<RemoteProfileNode>& nodes, const QString& path) {
        for (int i = 0; i < nodes.size(); ++i) {
            if (nodes[i].path == path) {
                nodes.removeAt(i);
                return true;
            }
            if (removeNodeFrom(nodes[i].children, path)) return true;
        }
        return false;
    }
};

}  // namespace openbc::app

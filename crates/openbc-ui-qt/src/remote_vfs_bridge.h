// remote_vfs_bridge.h
// ---------------------------------------------------------------------------
// VfsSessionBridge: the RemoteBrowseBridge implementation
// remote_path_browser_dialog.h has been waiting on a nullptr for. It talks
// to the openbc-vfs Rust crate through the openbc_vfs_* FFI surface in
// vfs_bridge.rs (declared in vfs_bridge_ffi.h) rather than any networking
// code of its own.
//
// One VfsSessionBridge is meant to be shared by every PathSelector in the
// app (see sharedVfsBridge() at the bottom) - RemotePathBrowserDialog only
// ever has one profile "active" (whichever is picked in its Location combo)
// at a time, so this keeps at most one live session per profile name and
// tears down a previous one before opening a new one, rather than leaking
// a connection every time the combo changes.
//
// Every openbc_vfs_* call blocks on Rust's own Tokio runtime (see
// vfs_bridge.rs's `block_on`). connectProfile()/listDirectory() - the two
// RemoteBrowseBridge methods the picker dialog calls interactively from the
// GUI thread - run on a QThreadPool worker and post their result back via
// QMetaObject::invokeMethod, the same off-GUI-thread pattern
// ComparisonRun already uses in folder_compare_view.h. ensureConnected(),
// remoteIsDir() and remoteReadFile() are meant to be called from callers
// that are already off the GUI thread themselves (CompareSession::refresh's
// background validation, TextCompareView::loadSide's QThreadPool lambda),
// so those call the FFI directly rather than hopping through another
// worker thread.
// ---------------------------------------------------------------------------
#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QThreadPool>
#include <functional>

#include "remote_path_browser_dialog.h"  // RemoteBrowseBridge, RemoteEntry
#include "remote_profile.h"
#include "vfs_bridge_ffi.h"

namespace openbc::app {

struct VfsCompareSide {
    bool exists = false;
    bool isDir = false;
    bool isLink = false;
    QString path;
    qint64 size = 0;
    QDateTime modified;
};

struct VfsCompareEntry {
    QString name;
    QString status;
    VfsCompareSide left;
    VfsCompareSide right;
};

namespace vfs_detail {

// Must match VfsProfileJson's protocol keys in vfs_bridge.rs exactly - keep
// this in sync with detail::protocolToKey in remote_profile_store.h (same
// strings, different serialization target).
inline QString protocolKey(RemoteProtocol protocol) {
    switch (protocol) {
    case RemoteProtocol::Sftp: return "sftp";
    case RemoteProtocol::Ftp: return "ftp";
    case RemoteProtocol::FtpsExplicit: return "ftps-explicit";
    case RemoteProtocol::FtpsImplicit: return "ftps-implicit";  // vfs_bridge.rs rejects this at connect time
    case RemoteProtocol::Smb: return "smb";
    case RemoteProtocol::NetworkDrive: return "network-drive";
    }
    return "sftp";
}

// Matches VfsProfileJson's #[serde(rename_all = "camelCase")] fields in
// vfs_bridge.rs field-for-field. If that struct gains/renames a field,
// mirror the change here.
inline QByteArray profileToFfiJson(const RemoteProfile& profile) {
    QJsonObject obj;
    obj["protocol"] = protocolKey(profile.protocol);
    obj["host"] = profile.host;
    obj["port"] = profile.port;
    obj["username"] = profile.username;
    obj["password"] = profile.password;
    obj["keyFile"] = profile.keyFile;
    obj["passphrase"] = profile.passphrase;
    obj["shareName"] = profile.shareName;
    obj["mountPath"] = profile.mountPath;
    obj["remoteRoot"] = profile.remoteRoot;
    obj["passiveMode"] = profile.passiveMode;
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

// Owns a Rust-allocated `char*` and always frees it via
// openbc_vfs_string_destroy, so every early-return path here still cleans
// up (RAII instead of a destroy call at every branch).
class FfiString {
public:
    explicit FfiString(char* raw) : raw_(raw) {}
    ~FfiString() {
        if (raw_) openbc_vfs_string_destroy(raw_);
    }
    FfiString(const FfiString&) = delete;
    FfiString& operator=(const FfiString&) = delete;
    bool isNull() const { return raw_ == nullptr; }
    QString toQString() const { return raw_ ? QString::fromUtf8(raw_) : QString(); }

private:
    char* raw_;
};

}  // namespace vfs_detail

// Inverse of RemotePathBrowserDialog::buildRemoteUri: turns a displayed
// "scheme://user@host/some/path" (or, for NetworkDrive, a plain mounted
// path) back into the bare remote-side path the FFI's listing_path()
// expects. Best-effort - it inverts exactly the construction
// buildRemoteUri does, so it stays correct as long as the two functions are
// kept in sync, but doesn't attempt to handle a path a user hand-edited
// into something buildRemoteUri would never have produced.
inline QString remoteRelativePath(const QString& displayPath, const RemoteProfile& profile) {
    if (profile.protocol == RemoteProtocol::NetworkDrive) {
        const QDir mountDir(profile.mountPath);
        const QString relative = mountDir.relativeFilePath(displayPath);
        return (relative.isEmpty() || relative == ".") ? QString() : relative;
    }
    const QString prefix = profile.displayAddress();
    QString path = displayPath;
    if (path.startsWith(prefix)) {
        path.remove(0, prefix.length());
    }
    return path.isEmpty() ? QStringLiteral("/") : path;
}

class VfsSessionBridge : public RemoteBrowseBridge {
public:
    void connectProfile(const RemoteProfile& profile,
                         std::function<void(bool ok, const QString& error)> done) override {
        const QString name = profile.name;
        QThreadPool::globalInstance()->start([this, profile, name, done]() {
            QString error;
            const bool ok = ensureConnected(profile, &error);
            QMetaObject::invokeMethod(
                qApp, [done, ok, error]() { if (done) done(ok, error); }, Qt::QueuedConnection);
        });
    }

    void listDirectory(
        const RemoteProfile& profile, const QString& path,
        std::function<void(bool ok, QList<RemoteEntry> entries, const QString& error)> done) override {
        int32_t sessionId = -1;
        {
            QMutexLocker lock(&mutex_);
            auto it = sessions_.find(profile.name);
            if (it == sessions_.end()) {
                if (done) done(false, {}, "Not connected to " + profile.name);
                return;
            }
            sessionId = it.value().id;
        }
        const QByteArray pathBytes = path.toUtf8();
        QThreadPool::globalInstance()->start([sessionId, pathBytes, done]() {
            char* listingRaw = nullptr;
            vfs_detail::FfiString error(openbc_vfs_list(
                sessionId, reinterpret_cast<const unsigned char*>(pathBytes.constData()),
                static_cast<size_t>(pathBytes.size()), &listingRaw));
            vfs_detail::FfiString listing(listingRaw);
            if (!error.isNull()) {
                const QString message = error.toQString();
                QMetaObject::invokeMethod(
                    qApp, [done, message]() { if (done) done(false, {}, message); }, Qt::QueuedConnection);
                return;
            }
            QList<RemoteEntry> entries;
            const auto doc = QJsonDocument::fromJson(listing.toQString().toUtf8());
            for (const auto value : doc.array()) {
                const auto obj = value.toObject();
                RemoteEntry entry;
                entry.name = obj["name"].toString();
                entry.isDir = obj["isDir"].toBool();
                entries.push_back(entry);
            }
            QMetaObject::invokeMethod(
                qApp, [done, entries]() { if (done) done(true, entries, {}); }, Qt::QueuedConnection);
        });
    }

    void disconnectProfile(const RemoteProfile& profile) override {
        QMutexLocker lock(&mutex_);
        auto it = sessions_.find(profile.name);
        if (it != sessions_.end()) {
            openbc_vfs_disconnect(it.value().id);
            sessions_.erase(it);
        }
    }

    // Connects (if not already connected) using the calling thread - safe
    // to call from a background thread, and expected to be: it blocks on
    // the network the same way the Rust side's own `block_on` does.
    bool ensureConnected(const RemoteProfile& profile, QString* error) {
        {
            QMutexLocker lock(&mutex_);
            if (sessions_.contains(profile.name)) return true;
        }
        // Only one profile is ever "current" in the picker dialog at a
        // time, but a caller could still hold paths from two different
        // profiles (left on one host, right on another) at once - don't
        // evict an unrelated session here, just add this one.
        const QByteArray json = vfs_detail::profileToFfiJson(profile);
        int32_t sessionId = 0;
        vfs_detail::FfiString err(openbc_vfs_connect(
            reinterpret_cast<const unsigned char*>(json.constData()),
            static_cast<size_t>(json.size()), &sessionId));
        if (!err.isNull()) {
            if (error) *error = err.toQString();
            return false;
        }
        QMutexLocker lock(&mutex_);
        sessions_[profile.name] = Session{sessionId, profile};
        return true;
    }

    // True/false for "is this remote path a directory"; call off the GUI
    // thread. Connects on demand so a remote path restored from MRU
    // history (rather than freshly browsed) still works.
    bool remoteIsDir(const RemoteProfile& profile, const QString& path, QString* error) {
        if (!ensureConnected(profile, error)) return false;
        const int32_t sessionId = sessionIdFor(profile);
        const QByteArray pathBytes = path.toUtf8();
        char* errorRaw = nullptr;
        const int32_t result = openbc_vfs_stat(
            sessionId, reinterpret_cast<const unsigned char*>(pathBytes.constData()),
            static_cast<size_t>(pathBytes.size()), &errorRaw);
        vfs_detail::FfiString err(errorRaw);
        if (result < 0) {
            if (error) *error = err.isNull() ? QStringLiteral("stat failed") : err.toQString();
            return false;
        }
        return result == 1;
    }

    // Reads a whole remote file into memory; call off the GUI thread. Fine
    // for the small-file comparisons TextCompareView needs (same trade-off
    // read_small_file makes on the Rust side) - not meant for huge files.
    bool remoteReadFile(const RemoteProfile& profile, const QString& path, QByteArray* out, QString* error) {
        if (!ensureConnected(profile, error)) return false;
        const int32_t sessionId = sessionIdFor(profile);
        const QByteArray pathBytes = path.toUtf8();
        size_t length = 0;
        char* errorRaw = nullptr;
        unsigned char* data = openbc_engine_read_file(
            sessionId, reinterpret_cast<const unsigned char*>(pathBytes.constData()),
            static_cast<size_t>(pathBytes.size()), &length, &errorRaw);
        vfs_detail::FfiString err(errorRaw);
        if (!data) {
            if (error) *error = err.isNull() ? QStringLiteral("read failed") : err.toQString();
            return false;
        }
        if (out) *out = QByteArray(reinterpret_cast<const char*>(data), static_cast<int>(length));
        openbc_vfs_buffer_destroy(data, length);
        return true;
    }

    bool remoteWriteFile(const RemoteProfile& profile, const QString& path,
                         const QByteArray& bytes, QString* error) {
        if (!ensureConnected(profile, error)) return false;
        const int32_t sessionId = sessionIdFor(profile);
        const QByteArray pathBytes = path.toUtf8();
        vfs_detail::FfiString err(openbc_engine_write_file(
            sessionId, reinterpret_cast<const unsigned char*>(pathBytes.constData()),
            static_cast<size_t>(pathBytes.size()),
            reinterpret_cast<const unsigned char*>(bytes.constData()),
            static_cast<size_t>(bytes.size())));
        if (!err.isNull()) {
            if (error) *error = err.toQString();
            return false;
        }
        return true;
    }

    bool remoteCreateDirectory(const RemoteProfile& profile, const QString& path, QString* error) {
        if (!ensureConnected(profile, error)) return false;
        const int32_t sessionId = sessionIdFor(profile);
        const QByteArray pathBytes = path.toUtf8();
        vfs_detail::FfiString err(openbc_engine_create_dir(
            sessionId, reinterpret_cast<const unsigned char*>(pathBytes.constData()),
            static_cast<size_t>(pathBytes.size())));
        if (!err.isNull()) {
            if (error) *error = err.toQString();
            return false;
        }
        return true;
    }

    bool remoteRename(const RemoteProfile& profile, const QString& from, const QString& to,
                      QString* error) {
        if (!ensureConnected(profile, error)) return false;
        const int32_t sessionId = sessionIdFor(profile);
        const QByteArray fromBytes = from.toUtf8();
        const QByteArray toBytes = to.toUtf8();
        vfs_detail::FfiString err(openbc_engine_rename(
            sessionId, reinterpret_cast<const unsigned char*>(fromBytes.constData()),
            static_cast<size_t>(fromBytes.size()),
            reinterpret_cast<const unsigned char*>(toBytes.constData()),
            static_cast<size_t>(toBytes.size())));
        if (!err.isNull()) {
            if (error) *error = err.toQString();
            return false;
        }
        return true;
    }

    bool remoteListDirectory(const RemoteProfile& profile, const QString& path,
                             QList<RemoteEntry>* out, QString* error) {
        if (!ensureConnected(profile, error)) return false;
        const int32_t sessionId = sessionIdFor(profile);
        const QByteArray pathBytes = path.toUtf8();
        char* listingRaw = nullptr;
        vfs_detail::FfiString ffiError(openbc_vfs_list(
            sessionId, reinterpret_cast<const unsigned char*>(pathBytes.constData()),
            static_cast<size_t>(pathBytes.size()), &listingRaw));
        vfs_detail::FfiString listing(listingRaw);
        if (!ffiError.isNull()) {
            if (error) *error = ffiError.toQString();
            return false;
        }
        const QJsonDocument document = QJsonDocument::fromJson(listing.toQString().toUtf8());
        if (!document.isArray()) {
            if (error) *error = QStringLiteral("The remote provider returned an invalid listing.");
            return false;
        }
        QList<RemoteEntry> entries;
        for (const QJsonValue& value : document.array()) {
            const QJsonObject object = value.toObject();
            entries.push_back({object.value("name").toString(), object.value("isDir").toBool()});
        }
        if (out) *out = std::move(entries);
        return true;
    }

    bool compareFolderLevel(const QString& leftPath, const RemoteProfile& leftProfile,
                            bool leftRemote, const QString& rightPath,
                            const RemoteProfile& rightProfile, bool rightRemote,
                            bool checkContent, bool ignoreTimestamps,
                            QList<VfsCompareEntry>* out, QString* error) {
        if ((leftRemote && !ensureConnected(leftProfile, error)) ||
            (rightRemote && !ensureConnected(rightProfile, error)) ||
            (!leftRemote && !ensureLocalConnected(error)) ||
            (!rightRemote && !ensureLocalConnected(error))) {
            return false;
        }

        const int32_t leftSession = leftRemote ? sessionIdFor(leftProfile) : localSessionId();
        const int32_t rightSession = rightRemote ? sessionIdFor(rightProfile) : localSessionId();
        const QByteArray leftBytes = (leftRemote ? remoteRelativePath(leftPath, leftProfile) : leftPath).toUtf8();
        const QByteArray rightBytes = (rightRemote ? remoteRelativePath(rightPath, rightProfile) : rightPath).toUtf8();
        char* resultRaw = nullptr;
        vfs_detail::FfiString ffiError(openbc_engine_compare_folder_level(
            leftSession, reinterpret_cast<const unsigned char*>(leftBytes.constData()),
            static_cast<size_t>(leftBytes.size()), rightSession,
            reinterpret_cast<const unsigned char*>(rightBytes.constData()),
            static_cast<size_t>(rightBytes.size()), checkContent ? 1 : 0,
            ignoreTimestamps ? 1 : 0, &resultRaw));
        vfs_detail::FfiString result(resultRaw);
        if (!ffiError.isNull()) {
            if (error) *error = ffiError.toQString();
            return false;
        }

        const QJsonDocument document = QJsonDocument::fromJson(result.toQString().toUtf8());
        if (!document.isArray()) {
            if (error) *error = QStringLiteral("The comparison engine returned an invalid listing.");
            return false;
        }
        QList<VfsCompareEntry> entries;
        for (const QJsonValue& value : document.array()) {
            const QJsonObject object = value.toObject();
            VfsCompareEntry entry;
            entry.name = object.value("name").toString();
            entry.status = object.value("status").toString();
            auto readSide = [](const QJsonValue& value, VfsCompareSide* side) {
                if (!value.isObject()) return;
                const QJsonObject object = value.toObject();
                side->exists = true;
                side->isDir = object.value("isDir").toBool();
                side->isLink = object.value("isLink").toBool();
                side->size = object.value("size").toVariant().toLongLong();
                const qint64 modifiedMs = object.value("mtimeMs").toVariant().toLongLong();
                if (modifiedMs != 0) side->modified = QDateTime::fromMSecsSinceEpoch(modifiedMs);
            };
            readSide(object.value("left"), &entry.left);
            readSide(object.value("right"), &entry.right);
            entries.push_back(std::move(entry));
        }
        if (out) *out = std::move(entries);
        return true;
    }

private:
    struct Session {
        int32_t id;
        RemoteProfile profile;
    };

    int32_t sessionIdFor(const RemoteProfile& profile) {
        QMutexLocker lock(&mutex_);
        auto it = sessions_.find(profile.name);
        return it == sessions_.end() ? -1 : it.value().id;
    }

    bool ensureLocalConnected(QString* error) {
        QMutexLocker lock(&mutex_);
        if (localSessionId_ >= 0) return true;
        QJsonObject profile;
        profile["protocol"] = "local";
        profile["mountPath"] = QDir::currentPath();
        const QByteArray json = QJsonDocument(profile).toJson(QJsonDocument::Compact);
        int32_t sessionId = -1;
        vfs_detail::FfiString err(openbc_vfs_connect(
            reinterpret_cast<const unsigned char*>(json.constData()),
            static_cast<size_t>(json.size()), &sessionId));
        if (!err.isNull()) {
            if (error) *error = err.toQString();
            return false;
        }
        localSessionId_ = sessionId;
        return true;
    }

    int32_t localSessionId() {
        QMutexLocker lock(&mutex_);
        return localSessionId_;
    }

    QMutex mutex_;
    QMap<QString, Session> sessions_;
    int32_t localSessionId_ = -1;
};

// Shared instance every PathSelector construction site passes in, so a
// session opened while browsing on one tab is reused by any other tab that
// picks the same saved profile.
inline VfsSessionBridge& sharedVfsBridge() {
    static VfsSessionBridge bridge;
    return bridge;
}

}  // namespace openbc::app
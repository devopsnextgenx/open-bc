#pragma once
// Shared helpers that give every file-compare viewer (hex, table, registry,
// media, picture, 3D) the same remote-aware path row as the text compare
// viewer: combo + Browse + remote-file picker, MRU history, Enter commits.
//
// The native backends (hex/table/media/picture/model3d) read LOCAL files only,
// so a remote path ("remote profile" path) is downloaded to a per-process temp
// file by resolveLocalPath() and the backend is handed that local copy while the
// UI keeps showing the remote path.

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLineEdit>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <functional>

#include "common/path_selector.h"
#include "remote/remote_profile_store.h"
#include "remote/remote_vfs_bridge.h"

namespace openbc_remote {

inline bool isRemotePath(const QString& path) {
    RemoteProfile profile;
    return !path.isEmpty() && RemoteProfileStore::findProfileForPath(path, &profile);
}

// Returns true and sets *local to a readable local path. Local paths pass through
// unchanged (and must be existing files). Remote paths are downloaded (cached per
// remote path for the process lifetime; pass refresh=true to re-download).
inline bool resolveLocalPath(const QString& path, QString* local, QString* error, bool refresh = false) {
    if (path.isEmpty()) { *local = path; return true; }
    RemoteProfile profile;
    if (!RemoteProfileStore::findProfileForPath(path, &profile)) {
        if (!QFileInfo(path).isFile()) { if (error) *error = "Not a file: " + path; return false; }
        *local = path;
        return true;
    }
    static QTemporaryDir dir;
    static QHash<QString, QString> cache;
    if (!dir.isValid()) { if (error) *error = "Cannot create temp folder for remote file"; return false; }
    if (!refresh && cache.contains(path) && QFileInfo(cache.value(path)).isFile()) {
        *local = cache.value(path);
        return true;
    }
    QByteArray bytes;
    QString err;
    if (!sharedVfsBridge().remoteReadFile(profile, remoteRelativePath(path, profile), &bytes, &err)) {
        if (error) *error = "Could not read remote file " + path + (err.isEmpty() ? QString() : " (" + err + ")");
        return false;
    }
    // Unique sub-folder keeps the original file name/extension (backends sniff by extension).
    QDir sub(dir.path());
    const QString folder = QString::number(cache.size() + 1) + "_" + QString::number(qHash(path), 16);
    sub.mkpath(folder);
    const QString target = sub.filePath(folder + "/" + QFileInfo(path).fileName());
    QFile f(target);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
        if (error) *error = "Could not write temp copy of " + path;
        return false;
    }
    f.close();
    cache.insert(path, target);
    *local = target;
    return true;
}

// Local path the backend should open for `path` (already downloaded at commit time).
inline QString localCopy(const QString& path) {
    QString local, err;
    return resolveLocalPath(path, &local, &err) ? local : path;
}

// Path selector identical to the text viewer's: remote picker enabled, MRU history,
// Enter / dropdown pick / Browse all commit through `onCommit(path)`.
inline PathSelector* makeRemoteFileSelector(QWidget* parent, const QString& side, const QString& initial,
                                            const QString& filter,
                                            std::function<void(QString)> onCommit) {
    auto* selector = new PathSelector(PathSelector::Mode::File, side, parent, /*showUp=*/false, filter,
                                      /*enableRemote=*/true, &sharedVfsBridge());
    if (!initial.isEmpty()) {
        selector->remember(initial);
        selector->setText(initial);
    }
    selector->onBrowsed = [onCommit](QString p) { onCommit(p); };
    QObject::connect(selector->combo()->lineEdit(), &QLineEdit::returnPressed, selector,
                     [selector, onCommit]() { onCommit(selector->text()); });
    QObject::connect(selector->combo(), QOverload<int>::of(&QComboBox::activated), selector,
                     [selector, onCommit](int) { onCommit(selector->text()); });
    return selector;
}

}  // namespace openbc_remote

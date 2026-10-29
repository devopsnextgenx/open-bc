// file_change_controller.h
// ---------------------------------------------------------------------------
// Glue between ONE viewer and the engine's external-change tracking
// (openbc-engine resource_watch, via ffi/resource_watch.rs):
//
//   viewer --track(side, path)-----> engine (baseline BEFORE the viewer reads the file)
//   engine --stale/fresh/unavailable--> FileChangeBanner (popup over the viewer's own tab)
//   banner --Reload Latest--> [confirmReload] -> engine rebase -> viewer.reloadSide(...)
//   banner --Keep Current---> engine keep (no more notices for that version)
//
// The viewer keeps ownership of loading, dirty state and its discard prompt; this class never
// reads files and never replaces loaded content by itself. It watches nothing on its own: all
// observation (native events, polling, remote checks) happens in Rust.
//
// Usage in a viewer (after its UI/layout exists):
//   changes_ = std::make_unique<FileChangeController>(this, [this](std::uint8_t side) { reloadSide(side); },
//                                                     /*wholeTab=*/true, [this]() { return confirmDiscard(); });
//   ...and call changes_->track(0, leftPath_); changes_->track(1, rightPath_); right before loading.
// Declare `changes_` as the LAST data member so it is destroyed first.
// ---------------------------------------------------------------------------
#pragma once

#include <QFileInfo>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>

#include "backend/backend_channel.h"
#include "backend/compare_backend_api.h"
#include "common/file_change_banner.h"
#include "common/remote_selector.h"

extern "C" {
struct OpenBcWatchSession;
OpenBcWatchSession* openbc_watch_create(OpenBcEventCallback cb, void* user);
void openbc_watch_destroy(OpenBcWatchSession*);
uint8_t openbc_watch_set_local(const OpenBcWatchSession*, uint8_t side, const char* path, uint8_t isDir);
uint8_t openbc_watch_set_remote(const OpenBcWatchSession*, uint8_t side, int32_t sessionId, const char* path, uint8_t isDir);
uint8_t openbc_watch_clear(const OpenBcWatchSession*, uint8_t side);
uint8_t openbc_watch_rebase(const OpenBcWatchSession*, uint8_t side);
uint8_t openbc_watch_keep(const OpenBcWatchSession*, uint8_t side);
}

namespace openbc::app {

class FileChangeController {
public:
    // reloadSide : re-reads from the source (remote copies must be fetched again). Called AFTER the
    //              engine baseline moved. `wholeTab` = the viewer reloads both sides in one call.
    // confirmReload : optional; return false to abort (e.g. the viewer's unsaved-changes prompt).
    FileChangeController(QWidget* viewer, std::function<void(std::uint8_t)> reloadSide, bool wholeTab,
                         std::function<bool()> confirmReload = {})
        : reloadSide_(std::move(reloadSide)), wholeTab_(wholeTab), confirmReload_(std::move(confirmReload)),
          channel_(viewer, &FileChangeController::noHandles, [this](const BackendChannel::Event& e) { onEvent(e); }) {
        banner_ = new FileChangeBanner(viewer);  // floating popup over the viewer, not part of its layout
        banner_->onReload = [this](const QList<std::uint8_t>& sides) { reload(sides); };
        banner_->onKeep = [this](const QList<std::uint8_t>& sides) {
            for (std::uint8_t s : sides) if (session_) openbc_watch_keep(session_, s);
        };
        session_ = openbc_watch_create(BackendChannel::callback(), channel_.user());
    }

    // After this returns no event callback runs any more (Rust guarantees it).
    ~FileChangeController() {
        if (session_) openbc_watch_destroy(session_);
    }

    FileChangeController(const FileChangeController&) = delete;
    FileChangeController& operator=(const FileChangeController&) = delete;

    // Register what `side` is about to load. Call BEFORE reading the file. Empty path = nothing there.
    void track(std::uint8_t side, const QString& path, bool isDirectory = false) {
        if (!session_ || side > 1) return;
        paths_[side] = path;
        directory_[side] = isDirectory;
        watching_[side] = false;
        banner_->clearSide(side);  // new input: any old notice belonged to the previous one
        if (path.isEmpty()) {
            openbc_watch_clear(session_, side);
            return;
        }
        startWatch(side);
    }

    // A remote side registered by track() before its connection existed (folder compare registers
    // on refresh(), the connection is opened by the comparison itself) is not watched yet. Call
    // this once the viewer has loaded: it starts those watches, from the state at that moment,
    // without touching sides that are already watched or any notice on screen.
    void trackPending() {
        for (std::uint8_t side = 0; side < 2; ++side)
            if (session_ && !paths_[side].isEmpty() && !watching_[side]) startWatch(side);
    }

    FileChangeBanner* banner() const { return banner_; }

private:
    static void noHandles(int, void*) {}  // watch events carry text only

    void startWatch(std::uint8_t side) {
        const QString& path = paths_[side];
        const bool isDirectory = directory_[side];
        RemoteProfile profile;
        const bool remote = RemoteProfileStore::findProfileForPath(path, &profile) &&
                            profile.protocol != RemoteProtocol::NetworkDrive;  // mounted drives are plain paths
        if (!remote) {
            watching_[side] = openbc_watch_set_local(session_, side, path.toUtf8().constData(), isDirectory ? 1 : 0) != 0;
            return;
        }
        // Reuse the connection the viewer already opened to load the file; never connect (block) here.
        const std::int32_t sessionId = sharedVfsBridge().connectedSessionId(profile);
        if (sessionId < 0) return;  // not connected: nothing to compare against, so nothing to watch
        const QByteArray relative = remoteRelativePath(path, profile).toUtf8();
        watching_[side] = openbc_watch_set_remote(session_, side, sessionId, relative.constData(), isDirectory ? 1 : 0) != 0;
    }

    void reload(const QList<std::uint8_t>& sides) {
        if (sides.isEmpty()) return;
        if (confirmReload_ && !confirmReload_()) return;  // user kept their edits: nothing changes
        // Baseline first, read second: a change during the read keeps the side marked stale.
        for (std::uint8_t s : sides) if (session_) openbc_watch_rebase(session_, s);
        if (wholeTab_) {
            reloadSide_(sides.first());
        } else {
            for (std::uint8_t s : sides) reloadSide_(s);
        }
        for (std::uint8_t s : sides) banner_->clearSide(s);
    }

    void onEvent(const BackendChannel::Event& event) {
        if (event.kind != OPENBC_EV_STATUS) return;
        const QString kind = event.message.section('|', 0, 0);
        const int sideValue = event.message.section('|', 1, 1).toInt();
        if (sideValue < 0 || sideValue > 1) return;
        const std::uint8_t side = static_cast<std::uint8_t>(sideValue);
        const QString detail = event.message.section('|', 2);
        if (kind == "stale") banner_->showStale(side, paths_[side], detail);
        else if (kind == "fresh") banner_->clearSide(side);
        else if (kind == "unavailable") banner_->showUnavailable(side, paths_[side], detail);
    }

    std::function<void(std::uint8_t)> reloadSide_;
    bool wholeTab_;
    std::function<bool()> confirmReload_;
    BackendChannel channel_;
    QPointer<FileChangeBanner> banner_;
    OpenBcWatchSession* session_ = nullptr;
    QString paths_[2];
    bool directory_[2] = {false, false};
    bool watching_[2] = {false, false};  // false for a remote side whose connection did not exist yet
};

}  // namespace openbc::app
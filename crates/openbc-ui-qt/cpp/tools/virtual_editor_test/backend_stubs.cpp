// Inert stand-ins for backend entry points that FileChangeController / archive_support reference.
// The real implementations live in the full Rust backend (not linked into this isolated test).
// Watching is a no-op here: external-change notices are exercised by the tests via the viewer's own
// callbacks, never through the engine's file watcher.
#include <cstddef>
#include <cstdint>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

typedef void (*OpenBcEventCallback)(void* user, int32_t kind, const char* message, void* handle);

// A tiny polling stand-in for the engine's resource watcher: same events ("stale|side|detail",
// "fresh|side"), same contract (rebase = new baseline + fresh, keep = new baseline, no notice).
struct OpenBcWatchSession {
    OpenBcEventCallback cb = nullptr;
    void* user = nullptr;
    std::mutex mutex;
    std::string path[2];
    long long size[2] = {-1, -1};
    long long mtime[2] = {0, 0};
    bool notified[2] = {false, false};
    std::atomic<bool> stop{false};
    std::thread worker;
};

static void statOf(const std::string& path, long long* size, long long* mtime) {
    struct stat st;
    if (path.empty() || ::stat(path.c_str(), &st) != 0) { *size = -1; *mtime = 0; return; }
    *size = st.st_size;
    *mtime = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

extern "C" {
OpenBcWatchSession* openbc_watch_create(OpenBcEventCallback cb, void* user) {
    auto* s = new OpenBcWatchSession;
    s->cb = cb;
    s->user = user;
    s->worker = std::thread([s]() {
        while (!s->stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            for (int side = 0; side < 2; ++side) {
                std::string msg;
                {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (s->path[side].empty() || s->notified[side]) continue;
                    long long size, mtime;
                    statOf(s->path[side], &size, &mtime);
                    if (size != s->size[side] || mtime != s->mtime[side]) {
                        s->notified[side] = true;
                        msg = "stale|" + std::to_string(side) + "|changed on disk";
                    }
                }
                if (!msg.empty()) s->cb(s->user, 0 /*OPENBC_EV_STATUS*/, msg.c_str(), nullptr);
            }
        }
    });
    return s;
}
void openbc_watch_destroy(OpenBcWatchSession* s) {
    if (!s) return;
    s->stop = true;
    s->worker.join();
    delete s;
}
uint8_t openbc_watch_set_local(const OpenBcWatchSession* cs, uint8_t side, const char* path, uint8_t) {
    auto* s = const_cast<OpenBcWatchSession*>(cs);
    std::lock_guard<std::mutex> lock(s->mutex);
    s->path[side] = path ? path : "";
    statOf(s->path[side], &s->size[side], &s->mtime[side]);
    s->notified[side] = false;
    return 1;
}
uint8_t openbc_watch_set_remote(const OpenBcWatchSession*, uint8_t, int32_t, const char*, uint8_t) { return 0; }
uint8_t openbc_watch_clear(const OpenBcWatchSession* cs, uint8_t side) {
    auto* s = const_cast<OpenBcWatchSession*>(cs);
    std::lock_guard<std::mutex> lock(s->mutex);
    s->path[side].clear();
    s->notified[side] = false;
    return 1;
}
uint8_t openbc_watch_rebase(const OpenBcWatchSession* cs, uint8_t side) {
    auto* s = const_cast<OpenBcWatchSession*>(cs);
    std::lock_guard<std::mutex> lock(s->mutex);
    statOf(s->path[side], &s->size[side], &s->mtime[side]);
    s->notified[side] = false;
    return 1;
}
uint8_t openbc_watch_keep(const OpenBcWatchSession* cs, uint8_t side) { return openbc_watch_rebase(cs, side); }
int32_t openbc_archive_depth(const unsigned char*, size_t) { return 0; }
char* openbc_vfs_connect(const std::uint8_t*, std::size_t, std::int32_t*) { return nullptr; }
void openbc_vfs_disconnect(std::int32_t) {}
char* openbc_vfs_list(std::int32_t, const std::uint8_t*, std::size_t, char**) { return nullptr; }
void openbc_vfs_string_destroy(char*) {}
}

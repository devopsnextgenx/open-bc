// backend_channel.h
// ---------------------------------------------------------------------------
// BackendChannel: receives backend events on the Rust worker thread, copies
// them, and re-posts them to the GUI thread (the architecture rule for the
// image-compare tab, reused by the table / registry / hex / media tabs).
//
// Owner contract: the owning widget must call the backend's *_session_destroy
// FIRST in its destructor. That call guarantees no further callbacks, so the
// channel (a member) can then be destroyed safely. Handles that arrive after
// the owner is gone are released by the shared_ptr deleter.
// ---------------------------------------------------------------------------
#pragma once

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>

namespace openbc::app {

class BackendChannel {
public:
    struct Event {
        int kind = 0;
        QString message;
        std::shared_ptr<void> handle;  // releases the Rust handle when the last copy dies
    };
    // Frees a handle of the given event kind (a plain function per tab).
    using Destroy = void (*)(int kind, void* handle);

    BackendChannel(QObject* owner, Destroy destroy, std::function<void(const Event&)> onEvent)
        : owner_(owner), destroy_(destroy), onEvent_(std::move(onEvent)) {}

    BackendChannel(const BackendChannel&) = delete;
    BackendChannel& operator=(const BackendChannel&) = delete;

    void* user() { return this; }
    static constexpr auto callback() { return &BackendChannel::trampoline; }

private:
    static void trampoline(void* user, int32_t kind, const char* message, void* handle) {
        auto* self = static_cast<BackendChannel*>(user);
        Event event;
        event.kind = kind;
        if (message) event.message = QString::fromUtf8(message);
        if (handle) {
            const Destroy destroy = self->destroy_;
            event.handle = std::shared_ptr<void>(handle, [destroy, kind](void* h) { destroy(kind, h); });
        }
        QMetaObject::invokeMethod(self->owner_, [self, event]() { self->onEvent_(event); },
                                  Qt::QueuedConnection);
    }

    QObject* owner_;
    Destroy destroy_;
    std::function<void(const Event&)> onEvent_;
};

// (ptr, len) from the backend to QString; null pointer gives an empty string.
inline QString backendString(const std::uint8_t* data, std::size_t length) {
    return data ? QString::fromUtf8(reinterpret_cast<const char*>(data), static_cast<int>(length))
                : QString();
}

}  // namespace openbc::app
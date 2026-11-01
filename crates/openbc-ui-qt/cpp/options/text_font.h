// text_font.h
// ---------------------------------------------------------------------------
// The font of the Text Compare editors and the "something about text views changed" notifier.
//
// Default (Tools > Options > Appearance, "Use default font"): DejaVu Sans Mono when installed,
// otherwise the system fixed-width font.
// A custom font is stored by Options as QFont::toString().
//
// Viewers subscribe once (owner-scoped, removed automatically when the owner is destroyed) and
// re-apply the font / diff colours when Options pushes a change.
// ---------------------------------------------------------------------------
#pragma once

#include <QFont>
#include <QFontDatabase>
#include <QObject>
#include <QPointer>
#include <QString>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "common/compare_font.h"

namespace openbc::app {

class TextFont {
public:
    static constexpr const char* kDefaultFamily = "DejaVu Sans Mono";
    static constexpr int kDefaultPointSize = 10;

    static TextFont& instance() {
        static TextFont s;
        return s;
    }

    // Font used by the text editors right now.
    QFont font() const { return font_; }

    // What "use default" resolves to on this machine.
    static QFont defaultFont() {
        return defaultCompareFont();
    }

    // `custom` is QFont::toString(); ignored when useDefault or when it does not parse.
    void configure(bool useDefault, const QString& custom) {
        QFont next = defaultFont();
        if (!useDefault && !custom.isEmpty()) {
            QFont parsed;
            if (parsed.fromString(custom) && !parsed.family().isEmpty()) next = parsed;
        }
        font_ = next;
        notify();
    }

    // Tell subscribers that colours used by text views changed (font unchanged).
    void notify() {
        for (std::size_t i = 0; i < subscribers_.size();) {
            if (!subscribers_[i].owner) {
                subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            const auto callback = subscribers_[i].callback;   // copy: a callback may add subscribers
            callback();
            ++i;
        }
    }

    // Calls `callback` now and after every change while `owner` lives.
    void subscribe(QObject* owner, std::function<void()> callback) {
        subscribers_.push_back({QPointer<QObject>(owner), callback});
        callback();
    }

private:
    TextFont() : font_(defaultFont()) {}

    struct Subscriber {
        QPointer<QObject> owner;
        std::function<void()> callback;
    };
    QFont font_;
    std::vector<Subscriber> subscribers_;
};

}  // namespace openbc::app

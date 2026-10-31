// appearance_state.h
// ---------------------------------------------------------------------------
// Runtime result of Tools > Options > Appearance, read by the style code (style/qt_style.h) and the
// viewers. Written ONLY by options/appearance_controller.h; never edit it from anywhere else.
// Keys are "<tab>/<item>", e.g. "Folder Views/Same". An invalid colour means "not overridden".
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QHash>
#include <QString>

namespace openbc::ui {

namespace appearance {

struct Entry {
  QColor text, background, info;
};
struct State {
  QHash<QString, Entry> colors;
  bool stripes = true;
  bool gradientCenter = true;
  bool colorFolders = true;
  int revision = 0; // bumped on every change; icon caches key on it
};
inline State &state() {
  static State s;
  return s;
}
inline QColor text(const QString &key, const QColor &fallback) {
  const auto it = state().colors.constFind(key);
  return it != state().colors.constEnd() && it->text.isValid() ? it->text : fallback;
}
inline QColor background(const QString &key, const QColor &fallback) {
  const auto it = state().colors.constFind(key);
  return it != state().colors.constEnd() && it->background.isValid() ? it->background : fallback;
}
inline QColor info(const QString &key, const QColor &fallback) {
  const auto it = state().colors.constFind(key);
  return it != state().colors.constEnd() && it->info.isValid() ? it->info : fallback;
}
} // namespace appearance

} // namespace openbc::ui

// toolbar_style.h
// ---------------------------------------------------------------------------
// ONE toolbar look for every compare viewer (folder, text, hex, table, registry, picture, 3D, media,
// binary), driven by Tools > Options > Appearance > Toolbars:
//
//   * "Show labels under icons"  -> Qt::ToolButtonTextUnderIcon  / Qt::ToolButtonIconOnly
//   * "Icon size" Small / Large  -> 16 px / 20 px
//
// A viewer never calls setIconSize / setToolButtonStyle / setObjectName on its toolbar itself:
//
//     toolbar_ = new QToolBar(this);
//     openbc::ui::toolbar::apply(toolbar_);          // objectName, look, live updates
//     ...
//     auto* menuBtn = new QToolButton(toolbar_);      // any QToolButton you put on the toolbar
//     openbc::ui::toolbar::styleButton(menuBtn);      // follows the same preference
//
// The preference is written by options/appearance_controller.h (the only writer), which then calls
// refreshAll() so open tabs update immediately. Header-only, no Q_OBJECT.
// ---------------------------------------------------------------------------
#pragma once

#include <QList>
#include <QPointer>
#include <QSize>
#include <QToolBar>
#include <QToolButton>

namespace openbc::ui::toolbar {

enum class IconSize { Small, Large };

struct Prefs {
  bool showLabels = true;              // default = the look before this option existed
  IconSize iconSize = IconSize::Large;
};

inline constexpr int kSmallPx = 16;
inline constexpr int kLargePx = 20;

inline Prefs& prefs() {
  static Prefs p;
  return p;
}

inline int iconPx() { return prefs().iconSize == IconSize::Small ? kSmallPx : kLargePx; }
inline QSize iconSize() { return QSize(iconPx(), iconPx()); }
inline Qt::ToolButtonStyle buttonStyle() {
  return prefs().showLabels ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly;
}

// Dynamic property that marks a QToolButton as "follows the toolbar preference".
inline constexpr const char* kButtonProperty = "obToolbarButton";

inline void styleButton(QToolButton* button) {
  if (!button) return;
  button->setProperty(kButtonProperty, true);
  button->setIconSize(iconSize());
  button->setToolButtonStyle(buttonStyle());
}

inline QList<QPointer<QToolBar>>& registry() {
  static QList<QPointer<QToolBar>> bars;
  return bars;
}

// Re-applies the preference to one toolbar and to every tagged button inside it.
inline void refresh(QToolBar* bar) {
  if (!bar) return;
  bar->setIconSize(iconSize());
  bar->setToolButtonStyle(buttonStyle());
  const auto buttons = bar->findChildren<QToolButton*>();
  for (QToolButton* button : buttons)
    if (button->property(kButtonProperty).toBool()) styleButton(button);
  bar->updateGeometry();
}

// Gives `bar` the shared look and registers it for live updates. Call once, right after creating it.
inline void apply(QToolBar* bar) {
  if (!bar) return;
  bar->setObjectName(QStringLiteral("textCompareToolbar"));  // the one toolbar rule in qt_style.h
  bar->setMovable(false);
  bar->setFloatable(false);
  registry().removeAll(QPointer<QToolBar>());  // drop dead entries
  registry().append(bar);
  refresh(bar);
}

inline void refreshAll() {
  registry().removeAll(QPointer<QToolBar>());
  const auto bars = registry();
  for (const QPointer<QToolBar>& bar : bars) refresh(bar.data());
}

}  // namespace openbc::ui::toolbar

// theme.h
// ---------------------------------------------------------------------------
// SINGLE SOURCE OF TRUTH for every colour and font size the application draws with, in BOTH themes.
// Read `theme.md` (crate root) before touching this file or adding any colour anywhere in the UI.
//
// Rules in one paragraph: no widget, stylesheet or paintEvent writes a hex colour or `QColor(0x..)`
// literal for something that must be readable. It asks this file instead (`theme::get(Token::x)`,
// or the `@x@` placeholders in the style sheet template in style/qt_style.h). A token has one value
// per theme, so a colour can never be "right in dark, wrong in light" again.
//
// Header-only, no Q_OBJECT, no moc, no resources (same constraints as style/qt_style.h).
// ---------------------------------------------------------------------------
#pragma once

#include <QApplication>
#include <cstddef>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QString>
#include <QStringList>

namespace openbc::ui {

enum class Theme { Dark, Light };

namespace theme {

// X(name, dark, light). Colours are "#RRGGBB" or "#AARRGGBB" (alpha FIRST, Qt convention).
// One token per line: tools/theme_test and the contrast table in theme.md parse this list.
//
// Light-theme design rules (see theme.md): surfaces are stepped greys, never pure white except
// text inputs and editors; text on any surface it is drawn on is >= 4.5:1 (WCAG AA); borders are
// visible (>= 1.5:1 against the surface they separate).
#define OPENBC_THEME_TOKENS(X) \
  /* ---- surfaces */ \
  X(window,            "#2d2d2d", "#e8ebf0") \
  X(chrome,            "#3e3e3e", "#dde1e8") \
  X(input,             "#232323", "#ffffff") \
  X(header,            "#363636", "#d3d8e1") \
  X(pane,              "#262833", "#f7f8fb") \
  X(editor,            "#1f2028", "#ffffff") \
  X(well,              "#1b1b1b", "#d9dde5") \
  X(side,              "#333333", "#e1e4eb") \
  X(tab,               "#333333", "#cfd4de") \
  X(tabHover,          "#3a3a3a", "#d9dde6") \
  X(raised,            "#3a3a3a", "#d0d5df") \
  X(menu,              "#2d2d2d", "#fbfcfd") \
  X(tooltipBg,         "#232323", "#fdfdf2") \
  X(ruleBar,           "#2b2b2b", "#e3e6ec") \
  X(ruleInput,         "#1e1e1e", "#ffffff") \
  /* ---- interaction states */ \
  X(select,            "#44475a", "#c9dcf7") \
  X(hover,             "#4d4d4d", "#cdd3de") \
  X(pressed,           "#232323", "#b8c1d1") \
  X(checked,           "#232323", "#c2cfe6") \
  X(checkedBorder,     "#232323", "#8ea5cf") \
  X(accent,            "#6272a4", "#2f6bc0") \
  X(button,            "#454545", "#f1f3f7") \
  X(buttonHover,       "#505050", "#e3e8f1") \
  X(buttonBorder,      "#5a5a5a", "#a7afbe") \
  X(tabCloseHover,     "#55585f", "#bfc6d3") \
  /* ---- borders */ \
  X(border,            "#505050", "#aab2c0") \
  X(borderSoft,        "#4a4a4a", "#bcc3cf") \
  X(divider,           "#5a5a5a", "#b3bac7") \
  X(borderHover,       "#5f5f5f", "#8f9aae") \
  X(selectionBorder,   "#5f637f", "#7f9cd0") \
  /* ---- text */ \
  X(text,              "#f8f8f2", "#1a1f2b") \
  X(textToolbar,       "#d8dae6", "#2a3347") \
  X(textTab,           "#b9b9b9", "#4a5468") \
  X(textTabHover,      "#e0e0e0", "#1f2838") \
  X(textStrong,        "#ffffff", "#0f1420") \
  X(textDim,           "#8b90a6", "#596175") \
  X(textDisabled,      "#777b8f", "#788196") \
  X(textBar,           "#c7cbe0", "#3a4357") \
  X(placeholder,       "#7b809a", "#6b7388") \
  X(link,              "#8be9fd", "#1666c5") \
  X(ink,               "#dde0ee", "#3a4256") \
  /* ---- scroll bars */ \
  X(scrollHandle,      "#5f6479", "#a6aec0") \
  X(scrollHandleHover, "#7880a0", "#8993ab") \
  X(scrollHandlePress, "#8b93bd", "#6f7b99") \
  /* ---- home tab */ \
  X(homeBg,            "#3f3f3f", "#eef0f4") \
  X(homeSelBg,         "#3b4b66", "#c6d9f7") \
  X(homeSelBorder,     "#d8dae6", "#2f6bc0") \
  X(homeBtn,           "#353535", "#f4f5f8") \
  X(homeBtnHover,      "#444444", "#e6eaf1") \
  X(homeBtnBorder,     "#4a4a4a", "#aab2c0") \
  X(homeBar,           "#3f3f3f", "#e6e9ef") \
  X(homeBarBorder,     "#555555", "#aab2c0") \
  X(homeSearch,        "#2a2a2a", "#ffffff") \
  X(homeFooter,        "#262626", "#d8dce5") \
  X(homeFooterBorder,  "#1d1d1d", "#b3bac7") \
  X(homeFooterInput,   "#1b1b1b", "#ffffff") \
  X(homeText,          "#f0f0f0", "#1a1f2b") \
  X(homeHint,          "#e6e6e6", "#3a4357") \
  X(homeFooterText,    "#d8d8d8", "#3a4357") \
  X(homeOpenBg,        "#1b3452", "#d3e3fa") \
  X(homeOpenBorder,    "#2f6db5", "#2f6bc0") \
  X(homeOpenHover,     "#234268", "#c2d8f6") \
  X(homeTileHover,     "#16ffffff", "#14000000") \
  X(homeTilePress,     "#3c000000", "#22000000") \
  /* ---- folder gutter (centre strip between the panes) */ \
  X(gutterEdge,        "#2a2a2a", "#d3d8e1") \
  X(gutterMid,         "#444444", "#f3f5f9") \
  /* ---- folder compare statuses (text colours on `pane`) */ \
  X(same,              "#f1fa8c", "#6b5600") \
  X(newer,             "#50fa7b", "#0a6a2b") \
  X(older,             "#fab68d", "#964509") \
  X(different,         "#ff5555", "#b01218") \
  X(orphan,            "#bd93f9", "#6a3dbf") \
  X(equalMarker,       "#9aa0b8", "#5c6479") \
  X(folderInfo,        "#6272a4", "#566594") \
  X(treeLine,          "#767a92", "#7d869c") \
  X(treeLineLeaf,      "#d0d3e4", "#6b7388") \
  /* ---- folder icons (fills, not text) */ \
  X(folderYellow,      "#ffe98f", "#e3b341") \
  X(folderRed,         "#ff8f8f", "#e5484d") \
  X(folderGreen,       "#6ef08f", "#2fa84f") \
  X(folderOrange,      "#ffb27a", "#e8801f") \
  X(folderPurple,      "#b787ff", "#8a5ad9") \
  X(folderBlue,        "#6db7ff", "#2f7fd8") \
  /* ---- semantic message colours */ \
  X(error,             "#e5534b", "#b71c1c") \
  X(warning,           "#e5a84b", "#8a5600") \
  X(info,              "#4e9cff", "#1a56b0") \
  X(gold,              "#f2c04a", "#9a6400") \
  /* ---- diff grids / status chips painted by hand */ \
  X(changedRow,        "#5a7a2d2d", "#4de06666") \
  X(changedCell,       "#aab03a3a", "#99d9534f") \
  X(onlyRow,           "#5a8a7a2d", "#59e0b84a") \
  X(gridCell,          "#33333d", "#d3d8e1") \
  X(gridCellCurrent,   "#3a5a8a", "#9fc0f0") \
  X(gridArrow,         "#e8eaf4", "#3a4256") \
  X(gridArrowOff,      "#707078", "#9aa2b3") \
  X(previewChanged,    "#7a2d2d", "#f6c9c9") \
  X(previewWhitespace, "#2d5a87", "#c7defa") \
  X(minimapBg,         "#232323", "#e9ecf2") \
  X(minimapView,       "#3cf8f8f2", "#331a1f2b") \
  X(navBorder,         "#3a3a3a", "#b3bac7") \
  X(navHot,            "#44475a", "#c9dcf7") \
  X(navArrow,          "#d8d8d8", "#3a4256") \
  X(navArrowOff,       "#707070", "#9aa2b3")

enum class Token : int {
#define X(name, dark, light) name,
  OPENBC_THEME_TOKENS(X)
#undef X
      Count
};

inline constexpr int kTokenCount = static_cast<int>(Token::Count);

struct Row {
  const char* name;
  const char* dark;
  const char* light;
};

inline constexpr Row kRows[] = {
#define X(name, dark, light) {#name, dark, light},
    OPENBC_THEME_TOKENS(X)
#undef X
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<std::size_t>(kTokenCount),
              "token table and enum out of step");

// ---------------------------------------------------------------------------
// Current theme (set ONLY by options/appearance_controller.h)
// ---------------------------------------------------------------------------

inline Theme& currentRef() {
  static Theme t = Theme::Dark;
  return t;
}
inline Theme current() { return currentRef(); }
inline bool isDark() { return currentRef() == Theme::Dark; }
inline void setCurrent(Theme t) { currentRef() = t; }

inline QColor colorFor(Token token, Theme which) {
  static QColor cache[2][kTokenCount];
  static bool filled = false;
  if (!filled) {
    for (int i = 0; i < kTokenCount; ++i) {
      cache[0][i] = QColor(QString::fromLatin1(kRows[i].dark));
      cache[1][i] = QColor(QString::fromLatin1(kRows[i].light));
    }
    filled = true;
  }
  return cache[which == Theme::Dark ? 0 : 1][static_cast<int>(token)];
}

// The colour of `token` in the theme that is active right now. Cheap enough for paintEvent.
inline QColor get(Token token) { return colorFor(token, current()); }

// "color: #rrggbb;" for a one-off setStyleSheet() on a label. Evaluated NOW, so it does not follow a
// later theme switch: for long-lived widgets add a rule to the template in style/qt_style.h instead.
inline QString textStyle(Token token) {
  return QStringLiteral("color: %1;").arg(get(token).name(QColor::HexRgb));
}

// CSS form for style sheets: "#rrggbb", or "rgba(r, g, b, a)" when the token has alpha.
inline QString css(const QColor& c) {
  if (c.alpha() == 255) return c.name(QColor::HexRgb);
  return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// ---------------------------------------------------------------------------
// Fonts. One UI family (the platform's own), sizes in POINTS relative to the base size, never px,
// so both themes and every DPI scale the same way. Monospace comes from the system fixed font.
// ---------------------------------------------------------------------------

inline double& basePointSizeRef() {
  static double v = 0.0;
  return v;
}

inline double basePointSize() {
  const double v = basePointSizeRef();
  if (v > 0.0) return v;
  const double app = QApplication::font().pointSizeF();
  return app > 0.0 ? app : 10.0;
}

inline QString monoFamily() {
  return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

// Call once, before the first window is shown. Same font in both themes. Never smaller than 9 pt:
// the system font on some Linux desktops is 8 pt, which is what made the light theme look tiny.
inline void applyApplicationFont(QApplication& app) {
  QFont f = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  if (f.pointSizeF() < 9.0) f.setPointSizeF(9.0);
  f.setStyleStrategy(static_cast<QFont::StyleStrategy>(QFont::PreferAntialias | QFont::PreferQuality));
  f.setHintingPreference(QFont::PreferDefaultHinting);
  basePointSizeRef() = f.pointSizeF();
  app.setFont(f);
}

inline QString pt(double scale) {
  return QStringLiteral("%1pt").arg(basePointSize() * scale, 0, 'f', 1);
}

// ---------------------------------------------------------------------------
// Style sheet template expansion: "@name@" -> colour of that token in `theme`;
// "@fsSmall@" etc. -> font sizes; "@mono@" -> quoted system fixed-width family + generic fallback.
// ---------------------------------------------------------------------------

inline QString expand(QString sheet, Theme which) {
  for (int i = 0; i < kTokenCount; ++i) {
    const QString key = QLatin1Char('@') + QString::fromLatin1(kRows[i].name) + QLatin1Char('@');
    if (sheet.contains(key)) sheet.replace(key, css(colorFor(static_cast<Token>(i), which)));
  }
  sheet.replace(QStringLiteral("@fsSmall@"), pt(0.90));
  sheet.replace(QStringLiteral("@fsBase@"), pt(1.00));
  sheet.replace(QStringLiteral("@fsLarge@"), pt(1.20));
  sheet.replace(QStringLiteral("@fsTitle@"), pt(1.45));
  sheet.replace(QStringLiteral("@mono@"),
                QStringLiteral("\"%1\", monospace").arg(monoFamily()));
  return sheet;
}

// Full QPalette for a theme. Built with QPalette(button, window) so Light/Midlight/Mid/Dark/Shadow
// are derived consistently (Fusion draws frames, group boxes and `palette(mid)` from those roles).
inline QPalette paletteFor(Theme which) {
  const auto c = [which](Token t) { return colorFor(t, which); };
  QPalette p(c(Token::chrome), c(Token::window));
  p.setColor(QPalette::Window, c(Token::window));
  p.setColor(QPalette::WindowText, c(Token::text));
  p.setColor(QPalette::Base, c(Token::input));
  p.setColor(QPalette::AlternateBase, c(Token::pane));
  p.setColor(QPalette::ToolTipBase, c(Token::tooltipBg));
  p.setColor(QPalette::ToolTipText, c(Token::text));
  p.setColor(QPalette::Text, c(Token::text));
  p.setColor(QPalette::Button, c(Token::chrome));
  p.setColor(QPalette::ButtonText, c(Token::text));
  p.setColor(QPalette::BrightText, c(Token::textStrong));
  p.setColor(QPalette::Highlight, c(Token::select));
  p.setColor(QPalette::HighlightedText, which == Theme::Dark ? c(Token::text) : c(Token::textStrong));
  p.setColor(QPalette::Link, c(Token::link));
  p.setColor(QPalette::LinkVisited, c(Token::link));
  p.setColor(QPalette::PlaceholderText, c(Token::placeholder));
  p.setColor(QPalette::Mid, c(Token::border));
  p.setColor(QPalette::Dark, c(Token::divider));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  p.setColor(QPalette::Accent, c(Token::accent));
#endif
  for (QPalette::ColorRole role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
    p.setColor(QPalette::Disabled, role, c(Token::textDisabled));
  p.setColor(QPalette::Disabled, QPalette::HighlightedText, c(Token::textDisabled));
  return p;
}

inline QStringList tokenNames() {
  QStringList out;
  for (const Row& r : kRows) out << QString::fromLatin1(r.name);
  return out;
}

}  // namespace theme
}  // namespace openbc::ui

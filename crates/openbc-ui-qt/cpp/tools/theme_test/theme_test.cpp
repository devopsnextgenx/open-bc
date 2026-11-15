// theme_test: guards the theme rules in theme.md.
//   1. every token has a value in both themes and the two themes differ for surfaces/text
//   2. text tokens meet WCAG AA (4.5:1) on the light surfaces they are drawn on
//   3. the style sheet template contains no hex literal and expands without a leftover "@token@"
//   4. both palettes build
// Build + run: cpp/tools/theme_test/build_and_run.sh   (exit code 0 = pass)
#include <QApplication>
#include <QRegularExpression>
#include <cmath>
#include <cstdio>

#include "style/qt_style.h"

using namespace openbc::ui;
using theme::Token;

static double lum(const QColor& c) {
  auto f = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  return 0.2126 * f(c.redF()) + 0.7152 * f(c.greenF()) + 0.0722 * f(c.blueF());
}
static double contrast(const QColor& a, const QColor& b) {
  double la = lum(a), lb = lum(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  int failures = 0;
  const auto fail = [&](const char* what) { std::printf("FAIL: %s\n", what); ++failures; };

  for (int i = 0; i < theme::kTokenCount; ++i) {
    const auto t = static_cast<Token>(i);
    if (!theme::colorFor(t, Theme::Dark).isValid() || !theme::colorFor(t, Theme::Light).isValid())
      fail(theme::kRows[i].name);
  }

  struct Pair { Token fg, bg; };
  const Pair pairs[] = {
      {Token::text, Token::window},   {Token::text, Token::chrome},  {Token::text, Token::input},
      {Token::text, Token::pane},     {Token::text, Token::editor},  {Token::text, Token::header},
      {Token::text, Token::select},   {Token::text, Token::menu},    {Token::text, Token::button},
      {Token::textToolbar, Token::chrome}, {Token::textTab, Token::tab}, {Token::textDim, Token::pane},
      {Token::textDim, Token::window}, {Token::textDim, Token::chrome}, {Token::placeholder, Token::input},
      {Token::ink, Token::chrome},    {Token::ink, Token::menu},     {Token::homeText, Token::homeBg},
      {Token::same, Token::pane},     {Token::newer, Token::pane},   {Token::older, Token::pane},
      {Token::different, Token::pane}, {Token::orphan, Token::pane}, {Token::folderInfo, Token::pane},
      {Token::same, Token::select},   {Token::different, Token::select}, {Token::error, Token::chrome},
      {Token::warning, Token::window}, {Token::info, Token::chrome}, {Token::text, Token::tooltipBg},
  };
  for (const Pair& p : pairs) {
    const double r = contrast(theme::colorFor(p.fg, Theme::Light), theme::colorFor(p.bg, Theme::Light));
    if (r < 4.5) {
      std::printf("FAIL: light contrast %s on %s = %.2f\n", theme::kRows[int(p.fg)].name,
                  theme::kRows[int(p.bg)].name, r);
      ++failures;
    }
  }

  const QString tmpl = applicationStyleSheetTemplate();
  if (QRegularExpression(QStringLiteral("#[0-9a-fA-F]{3,8}\\b")).match(tmpl).hasMatch())
    fail("hex literal in style sheet template (use an @token@)");
  for (Theme th : {Theme::Dark, Theme::Light}) {
    if (applicationStyleSheet(th).contains(QLatin1Char('@'))) fail("unexpanded @placeholder@");
    (void)theme::paletteFor(th);
  }
  if (theme::colorFor(Token::window, Theme::Dark) == theme::colorFor(Token::window, Theme::Light))
    fail("window token identical in both themes");

  std::printf(failures ? "%d failure(s)\n" : "theme_test OK\n", failures);
  return failures ? 1 : 0;
}

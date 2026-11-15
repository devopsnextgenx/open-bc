# theme.md - colours, fonts and toolbar look (Dark and Light)

Read this before adding **any** colour, font size or toolbar to the Qt front-end. Rules for humans and AI assistants.

## 1. The one rule

Every colour the UI draws comes from **one table**: `cpp/style/theme.h` (`OPENBC_THEME_TOKENS`). Each row is
`name, dark value, light value`. Nothing else in the code base may contain a hex literal or `QColor(0x..)` for a
colour that has to be readable on a theme surface.

| You are writing | Do this |
|---|---|
| a rule in the app style sheet | use `@token@` in the template in `style/qt_style.h` (`applicationStyleSheetTemplate()`) |
| a `paintEvent` / delegate | `theme::get(theme::Token::x)` (cheap; re-read on every paint, so a theme switch just works) |
| an icon (`QPainter` in `icons::make`) | evaluate colours **inside** the paint lambda (`color::ink()`, `theme::get(...)`), never capture them |
| a status colour for folders | `color::same()/newer()/older()/different()/orphan()` (token + user override from Options) |
| a one-off message label | `setStyleSheet(theme::textStyle(Token::error))` (evaluated now) or, for long-lived widgets, give it an `objectName` and add a rule to the template |
| a new colour | add a row to `OPENBC_THEME_TOKENS` with BOTH values, check contrast (section 4), use it |

`QPalette` is built from the same table (`theme::paletteFor`), so widgets that only use palette roles are
automatically right in both themes. Prefer `palette(...)` / palette roles to tokens when a role fits.

## 2. How a theme switch works

`AppearanceController::applyTheme` (options/appearance_controller.h) is the only place that switches themes:

1. `theme::setCurrent(Dark|Light)` - every `theme::get()` now returns the new value
2. `++appearance::state().revision` - icon caches (folder / marker icons) rebuild
3. `setPalette(paletteFor(...))` and `setStyleSheet(applicationStyleSheet(...))` - the template is expanded again
4. all widgets `update()` - custom painting and icon lambdas re-read the tokens

Consequences: never cache a `QColor` taken from a token in a member, a `static`, or a lambda capture. Widgets
that build coloured `QIcon`s up front (e.g. the folder toolbar dash icons) rebuild them in
`changeEvent(QEvent::PaletteChange)`.

User colour choices (Options > Appearance > Folder Views / File Views) are stored as **overrides only**
(`AppOptions::colors`, an invalid `QColor` field = "follow the theme"). They used to store all three fields, which
froze the *dark* defaults into the light theme; `OptionsStore::load()` migrates old files by dropping fields that equal
the dark default.

## 3. Fonts

* One UI font in both themes: the platform's general font, never below 9 pt (`theme::applyApplicationFont`, called once
  in `main_window.h` before the first window). Hinting/antialiasing are the Qt defaults for that font.
* Sizes in the style sheet are **points relative to the base**, never `px`: `@fsSmall@` (0.90x: toolbar captions,
  attribute labels), `@fsBase@` (1.0x), `@fsLarge@` (1.20x), `@fsTitle@` (1.45x). The same factors apply in both themes.
* Monospace: `@mono@` = the system fixed font (`QFontDatabase::FixedFont`) + `monospace`. Do not name `DejaVu Sans Mono`
  (absent on Windows). Text Compare editor font stays an Options choice (`options/text_font.h`).
* No `font-weight` tricks to "fix" a theme; if light text looks thin, raise the text token contrast instead.

## 4. Contrast rules (checked by `cpp/tools/theme_test`)

* Text on the surface it is drawn on: **>= 4.5:1** in the Light theme (WCAG AA). Icons, tree lines and focus rings: >= 3:1.
* Light surfaces are stepped greys (`window` < `chrome` < `pane`, `input`/`editor` white) so areas are distinguishable;
  borders are >= 1.5:1 against the surface they divide. Pure white is only for text inputs and editors.
* Status colours (same/newer/older/different/orphan) have separate dark-on-light values; the pastel dark values are
  unreadable on light surfaces, so they are never reused.
* Dark values are the original look and were not changed. Dark pairs below 4.5:1 (examples; kept on purpose for
  compatibility): `textDim` on `chrome`/`window`, `placeholder` on `input`, `folderInfo` on `pane`, `error` on `chrome`.
* `textDisabled` is exempt (disabled controls).

Run: `cpp/tools/theme_test/build_and_run.sh` (needs Qt6Widgets; uses the offscreen platform).

## 5. Toolbars (all compare viewers)

`cpp/style/toolbar_style.h` is the single toolbar look. A viewer does:

```cpp
toolbar_ = new QToolBar(this);
openbc::ui::toolbar::apply(toolbar_);          // objectName "textCompareToolbar", not movable, live updates
auto* b = new QToolButton(toolbar_);           // custom button on the toolbar (menu buttons etc.)
openbc::ui::toolbar::styleButton(b);
```

and **never** calls `setIconSize`, `setToolButtonStyle` or `setObjectName("textCompareToolbar")` itself. The folder
compare viewer uses the same helper now (it used to be icon-only with its own look), including its split buttons
(Diffs, Copy).

User preference: **Tools > Options > Appearance > Toolbars** - "Show labels under toolbar icons" and "Icon size"
(Small 16 px / Large 20 px). Stored as `appearance/toolbarLabels` and `appearance/toolbarIconSize` in
`preferences.ini`; applied live by `AppearanceController` -> `toolbar::refreshAll()`. Defaults (labels on, large)
equal the previous look of the text/hex/table/... viewers. Small screens: labels off + small icons.

The Diffs / Copy group texts and tooltips are localised (`folder.filter_*`, `folder.diff_show_*`, `folder.copy*`,
`folder.move_*`; toolbar options `options.toolbar_*`) in `messages.json`, `messages_fr.json`, `messages_cn.json`
and the built-in table `cpp/i18n/messages_defaults.h`.

## 6. Checklist for a new widget / viewer

1. No hex literal; tokens or palette roles only.
2. Open it in Dark **and** Light (Options > Appearance, Preview) and flip the theme while it is open.
3. Toolbar via `toolbar::apply`; texts via `i18n::t`.
4. New token -> both values + `theme_test` passes.

## 7. Token reference

**Surfaces**

| Token | Dark | Light |
|---|---|---|
| `window` | `#2d2d2d` | `#e8ebf0` |
| `chrome` | `#3e3e3e` | `#dde1e8` |
| `input` | `#232323` | `#ffffff` |
| `header` | `#363636` | `#d3d8e1` |
| `pane` | `#262833` | `#f7f8fb` |
| `editor` | `#1f2028` | `#ffffff` |
| `well` | `#1b1b1b` | `#d9dde5` |
| `side` | `#333333` | `#e1e4eb` |
| `tab` | `#333333` | `#cfd4de` |
| `tabHover` | `#3a3a3a` | `#d9dde6` |
| `raised` | `#3a3a3a` | `#d0d5df` |
| `menu` | `#2d2d2d` | `#fbfcfd` |
| `tooltipBg` | `#232323` | `#fdfdf2` |
| `ruleBar` | `#2b2b2b` | `#e3e6ec` |
| `ruleInput` | `#1e1e1e` | `#ffffff` |

**Interaction states**

| Token | Dark | Light |
|---|---|---|
| `select` | `#44475a` | `#c9dcf7` |
| `hover` | `#4d4d4d` | `#cdd3de` |
| `pressed` | `#232323` | `#b8c1d1` |
| `checked` | `#232323` | `#c2cfe6` |
| `checkedBorder` | `#232323` | `#8ea5cf` |
| `accent` | `#6272a4` | `#2f6bc0` |
| `button` | `#454545` | `#f1f3f7` |
| `buttonHover` | `#505050` | `#e3e8f1` |
| `buttonBorder` | `#5a5a5a` | `#a7afbe` |
| `tabCloseHover` | `#55585f` | `#bfc6d3` |

**Borders**

| Token | Dark | Light |
|---|---|---|
| `border` | `#505050` | `#aab2c0` |
| `borderSoft` | `#4a4a4a` | `#bcc3cf` |
| `divider` | `#5a5a5a` | `#b3bac7` |
| `borderHover` | `#5f5f5f` | `#8f9aae` |
| `selectionBorder` | `#5f637f` | `#7f9cd0` |

**Text**

| Token | Dark | Light |
|---|---|---|
| `text` | `#f8f8f2` | `#1a1f2b` |
| `textToolbar` | `#d8dae6` | `#2a3347` |
| `textTab` | `#b9b9b9` | `#4a5468` |
| `textTabHover` | `#e0e0e0` | `#1f2838` |
| `textStrong` | `#ffffff` | `#0f1420` |
| `textDim` | `#8b90a6` | `#596175` |
| `textDisabled` | `#777b8f` | `#788196` |
| `textBar` | `#c7cbe0` | `#3a4357` |
| `placeholder` | `#7b809a` | `#6b7388` |
| `link` | `#8be9fd` | `#1666c5` |
| `ink` | `#dde0ee` | `#3a4256` |

**Scroll bars**

| Token | Dark | Light |
|---|---|---|
| `scrollHandle` | `#5f6479` | `#a6aec0` |
| `scrollHandleHover` | `#7880a0` | `#8993ab` |
| `scrollHandlePress` | `#8b93bd` | `#6f7b99` |

**Home tab**

| Token | Dark | Light |
|---|---|---|
| `homeBg` | `#3f3f3f` | `#eef0f4` |
| `homeSelBg` | `#3b4b66` | `#c6d9f7` |
| `homeSelBorder` | `#d8dae6` | `#2f6bc0` |
| `homeBtn` | `#353535` | `#f4f5f8` |
| `homeBtnHover` | `#444444` | `#e6eaf1` |
| `homeBtnBorder` | `#4a4a4a` | `#aab2c0` |
| `homeBar` | `#3f3f3f` | `#e6e9ef` |
| `homeBarBorder` | `#555555` | `#aab2c0` |
| `homeSearch` | `#2a2a2a` | `#ffffff` |
| `homeFooter` | `#262626` | `#d8dce5` |
| `homeFooterBorder` | `#1d1d1d` | `#b3bac7` |
| `homeFooterInput` | `#1b1b1b` | `#ffffff` |
| `homeText` | `#f0f0f0` | `#1a1f2b` |
| `homeHint` | `#e6e6e6` | `#3a4357` |
| `homeFooterText` | `#d8d8d8` | `#3a4357` |
| `homeOpenBg` | `#1b3452` | `#d3e3fa` |
| `homeOpenBorder` | `#2f6db5` | `#2f6bc0` |
| `homeOpenHover` | `#234268` | `#c2d8f6` |
| `homeTileHover` | `#16ffffff` | `#14000000` |
| `homeTilePress` | `#3c000000` | `#22000000` |

**Folder gutter (centre strip between the panes)**

| Token | Dark | Light |
|---|---|---|
| `gutterEdge` | `#2a2a2a` | `#d3d8e1` |
| `gutterMid` | `#444444` | `#f3f5f9` |

**Folder compare statuses (text colours on `pane`)**

| Token | Dark | Light |
|---|---|---|
| `same` | `#f1fa8c` | `#6b5600` |
| `newer` | `#50fa7b` | `#0a6a2b` |
| `older` | `#fab68d` | `#964509` |
| `different` | `#ff5555` | `#b01218` |
| `orphan` | `#bd93f9` | `#6a3dbf` |
| `equalMarker` | `#9aa0b8` | `#5c6479` |
| `folderInfo` | `#6272a4` | `#566594` |
| `treeLine` | `#767a92` | `#7d869c` |
| `treeLineLeaf` | `#d0d3e4` | `#6b7388` |

**Folder icons (fills, not text)**

| Token | Dark | Light |
|---|---|---|
| `folderYellow` | `#ffe98f` | `#e3b341` |
| `folderRed` | `#ff8f8f` | `#e5484d` |
| `folderGreen` | `#6ef08f` | `#2fa84f` |
| `folderOrange` | `#ffb27a` | `#e8801f` |
| `folderPurple` | `#b787ff` | `#8a5ad9` |
| `folderBlue` | `#6db7ff` | `#2f7fd8` |

**Semantic message colours**

| Token | Dark | Light |
|---|---|---|
| `error` | `#e5534b` | `#b71c1c` |
| `warning` | `#e5a84b` | `#8a5600` |
| `info` | `#4e9cff` | `#1a56b0` |
| `gold` | `#f2c04a` | `#9a6400` |

**Diff grids / status chips painted by hand**

| Token | Dark | Light |
|---|---|---|
| `changedRow` | `#5a7a2d2d` | `#4de06666` |
| `changedCell` | `#aab03a3a` | `#99d9534f` |
| `onlyRow` | `#5a8a7a2d` | `#59e0b84a` |
| `gridCell` | `#33333d` | `#d3d8e1` |
| `gridCellCurrent` | `#3a5a8a` | `#9fc0f0` |
| `gridArrow` | `#e8eaf4` | `#3a4256` |
| `gridArrowOff` | `#707078` | `#9aa2b3` |
| `previewChanged` | `#7a2d2d` | `#f6c9c9` |
| `previewWhitespace` | `#2d5a87` | `#c7defa` |
| `minimapBg` | `#232323` | `#e9ecf2` |
| `minimapView` | `#3cf8f8f2` | `#331a1f2b` |
| `navBorder` | `#3a3a3a` | `#b3bac7` |
| `navHot` | `#44475a` | `#c9dcf7` |
| `navArrow` | `#d8d8d8` | `#3a4256` |
| `navArrowOff` | `#707070` | `#9aa2b3` |

Alpha values use Qt's `#AARRGGBB` order in the table; style sheets get `rgba(r, g, b, a)` automatically.

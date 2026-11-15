#pragma once

// Look & feel for the OpenBC Qt front-end (dark, Compare-like).
//
// Everything here is header-only and free of Q_OBJECT so it works with any
// build setup (no moc step, no .qrc resources): icons are drawn with QPainter
// and the tree's dotted connector lines are painted by CompareTree.

#include <QAbstractItemView>
#include <QColor>
#include <QFontMetrics>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QIconEngine>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QList>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QScrollBar>
#include <QString>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include "options/appearance_state.h"
#include "style/theme.h"
#include "style/toolbar_style.h"
#include <functional>
#include <vector>
#include "i18n/i18n.h"

namespace openbc::ui {

// ---------------------------------------------------------------------------
// Comparison vocabulary shared by the UI and the comparison engine
// ---------------------------------------------------------------------------

// Status of ONE side of a row (the two sides of a pair can differ, e.g. the
// left file is Newer while the right one is Older).
enum class RowStatus { Pending, Equal, Newer, Older, Different, Orphan };

// Status of the PAIR, used by the "show/hide" toggles in the toolbar.
enum class PairClass {
  Pending = -1,
  Same = 0,
  OrphanLeft,
  OrphanRight,
  LeftNewer,
  RightNewer,
  Different,
};
constexpr int kPairClassCount = 6;

constexpr int kPathRole = Qt::UserRole;
constexpr int kStatusRole = Qt::UserRole + 1;
constexpr int kClassRole = Qt::UserRole + 2;
constexpr int kIsDirRole = Qt::UserRole + 3;
constexpr int kChildrenLoadedRole = Qt::UserRole + 4;
constexpr int kCompareRevisionRole = Qt::UserRole + 5;
constexpr int kFolderStatusMaskRole = Qt::UserRole + 6;

inline constexpr quint32 statusBit(RowStatus status) {
  return 1u << static_cast<int>(status);
}

// ---------------------------------------------------------------------------
// Palette (sampled from the reference screenshot)
// ---------------------------------------------------------------------------

namespace color {
// Every value below comes from style/theme.h (one entry per theme). Do not put literals back here.
inline QColor tok(theme::Token t) { return theme::get(t); }

inline QColor windowBg() { return tok(theme::Token::window); }
inline QColor chromeBg() { return tok(theme::Token::chrome); }
inline QColor inputBg() { return tok(theme::Token::input); }
inline QColor headerBg() { return tok(theme::Token::header); }
inline QColor border() { return tok(theme::Token::border); }
inline QColor rowA() { return appearance::background("Folder Views/Default text", tok(theme::Token::pane)); }
inline QColor rowB() {
  if (!appearance::state().stripes) return rowA();
  const QColor base = appearance::background("Folder Views/Default text", QColor());
  if (base.isValid()) return base.lightness() < 128 ? base.lighter(112) : base.darker(106);
  return theme::isDark() ? QColor(0x2d, 0x2f, 0x3e) : QColor(0xed, 0xf0, 0xf6);
}
inline QColor selection() { return appearance::background("Folder Views/Selection", tok(theme::Token::select)); }
inline QColor selectionBorder() { return tok(theme::Token::selectionBorder); }
inline QColor text() { return tok(theme::Token::text); }
inline QColor dimText() { return tok(theme::Token::folderInfo); }
// Theme-aware colour of icon strokes / glyphs drawn on toolbar and menu surfaces.
inline QColor ink() { return tok(theme::Token::ink); }
// Name column text of folder rows and the dimmed size/date columns of folder rows.
inline QColor folderText() { return appearance::text("Folder Views/Default text", tok(theme::Token::text)); }
inline QColor folderInfo() { return appearance::info("Folder Views/Default text", dimText()); }
inline QColor treeLine() { return tok(theme::Token::treeLine); }

inline QColor same() { return appearance::text("Folder Views/Same", tok(theme::Token::same)); }
inline QColor newer() { return appearance::text("Folder Views/Newer", tok(theme::Token::newer)); }
inline QColor older() { return appearance::text("Folder Views/Older", tok(theme::Token::older)); }
inline QColor different() { return appearance::text("Folder Views/Different", tok(theme::Token::different)); }
inline QColor orphan() { return appearance::text("Folder Views/Orphan", tok(theme::Token::orphan)); }
inline QColor equalMarker() { return appearance::text("Folder Views/Unknown", tok(theme::Token::equalMarker)); }

inline QColor folderYellow() { return tok(theme::Token::folderYellow); }
inline QColor folderRed() { return tok(theme::Token::folderRed); }
inline QColor folderGreen() { return tok(theme::Token::folderGreen); }
inline QColor folderOrange() { return tok(theme::Token::folderOrange); }
inline QColor folderPurple() { return tok(theme::Token::folderPurple); }
inline QColor folderBlue() { return tok(theme::Token::folderBlue); }

// Colours for "group" nodes in the session-history tree (date buckets, the
// Personal node, and any other named node). These are deliberately a
// separate palette from the diff-status folder colours above, since group
// nodes never represent an actual folder-compare session.
inline QColor groupToday() { return QColor(0x5a, 0xb8, 0xf2); }
inline QColor groupYesterday() { return QColor(0x4c, 0xd6, 0xb0); }
inline QColor groupThisWeek() { return QColor(0xb0, 0x7a, 0xf2); }
inline QColor groupLastWeek() { return QColor(0xf2, 0xb4, 0x4a); }
inline QColor groupOlder() { return QColor(0x8a, 0x8f, 0xa8); }
inline QColor groupPersonal() { return QColor(0xf2, 0x7a, 0xb8); }
inline QColor groupDefault() { return QColor(0x8f, 0x9b, 0xb3); }

// Pool of bright, mutually-distinct colours handed out (and persisted) to
// new named nodes created under "Personal", so siblings stay easy to tell
// apart at a glance.
inline QList<QColor> groupNodePalette() {
  return {
      QColor(0x6c, 0xa0, 0xf5), QColor(0x4c, 0xd6, 0xc0),
      QColor(0xf2, 0x8a, 0x6a), QColor(0xe0, 0x7a, 0xd6),
      QColor(0xa8, 0xdc, 0x4a), QColor(0x5a, 0xd6, 0xe8),
      QColor(0xf2, 0xd0, 0x4a), QColor(0x9a, 0x8a, 0xf2),
      QColor(0xff, 0x8f, 0xc8), QColor(0x7d, 0xe8, 0x9a),
  };
}
} // namespace color

inline QColor statusTextColor(RowStatus status) {
  switch (status) {
  case RowStatus::Newer:
    return color::newer();
  case RowStatus::Older:
    return color::older();
  case RowStatus::Different:
    return color::different();
  case RowStatus::Orphan:
    return color::orphan();
  case RowStatus::Equal:
  case RowStatus::Pending:
  default:
    return color::folderText();
  }
}

// Both palettes are built from the same token table (style/theme.h), so they cannot drift apart.
inline QPalette darkPalette() { return theme::paletteFor(Theme::Dark); }
inline QPalette lightPalette() { return theme::paletteFor(Theme::Light); }

// ---------------------------------------------------------------------------
// Application style sheet
// ---------------------------------------------------------------------------

// The template. Colours are "@token@" placeholders resolved per theme by theme::expand() (style/theme.h);
// font sizes are "@fsSmall@" / "@fsBase@" / "@fsLarge@" / "@fsTitle@" (points relative to the base font).
// Add a rule here with tokens only: a hex literal in this template is a bug (theme_test fails on it).
// (Three literals: MSVC rejects a single string literal over ~16k characters.)
inline QString applicationStyleSheetTemplate() {
  return QStringLiteral(R"CSS(
        QMainWindow, QDialog { background: @window@; }
        QWidget { color: @text@; }
        QToolTip {
            background: @tooltipBg@; color: @text@;
            border: 1px solid @border@; padding: 3px;
        }

        /* ---- menus ---- */
        QMenuBar { background: @window@; color: @text@; padding: 1px 2px; }
        QMenuBar::item { background: transparent; padding: 4px 8px; }
        QMenuBar::item:selected { background: @select@; }
        QMenu { background: @menu@; color: @text@; border: 1px solid @border@; padding: 3px 0; }
        QMenu::item { background: transparent; padding: 5px 30px 5px 30px; }
        QMenu::item:selected { background: @select@; color: @text@; }
        QMenu::item:disabled { color: @textDisabled@; }
        QMenu::icon { padding-left: 6px; }
        QMenu::separator { height: 1px; background: @border@; margin: 4px 6px; }

        /* ---- session tabs ---- */
        QTabWidget::pane { border: 0; background: @chrome@; }
        QTabBar { background: @window@; }
        QTabBar::tab {
            background: @tab@; color: @textTab@;
            border: 1px solid @borderSoft@; border-bottom: none;
            border-top-left-radius: 5px; border-top-right-radius: 5px;
            padding: 6px 8px 6px 10px; margin: 6px 2px 0 0;
            min-width: 110px; max-width: 300px;
        }
        QTabBar::tab:hover:!selected { background: @tabHover@; color: @textTabHover@; }
        QTabBar::tab:selected {
            background: @chrome@; color: @textStrong@; border-color: @borderHover@;
        }
        QTabBar QToolButton { background: @tab@; border: 0; }
        QToolButton#tabClose {
            background: transparent; border: 0; border-radius: 3px; padding: 1px;
        }
        QToolButton#tabClose:hover { background: @tabCloseHover@; }
        QToolButton#newTab { background: transparent; border: 0; border-radius: 3px; margin: 6px 4px 0 4px; padding: 3px; }
        QToolButton#newTab:hover { background: @hover@; }

        /* ---- home tab ---- */
        QWidget#homeView { background: @homeBg@; }
        QSplitter#homeSplit::handle { background: @window@; width: 1px; }
        QWidget#homeSidebar { background: @side@; }
        QLabel#homeSessionsHeader { background: @raised@; color: @homeText@; padding: 6px 9px 5px 9px; }
        QTreeWidget#homeHistory { background: @side@; border: 0; outline: 0; }
        QTreeWidget#homeHistory::item { height: 18px; padding: 0 2px; }
        QTreeWidget#homeHistory::item:hover:!selected { background: @tabHover@; }
        QTreeWidget#homeHistory::item:selected { background: @homeSelBg@; color: @text@; border: 1px solid @homeSelBorder@; }
        QWidget#homeSidebarBar { background: @side@; }
        QToolButton#homeBarButton {
            background: @homeBar@; border: 1px solid @homeBarBorder@; border-radius: 0;
            min-width: 20px; max-width: 20px; min-height: 18px; max-height: 18px; padding: 0;
        }
        QToolButton#homeBarButton:hover { background: @hover@; }
        QLineEdit#homeSearch { background: @homeSearch@; border: 1px solid @homeBarBorder@; border-radius: 0; min-height: 16px; }
        QWidget#homeContent { background: @homeBg@; }
        QLabel#homeInfoName { color: @homeText@; font-size: @fsTitle@; }
        QLabel#homeInfoPath { color: @homeText@; font-size: @fsSmall@; }
        QPushButton#homeOpen, QPushButton#homeEdit {
            min-width: 82px; max-width: 82px; min-height: 22px; max-height: 22px;
            border-radius: 0; padding: 0; background: @homeBtn@; color: @text@; border: 1px solid @homeBtnBorder@;
        }
        QPushButton#homeOpen { background: @homeOpenBg@; border-color: @homeOpenBorder@; }
        QPushButton#homeOpen:hover { background: @homeOpenHover@; }
        QPushButton#homeEdit:hover { background: @homeBtnHover@; }
        QLabel#homeHint { color: @homeHint@; font-size: @fsBase@; }
        QToolButton#homeTile {
            background: transparent; border: 0; border-radius: 3px; color: @homeText@; padding: 4px;
        }
        QToolButton#homeTile:hover { background: @homeTileHover@; }
        QToolButton#homeTile:pressed { background: @homeTilePress@; }
        QFrame#homeFooter { background: @homeFooter@; border: 0; border-top: 1px solid @homeFooterBorder@; }
        QLabel#homeFooterTitle { color: @homeText@; font-size: @fsLarge@; font-weight: 700; background: transparent; }
        QLabel#homeFooterText { color: @homeFooterText@; font-size: @fsSmall@; background: transparent; }
        QLineEdit#homeFooterSearch { background: @homeFooterInput@; border: 1px solid @homeBarBorder@; border-radius: 0; }
        QToolButton#homeFooterIcon { background: transparent; border: 0; }

)CSS") +
      QStringLiteral(R"CSS(        /* ---- per-tab toolbar ---- */
        QToolBar {
            background: @chrome@; border: 0; border-top: 1px solid @border@;
            spacing: 3px; padding: 3px 5px;
        }
        QToolBar::separator { background: @divider@; width: 1px; margin: 4px 5px; }
        QToolBar QLabel { background: transparent; padding: 0 3px 0 6px; }
        QToolButton {
            background: transparent; border: 1px solid transparent;
            border-radius: 3px; padding: 2px;
        }
        QToolButton:hover { background: @hover@; border-color: @borderHover@; }
        QToolButton:pressed { background: @pressed@; }
        QToolButton:checked { background: @checked@; border-color: @checkedBorder@; }
        QToolButton:checked:hover { border-color: @borderHover@; }
        QToolButton:disabled { background: transparent; }

        /* ---- inputs ---- */
        QLineEdit, QComboBox {
            background: @input@; color: @text@;
            border: 1px solid @borderSoft@; border-radius: 2px;
            padding: 1px 6px; min-height: 22px;
            selection-background-color: @select@; selection-color: @text@;
        }
        QLineEdit:focus, QComboBox:focus { border-color: @accent@; }
        QComboBox QAbstractItemView {
            background: @menu@; border: 1px solid @border@; color: @text@;
            selection-background-color: @select@; selection-color: @text@;
            outline: 0;
        }
        QPushButton {
            padding: 4px 10px; min-height: 22px;
            border: 1px solid @buttonBorder@; border-radius: 3px;
            background: @button@; color: @text@;
        }
        QPushButton:hover { background: @buttonHover@; }

)CSS") +
      QStringLiteral(R"CSS(        /* ---- shared find / replace bar ---- */
        #findReplaceBar { background: @ruleBar@; border-top: 1px solid @border@; }
        #findReplaceBar QLineEdit {
            background: @ruleInput@; color: @text@; border: 1px solid @border@;
            border-radius: 3px; padding: 2px 6px;
        }
        #findReplaceBar QLabel { color: @textBar@; }

        /* ---- media compare: draggable divider, picture compare: fit button ---- */
        QSplitter#mediaSplit::handle:vertical { background: @divider@; margin: 3px 0; border-radius: 1px; }
        QSplitter#mediaSplit::handle:vertical:hover, QSplitter#mediaSplit::handle:vertical:pressed { background: @info@; }
        QToolButton#fitButton:checked { border-color: @info@; }

        /* ---- panes ---- */
        QSplitter::handle:horizontal { background: @border@; width: 3px; }
        QSplitter::handle:vertical { background: @window@; height: 4px; }
        QFrame#pathBar { background: @input@; border: 0; }
        QFrame#pathBar QComboBox { border-color: @input@; border-radius: 0; }
        QFrame#pathBar QComboBox:focus { border-color: @accent@; }
        QFrame#pathBar QToolButton { margin: 0 1px; }
        QFrame#footer { background: @chrome@; border: 0; border-top: 1px solid @border@; }
        QFrame#footer QLabel { background: transparent; padding: 3px 6px; }
        QFrame#footerDivider { background: @divider@; max-width: 1px; }
        QFrame#gutterHeader {
            background: @header@; border: 0; border-bottom: 1px solid @border@;
        }
        QFrame#gutterBody { background: @pane@; border: 0; }

        QTreeView, QTreeWidget {
            background: @pane@; color: @text@; border: 0; outline: 0;
        }
        QHeaderView { background: @header@; border: 0; }
        QHeaderView::section {
            background: @header@; color: @text@;
            padding: 4px 7px; border: 0;
            border-right: 1px solid @borderSoft@; border-bottom: 1px solid @border@;
        }

        QPlainTextEdit#console {
            background: @input@; color: @text@; border: 0;
            border-top: 1px solid @border@; padding: 3px 6px;
            selection-background-color: @select@;
        }
        QLabel#compareStatus {
            background: @chrome@; color: @text@;
            border-top: 1px solid @border@; padding: 3px 8px;
        }
        QFrame#compareRuleBar {
            background: @ruleBar@; border-top: 1px solid @border@;
        }
        QFrame#compareRuleBar QLabel { color: @textBar@; }
        QLineEdit#compareRuleInput {
            background: @ruleInput@; color: @text@; border: 1px solid @border@;
            border-radius: 3px; padding: 2px 6px;
        }

        QWidget#textCompareView { background: @pane@; }
        QWidget#textCompareView QPlainTextEdit {
            background: @editor@; color: @text@; border: 0;
            padding: 4px 8px; selection-background-color: @select@; selection-color: @text@;
        }
        QWidget#textCompareView QSplitter::handle:horizontal { background: @border@; width: 2px; }
        QToolBar#textCompareToolbar {
            background: @chrome@; border: 0; border-top: 1px solid @border@;
            border-bottom: 1px solid @border@; spacing: 2px; padding: 3px 5px;
        }
        QToolBar#textCompareToolbar::separator { background: @divider@; width: 1px; margin: 4px 5px; }
        QToolBar#textCompareToolbar QLabel { color: @textToolbar@; font-size: @fsSmall@; }
        QToolBar#textCompareToolbar QToolButton {
            background: transparent; border: 1px solid transparent; border-radius: 3px;
            padding: 3px 6px; color: @textToolbar@; font-size: @fsSmall@;
        }
        QToolBar#textCompareToolbar QToolButton:hover { background: @hover@; border-color: @borderHover@; }
        QToolBar#textCompareToolbar QToolButton:checked { background: @checked@; border-color: @checkedBorder@; }
        QToolBar#textCompareToolbar QToolButton:pressed { background: @pressed@; }
        QToolBar#textCompareToolbar QToolButton:disabled { color: @textDisabled@; }
        QToolBar#textCompareToolbar QToolButton::menu-button { border: 0; width: 14px; }
        QFrame#diffLinePreview { background: @input@; border: 0; border-top: 1px solid @border@; }
        QFrame#diffLinePreview QLabel { font-family: @mono@; }
        QLineEdit#previewText {
            font-family: @mono@; border-radius: 0;
        }
        QLabel#previewLineNo {
            font-family: @mono@; color: @textDim@;
            background: @well@; padding: 0 6px; border: 1px solid transparent;
        }

        QFrame#sideHeader { background: @input@; border: 0; border-bottom: 1px solid @raised@; }
        QLabel#sidePath {
            font-weight: 700; color: @text@; background: transparent; padding: 0;
        }
        QLabel#sideAttrs {
            color: @textDim@; background: transparent; padding: 0; font-size: @fsSmall@;
        }
        QToolButton#saveSideBtn {
            background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 2px;
        }
        QToolButton#saveSideBtn:hover { background: @hover@; border-color: @borderHover@; }
        QToolButton#saveSideBtn:disabled { background: transparent; }
        /* Blue = this side has copied/edited lines that are not yet written to
        disk. Kept distinct from the green row-highlight used by the minimap
        and the dirty-row gutter bars, so the button reads as "action needed"
        rather than "state". */
        QToolButton#saveSideBtn[dirty="true"] {
            background: rgba(78, 156, 255, 45); border-color: @info@;
        }
        QToolButton#saveSideBtn[dirty="true"]:hover { background: rgba(78, 156, 255, 80); }
        QToolButton#saveSideBtn[dirty="true"]:pressed { background: rgba(78, 156, 255, 110); }

        QScrollBar:vertical { background: @well@; width: 15px; margin: 0; border-left: 1px solid @raised@; }
        QScrollBar:horizontal { background: @well@; height: 15px; margin: 0; border-top: 1px solid @raised@; }
        QScrollBar::handle:vertical { background: @scrollHandle@; min-height: 32px; border-radius: 4px; margin: 2px 3px; }
        QScrollBar::handle:horizontal { background: @scrollHandle@; min-width: 32px; border-radius: 4px; margin: 3px 2px; }
        QScrollBar::handle:hover { background: @scrollHandleHover@; }
        QScrollBar::handle:pressed { background: @scrollHandlePress@; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
    )CSS");
}

inline QString applicationStyleSheet(Theme which = Theme::Dark) {
  QString sheet = theme::expand(applicationStyleSheetTemplate(), which);

  // Appearance overrides last, so they win over the defaults above in both themes.
  const auto css = [](const QColor &c) { return c.name(QColor::HexRgb); };
  const auto &colors = appearance::state().colors;
  if (const auto it = colors.constFind("Folder Views/Log panel"); it != colors.constEnd()) {
    sheet += QString("QPlainTextEdit#console { %1%2 }\n")
                 .arg(it->text.isValid() ? "color: " + css(it->text) + ";" : QString(),
                      it->background.isValid() ? " background: " + css(it->background) + ";" : QString());
  }
  if (const auto it = colors.constFind("File Views/Default text"); it != colors.constEnd()) {
    sheet += QString("QWidget#textCompareView QPlainTextEdit { %1%2 }\n")
                 .arg(it->text.isValid() ? "color: " + css(it->text) + ";" : QString(),
                      it->background.isValid() ? " background: " + css(it->background) + ";" : QString());
  }
  if (const auto it = colors.constFind("File Views/Selection");
      it != colors.constEnd() && it->background.isValid()) {
    sheet += QString("QWidget#textCompareView QPlainTextEdit { selection-background-color: %1; }\n")
                 .arg(css(it->background));
  }
  // "Use gradient for center background": the strip between the two folder panes.
  const QString edge = theme::css(theme::colorFor(theme::Token::gutterEdge, which));
  const QString mid = theme::css(theme::colorFor(theme::Token::gutterMid, which));
  const QString flat = theme::css(theme::colorFor(theme::Token::chrome, which));
  sheet += appearance::state().gradientCenter
               ? QStringLiteral("QWidget#folderGutter { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
                                " stop:0 %1, stop:0.5 %2, stop:1 %1); }\n").arg(edge, mid)
               : QStringLiteral("QWidget#folderGutter { background: %1; }\n").arg(flat);
  return sheet;
}

// ---------------------------------------------------------------------------
// Vector icons (no image resources needed)
// ---------------------------------------------------------------------------

namespace icons {

// Paints in a 16x16 design space; the engine scales that to whatever size /
// devicePixelRatio Qt asks for, so icons stay crisp on HiDPI displays.
using PaintFn = std::function<void(QPainter &, QIcon::Mode, QIcon::State)>;

class Engine : public QIconEngine {
public:
  explicit Engine(PaintFn fn) : fn_(std::move(fn)) {}

  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
             QIcon::State state) override {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (mode == QIcon::Disabled) {
      painter->setOpacity(painter->opacity() * 0.4);
    }
    painter->translate(rect.topLeft());
    painter->scale(rect.width() / 16.0, rect.height() / 16.0);
    fn_(*painter, mode, state);
    painter->restore();
  }

  QPixmap pixmap(const QSize &size, QIcon::Mode mode,
                 QIcon::State state) override {
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    paint(&painter, QRect(QPoint(0, 0), size), mode, state);
    return pm;
  }

  QIconEngine *clone() const override { return new Engine(fn_); }

private:
  PaintFn fn_;
};

inline QIcon make(PaintFn fn) { return QIcon(new Engine(std::move(fn))); }

// Folder: closed (QIcon::Off) or open (QIcon::On, used for expanded rows).
// A second colour paints the lower-right diagonal half ("mixed" folder).
inline QIcon folderIcon(const QColor &base, const QColor &second = QColor()) {
  return make([base, second](QPainter &p, QIcon::Mode, QIcon::State state) {
    p.setPen(Qt::NoPen);
    const QColor back = base.darker(155);
    QPainterPath tab;
    tab.addRoundedRect(QRectF(1.2, 2.4, 6.4, 3.4), 1.0, 1.0);
    if (state != QIcon::On) {
      QPainterPath body;
      body.addRoundedRect(QRectF(1.0, 4.6, 14.0, 9.6), 1.3, 1.3);
      p.setBrush(back);
      p.drawPath(tab);
      p.setBrush(base);
      p.drawPath(body);
      if (second.isValid()) {
        p.save();
        p.setClipPath(body);
        p.setBrush(second);
        p.drawPolygon(QPolygonF(
            {QPointF(0.5, 14.6), QPointF(15.6, 14.6), QPointF(15.6, 4.2)}));
        p.restore();
      }
    } else {
      QPainterPath backBody;
      backBody.addRoundedRect(QRectF(1.0, 3.6, 12.6, 10.2), 1.2, 1.2);
      p.setBrush(back);
      p.drawPath(tab);
      p.drawPath(backBody);
      QPainterPath flap;
      flap.moveTo(3.6, 6.8);
      flap.lineTo(15.7, 6.8);
      flap.lineTo(13.0, 14.2);
      flap.lineTo(0.8, 14.2);
      flap.closeSubpath();
      p.setBrush(base);
      p.drawPath(flap);
      if (second.isValid()) {
        p.save();
        p.setClipPath(flap);
        p.setBrush(second);
        p.drawPolygon(QPolygonF(
            {QPointF(0.5, 14.6), QPointF(15.8, 14.6), QPointF(15.8, 6.4)}));
        p.restore();
      }
    }
  });
}

// Folder icon coloured by which kinds of differences it contains.
inline QIcon folderIconForMask(quint32 mask) {
  static QHash<quint32, QIcon> cache;
  static int cachedRevision = -1;
  if (cachedRevision != appearance::state().revision) { // theme or colours changed
    cache.clear();
    cachedRevision = appearance::state().revision;
  }
  // "Color folders to reflect content" off: every folder keeps the plain yellow icon (mask 0).
  if (!appearance::state().colorFolders) mask = 0;
  const auto found = cache.constFind(mask);
  if (found != cache.constEnd()) {
    return *found;
  }
  std::vector<QColor> colors;
  if (mask & statusBit(RowStatus::Different))
    colors.push_back(color::folderRed());
  if (mask & statusBit(RowStatus::Orphan))
    colors.push_back(color::folderBlue());
  if (mask & statusBit(RowStatus::Equal))
    colors.push_back(color::folderGreen());
  if (mask & statusBit(RowStatus::Newer))
    colors.push_back(color::folderOrange());
  if (mask & statusBit(RowStatus::Older))
    colors.push_back(color::folderPurple());
  QIcon icon;
  if (colors.empty()) {
    icon = folderIcon(color::folderYellow());
  } else if (colors.size() == 1) {
    icon = folderIcon(colors[0]);
  } else {
    icon = folderIcon(colors[0], colors[1]);
  }
  cache.insert(mask, icon);
  return icon;
}

// A "group" icon - two overlapping rounded cards - used for every node in
// the session-history tree that groups sessions together (date buckets, the
// Personal node, and any other named node). folderIcon() above stays
// reserved for rows that represent an actual folder-compare session, so the
// two are never visually confused.
inline QIcon groupNodeIcon(const QColor &color) {
  return make([color](QPainter &p, QIcon::Mode, QIcon::State state) {
    p.setPen(Qt::NoPen);
    const bool open = state == QIcon::On;
    p.setBrush(color.darker(150));
    p.drawRoundedRect(QRectF(4.4, 1.6, 9.4, 9.0), 2.2, 2.2);
    p.setBrush(open ? color.lighter(112) : color);
    p.drawRoundedRect(QRectF(2.2, 4.8, 9.4, 9.0), 2.2, 2.2);
    p.setPen(QPen(color.darker(190), 1.1));
    p.drawLine(QPointF(4.6, 9.1), QPointF(9.4, 9.1));
  });
}

// Small square shown in front of file names, coloured by status.
inline QIcon markerIcon(RowStatus status) {
  static QHash<int, QIcon> cache;
  static int cachedRevision = -1;
  if (cachedRevision != appearance::state().revision) { // colours changed: rebuild lazily
    cache.clear();
    cachedRevision = appearance::state().revision;
  }
  const int key = static_cast<int>(status);
  const auto found = cache.constFind(key);
  if (found != cache.constEnd()) {
    return *found;
  }
  const QColor c = status == RowStatus::Equal || status == RowStatus::Pending
                       ? color::equalMarker()
                       : statusTextColor(status);
  QIcon icon = make([c](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(4.5, 4.5, 7.0, 7.0), 1.0, 1.0);
  });
  cache.insert(key, icon);
  return icon;
}

// Two coloured dashes (either may be invalid = empty slot). Bright when
// checked (QIcon::On), dimmed when unchecked - like the legend toggles in
// Compare's toolbar.
inline QIcon dashIcon(const QColor &left, const QColor &right) {
  return make([left, right](QPainter &p, QIcon::Mode, QIcon::State state) {
    p.setPen(Qt::NoPen);
    if (state != QIcon::On) {
      p.setOpacity(p.opacity() * 0.35);
    }
    if (left.isValid()) {
      p.setBrush(left);
      p.drawRoundedRect(QRectF(0.8, 6.2, 6.4, 3.6), 0.8, 0.8);
    }
    if (right.isValid()) {
      p.setBrush(right);
      p.drawRoundedRect(QRectF(8.8, 6.2, 6.4, 3.6), 0.8, 0.8);
    }
  });
}

enum class Glyph {
  Compare,
  Refresh,
  Swap,
  ExpandAll,
  CollapseAll,
  Contents,
  Timestamps,
  Filter,
  FilterClear,
  FolderOpen,
  FolderUp,
  Plus,
  Close,
  Home,
  Sessions,
  ShowAll,
  ShowDiffs,
  Context,
  Minor,
  Rules,
  Format,
  CopyLine,
  NextSection,
  PrevSection,
  Find,
  MinimapPixel,
  Save,
  Play,
  Pause,
  Stop,
  VolumeOn,
  VolumeMuted,
  Cover,
};

inline QIcon glyph(Glyph which) {
  switch (which) {
  case Glyph::Compare:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      p.setPen(QPen(color::newer(), 2.0, Qt::SolidLine, Qt::RoundCap,
                    Qt::RoundJoin));
      p.setBrush(Qt::NoBrush);
      QPainterPath check;
      check.moveTo(2.8, 8.6);
      check.lineTo(6.4, 12.2);
      check.lineTo(13.4, 3.8);
      p.drawPath(check);
    });
  case Glyph::Refresh:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor gold = theme::get(theme::Token::gold);
      p.setPen(QPen(gold, 1.9, Qt::SolidLine, Qt::RoundCap));
      p.setBrush(Qt::NoBrush);
      p.drawArc(QRectF(2.6, 2.6, 10.8, 10.8), 50 * 16, 270 * 16);
      p.setPen(Qt::NoPen);
      p.setBrush(gold);
      p.drawPolygon(QPolygonF(
          {QPointF(14.6, 2.0), QPointF(14.6, 7.4), QPointF(9.4, 6.2)}));
    });
  case Glyph::Swap:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor c(0xf7, 0xc2, 0x3f);
      p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::FlatCap));
      p.drawLine(QPointF(3.2, 5.6), QPointF(12.0, 5.6));
      p.drawLine(QPointF(4.0, 10.4), QPointF(12.8, 10.4));
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      p.drawPolygon(QPolygonF(
          {QPointF(15.4, 5.6), QPointF(10.4, 2.2), QPointF(10.4, 9.0)}));
      p.drawPolygon(QPolygonF(
          {QPointF(0.6, 10.4), QPointF(5.6, 7.0), QPointF(5.6, 13.8)}));
    });
  case Glyph::ExpandAll:
  case Glyph::CollapseAll: {
    const bool expand = which == Glyph::ExpandAll;
    return make([expand](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(QColor(0x8b, 0x90, 0xa8), 1.0));
      p.drawLine(QPointF(3.5, 3.2), QPointF(3.5, 13.0));
      p.drawLine(QPointF(3.5, 8.0), QPointF(6.0, 8.0));
      p.drawLine(QPointF(3.5, 13.0), QPointF(6.0, 13.0));
      p.setPen(Qt::NoPen);
      p.setBrush(color::folderYellow());
      p.drawRoundedRect(QRectF(1.0, 1.4, 5.0, 3.4), 0.8, 0.8);
      p.drawRoundedRect(QRectF(6.2, 6.6, 3.0, 2.8), 0.6, 0.6);
      p.drawRoundedRect(QRectF(6.2, 11.6, 3.0, 2.8), 0.6, 0.6);
      p.setBrush(ink);
      p.drawRoundedRect(QRectF(10.0, 7.2, 5.0, 1.6), 0.6, 0.6);
      p.drawRoundedRect(QRectF(10.0, 12.2, 5.0, 1.6), 0.6, 0.6);
      p.setPen(QPen(expand ? color::newer() : color::different(), 1.5,
                    Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(8.2, 3.1), QPointF(13.6, 3.1));
      if (expand) {
        p.drawLine(QPointF(10.9, 0.6), QPointF(10.9, 5.6));
      }
    });
  }
  case Glyph::Contents:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
      p.setBrush(Qt::NoBrush);
      for (const qreal dy : {0.0, 3.8}) {
        QPainterPath wave;
        wave.moveTo(2.2, 5.4 + dy);
        wave.cubicTo(4.0, 2.6 + dy, 6.2, 2.6 + dy, 8.0, 5.4 + dy);
        wave.cubicTo(9.8, 8.2 + dy, 12.0, 8.2 + dy, 13.8, 5.4 + dy);
        p.drawPath(wave);
      }
    });
  case Glyph::Timestamps:
    return make([](QPainter &p, QIcon::Mode, QIcon::State state) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.4));
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(QRectF(2.2, 2.2, 11.6, 11.6));
      p.drawLine(QPointF(8.0, 4.6), QPointF(8.0, 8.2));
      p.drawLine(QPointF(8.0, 8.2), QPointF(10.6, 9.8));
      if (state == QIcon::On) { // checked = "ignore timestamps"
        p.setPen(QPen(color::different(), 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2.0, 14.0), QPointF(14.0, 2.0));
      }
    });
  case Glyph::Filter:
  case Glyph::FilterClear: {
    const bool clear = which == Glyph::FilterClear;
    return make([clear](QPainter &p, QIcon::Mode, QIcon::State) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0x62, 0xd6, 0xe8));
      QPainterPath funnel;
      funnel.moveTo(1.6, 2.6);
      funnel.lineTo(14.4, 2.6);
      funnel.lineTo(9.6, 8.4);
      funnel.lineTo(9.6, 13.6);
      funnel.lineTo(6.4, 12.0);
      funnel.lineTo(6.4, 8.4);
      funnel.closeSubpath();
      p.drawPath(funnel);
      if (clear) {
        p.setPen(QPen(color::different(), 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(9.6, 9.6), QPointF(14.6, 14.6));
        p.drawLine(QPointF(14.6, 9.6), QPointF(9.6, 14.6));
      }
    });
  }
  case Glyph::FolderOpen: {
    const QIcon folder = folderIcon(color::folderYellow());
    return make([folder](QPainter &p, QIcon::Mode, QIcon::State) {
      folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal,
                   QIcon::On);
    });
  }
  case Glyph::FolderUp: {
    const QIcon folder = folderIcon(color::folderYellow());
    return make([folder](QPainter &p, QIcon::Mode, QIcon::State) {
      folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal,
                   QIcon::Off);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0x3b, 0x8e, 0xea));
      p.drawPolygon(
          QPolygonF({QPointF(8.0, 5.6), QPointF(12.4, 10.6), QPointF(9.6, 10.6),
                     QPointF(9.6, 14.2), QPointF(6.4, 14.2), QPointF(6.4, 10.6),
                     QPointF(3.6, 10.6)}));
    });
  }
  case Glyph::Plus:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(8.0, 3.2), QPointF(8.0, 12.8));
      p.drawLine(QPointF(3.2, 8.0), QPointF(12.8, 8.0));
    });
  case Glyph::Close:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      p.setPen(
          QPen(QColor(0xa8, 0xac, 0xbe), 1.6, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(4.4, 4.4), QPointF(11.6, 11.6));
      p.drawLine(QPointF(11.6, 4.4), QPointF(4.4, 11.6));
    });
  case Glyph::Home:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawPolygon(
          QPolygonF({QPointF(8.0, 1.8), QPointF(14.6, 7.4), QPointF(12.4, 7.4),
                     QPointF(12.4, 14.0), QPointF(3.6, 14.0), QPointF(3.6, 7.4),
                     QPointF(1.4, 7.4)}));
      p.setBrush(QColor(0x2d, 0x2d, 0x2d));
      p.drawRect(QRectF(6.6, 9.6, 2.8, 4.4));
    });
  case Glyph::Sessions:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0x62, 0x72, 0xa4));
      p.drawRoundedRect(QRectF(2.0, 2.0, 10.0, 6.4), 1.0, 1.0);
      p.setBrush(ink);
      p.drawRoundedRect(QRectF(4.0, 7.4, 10.0, 6.4), 1.0, 1.0);
    });
  case Glyph::ShowAll:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap));
      for (const qreal y : {3.2, 8.0, 12.8}) {
        p.drawLine(QPointF(2.0, y), QPointF(14.0, y));
      }
    });
  case Glyph::ShowDiffs:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      p.setPen(QPen(color::different(), 1.6, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(2.0, 3.2), QPointF(14.0, 3.2));
      p.drawLine(QPointF(2.0, 12.8), QPointF(14.0, 12.8));
      p.setPen(QPen(QColor(0x6a, 0x6f, 0x82), 1.4, Qt::DashLine));
      p.drawLine(QPointF(2.0, 8.0), QPointF(14.0, 8.0));
    });
  case Glyph::Context:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(QColor(0x6a, 0x6f, 0x82), 1.4, Qt::DashLine));
      p.drawLine(QPointF(2.0, 3.2), QPointF(14.0, 3.2));
      p.drawLine(QPointF(2.0, 12.8), QPointF(14.0, 12.8));
      p.setPen(QPen(ink, 1.6, Qt::SolidLine));
      p.drawLine(QPointF(2.0, 8.0), QPointF(14.0, 8.0));
    });
  case Glyph::Minor:
    return make([](QPainter &p, QIcon::Mode, QIcon::State state) {
      p.setPen(
          QPen(QColor(0x4e, 0xa1, 0xff), 1.6, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(2.4, 5.4), QPointF(13.6, 5.4));
      p.drawLine(QPointF(2.4, 10.6), QPointF(9.0, 10.6));
      if (state == QIcon::On) {
        p.setPen(QPen(color::different(), 1.9, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2.0, 14.0), QPointF(14.0, 2.0));
      }
    });
  case Glyph::Rules:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap));
      for (const qreal y : {3.6, 8.0, 12.4}) {
        p.drawLine(QPointF(1.6, y), QPointF(14.4, y));
      }
      p.setPen(Qt::NoPen);
      p.setBrush(theme::get(theme::Token::gold));
      p.drawEllipse(QPointF(10.5, 3.6), 1.7, 1.7);
      p.drawEllipse(QPointF(5.5, 8.0), 1.7, 1.7);
      p.drawEllipse(QPointF(9.0, 12.4), 1.7, 1.7);
    });
  case Glyph::Format:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      QFont font;
      font.setPixelSize(11);
      font.setBold(true);
      p.setFont(font);
      p.setPen(ink);
      p.drawText(QRectF(0.5, 0.5, 15.0, 15.0), Qt::AlignCenter,
                 QStringLiteral("Abc"));
    });
  case Glyph::CopyLine:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      p.setPen(Qt::NoPen);
      p.setBrush(theme::get(theme::Token::gold));
      p.drawPolygon(
          QPolygonF({QPointF(1.5, 5.8), QPointF(8.2, 5.8), QPointF(8.2, 2.8),
                     QPointF(14.5, 8.0), QPointF(8.2, 13.2), QPointF(8.2, 10.2),
                     QPointF(1.5, 10.2)}));
    });
  case Glyph::NextSection:
  case Glyph::PrevSection: {
    return make([next = which == Glyph::NextSection](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      if (next) {
        p.drawPolygon(QPolygonF(
            {QPointF(3.0, 4.5), QPointF(13.0, 4.5), QPointF(8.0, 11.5)}));
      } else {
        p.drawPolygon(QPolygonF(
            {QPointF(3.0, 11.5), QPointF(13.0, 11.5), QPointF(8.0, 4.5)}));
      }
    });
  case Glyph::Find:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.7, Qt::SolidLine, Qt::RoundCap));
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(QPointF(6.6, 6.6), 4.4, 4.4);
      p.drawLine(QPointF(10.0, 10.0), QPointF(14.2, 14.2));
    });
  case Glyph::MinimapPixel:
    return make([](QPainter &p, QIcon::Mode, QIcon::State state) {
      const QColor ink = color::ink();
      p.setPen(QPen(ink, state == QIcon::On ? 1.1 : 2.6, Qt::SolidLine,
                    Qt::FlatCap));
      for (const qreal y : {2.4, 5.2, 8.0, 10.8, 13.6}) {
        p.drawLine(QPointF(2.0, y), QPointF(14.0, y));
      }
    });
  case Glyph::Save:
    // Classic floppy-disk glyph: body, dark shutter block top-left, and a
    // small paper-label rectangle lower half - drawn with the same "gold"
    // accent as Refresh/Swap so an unsaved side reads as actionable.
    return make([](QPainter &p, QIcon::Mode, QIcon::State state) {
      const QColor gold = theme::get(theme::Token::gold);
      const QColor ink = color::ink();
      const QColor body = state == QIcon::On ? gold : ink;
      p.setPen(Qt::NoPen);
      p.setBrush(body);
      QPainterPath shell;
      shell.addRoundedRect(QRectF(2.2, 1.8, 11.6, 12.4), 1.4, 1.4);
      p.drawPath(shell);
      p.setBrush(QColor(0x23, 0x23, 0x23));
      p.drawRect(QRectF(4.4, 2.6, 6.0, 3.6));
      p.setBrush(body.darker(160));
      p.drawRect(QRectF(4.6, 8.2, 6.8, 4.6));
    });
  case Glyph::Play:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      p.setPen(Qt::NoPen);
      p.setBrush(color::newer());
      p.drawPolygon(QPolygonF(
          {QPointF(4.2, 2.4), QPointF(13.4, 8.0), QPointF(4.2, 13.6)}));
    });
  case Glyph::Pause:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawRoundedRect(QRectF(3.6, 2.6, 3.2, 10.8), 0.8, 0.8);
      p.drawRoundedRect(QRectF(9.2, 2.6, 3.2, 10.8), 0.8, 0.8);
    });
  case Glyph::Stop:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawRoundedRect(QRectF(3.4, 3.4, 9.2, 9.2), 1.2, 1.2);
    });
  case Glyph::VolumeOn:
  case Glyph::VolumeMuted: {
    const bool muted = which == Glyph::VolumeMuted;
    return make([muted](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor ink = color::ink();
      p.setPen(Qt::NoPen);
      p.setBrush(ink);
      p.drawPolygon(QPolygonF({QPointF(1.6, 6.0), QPointF(4.4, 6.0),
                               QPointF(8.0, 2.8), QPointF(8.0, 13.2),
                               QPointF(4.4, 10.0), QPointF(1.6, 10.0)}));
      p.setBrush(Qt::NoBrush);
      if (muted) {
        p.setPen(QPen(color::different(), 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(10.4, 5.6), QPointF(14.6, 10.4));
        p.drawLine(QPointF(14.6, 5.6), QPointF(10.4, 10.4));
      } else {
        p.setPen(QPen(ink, 1.5, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(7.0, 5.0, 5.0, 6.0), -60 * 16, 120 * 16);
        p.drawArc(QRectF(6.0, 2.8, 8.4, 10.4), -60 * 16, 120 * 16);
      }
    });
  }
  case Glyph::Cover:
    return make([](QPainter &p, QIcon::Mode, QIcon::State) {
      const QColor gold = theme::get(theme::Token::gold);
      const QColor ink = color::ink();
      p.setPen(QPen(ink, 1.3));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(1.8, 2.8, 12.4, 10.4), 1.4, 1.4);
      p.setPen(Qt::NoPen);
      p.setBrush(color::newer());
      p.drawPolygon(
          QPolygonF({QPointF(3.4, 12.0), QPointF(6.8, 7.2), QPointF(9.0, 10.0),
                     QPointF(10.4, 8.6), QPointF(12.8, 12.0)}));
      p.setBrush(gold);
      p.drawEllipse(QPointF(10.6, 5.6), 1.2, 1.2);
    });
  }
  }
  return QIcon();
}

// Colour-parameterised floppy-disk glyph, for callers that need to draw
// the save icon in an "attention" colour (e.g. the per-side save buttons
// when that side has copied-but-unsaved lines). Glyph::Save itself paints
// in the generic ink / gold palette used by the toolbar; this variant lets
// the caller pick the body colour directly.
inline QIcon saveIcon(const QColor &body) {
  return make([body](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(Qt::NoPen);
    p.setBrush(body);
    QPainterPath shell;
    shell.addRoundedRect(QRectF(2.2, 1.8, 11.6, 12.4), 1.4, 1.4);
    p.drawPath(shell);
    // Dark shutter block top-left.
    p.setBrush(QColor(0x23, 0x23, 0x23));
    p.drawRect(QRectF(4.4, 2.6, 6.0, 3.6));
    // Slightly darker paper-label rectangle lower half.
    p.setBrush(body.darker(160));
    p.drawRect(QRectF(4.6, 8.2, 6.8, 4.6));
  });
}

// ---------------------------------------------------------------------------
// Picture Compare icons: the Tol / Range / Blend mode glyphs (a square and a
// circle overlapping, with the overlap in a third colour), the "fit to window"
// glyph (a frame with arrowheads pointing outward), the metadata toggle and the
// controls-left/right toggle. QIcon::On is the checked state.
// ---------------------------------------------------------------------------
namespace pictureicons {

inline QIcon overlap(const QColor &square, const QColor &circle,
                     const QColor &both) {
  return make([square, circle, both](QPainter &p, QIcon::Mode, QIcon::State) {
    const QRectF sq(1.3, 1.3, 8.4, 8.4);
    const QRectF ci(6.0, 6.0, 8.6, 8.6);
    p.setPen(QPen(QColor(0x2a, 0x2a, 0x2a), 1.0));
    p.setBrush(square);
    p.drawRect(sq);
    p.setBrush(circle);
    p.drawEllipse(ci);
    QPainterPath a;
    a.addRect(sq);
    QPainterPath b;
    b.addEllipse(ci);
    p.setPen(Qt::NoPen);
    p.setBrush(both);
    p.drawPath(a.intersected(b));
  });
}

inline QIcon tolerance() {
  return overlap(QColor(0xe8, 0x30, 0x30), QColor(0xe8, 0x30, 0x30),
                 QColor(0x1e, 0x7b, 0xf0));
}
inline QIcon range() {
  return overlap(QColor(0xc8, 0xc0, 0x10), QColor(0xc8, 0xc0, 0x10),
                 QColor(0x6e, 0x66, 0x08));
}
inline QIcon blend() {
  return overlap(QColor(0x20, 0xd0, 0x20), QColor(0x6c, 0x78, 0xf0),
                 QColor(0x10, 0xc8, 0xc8));
}

// Fit to window: square frame, arrowheads pointing out of all four sides.
inline QIcon fit() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    const QColor ink = color::ink();
    p.setPen(QPen(ink, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(4.8, 4.8, 6.4, 6.4), 0.8, 0.8);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPolygon(
        QPolygonF({QPointF(8.0, 0.8), QPointF(10.4, 3.6), QPointF(5.6, 3.6)}));
    p.drawPolygon(QPolygonF(
        {QPointF(8.0, 15.2), QPointF(10.4, 12.4), QPointF(5.6, 12.4)}));
    p.drawPolygon(
        QPolygonF({QPointF(0.8, 8.0), QPointF(3.6, 5.6), QPointF(3.6, 10.4)}));
    p.drawPolygon(QPolygonF(
        {QPointF(15.2, 8.0), QPointF(12.4, 5.6), QPointF(12.4, 10.4)}));
  });
}

// "i" in a ring (Metadata).
inline QIcon info() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    const QColor ink = color::ink();
    p.setPen(QPen(ink, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(1.8, 1.8, 12.4, 12.4));
    p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(8.0, 7.2), QPointF(8.0, 11.2));
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawEllipse(QPointF(8.0, 5.0), 1.0, 1.0);
  });
}

// A window with a filled side strip: strip on the left when unchecked (controls
// on the left), on the right when checked (controls on the right).
inline QIcon controlsSide() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State state) {
    const QColor ink = color::ink();
    const QColor accent(0x4e, 0x9c, 0xff);
    p.setPen(QPen(ink, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1.6, 2.6, 12.8, 10.8), 1.2, 1.2);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    const bool right = state == QIcon::On;
    p.drawRect(QRectF(right ? 9.4 : 2.2, 3.2, 4.4, 9.6));
  });
}

} // namespace pictureicons

// ---------------------------------------------------------------------------
// Context-menu icons. Same 16x16 design space as the toolbar glyphs, drawn in
// code so the menus need no image resources.
// ---------------------------------------------------------------------------
namespace menuicons {

enum class SyncDir { Right, Left, Both };

// Solid block arrow; `right` = false mirrors it.
inline void paintArrow(QPainter &p, const QColor &fill, bool right) {
  p.save();
  if (!right) {
    p.translate(16.0, 0.0);
    p.scale(-1.0, 1.0);
  }
  p.setPen(Qt::NoPen);
  p.setBrush(fill);
  p.drawPolygon(
      QPolygonF({QPointF(1.5, 5.8), QPointF(8.2, 5.8), QPointF(8.2, 2.8),
                 QPointF(14.5, 8.0), QPointF(8.2, 13.2), QPointF(8.2, 10.2),
                 QPointF(1.5, 10.2)}));
  p.restore();
}

// "Copy to ..." (yellow) and "Move to ..." (red) share the arrow shape.
inline QIcon arrow(const QColor &fill, bool right) {
  return make([fill, right](QPainter &p, QIcon::Mode, QIcon::State) {
    paintArrow(p, fill, right);
  });
}
inline QIcon copyArrow(bool right) {
  return arrow(QColor(0xf5, 0xc2, 0x3d), right);
}
inline QIcon moveArrow(bool right) {
  return arrow(QColor(0xe8, 0x4a, 0x4a), right);
}

// Folder with a small up-arrow badge (Copy to Folder / Move to Folder).
inline QIcon folderWithArrow(const QColor &badge) {
  const QIcon folder = folderIcon(color::folderYellow());
  return make([folder, badge](QPainter &p, QIcon::Mode, QIcon::State) {
    folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal,
                 QIcon::Off);
    p.setPen(QPen(QColor(0x1e, 0x1e, 0x1e), 0.7));
    p.setBrush(badge);
    p.drawPolygon(
        QPolygonF({QPointF(4.5, 6.0), QPointF(8.2, 10.0), QPointF(5.9, 10.0),
                   QPointF(5.9, 14.6), QPointF(3.1, 14.6), QPointF(3.1, 10.0),
                   QPointF(0.8, 10.0)}));
  });
}
inline QIcon copyToFolder() {
  return folderWithArrow(QColor(0xf5, 0xc2, 0x3d));
}
inline QIcon moveToFolder() {
  return folderWithArrow(QColor(0xe8, 0x4a, 0x4a));
}

// Folder with a green "+" (New Folder).
inline QIcon newFolder() {
  const QIcon folder = folderIcon(color::folderYellow());
  return make([folder](QPainter &p, QIcon::Mode, QIcon::State) {
    folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal,
                 QIcon::Off);
    p.setPen(QPen(QColor(0x2f, 0xc4, 0x4f), 2.2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(11.5, 8.5), QPointF(11.5, 14.5));
    p.drawLine(QPointF(8.5, 11.5), QPointF(14.5, 11.5));
  });
}

// "=?" (Compare Contents)
inline QIcon compareContents() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(QPen(color::ink(), 1.5, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(1.5, 6.0), QPointF(7.0, 6.0));
    p.drawLine(QPointF(1.5, 10.0), QPointF(7.0, 10.0));
    QFont font;
    font.setPixelSize(13);
    font.setBold(true);
    p.setFont(font);
    p.setPen(theme::get(theme::Token::gold));
    p.drawText(QRectF(7.5, 0.5, 8.0, 15.0), Qt::AlignCenter,
               QStringLiteral("?"));
  });
}

// Red cross (Delete)
inline QIcon remove() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(QPen(QColor(0xe8, 0x3f, 0x3f), 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(3.5, 3.5), QPointF(12.5, 12.5));
    p.drawLine(QPointF(12.5, 3.5), QPointF(3.5, 12.5));
  });
}

// Text box with a caret (Rename)
inline QIcon rename() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(QPen(color::ink(), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1.5, 4.5, 13.0, 7.0), 1.0, 1.0);
    p.setPen(QPen(QColor(0x5a, 0xa9, 0xff), 1.4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(8.0, 2.5), QPointF(8.0, 13.5));
  });
}

// Calendar + clock (Touch = change timestamp)
inline QIcon touch() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    const QColor outline(0x1e, 0x1e, 0x1e);
    p.setPen(QPen(outline, 0.7));
    p.setBrush(QColor(0xe6, 0xe8, 0xf2));
    p.drawRoundedRect(QRectF(1.5, 2.5, 10.0, 11.0), 1.0, 1.0);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x3b, 0x8e, 0xea));
    p.drawRect(QRectF(2.0, 3.0, 9.0, 2.6));
    p.setPen(QPen(outline, 0.8));
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(11.5, 11.5), 3.6, 3.6);
    p.drawLine(QPointF(11.5, 11.5), QPointF(11.5, 9.3));
    p.drawLine(QPointF(11.5, 11.5), QPointF(13.0, 12.3));
  });
}

// Green tick (Ignored, when active)
inline QIcon check() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(QPen(QColor(0x2f, 0xc4, 0x4f), 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(3.0, 8.6), QPointF(6.6, 12.2));
    p.drawLine(QPointF(6.6, 12.2), QPointF(13.2, 4.6));
  });
}

// Printer (File Compare Report)
inline QIcon report() {
  return make([](QPainter &p, QIcon::Mode, QIcon::State) {
    const QColor outline(0x1e, 0x1e, 0x1e);
    p.setPen(QPen(outline, 0.7));
    p.setBrush(Qt::white);
    p.drawRect(QRectF(4.0, 1.8, 8.0, 5.0)); // paper in
    p.setBrush(QColor(0x9a, 0xa0, 0xb8));
    p.drawRoundedRect(QRectF(1.5, 6.0, 13.0, 6.0), 1.0, 1.0); // body
    p.setBrush(Qt::white);
    p.drawRect(QRectF(4.0, 10.0, 8.0, 4.2)); // paper out
    p.setPen(QPen(QColor(0x80, 0x84, 0x98), 0.7));
    p.drawLine(QPointF(5.2, 11.8), QPointF(10.8, 11.8));
    p.drawLine(QPointF(5.2, 13.2), QPointF(9.0, 13.2));
  });
}

// Double arrowheads (Synchronize submenu). Green = update, red = mirror.
inline QIcon chevrons(const QColor &fill, SyncDir dir) {
  return make([fill, dir](QPainter &p, QIcon::Mode, QIcon::State) {
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    auto head = [&p](double x, bool right) { // 6 wide, 9 tall
      const double tip = right ? x + 6.0 : x;
      const double base = right ? x : x + 6.0;
      p.drawPolygon(QPolygonF(
          {QPointF(base, 3.5), QPointF(tip, 8.0), QPointF(base, 12.5)}));
    };
    switch (dir) {
    case SyncDir::Right:
      head(1.5, true);
      head(8.0, true);
      break;
    case SyncDir::Left:
      head(1.5, false);
      head(8.0, false);
      break;
    case SyncDir::Both:
      head(1.5, false);
      head(8.5, true);
      break;
    }
  });
}
inline QIcon update(SyncDir dir) {
  return chevrons(QColor(0x3f, 0xe0, 0x4a), dir);
}
inline QIcon mirror(SyncDir dir) {
  return chevrons(QColor(0xf0, 0x50, 0x50), dir);
}

} // namespace menuicons

} // namespace icons

// ---------------------------------------------------------------------------
// Tree view: banded rows, status-coloured text, dotted connector lines
// ---------------------------------------------------------------------------

inline int treeRowHeight(const QFontMetrics &fm) {
  const int h = qMax(22, fm.height() + 6);
  return h + (h & 1); // keep even so the dotted lines stay in phase
}

// Row banding is derived from the row's position so the delegate (cells) and
// CompareTree::drawBranches (the indent area) always agree, and so both trees
// of a comparison band identically.
inline bool oddBand(const QAbstractItemView *view, int viewportTop,
                    int rowHeight) {
  const int scrolled = view && view->verticalScrollBar()
                           ? view->verticalScrollBar()->value()
                           : 0;
  return rowHeight > 0 &&
         (((viewportTop + scrolled * rowHeight) / rowHeight) & 1);
}

inline void paintRowBackground(QPainter *painter, const QRect &r, bool odd,
                               bool selected) {
  painter->fillRect(r, selected ? color::selection()
                                : (odd ? color::rowB() : color::rowA()));
  if (selected) {
    painter->setPen(color::selectionBorder());
    painter->drawLine(r.left(), r.top(), r.right(), r.top());
    painter->drawLine(r.left(), r.bottom(), r.right(), r.bottom());
  }
}

class CompareDelegate : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(treeRowHeight(option.fontMetrics));
    return size;
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QRect r = opt.rect;
    const bool selected = opt.state & QStyle::State_Selected;
    const auto *view = qobject_cast<const QAbstractItemView *>(opt.widget);

    painter->save();
    paintRowBackground(painter, r, oddBand(view, r.top(), r.height()),
                       selected);

    QRect content = r.adjusted(6, 0, -6, 0);
    if (index.column() == 0 && !opt.icon.isNull()) {
      constexpr int iconSize = 16;
      const QRect iconRect(content.left(),
                           r.top() + (r.height() - iconSize) / 2, iconSize,
                           iconSize);
      const bool open = opt.state & QStyle::State_Open;
      opt.icon.paint(painter, iconRect, Qt::AlignCenter, QIcon::Normal,
                     open ? QIcon::On : QIcon::Off);
      content.setLeft(iconRect.right() + 6);
    } else if (index.column() == 1 && !opt.icon.isNull()) {
      constexpr int iconSize = 16;
      const QRect iconRect(content.left() + (content.width() - iconSize) / 2,
                           r.top() + (r.height() - iconSize) / 2, iconSize,
                           iconSize);
      opt.icon.paint(painter, iconRect, Qt::AlignCenter);
    }

    const QBrush fg = index.data(Qt::ForegroundRole).value<QBrush>();
    painter->setPen(fg.style() == Qt::NoBrush ? color::folderText() : fg.color());
    painter->setFont(opt.font);
    const Qt::Alignment align = opt.displayAlignment & Qt::AlignHorizontal_Mask
                                    ? opt.displayAlignment
                                    : Qt::AlignLeft | Qt::AlignVCenter;
    painter->drawText(
        content, static_cast<int>(align | Qt::AlignVCenter),
        opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, content.width()));
    painter->restore();
  }
};

class CompareTree : public QTreeWidget {
public:
  explicit CompareTree(QWidget *parent = nullptr) : QTreeWidget(parent) {
    setColumnCount(6);
    setHeaderLabels(
        {::openbc::i18n::t("common.name"), ::openbc::i18n::t("style.status"), ::openbc::i18n::t("style.ext"), ::openbc::i18n::t("style.size"), ::openbc::i18n::t("style.modified"), ::openbc::i18n::t("common.attributes")});
    setItemDelegate(new CompareDelegate(this));
    setRootIsDecorated(true);
    setIndentation(20);
    setUniformRowHeights(true);
    setAlternatingRowColors(false); // banding is painted by the delegate
    setAllColumnsShowFocus(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setEditTriggers(QAbstractItemView::EditKeyPressed);
    setFrameShape(QFrame::NoFrame);
    setTextElideMode(Qt::ElideRight);

    QHeaderView *h = header();
    h->setStretchLastSection(false);
    h->setSectionsClickable(false);
    h->setHighlightSections(false);
    h->setMinimumSectionSize(36);
    h->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column) {
      h->setSectionResizeMode(column, QHeaderView::Interactive);
    }
    h->resizeSection(1, 30);
    h->resizeSection(2, 64);
    h->resizeSection(3, 110);
    h->resizeSection(4, 150);
    h->resizeSection(5, 78);
    headerItem()->setTextAlignment(1, Qt::AlignCenter);
    headerItem()->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        event->modifiers().testFlag(Qt::ControlModifier)) {
      if (auto *item = itemAt(event->pos())) {
        // NoUpdate: the default command under ExtendedSelection is ClearAndSelect, which would
        // drop every other selected row before the toggle below.
        setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        item->setSelected(!item->isSelected());
        event->accept();
        return;
      }
    }
    QTreeWidget::mousePressEvent(event);
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_F2) {
      if (auto *item = currentItem()) {
        editItem(item, 0);
        event->accept();
        return;
      }
    }
    QTreeWidget::keyPressEvent(event);
  }

  // Classic tree look: dotted connectors between siblings, small [+]/[-]
  // boxes for expandable folders. Top-level rows get no connectors.
  void drawBranches(QPainter *painter, const QRect &rect,
                    const QModelIndex &index) const override {
    if (!index.isValid()) {
      return;
    }
    const int rowH = rect.height();
    const bool selected =
        selectionModel() && selectionModel()->isSelected(index);
    const bool odd = oddBand(this, rect.top(), rowH);
    const QColor bg =
        selected ? color::selection() : (odd ? color::rowB() : color::rowA());

    painter->save();
    paintRowBackground(painter, rect, odd, selected);
    painter->setRenderHint(QPainter::Antialiasing, false);

    std::vector<QModelIndex> chain; // root ancestor ... index
    for (QModelIndex i = index; i.isValid(); i = i.parent()) {
      chain.push_back(i);
    }
    std::reverse(chain.begin(), chain.end());
    const int depth = static_cast<int>(chain.size()) - 1;
    const int ind = indentation();
    const int top = rect.top();
    const int bottom = top + rowH - 1;
    const int mid = top + rowH / 2;
    auto cx = [&](int level) { return rect.left() + level * ind + ind / 2; };

    QPen dotted(color::treeLine());
    dotted.setStyle(Qt::CustomDashLine);
    dotted.setDashPattern({1.0, 1.0});
    painter->setPen(dotted);

    // Vertical trunks of every ancestor level that still continues.
    for (int level = 0; level < depth; ++level) {
      const bool next = hasNextVisibleSibling(chain[level + 1]);
      const int x = cx(level);
      if (level < depth - 1) {
        if (next) {
          painter->drawLine(x, top, x, bottom);
        }
      } else {
        painter->drawLine(x, top, x, next ? bottom : mid);
      }
    }

    const bool expandable = model()->hasChildren(index);
    if (depth >= 1) {
      const int x0 = cx(depth - 1);
      const int x1 = expandable ? cx(depth) - 5 : cx(depth) + 3;
      if (x1 > x0) {
        painter->drawLine(x0, mid, x1, mid);
      }
    }

    if (expandable) {
      const bool open = isExpanded(index);
      const int x = cx(depth);
      painter->setBrush(bg);
      painter->setPen(QPen(color::treeLine(), 1));
      painter->drawRect(QRect(x - 4, mid - 4, 8, 8));
      painter->setPen(QPen(theme::get(theme::Token::treeLineLeaf), 1));
      painter->drawLine(x - 2, mid, x + 2, mid);
      if (!open) {
        painter->drawLine(x, mid - 2, x, mid + 2);
      } else {
        painter->setPen(dotted);
        painter->drawLine(x, mid + 5, x, bottom);
      }
    }
    painter->restore();
  }

private:
  bool hasNextVisibleSibling(const QModelIndex &idx) const {
    const QModelIndex parent = idx.parent();
    const int count = model()->rowCount(parent);
    for (int row = idx.row() + 1; row < count; ++row) {
      if (!isRowHidden(row, parent)) {
        return true;
      }
    }
    return false;
  }
};

} // namespace openbc::ui
// appearance_controller.h
// ---------------------------------------------------------------------------
// Applies Tools > Options > Appearance to the running application. This is the ONLY place that
// turns the committed options into behaviour for that page:
//
//   theme (Follow system / Light / Dark) + Preview   -> palette + application style sheet
//   colours, stripes, centre gradient, folder icons  -> style/qt_style.h via options/appearance_state.h
//   font                                             -> options/text_font.h (Text Compare editors)
//
// It also owns the View > Dark Theme action, which is just a shortcut that commits Light/Dark.
// Persistence lives in OptionsStore (options/app_options.h); the page UI in options/options_dialog.h.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QObject>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QWidget>

#include "options/app_options.h"
#include "options/appearance_state.h"
#include "options/text_font.h"
#include "style/qt_style.h"

namespace openbc::app {

class AppearanceController {
public:
    // `darkThemeAction` stays owned by its parent (the main window); `context` bounds the connections.
    AppearanceController(QApplication& application, OptionsStore& store, QAction* darkThemeAction,
                         QObject* context)
        : application_(application), store_(store), darkThemeAction_(darkThemeAction), context_(context) {}
    AppearanceController(const AppearanceController&) = delete;
    AppearanceController& operator=(const AppearanceController&) = delete;

    // Hooks the store, the View > Dark Theme action and the system colour scheme, then applies the
    // committed options once.
    void install() {
        store_.addListener([this](const AppOptions& options) { apply(options); });
        store_.previewTheme = [this](ThemeChoice choice) { applyTheme(choice); };
        darkThemeAction_->setCheckable(true);
        QObject::connect(darkThemeAction_, &QAction::toggled, context_, [this](bool dark) {
            AppOptions next = store_.options();
            next.theme = dark ? ThemeChoice::Dark : ThemeChoice::Light;
            store_.commit(next);
        });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, context_, [this]() {
            if (store_.options().theme == ThemeChoice::FollowSystem) applyTheme(ThemeChoice::FollowSystem);
        });
#endif
        apply(store_.options());
    }

private:
    static bool isDark(ThemeChoice choice) {
        if (choice == ThemeChoice::Light) return false;
        if (choice == ThemeChoice::Dark) return true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        return QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
#else
        return true;   // no system query before Qt 6.5: keep the app's original dark look
#endif
    }

    void applyTheme(ThemeChoice choice) {
        const bool dark = isDark(choice);
        application_.setPalette(dark ? openbc::ui::darkPalette() : openbc::ui::lightPalette());
        application_.setStyleSheet(openbc::ui::applicationStyleSheet(dark ? openbc::ui::Theme::Dark
                                                                           : openbc::ui::Theme::Light));
        {
            const QSignalBlocker block(darkThemeAction_);
            darkThemeAction_->setChecked(dark);
        }
        for (QWidget* widget : QApplication::allWidgets()) widget->update();   // custom-painted rows
    }

    // Colours first (the style sheet built by applyTheme reads them), then the theme, then the font.
    void apply(const AppOptions& o) {
        namespace ui = openbc::ui;
        auto& state = ui::appearance::state();
        state.colors.clear();
        for (auto it = o.colors.constBegin(); it != o.colors.constEnd(); ++it) {
            const int slash = it.key().indexOf('/');
            const ItemColors def = AppOptions::defaultColors(it.key().left(slash), it.key().mid(slash + 1));
            ui::appearance::Entry entry;   // only what differs from the default counts as a user choice
            if (it.value().text != def.text) entry.text = it.value().text;
            if (it.value().background != def.background) entry.background = it.value().background;
            if (it.value().folderInfo != def.folderInfo) entry.info = it.value().folderInfo;
            if (entry.text.isValid() || entry.background.isValid() || entry.info.isValid())
                state.colors.insert(it.key(), entry);
        }
        state.stripes = o.useStripes;
        state.gradientCenter = o.useGradientForCenterBackground;
        state.colorFolders = o.colorFoldersToReflectContent;
        ++state.revision;
        applyTheme(o.theme);
        TextFont::instance().configure(o.useSystemFont, o.customFont);   // also repaints text diffs
    }

    QApplication& application_;
    OptionsStore& store_;
    QAction* darkThemeAction_;
    QObject* context_;
};

}  // namespace openbc::app

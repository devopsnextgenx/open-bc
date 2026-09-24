// main_window.h
// ---------------------------------------------------------------------------
// Application shell: menu bar, tab widget, session/tab lifecycle
// (add/close/duplicate), and openbc_run_gui() itself. Extracted from the
// tail of the original qt_app.cpp with one functional fix: rebuildMenus()
// previously only populated the Actions/Edit/Search/View menus for a
// CompareSession (folder-compare) tab, leaving them silently empty for a
// TextCompareView (text-compare) tab - now it fills them from
// TextCompareView's own menu-item accessors instead, which is how
// Find/Replace, Undo/Redo, the minimap toggle, etc. actually reach the menu
// bar for a text-compare tab.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QPoint>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <functional>

#include "compare_session.h"
#include "qt_style.h"
#include "text_compare_view.h"

namespace openbc::app {

extern "C" int openbc_run_gui() {
    int argc = 1;
    char application_name[] = "openbc-qt";
    char* argv[] = {application_name, nullptr};
    QApplication application(argc, argv);

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    application.setPalette(openbc::ui::darkPalette());
    application.setStyleSheet(openbc::ui::applicationStyleSheet(openbc::ui::Theme::Dark));

    QMainWindow window;
    window.resize(1280, 820);

    auto* sessionMenu = window.menuBar()->addMenu("&Session");
    auto* actionsMenu = window.menuBar()->addMenu("&Actions");
    auto* editMenu = window.menuBar()->addMenu("&Edit");
    auto* searchMenu = window.menuBar()->addMenu("&Search");
    auto* viewMenu = window.menuBar()->addMenu("&View");
    auto* toolsMenu = window.menuBar()->addMenu("&Tools");
    window.menuBar()->addMenu("&Help");

    auto* tabs = new QTabWidget(&window);
    tabs->setDocumentMode(true);
    tabs->setMovable(true);
    tabs->setElideMode(Qt::ElideRight);
    tabs->setUsesScrollButtons(true);
    tabs->tabBar()->setDrawBase(false);
    window.setCentralWidget(tabs);

    auto currentSession = [&]() { return dynamic_cast<CompareSession*>(tabs->currentWidget()); };
    auto* darkThemeAction = new QAction("Dark Theme", &window);
    darkThemeAction->setCheckable(true);
    darkThemeAction->setChecked(true);
    QObject::connect(darkThemeAction, &QAction::toggled, &window, [&](bool dark) {
        const auto theme = dark ? openbc::ui::Theme::Dark : openbc::ui::Theme::Light;
        application.setPalette(dark ? openbc::ui::darkPalette() : openbc::ui::lightPalette());
        application.setStyleSheet(openbc::ui::applicationStyleSheet(theme));
    });

    // The Actions / Edit / Search / View / Tools menus always show the actions
    // of the tab that is currently selected, so a command can only ever affect
    // the tab you are looking at.
    auto rebuildMenus = [&]() {
        auto fill = [](QMenu* menu, const QList<QAction*>& items) {
            menu->clear();
            for (QAction* action : items) {
                if (action) {
                    menu->addAction(action);
                } else {
                    menu->addSeparator();
                }
            }
        };
        // The Actions/Edit/Search/View/Tools menus reflect whichever kind of
        // tab is active: a folder-compare (CompareSession) tab has all
        // five; a text-compare (TextCompareView) tab has no Tools menu of
        // its own, so that one is simply left empty rather than stale.
        CompareSession* session = currentSession();
        auto* textView = dynamic_cast<TextCompareView*>(tabs->currentWidget());
        fill(actionsMenu, session   ? session->actionsMenuItems()
                          : textView ? textView->actionsMenuItems()
                                     : QList<QAction*>());
        fill(editMenu, session   ? session->editMenuItems()
                       : textView ? textView->editMenuItems()
                                  : QList<QAction*>());
        fill(searchMenu, session   ? session->searchMenuItems()
                         : textView ? textView->searchMenuItems()
                                    : QList<QAction*>());
        fill(viewMenu, session   ? session->viewMenuItems()
                       : textView ? textView->viewMenuItems()
                                  : QList<QAction*>());
        viewMenu->addSeparator();
        viewMenu->addAction(darkThemeAction);
        if (textView) viewMenu->addAction(textView->syntaxStyleMenuAction());
        fill(toolsMenu, session ? session->toolsMenuItems() : QList<QAction*>());
    };
    auto updateWindowTitle = [&]() {
        CompareSession* session = currentSession();
        if (session) {
            window.setWindowTitle(session->title() + " - Folder Compare - OpenBC");
        } else if (auto* textView = dynamic_cast<TextCompareView*>(tabs->currentWidget())) {
            window.setWindowTitle(textView->title() + " - Text Compare - OpenBC");
        } else {
            window.setWindowTitle("OpenBC - Folder Compare");
        }
    };

    std::function<CompareSession*(const QString&, const QString&, bool)> addSession;
    std::function<void(int)> closeTab;

    addSession = [&](const QString& left, const QString& right, bool run) -> CompareSession* {
        auto* session = new CompareSession;
        session->setPaths(left, right);
        const int index =
            tabs->addTab(session, openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow()),
                         session->title());
        tabs->setTabToolTip(index, session->detailedTitle());

        auto* close = new QToolButton;
        close->setObjectName("tabClose");
        close->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Close));
        close->setToolTip("Close tab (Ctrl+W)");
        close->setFocusPolicy(Qt::NoFocus);
        close->setAutoRaise(true);
        tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
        QObject::connect(close, &QToolButton::clicked, session,
                         [&, session]() { closeTab(tabs->indexOf(session)); });

        session->onTitleChanged = [&, session]() {
            const int i = tabs->indexOf(session);
            if (i < 0) {
                return;
            }
            tabs->setTabText(i, session->title());
            tabs->setTabToolTip(i, session->detailedTitle());
            if (tabs->currentIndex() == i) {
                updateWindowTitle();
            }
        };
        session->onTextCompare = [&](const QString& leftPath, const QString& rightPath,
                                     const QString& leftText, const QString& rightText) {
            auto* textView = new TextCompareView(leftPath, rightPath, leftText, rightText);
            const int textIndex = tabs->addTab(
                textView, openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Compare),
                textView->title());
            tabs->setTabToolTip(textIndex, leftPath + " <-> " + rightPath);
            auto* textClose = new QToolButton;
            textClose->setObjectName("tabClose");
            textClose->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Close));
            textClose->setToolTip("Close tab (Ctrl+W)");
            textClose->setFocusPolicy(Qt::NoFocus);
            textClose->setAutoRaise(true);
            tabs->tabBar()->setTabButton(textIndex, QTabBar::RightSide, textClose);
            QObject::connect(textClose, &QToolButton::clicked, textView,
                             [&, textView]() { closeTab(tabs->indexOf(textView)); });
            // Home / Sessions both act like Beyond Compare: jump back to the
            // folder-compare session tab this text view was opened from.
            textView->onHomeRequested = [&, session]() {
                const int sessionIndex = tabs->indexOf(session);
                if (sessionIndex >= 0) tabs->setCurrentIndex(sessionIndex);
            };
            textView->onSessionsRequested = textView->onHomeRequested;
            textView->onTitleChanged = [&, textView]() {
                const int i = tabs->indexOf(textView);
                if (i < 0) return;
                tabs->setTabText(i, textView->title());
                if (tabs->currentIndex() == i) updateWindowTitle();
            };
            tabs->setCurrentIndex(textIndex);
        };
        tabs->setCurrentIndex(index);
        if (run) {
            session->refresh();
        }
        return session;
    };

    closeTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        auto* widget = tabs->widget(index);
        if (auto* session = dynamic_cast<CompareSession*>(widget)) {
            session->cancelRun();
        }
        tabs->removeTab(index);
        widget->deleteLater();
        if (tabs->count() == 0) {
            addSession(QString(), QString(), false);  // never leave the window empty
        }
    };

    auto duplicateTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        auto* source = dynamic_cast<CompareSession*>(tabs->widget(index));
        if (!source) {
            return;
        }
        auto* copy = addSession(source->leftText(), source->rightText(), false);
        copy->copySettingsFrom(*source);
        copy->refresh();
    };

    auto closeOthers = [&](int keep) {
        auto* keepWidget = tabs->widget(keep);
        for (int i = tabs->count() - 1; i >= 0; --i) {
            if (tabs->widget(i) != keepWidget) {
                closeTab(i);
            }
        }
    };

    // Session menu: these manage the tabs themselves.
    auto* newTab = sessionMenu->addAction("New Folder Compare");
    newTab->setShortcut(QKeySequence("Ctrl+T"));
    auto* duplicate = sessionMenu->addAction("Duplicate Tab");
    auto* openComparison = sessionMenu->addAction("Open comparison");
    sessionMenu->addSeparator();
    auto* closeCurrent = sessionMenu->addAction("Close Tab");
    closeCurrent->setShortcut(QKeySequence("Ctrl+W"));
    auto* closeOthersAction = sessionMenu->addAction("Close Other Tabs");
    sessionMenu->addSeparator();
    auto* exitAction = sessionMenu->addAction("Exit");
    exitAction->setShortcut(QKeySequence("Ctrl+Q"));

    QObject::connect(newTab, &QAction::triggered, [&]() { addSession(QString(), QString(), false); });
    QObject::connect(duplicate, &QAction::triggered, [&]() { duplicateTab(tabs->currentIndex()); });
    QObject::connect(openComparison, &QAction::triggered, [&]() {
        if (auto* session = currentSession()) session->log("Open comparison requested");
    });
    QObject::connect(closeCurrent, &QAction::triggered, [&]() { closeTab(tabs->currentIndex()); });
    QObject::connect(closeOthersAction, &QAction::triggered, [&]() { closeOthers(tabs->currentIndex()); });
    QObject::connect(exitAction, &QAction::triggered, &window, &QWidget::close);

    auto* plus = new QToolButton(tabs);
    plus->setObjectName("newTab");
    plus->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Plus));
    plus->setToolTip("New folder compare (Ctrl+T)");
    plus->setAutoRaise(true);
    tabs->setCornerWidget(plus, Qt::TopRightCorner);
    QObject::connect(plus, &QToolButton::clicked, [&]() { addSession(QString(), QString(), false); });

    tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(tabs->tabBar(), &QWidget::customContextMenuRequested, [&](const QPoint& pos) {
        const int index = tabs->tabBar()->tabAt(pos);
        if (index < 0) {
            return;
        }
        QMenu menu;
        QAction* dup = menu.addAction("Duplicate Tab");
        menu.addSeparator();
        QAction* close = menu.addAction("Close Tab");
        QAction* others = menu.addAction("Close Other Tabs");
        others->setEnabled(tabs->count() > 1);
        QAction* chosen = menu.exec(tabs->tabBar()->mapToGlobal(pos));
        if (chosen == dup) {
            duplicateTab(index);
        } else if (chosen == close) {
            closeTab(index);
        } else if (chosen == others) {
            closeOthers(index);
        }
    });

    QObject::connect(tabs, &QTabWidget::currentChanged, [&](int) {
        rebuildMenus();
        updateWindowTitle();
    });

    // if linux
    #ifdef Q_OS_LINUX
    addSession("/home/kira/tmp/source", "/home/kira/tmp/target", true);
    #endif
    // if windows
    #ifdef Q_OS_WINDOWS
    addSession("D:/personal/github/devopsnextgenx/open-bc/test/source", "D:/personal/github/devopsnextgenx/open-bc/test/target", true);
    #endif
    rebuildMenus();
    updateWindowTitle();
    window.show();

    return application.exec();
}

}  // namespace openbc::app

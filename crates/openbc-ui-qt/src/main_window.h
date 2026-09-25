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
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QKeySequence>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QPoint>
#include <QSettings>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <functional>

#include "folder_compare_view.h"
#include "backend_api.h"
#include "home_view.h"
#include "qt_style.h"
#include "session_history.h"
#include "text_compare_view.h"

namespace openbc::app {

class PersistedMainWindow : public QMainWindow {
public:
    explicit PersistedMainWindow(QSettings& preferences, QWidget* parent = nullptr)
        : QMainWindow(parent), preferences_(preferences) {
        const auto document = QJsonDocument::fromJson(
            preferences_.value("window/geometry").toString().toUtf8());
        const QJsonObject geometry = document.isObject() ? document.object() : QJsonObject();
        const bool maximized = geometry.value("maximized").toBool(true);
        const int width = geometry.value("width").toInt();
        const int height = geometry.value("height").toInt();
        if (width > 0 && height > 0) {
            setGeometry(geometry.value("x").toInt(), geometry.value("y").toInt(), width, height);
        }
        if (maximized) {
            setWindowState(windowState() | Qt::WindowMaximized);
        }
    }

    void saveWindowState() {
        const QRect restoredGeometry = normalGeometry().isValid() ? normalGeometry() : geometry();
        const QJsonObject geometry{{"x", restoredGeometry.x()},
                                   {"y", restoredGeometry.y()},
                                   {"width", restoredGeometry.width()},
                                   {"height", restoredGeometry.height()},
                                   {"maximized", isMaximized()}};
        preferences_.setValue(
            "window/geometry",
            QString::fromUtf8(QJsonDocument(geometry).toJson(QJsonDocument::Compact)));
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        saveWindowState();
        QMainWindow::closeEvent(event);
    }

private:
    QSettings& preferences_;
};

extern "C" int openbc_run_gui() {
    if (openbc_initialize_observability() != 0) {
        return 1;
    }
    int argc = 1;
    char application_name[] = "openbc-qt";
    char* argv[] = {application_name, nullptr};
    QApplication application(argc, argv);

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    application.setPalette(openbc::ui::darkPalette());
    application.setStyleSheet(openbc::ui::applicationStyleSheet(openbc::ui::Theme::Dark));
    QDir().mkpath(SessionHistory::rootPath());
    QSettings preferences(SessionHistory::rootPath() + "/preferences.ini", QSettings::IniFormat);

    PersistedMainWindow window(preferences);

    auto* sessionMenu = window.menuBar()->addMenu("&Session");
    auto* actionsMenu = window.menuBar()->addMenu("&Actions");
    auto* editMenu = window.menuBar()->addMenu("&Edit");
    auto* searchMenu = window.menuBar()->addMenu("&Search");
    auto* viewMenu = window.menuBar()->addMenu("&View");
    auto* toolsMenu = window.menuBar()->addMenu("&Tools");
    auto* helpMenu = window.menuBar()->addMenu("&Help");
    auto* downloadLog = helpMenu->addAction("Download Log...");
    auto* downloadInstrumentation = helpMenu->addAction("Download Instrumentation Report...");

    auto copyReport = [&](const QString& source, const QString& title, const QString& filter) {
        const QString destination = QFileDialog::getSaveFileName(&window, title, QDir::homePath(), filter);
        if (destination.isEmpty()) {
            return;
        }
        QFile::remove(destination);
        if (!QFile::copy(source, destination)) {
            openbc_log_message(reinterpret_cast<const std::uint8_t*>("report download failed"), 23);
        } else {
            openbc_log_message(reinterpret_cast<const std::uint8_t*>("report downloaded"), 17);
        }
    };
    QObject::connect(downloadLog, &QAction::triggered, [&]() {
        const QString username = qEnvironmentVariable("USER", "unknown");
        QString logPath = "/var/log/open-bc-" + username + ".log";
        if (!QFile::exists(logPath)) {
            logPath = QDir::homePath() + "/.config/open-bc/open-bc-" + username + ".log";
        }
        copyReport(logPath, "Download OpenBC log", "Log files (*.log)");
    });
    QObject::connect(downloadInstrumentation, &QAction::triggered, [&]() {
        const QString directory = QDir::homePath() + "/.config/open-bc/instrumentation";
        const QStringList reports = QDir(directory).entryList({"*.yml"}, QDir::Files, QDir::Time);
        if (!reports.isEmpty()) {
            copyReport(directory + "/" + reports.constFirst(), "Download instrumentation report", "YAML files (*.yml)");
        }
    });
    openbc_log_message(reinterpret_cast<const std::uint8_t*>("Qt GUI initialized"), 18);

    auto* tabs = new QTabWidget(&window);
    tabs->setDocumentMode(true);
    tabs->setMovable(true);
    tabs->setElideMode(Qt::ElideRight);
    tabs->setUsesScrollButtons(true);
    tabs->tabBar()->setDrawBase(false);
    window.setCentralWidget(tabs);

    auto* home = new HomeView;
    const int homeIndex = tabs->addTab(home, "Home");
    tabs->setTabToolTip(homeIndex, "OpenBC home and session history");

    auto currentSession = [&]() { return dynamic_cast<CompareSession*>(tabs->currentWidget()); };
    auto* darkThemeAction = new QAction("Dark Theme", &window);
    darkThemeAction->setCheckable(true);
    darkThemeAction->setChecked(preferences.value("theme/dark", true).toBool());
    QObject::connect(darkThemeAction, &QAction::toggled, &window, [&](bool dark) {
        const auto theme = dark ? openbc::ui::Theme::Dark : openbc::ui::Theme::Light;
        application.setPalette(dark ? openbc::ui::darkPalette() : openbc::ui::lightPalette());
        application.setStyleSheet(openbc::ui::applicationStyleSheet(theme));
        preferences.setValue("theme/dark", dark);
    });

    auto closeTextTabOnEscape = new QAction(&window);
    closeTextTabOnEscape->setShortcut(Qt::Key_Escape);
    closeTextTabOnEscape->setShortcutContext(Qt::ApplicationShortcut);
    window.addAction(closeTextTabOnEscape);

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
    std::function<void(const QString&, const QString&, const QString&, const QString&, CompareSession*)>
        addTextView;
    std::function<void(int)> closeTab;

    addSession = [&](const QString& left, const QString& right, bool run) -> CompareSession* {
        auto* session = new CompareSession(&preferences);
        session->setPaths(left, right);
        if (run && !left.isEmpty() && !right.isEmpty()) {
            SessionHistory::record("folder", left, right);
            home->refreshHistory();
        }
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
            addTextView(leftPath, rightPath, leftText, rightText, session);
        };
        tabs->setCurrentIndex(index);
        if (run) {
            session->refresh();
        }
        return session;
    };

    addTextView = [&](const QString& leftPath, const QString& rightPath, const QString& leftText,
                      const QString& rightText, CompareSession* origin) {
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
        textView->onHomeRequested = [&, origin]() {
            const int index = origin ? tabs->indexOf(origin) : homeIndex;
            if (index >= 0) tabs->setCurrentIndex(index);
        };
        textView->onSessionsRequested = textView->onHomeRequested;
        textView->onTitleChanged = [&, textView]() {
            const int i = tabs->indexOf(textView);
            if (i < 0) return;
            tabs->setTabText(i, textView->title());
            if (tabs->currentIndex() == i) updateWindowTitle();
        };
        SessionHistory::record("text", leftPath, rightPath);
        home->refreshHistory();
        tabs->setCurrentIndex(textIndex);
    };

    closeTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        auto* widget = tabs->widget(index);
        if (auto* session = dynamic_cast<CompareSession*>(widget)) {
            session->cancelRun();
            const QString left = session->leftText();
            const QString right = session->rightText();
            if (!left.isEmpty() || !right.isEmpty()) {
                SessionHistory::record("folder", left, right);
            }
        }
        if (widget == home) {
            return;
        }
        tabs->removeTab(index);
        widget->deleteLater();
        home->refreshHistory();
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
    QObject::connect(closeTextTabOnEscape, &QAction::triggered, [&]() {
        if (dynamic_cast<TextCompareView*>(tabs->currentWidget())) {
            closeTab(tabs->currentIndex());
        }
    });

    home->onNewFolderCompare = [&]() { addSession(QString(), QString(), false); };
    home->onNewTextCompare = [&]() {
        addTextView(QString(), QString(), QString(), QString(), nullptr);
    };
    home->onOpenHistory = [&](const SessionHistoryEntry& entry) {
        if (entry.kind == "folder") {
            addSession(entry.left, entry.right, true);
        } else if (!entry.left.isEmpty() || !entry.right.isEmpty()) {
            QFile leftFile(entry.left);
            QFile rightFile(entry.right);
            const QString leftText = leftFile.open(QIODevice::ReadOnly)
                                          ? QString::fromUtf8(leftFile.readAll()) : QString();
            const QString rightText = rightFile.open(QIODevice::ReadOnly)
                                           ? QString::fromUtf8(rightFile.readAll()) : QString();
            addTextView(entry.left, entry.right, leftText, rightText, nullptr);
        }
    };

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

    tabs->setCurrentIndex(homeIndex);
    rebuildMenus();
    updateWindowTitle();
    window.show();

    return application.exec();
}

}  // namespace openbc::app

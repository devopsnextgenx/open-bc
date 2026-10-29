// main_window.h
// ---------------------------------------------------------------------------
// Application shell: menu bar, tab widget, session/tab lifecycle
// (add/close/duplicate), and openbc_run_gui() itself. Extracted from the
// tail of the original qt_app.cpp with one functional fix: rebuildMenus()
// previously only populated the Actions/Edit/Search/View menus for a
// FolderCompareViewer (folder-compare) tab, leaving them silently empty for a
// TextCompareViewer (text-compare) tab - now it fills them from
// TextCompareViewer's own menu-item accessors instead, which is how
// Find/Replace, Undo/Redo, the minimap toggle, etc. actually reach the menu
// bar for a text-compare tab.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QKeySequence>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QPlainTextEdit>
#include <QThreadPool>
#include <QTimer>
#include <QSettings>
#include <QStatusBar>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <functional>
#include <optional>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include "viewers/model3d/model3d_viewport.h"
#include "home/home_view.h"
#include "viewers/model3d/model3d_compare_viewer.h"
#include "viewers/picture/picture_compare_viewer.h"
#include "viewers/table/table_compare_viewer.h"
#include "viewers/registry/registry_compare_viewer.h"
#include "viewers/hex/hex_compare_viewer.h"
#include "viewers/media/media_compare_viewer.h"
#include "viewers/folder/folder_compare_viewer.h"
#include "viewers/text/text_compare_viewer.h"
#include "backend/backend_api.h"
#include "common/drop_filter.h"
#include "common/drop_payload.h"
#include "common/drop_router.h"
#include "home/home_view.h"
#include "style/qt_style.h"
#include "home/session_history.h"
#include "app/cli_options.h"
#include "app/dependency_check.h"
#include "app/app_options.h"
#include "app/options_dialog.h"
#include "app/single_instance.h"
#include "app/app_info.h"
#include "app/about_dialog.h"
namespace openbc::app {

class PersistedMainWindow : public QMainWindow {
public:
    // Set by the tab host: true when every tab with unsaved edits may be closed.
    std::function<bool()> confirmAllTabsClose;

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
        if (confirmAllTabsClose && !confirmAllTabsClose()) {
            event->ignore();
            return;
        }
        saveWindowState();
        QMainWindow::closeEvent(event);
    }

private:
    QSettings& preferences_;
};

extern "C" int openbc_run_gui(int argc, char** argv) {
    // --- Command line (runs before QApplication so --help / --dependency-check never need a display)
    QStringList cliArguments;
    for (int i = 1; i < argc; ++i) cliArguments << QString::fromLocal8Bit(argv[i]);
    const LaunchRequest launch = parseLaunchArguments(cliArguments);
    if (!launch.error.isEmpty()) {
        printToConsole("openbc: " + launch.error + "\n", true);
        return 64;
    }
    if (launch.showHelp) {
        printToConsole(cliHelpText());
        return 0;
    }
    if (launch.showVersion) {
        printToConsole("openbc " + appVersion() + "\n");
        return 0;
    }
    if (launch.dependencyCheck) {
        const auto report = openbc::depcheck::check(currentExecutablePath().toStdString());
        printToConsole(QString::fromStdString(openbc::depcheck::format(report)));
        return report.ok() ? 0 : 2;
    }
    if (!preflightQtRuntime()) {
        return 1;
    }
    if (openbc_initialize_observability() != 0) {
        return 1;
    }
    #ifdef Q_OS_WIN
    qputenv("QT_MEDIA_BACKEND", "windows");   // Windows Media Foundation; no bundled codecs
    #endif
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);   // <-- new, must precede QApplication
    QApplication application(argc, argv);
    // Wayland app_id / X11 class. MUST equal the .desktop file name without ".desktop" and the
    // Flatpak app-id, or GNOME cannot tie this window to its launcher and shows a generic dock icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.devopsnextgenx.open-bc"));
    SingleInstance singleInstance(launch);
    if (!singleInstance.isPrimary()) {
        return singleInstance.wasForwarded() ? 0 : 1;
    }
    // Prefer the installed theme icon (same one the launcher uses); fall back to the embedded PNG
    // when running straight from the build tree.
    QApplication::setWindowIcon(QIcon::fromTheme(
        QStringLiteral("io.github.devopsnextgenx.open-bc"), QIcon(QStringLiteral(":/icons/openbc.png"))));

    QApplication::setStyle(QStyleFactory::create("Fusion"));
    application.setPalette(openbc::ui::darkPalette());
    application.setStyleSheet(openbc::ui::applicationStyleSheet(openbc::ui::Theme::Dark));
    QDir().mkpath(SessionHistory::rootPath());
    QSettings preferences(SessionHistory::rootPath() + "/preferences.ini", QSettings::IniFormat);

    PersistedMainWindow window(preferences);
    window.setWindowIcon(QApplication::windowIcon());

    // Tools > Options. The store holds the committed options (mocked for now:
    // in-memory only, see app_options.h); the dialog edits a copy of them.
    OptionsStore optionsStore;

    // Put the main window into GL-composed mode before it is first shown. Otherwise the first
    // 3D tab flips the backing store and Qt recreates the native window mid-session, which
    // shows up as the window minimizing/maximizing. 1x1, invisible, never interacted with.
    {
        auto* glWarmup = new QOpenGLWidget(&window);
        glWarmup->setFormat(openbc::app::model3dSurfaceFormat());
        glWarmup->setFixedSize(1, 1);
        glWarmup->move(0, 0);
        glWarmup->setAttribute(Qt::WA_TransparentForMouseEvents);
        glWarmup->lower();
    }

    auto* sessionMenu = window.menuBar()->addMenu("&Session");
    auto* actionsMenu = window.menuBar()->addMenu("&Actions");
    auto* editMenu = window.menuBar()->addMenu("&Edit");
    auto* searchMenu = window.menuBar()->addMenu("&Search");
    auto* viewMenu = window.menuBar()->addMenu("&View");
    auto* toolsMenu = window.menuBar()->addMenu("&Tools");
    auto* helpMenu = window.menuBar()->addMenu("&Help");
    auto* aboutAction = helpMenu->addAction("About");
    QObject::connect(aboutAction, &QAction::triggered, &window, [&]() { showAboutDialog(&window); });
    // auto* downloadLog = helpMenu->addAction("Download Log...");
    // auto* downloadInstrumentation = helpMenu->addAction("Download Instrumentation Report...");

    // auto copyReport = [&](const QString& source, const QString& title, const QString& filter) {
    //     const QString destination = QFileDialog::getSaveFileName(&window, title, QDir::homePath(), filter);
    //     if (destination.isEmpty()) {
    //         return;
    //     }
    //     QFile::remove(destination);
    //     if (!QFile::copy(source, destination)) {
    //         openbc_log_message(reinterpret_cast<const std::uint8_t*>("report download failed"), 23);
    //     } else {
    //         openbc_log_message(reinterpret_cast<const std::uint8_t*>("report downloaded"), 17);
    //     }
    // };
    // QObject::connect(downloadLog, &QAction::triggered, [&]() {
    //     const QString username = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "unknown"))   /* USERNAME on Windows */;
    //     QString logPath = "/var/log/open-bc-" + username + ".log";
    //     if (!QFile::exists(logPath)) {
    //         logPath = QDir::homePath() + "/.config/open-bc/open-bc-" + username + ".log";
    //     }
    //     copyReport(logPath, "Download OpenBC log", "Log files (*.log)");
    // });
    // QObject::connect(downloadInstrumentation, &QAction::triggered, [&]() {
    //     const QString directory = QDir::homePath() + "/.config/open-bc/instrumentation";
    //     const QStringList reports = QDir(directory).entryList({"*.yml"}, QDir::Files, QDir::Time);
    //     if (!reports.isEmpty()) {
    //         copyReport(directory + "/" + reports.constFirst(), "Download instrumentation report", "YAML files (*.yml)");
    //     }
    // });
    openbc_log_message(reinterpret_cast<const std::uint8_t*>("Qt GUI initialized"), 18);

    auto* tabs = new QTabWidget(&window);
    tabs->setDocumentMode(true);
    tabs->setMovable(true);
    tabs->setElideMode(Qt::ElideRight);
    tabs->setUsesScrollButtons(true);
    tabs->tabBar()->setDrawBase(false);
    window.setCentralWidget(tabs);

    auto* home = new HomeView;
    home->enableUpdateNotices(&preferences);  // bottom-right "update available" / once-per-install "what's new"
    const int homeIndex = tabs->addTab(home, "Home");
    tabs->setTabToolTip(homeIndex, "OpenBC home and session history");

    auto currentSession = [&]() { return dynamic_cast<FolderCompareViewer*>(tabs->currentWidget()); };
    auto registerInstrumentationSession = [](const QString& sessionId, const QString& label) {
        const QByteArray idBytes = sessionId.toUtf8();
        const QByteArray labelBytes = label.toUtf8();
        openbc_register_instrumentation_session(
            reinterpret_cast<const std::uint8_t*>(idBytes.constData()), idBytes.size(),
            reinterpret_cast<const std::uint8_t*>(labelBytes.constData()), labelBytes.size());
    };
    auto* darkThemeAction = new QAction("Dark Theme", &window);
    darkThemeAction->setCheckable(true);
    darkThemeAction->setChecked(preferences.value("theme/dark", true).toBool());
    QObject::connect(darkThemeAction, &QAction::toggled, &window, [&](bool dark) {
        const auto theme = dark ? openbc::ui::Theme::Dark : openbc::ui::Theme::Light;
        application.setPalette(dark ? openbc::ui::darkPalette() : openbc::ui::lightPalette());
        application.setStyleSheet(openbc::ui::applicationStyleSheet(theme));
        preferences.setValue("theme/dark", dark);
    });

    // Tools > Options... lives in every tab's Tools menu (rebuildMenus re-adds it
    // after the tab's own tools, since it clears that menu on each tab switch).
    auto* optionsAction = new QAction("&Options...", &window);
    QObject::connect(optionsAction, &QAction::triggered, &window,
                     [&]() { OptionsDialog::edit(optionsStore, &window); });

    // Esc closes the current compare tab - every viewer except Home and the folder compare
    // viewer (Esc there is too easy to hit by accident; it holds the session's context).
    auto* closeTabOnEscape = new QAction(&window);
    closeTabOnEscape->setShortcut(Qt::Key_Escape);
    closeTabOnEscape->setShortcutContext(Qt::ApplicationShortcut);
    window.addAction(closeTabOnEscape);

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
        // tab is active: a folder-compare (FolderCompareViewer) tab has all
        // five; a text-compare (TextCompareViewer) tab has no Tools menu of
        // its own, so that one is simply left empty rather than stale.
        FolderCompareViewer* session = currentSession();
        QWidget* current = tabs->currentWidget();
        auto* textView = dynamic_cast<TextCompareViewer*>(current);
        auto* pictureView = dynamic_cast<PictureCompareViewer*>(current);
        auto* model3dView = dynamic_cast<Model3dCompareViewer*>(current);
        auto* tableView = dynamic_cast<TableCompareViewer*>(current);
        auto* registryView = dynamic_cast<RegistryCompareViewer*>(current);
        auto* hexView = dynamic_cast<HexCompareViewer*>(current);
        auto* mediaView = dynamic_cast<MediaCompareViewer*>(current);
        fill(actionsMenu, session   ? session->actionsMenuItems()
                          : textView ? textView->actionsMenuItems()
                          : pictureView ? pictureView->actionsMenuItems()
                          : model3dView ? model3dView->actionsMenuItems()
                          : tableView ? tableView->actionsMenuItems()
                          : registryView ? registryView->actionsMenuItems()
                          : hexView ? hexView->actionsMenuItems()
                          : mediaView ? mediaView->actionsMenuItems()
                                     : QList<QAction*>());
        fill(editMenu, session   ? session->editMenuItems()
                       : textView ? textView->editMenuItems()
                       : pictureView ? pictureView->editMenuItems()
                       : model3dView ? model3dView->editMenuItems()
                       : tableView ? tableView->editMenuItems()
                       : registryView ? registryView->editMenuItems()
                                  : QList<QAction*>());
        fill(searchMenu, session   ? session->searchMenuItems()
                         : textView ? textView->searchMenuItems()
                         : pictureView ? pictureView->searchMenuItems()
                         : model3dView ? model3dView->searchMenuItems()
                         : tableView ? tableView->searchMenuItems()
                         : registryView ? registryView->searchMenuItems()
                         : hexView ? hexView->searchMenuItems()
                                    : QList<QAction*>());
        fill(viewMenu, session   ? session->viewMenuItems()
                       : textView ? textView->viewMenuItems()
                       : pictureView ? pictureView->viewMenuItems()
                       : model3dView ? model3dView->viewMenuItems()
                       : tableView ? tableView->viewMenuItems()
                       : registryView ? registryView->viewMenuItems()
                                  : QList<QAction*>());
        viewMenu->addSeparator();
        viewMenu->addAction(darkThemeAction);
        if (textView) viewMenu->addAction(textView->syntaxStyleMenuAction());
        fill(toolsMenu, session   ? session->toolsMenuItems()
                        : pictureView ? pictureView->toolsMenuItems()
                        : model3dView ? model3dView->toolsMenuItems()
                                      : QList<QAction*>());
        if (!toolsMenu->isEmpty()) toolsMenu->addSeparator();
        toolsMenu->addAction(optionsAction);
    };
    auto updateWindowTitle = [&]() {
        FolderCompareViewer* session = currentSession();
        if (session) {
            window.setWindowTitle(session->title() + " - Folder Compare - OpenBC");
        } else if (auto* textView = dynamic_cast<TextCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(textView->title() + " - Text Compare - OpenBC");
        } else if (auto* pictureView = dynamic_cast<PictureCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(pictureView->title() + " - Picture Compare - OpenBC");
        } else if (auto* model3dView = dynamic_cast<Model3dCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(model3dView->title() + " - 3D Model Compare - OpenBC");
        } else if (auto* tableView = dynamic_cast<TableCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(tableView->title() + " - Table Compare - OpenBC");
        } else if (auto* registryView = dynamic_cast<RegistryCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(registryView->title() + " - Registry Compare - OpenBC");
        } else if (auto* hexView = dynamic_cast<HexCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(hexView->title() + " - Hex Compare - OpenBC");
        } else if (auto* mediaView = dynamic_cast<MediaCompareViewer*>(tabs->currentWidget())) {
            window.setWindowTitle(mediaView->title() + " - Media Compare - OpenBC");
        } else {
            window.setWindowTitle("OpenBC - Folder Compare");
        }
    };

    std::function<FolderCompareViewer*(const QString&, const QString&, bool)> addSession;
    std::function<void(const QString&, const QString&, const QString&, const QString&, FolderCompareViewer*)>
        addTextView;
    std::function<void(const QString&, const QString&, bool)> addPictureView;
    std::function<void(const QString&, const QString&, bool)> addModel3dView;
    std::function<void(const QString&, const QString&)> addTableView;
    std::function<void(const QString&, const QString&)> addRegistryView;
    std::function<void(const QString&, const QString&)> addHexView;
    std::function<void(const QString&, const QString&)> addMediaView;
    std::function<void(int)> closeTab;
    std::function<void(QWidget*)> installDropRouting;  // defined with the drop coordinator below

    // One place builds the tab's close button so every tab looks and behaves the same.
    auto attachCloseButton = [&](int index, QWidget* view) {
        auto* close = new QToolButton;
        close->setObjectName("tabClose");
        close->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Close));
        close->setToolTip("Close tab (Ctrl+W)");
        close->setFocusPolicy(Qt::NoFocus);
        close->setAutoRaise(true);
        tabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
        QObject::connect(close, &QToolButton::clicked, view, [&, view]() { closeTab(tabs->indexOf(view)); });
        // Every tab goes through here exactly once, so every tab gets the shared drop routing.
        if (installDropRouting) installDropRouting(view);
    };

    // Shared by the table / registry / hex / media viewers: tab + close button, title and
    // Home/Sessions hooks, and the session-history record (full session kind, e.g.
    // "table-compare"). Both paths empty (a tile opened with nothing chosen) is not recorded.
    auto addViewerTab = [&](auto* view, const QString& sessionKind) {
        const int index = tabs->addTab(view, sessionTypeIcon(sessionKind), view->title());
        tabs->setTabToolTip(index, view->detailedTitle());
        attachCloseButton(index, view);
        view->onHomeRequested = [&]() { tabs->setCurrentIndex(homeIndex); };
        view->onSessionsRequested = view->onHomeRequested;
        view->onTitleChanged = [&, view]() {
            const int i = tabs->indexOf(view);
            if (i < 0) return;
            tabs->setTabText(i, view->title());
            tabs->setTabToolTip(i, view->detailedTitle());
            if (tabs->currentIndex() == i) updateWindowTitle();
        };
        if (!view->leftText().isEmpty() || !view->rightText().isEmpty()) {
            SessionHistory::record(sessionKind, view->leftText(), view->rightText());
            home->refreshHistory();
        }
        tabs->setCurrentIndex(index);
    };

    addSession = [&](const QString& left, const QString& right, bool run) -> FolderCompareViewer* {
        auto* session = new FolderCompareViewer(&preferences);
        session->setPaths(left, right);
        registerInstrumentationSession(session->instrumentationSessionId(), session->title());
        session->onComparisonRequested = [&](const QString& requestedLeft, const QString& requestedRight) {
            SessionHistory::record("folder", requestedLeft, requestedRight);
            home->refreshHistory();
        };
        const int index =
            tabs->addTab(session, openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow()),
                         session->title());
        tabs->setTabToolTip(index, session->detailedTitle());

        attachCloseButton(index, session);

        session->onTitleChanged = [&, session]() {
            registerInstrumentationSession(session->instrumentationSessionId(), session->title());
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
        session->onPictureCompare = [&](const QString& leftPath, const QString& rightPath) {
            addPictureView(leftPath, rightPath, true);
        };
        session->onModel3dCompare = [&](const QString& leftPath, const QString& rightPath) {
            addModel3dView(leftPath, rightPath, true);
        };
        session->onTableCompare = [&](const QString& l, const QString& r) { addTableView(l, r); };
        session->onRegistryCompare = [&](const QString& l, const QString& r) { addRegistryView(l, r); };
        session->onMediaCompare = [&](const QString& l, const QString& r) { addMediaView(l, r); };
        session->onHexCompare = [&](const QString& l, const QString& r) { addHexView(l, r); };
        session->onNewFolderCompare = [&](const QString& leftPath, const QString& rightPath) {
            addSession(leftPath, rightPath, true);
        };
        tabs->setCurrentIndex(index);
        if (run) {
            session->refresh();
        }
        return session;
    };

    addTextView = [&](const QString& leftPath, const QString& rightPath, const QString& leftText,
                      const QString& rightText, FolderCompareViewer* origin) {
        auto* textView = new TextCompareViewer(leftPath, rightPath, leftText, rightText);
        registerInstrumentationSession(textView->instrumentationSessionId(), textView->title());
        const int textIndex = tabs->addTab(
            textView, openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Compare),
            textView->title());
        tabs->setTabToolTip(textIndex, leftPath + " <-> " + rightPath);
        attachCloseButton(textIndex, textView);
        textView->onHomeRequested = [&, origin]() {
            const int index = origin ? tabs->indexOf(origin) : homeIndex;
            if (index >= 0) tabs->setCurrentIndex(index);
        };
        textView->onSessionsRequested = textView->onHomeRequested;
        textView->onTitleChanged = [&, textView]() {
            registerInstrumentationSession(textView->instrumentationSessionId(), textView->title());
            const int i = tabs->indexOf(textView);
            if (i < 0) return;
            tabs->setTabText(i, textView->title());
            if (tabs->currentIndex() == i) updateWindowTitle();
        };
        SessionHistory::record("text", leftPath, rightPath);
        home->refreshHistory();
        tabs->setCurrentIndex(textIndex);
    };

    addPictureView = [&](const QString& left, const QString& right, bool run) {
        auto* view = new PictureCompareViewer(&preferences);
        view->setPaths(left, right);
        const int pictureIndex = tabs->addTab(view, sessionTypeIcon("picture-compare"), view->title());
        tabs->setTabToolTip(pictureIndex, view->detailedTitle());
        attachCloseButton(pictureIndex, view);
        view->onComparisonRequested = [&](const QString& requestedLeft, const QString& requestedRight) {
            SessionHistory::record("picture", requestedLeft, requestedRight);
            home->refreshHistory();
        };
        view->onTitleChanged = [&, view]() {
            const int i = tabs->indexOf(view);
            if (i < 0) return;
            tabs->setTabText(i, view->title());
            tabs->setTabToolTip(i, view->detailedTitle());
            if (tabs->currentIndex() == i) updateWindowTitle();
        };
        tabs->setCurrentIndex(pictureIndex);
        if (run && !left.isEmpty() && !right.isEmpty()) view->compare();
    };

    addModel3dView = [&](const QString& left, const QString& right, bool run) {
        auto* view = new Model3dCompareViewer(&preferences);
        view->setPaths(left, right);
        const int index = tabs->addTab(view, sessionTypeIcon("model3d-compare"), view->title());
        tabs->setTabToolTip(index, view->detailedTitle());
        attachCloseButton(index, view);
        view->onComparisonRequested = [&](const QString& l, const QString& r) {
            SessionHistory::record("model3d", l, r);
            home->refreshHistory();
        };
        view->onTitleChanged = [&, view]() {
            const int i = tabs->indexOf(view);
            if (i < 0) return;
            tabs->setTabText(i, view->title());
            tabs->setTabToolTip(i, view->detailedTitle());
            if (tabs->currentIndex() == i) updateWindowTitle();
        };
        tabs->setCurrentIndex(index);
        if (run && !left.isEmpty() && !right.isEmpty()) view->compare();
    };

    addTableView = [&](const QString& left, const QString& right) {
        addViewerTab(new TableCompareViewer(left, right), "table-compare");
    };
    addRegistryView = [&](const QString& left, const QString& right) {
        addViewerTab(new RegistryCompareViewer(left, right), "registry-compare");
    };
    addHexView = [&](const QString& left, const QString& right) {
        addViewerTab(new HexCompareViewer(left, right), "hex-compare");
    };
    addMediaView = [&](const QString& left, const QString& right) {
        addViewerTab(new MediaCompareViewer(left, right), "media-compare");
    };

    closeTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        auto* widget = tabs->widget(index);
        if (auto* session = dynamic_cast<FolderCompareViewer*>(widget)) {
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
        // Tabs with editable sides ask first (shared UnsavedChangesDialog); Cancel keeps the tab.
        bool mayClose = true;
        if (auto* v = dynamic_cast<TextCompareViewer*>(widget)) mayClose = v->confirmClose();
        else if (auto* v = dynamic_cast<TableCompareViewer*>(widget)) mayClose = v->confirmClose();
        else if (auto* v = dynamic_cast<RegistryCompareViewer*>(widget)) mayClose = v->confirmClose();
        else if (auto* v = dynamic_cast<MediaCompareViewer*>(widget)) mayClose = v->confirmClose();
        if (!mayClose) {
            return;
        }
        tabs->removeTab(index);
        widget->deleteLater();
        home->refreshHistory();
        if (tabs->count() == 0) {
            addSession(QString(), QString(), false);  // never leave the window empty
        }
    };

    // Closing the window: every tab with unsaved edits gets its dialog (tab is brought to front first).
    window.confirmAllTabsClose = [&]() {
        for (int i = 0; i < tabs->count(); ++i) {
            QWidget* w = tabs->widget(i);
            bool dirty = false;
            if (auto* v = dynamic_cast<TextCompareViewer*>(w)) dirty = v->hasUnsavedChanges();
            else if (auto* v = dynamic_cast<TableCompareViewer*>(w)) dirty = v->hasUnsavedChanges();
            else if (auto* v = dynamic_cast<RegistryCompareViewer*>(w)) dirty = v->hasUnsavedChanges();
            else if (auto* v = dynamic_cast<MediaCompareViewer*>(w)) dirty = v->hasUnsavedChanges();
            if (!dirty) continue;
            tabs->setCurrentIndex(i);
            bool ok = true;
            if (auto* v = dynamic_cast<TextCompareViewer*>(w)) ok = v->confirmClose();
            else if (auto* v = dynamic_cast<TableCompareViewer*>(w)) ok = v->confirmClose();
            else if (auto* v = dynamic_cast<RegistryCompareViewer*>(w)) ok = v->confirmClose();
            else if (auto* v = dynamic_cast<MediaCompareViewer*>(w)) ok = v->confirmClose();
            if (!ok) return false;
        }
        return true;
    };

    auto duplicateTab = [&](int index) {
        if (index < 0 || index >= tabs->count()) {
            return;
        }
        if (auto* picture = dynamic_cast<PictureCompareViewer*>(tabs->widget(index))) {
            addPictureView(picture->leftText(), picture->rightText(), true);
            return;
        }
        if (auto* m = dynamic_cast<Model3dCompareViewer*>(tabs->widget(index))) {
            addModel3dView(m->leftText(), m->rightText(), true);
            return;
        }
        if (auto* v = dynamic_cast<TableCompareViewer*>(tabs->widget(index))) {
            addTableView(v->leftText(), v->rightText());
            return;
        }
        if (auto* v = dynamic_cast<RegistryCompareViewer*>(tabs->widget(index))) {
            addRegistryView(v->leftText(), v->rightText());
            return;
        }
        if (auto* v = dynamic_cast<HexCompareViewer*>(tabs->widget(index))) {
            addHexView(v->leftText(), v->rightText());
            return;
        }
        if (auto* v = dynamic_cast<MediaCompareViewer*>(tabs->widget(index))) {
            addMediaView(v->leftText(), v->rightText());
            return;
        }
        auto* source = dynamic_cast<FolderCompareViewer*>(tabs->widget(index));
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
    QObject::connect(closeTabOnEscape, &QAction::triggered, [&]() {
        QWidget* current = tabs->currentWidget();
        if (!current || current == home || dynamic_cast<FolderCompareViewer*>(current)) {
            return;
        }
        closeTab(tabs->currentIndex());
    });

    // ------------------------------------------------------------------------------------------
    // Drag-and-drop. ONE policy (common/drop_router.h) decides for Home and every compare tab;
    // the tabs themselves no longer parse drops. This block only translates between the router's
    // answer and the existing tab lambdas (addSession / addPictureView / ...).
    //   ApplyToTab       -> the tab's own applyDroppedPaths() (keeps its validation + discard prompt)
    //   ReplaceEmptyTab  -> a fresh viewer takes the empty tab's slot; the empty one is closed
    //   OpenNewTab       -> a new tab; the existing tab is left exactly as it was
    // ------------------------------------------------------------------------------------------
    auto dropContextFor = [&](QWidget* w) {
        drop::TabContext ctx;
        QString left, right;
        bool dirty = false;
        if (w == home) {
            ctx.tab = drop::TabKind::Home;
            return ctx;
        }
        if (auto* v = dynamic_cast<FolderCompareViewer*>(w)) { ctx.tab = drop::TabKind::Folder; left = v->leftText(); right = v->rightText(); }
        else if (auto* v = dynamic_cast<TextCompareViewer*>(w)) { ctx.tab = drop::TabKind::Text; left = v->leftText(); right = v->rightText(); dirty = v->hasUnsavedChanges(); }
        else if (auto* v = dynamic_cast<PictureCompareViewer*>(w)) { ctx.tab = drop::TabKind::Picture; left = v->leftText(); right = v->rightText(); }
        else if (auto* v = dynamic_cast<Model3dCompareViewer*>(w)) { ctx.tab = drop::TabKind::Model3d; left = v->leftText(); right = v->rightText(); }
        else if (auto* v = dynamic_cast<TableCompareViewer*>(w)) { ctx.tab = drop::TabKind::Table; left = v->leftText(); right = v->rightText(); dirty = v->hasUnsavedChanges(); }
        else if (auto* v = dynamic_cast<RegistryCompareViewer*>(w)) { ctx.tab = drop::TabKind::Registry; left = v->leftText(); right = v->rightText(); dirty = v->hasUnsavedChanges(); }
        else if (auto* v = dynamic_cast<HexCompareViewer*>(w)) { ctx.tab = drop::TabKind::Hex; left = v->leftText(); right = v->rightText(); }
        else if (auto* v = dynamic_cast<MediaCompareViewer*>(w)) { ctx.tab = drop::TabKind::Media; left = v->leftText(); right = v->rightText(); dirty = v->hasUnsavedChanges(); }
        ctx.leftSet = !left.isEmpty();
        ctx.rightSet = !right.isEmpty();
        ctx.occupancy = dirty ? drop::Occupancy::Dirty
                              : (ctx.leftSet || ctx.rightSet ? drop::Occupancy::Populated : drop::Occupancy::Empty);
        return ctx;
    };
    // Empty string = leave that side as it is.
    auto applyDroppedPaths = [&](QWidget* w, const QString& left, const QString& right) {
        if (auto* v = dynamic_cast<FolderCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<TextCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<PictureCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<Model3dCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<TableCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<RegistryCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<HexCompareViewer*>(w)) v->applyDroppedPaths(left, right);
        else if (auto* v = dynamic_cast<MediaCompareViewer*>(w)) v->applyDroppedPaths(left, right);
    };
    // Opens a new tab of `kind` (every add*View makes its tab current) and returns it.
    auto openKindTab = [&](drop::TabKind kind, const QString& left, const QString& right) -> QWidget* {
        const bool both = !left.isEmpty() && !right.isEmpty();
        switch (kind) {
        case drop::TabKind::Folder: addSession(left, right, !left.isEmpty() || !right.isEmpty()); break;  // a lone side is listed
        case drop::TabKind::Picture: addPictureView(left, right, both); break;
        case drop::TabKind::Model3d: addModel3dView(left, right, both); break;
        case drop::TabKind::Table: addTableView(left, right); break;
        case drop::TabKind::Registry: addRegistryView(left, right); break;
        case drop::TabKind::Hex: addHexView(left, right); break;
        case drop::TabKind::Media: addMediaView(left, right); break;
        case drop::TabKind::Text:
            // Text files are read by the viewer itself (async, remote-aware), not here on the GUI thread.
            addTextView(QString(), QString(), QString(), QString(), nullptr);
            if (auto* v = dynamic_cast<TextCompareViewer*>(tabs->currentWidget())) {
                v->applyDroppedPaths(left, right);
                if (!left.isEmpty() || !right.isEmpty()) {
                    SessionHistory::record("text", left, right);
                    home->refreshHistory();
                }
            }
            break;
        case drop::TabKind::Home: break;
        }
        return tabs->currentWidget();
    };
    auto handleDrop = [&](QWidget* destination, const DropParse& parsed, std::optional<drop::Side> sideHint) {
        const QString problems = parsed.problems.join("  |  ");
        auto say = [&](const QString& text) { if (!text.isEmpty()) window.statusBar()->showMessage(text, 12000); };
        drop::TabContext ctx = dropContextFor(destination);
        ctx.sideHint = sideHint;
        const drop::Decision decision = drop::route(ctx, toRouterItems(parsed.items));
        if (decision.action == drop::Action::Reject) {
            // Nothing is opened or changed; say why (invalid entries first, they are usually the reason).
            say(parsed.items.isEmpty() && !problems.isEmpty() ? problems : QString::fromStdString(decision.message));
            return;
        }
        const QString left = fromRouterPath(decision.left);
        const QString right = fromRouterPath(decision.right);
        switch (decision.action) {
        case drop::Action::ApplyToTab:
            tabs->setCurrentWidget(destination);
            applyDroppedPaths(destination, left, right);  // asks before discarding unsaved edits
            break;
        case drop::Action::ReplaceEmptyTab: {
            const int slot = tabs->indexOf(destination);
            QWidget* fresh = openKindTab(decision.target, left, right);
            const int from = tabs->indexOf(fresh);
            if (fresh && slot >= 0 && from >= 0) tabs->tabBar()->moveTab(from, slot);  // fresh viewer takes the slot
            closeTab(tabs->indexOf(destination));  // nothing to confirm: the tab was empty
            if (fresh) tabs->setCurrentWidget(fresh);
            break;
        }
        case drop::Action::OpenNewTab:
            openKindTab(decision.target, left, right);
            break;
        case drop::Action::Reject: break;
        }
        QStringList notes;
        if (!decision.message.empty()) notes << QString::fromStdString(decision.message);
        if (!problems.isEmpty()) notes << "Ignored: " + problems;
        say(notes.join("  "));
    };
    installDropRouting = [&, handleDrop](QWidget* tab) {
        std::function<bool(QObject*)> skip;
        // Home's per-type tiles keep their explicit "open exactly this kind" behaviour.
        if (tab == home) skip = [](QObject* o) { return dynamic_cast<SessionTile*>(o) != nullptr; };
        auto* filter = new DropRouteFilter(tab, skip);
        const QPointer<QWidget> guard(tab);
        filter->onDrop = [guard, handleDrop](const DropParse& parsed, std::optional<drop::Side> side) {
            if (guard) handleDrop(guard, parsed, side);  // the tab may have been closed meanwhile
        };
        filter->onHint = [&](const QString& text) { window.statusBar()->showMessage(text, 6000); };
    };
    installDropRouting(home);

    home->onNewFolderCompare = [&]() { addSession(QString(), QString(), false); };
    home->onNewTextCompare = [&]() {
        addTextView(QString(), QString(), QString(), QString(), nullptr);
    };
    // When onNewSession is set HomeView no longer falls back to onNewFolderCompare /
    // onNewTextCompare, so every launchable kind is routed here.
    // The new viewers have no file picker of their own: a tile clicked (or one file dropped)
    // asks for the missing path(s) here. Only choosing paths happens in the UI.
    auto askForPair = [&](const QString& title, const QString& filter, QStringList paths) -> QStringList {
        while (paths.size() < 2) {
            const QString chosen = QFileDialog::getOpenFileName(
                &window, title + (paths.isEmpty() ? " - choose left file" : " - choose right file"), QString(), filter);
            if (chosen.isEmpty()) return {};
            paths << chosen;
        }
        return paths;
    };
    home->onNewSession = [&, askForPair](const QString& kind, const QStringList& paths) {
        if (kind == "table-compare") {
            const QStringList pair = askForPair("Table Compare", "Tables (*.csv *.tsv *.xlsx *.xlsm *.xlsb *.xls *.ods);;All files (*)", paths);
            if (pair.size() == 2) addTableView(pair[0], pair[1]);
        } else if (kind == "registry-compare") {
            const QStringList pair = askForPair("Registry Compare", "Registry exports (*.reg);;All files (*)", paths);
            if (pair.size() == 2) addRegistryView(pair[0], pair[1]);
        } else if (kind == "hex-compare") {
            const QStringList pair = askForPair("Hex Compare", "All files (*)", paths);
            if (pair.size() == 2) addHexView(pair[0], pair[1]);
        } else if (kind == "media-compare") {
            const QStringList pair = askForPair(
                "Media Compare",
                "Media (*.mp3 *.wav *.flac *.ogg *.oga *.opus *.m4a *.aac *.wma *.mp4 *.m4v *.mov *.avi *.mkv *.webm);;All files (*)", paths);
            if (pair.size() == 2) addMediaView(pair[0], pair[1]);
        } else if (kind == "folder-compare") {
            addSession(paths.value(0), paths.value(1), paths.size() >= 2);
        } else if (kind == "text-compare") {
            addTextView(QString(), QString(), QString(), QString(), nullptr);
        } else if (kind == "picture-compare") {
            addPictureView(paths.value(0), paths.value(1), paths.size() >= 2);
        } else if (kind == "model3d-compare") {
            addModel3dView(paths.value(0), paths.value(1), paths.size() >= 2);
        }
    };
    home->onOpenHistory = [&](const SessionHistoryEntry& entry) {
        if (entry.kind == "table" || entry.kind == "table-compare") {
            addTableView(entry.left, entry.right);
        } else if (entry.kind == "registry" || entry.kind == "registry-compare") {
            addRegistryView(entry.left, entry.right);
        } else if (entry.kind == "hex" || entry.kind == "hex-compare") {
            addHexView(entry.left, entry.right);
        } else if (entry.kind == "media" || entry.kind == "media-compare") {
            addMediaView(entry.left, entry.right);
        } else if (entry.kind == "picture") {
            addPictureView(entry.left, entry.right, true);
        } else if (entry.kind == "folder") {
            addSession(entry.left, entry.right, true);
        } else if (entry.kind == "model3d") {
            addModel3dView(entry.left, entry.right, true);
        } else if (!entry.left.isEmpty() || !entry.right.isEmpty()) {
            const QPointer<PersistedMainWindow> mainWindow(&window);
            window.statusBar()->showMessage("Loading comparison files...");
            QThreadPool::globalInstance()->start([&, entry, mainWindow]() {
                QFile leftFile(entry.left);
                QFile rightFile(entry.right);
                const QString leftText = leftFile.open(QIODevice::ReadOnly)
                                             ? QString::fromUtf8(leftFile.readAll()) : QString();
                const QString rightText = rightFile.open(QIODevice::ReadOnly)
                                              ? QString::fromUtf8(rightFile.readAll()) : QString();
                if (!mainWindow) return;
                QMetaObject::invokeMethod(mainWindow, [&, entry, leftText, rightText, mainWindow]() {
                    if (!mainWindow) return;
                    addTextView(entry.left, entry.right, leftText, rightText, nullptr);
                    mainWindow->statusBar()->clearMessage();
                }, Qt::QueuedConnection);
            });
        }
    };

    auto* plus = new QToolButton(tabs);
    plus->setObjectName("newTab");
    plus->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Plus));
    plus->setToolTip("New folder compare (Ctrl+T)");
    plus->setAutoRaise(true);
    tabs->setCornerWidget(plus, Qt::TopRightCorner);
    QObject::connect(plus, &QToolButton::clicked, [&]() { addSession(QString(), QString(), false); });
    QObject::connect(tabs->tabBar(), &QTabBar::tabBarDoubleClicked, [&](int index) {
        if (index < 0) addSession(QString(), QString(), false);
    });

    auto activeInstrumentationSessionId = [&]() {
        if (auto* session = currentSession()) return session->instrumentationSessionId();
        if (auto* textView = dynamic_cast<TextCompareViewer*>(tabs->currentWidget())) {
            return textView->instrumentationSessionId();
        }
        return QString();
    };
    auto* instrumentationButton = new QToolButton(&window);
    instrumentationButton->setText("Instrumentation");
    instrumentationButton->setToolTip("Open the current tab's instrumentation report");
    instrumentationButton->setAutoRaise(true);
    window.statusBar()->addPermanentWidget(instrumentationButton);
    QObject::connect(instrumentationButton, &QToolButton::clicked, [&]() {
        const QString sessionId = activeInstrumentationSessionId();
        if (sessionId.isEmpty()) return;
        const QByteArray sessionIdBytes = sessionId.toUtf8();
        std::size_t reportLength = 0;
        char* report = openbc_instrumentation_session_report(
            reinterpret_cast<const std::uint8_t*>(sessionIdBytes.constData()),
            sessionIdBytes.size(), &reportLength);
        QString reportText = report
                                 ? QString::fromUtf8(report, static_cast<int>(reportLength))
                                 : QString("No instrumentation report is available for this tab.");
        if (report) openbc_instrumentation_report_destroy(report);

        auto* dialog = new QDialog(&window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle("Instrumentation - " + tabs->tabText(tabs->currentIndex()));
        dialog->resize(820, 520);
        auto* layout = new QVBoxLayout(dialog);
        auto* reportView = new QPlainTextEdit(dialog);
        reportView->setReadOnly(true);
        reportView->setLineWrapMode(QPlainTextEdit::NoWrap);
        reportView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        reportView->setPlainText(reportText);
        layout->addWidget(reportView);
        dialog->show();
    });

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
        instrumentationButton->setEnabled(!activeInstrumentationSessionId().isEmpty());
    });

    tabs->setCurrentIndex(homeIndex);
    rebuildMenus();
    updateWindowTitle();
    instrumentationButton->setEnabled(!activeInstrumentationSessionId().isEmpty());

    auto focusWindow = [&]() {
        while (QApplication::overrideCursor()) QApplication::restoreOverrideCursor();
        window.unsetCursor();
        if (window.isMinimized()) {
            window.setWindowState(window.windowState() & ~Qt::WindowMinimized);
        }
        window.show();
        window.raise();
        window.activateWindow();
    };
    auto openBlankFolderCompare = [&]() {
        for (int i = 0; i < tabs->count(); ++i) {
            auto* session = dynamic_cast<FolderCompareViewer*>(tabs->widget(i));
            if (session && session->leftText().isEmpty() && session->rightText().isEmpty()) {
                tabs->setCurrentIndex(i);
                focusWindow();
                return;
            }
        }
        addSession(QString(), QString(), false);
        focusWindow();
    };
    auto dispatchComparison = [&](const LaunchRequest& request) {
        QString launchError;
        const QString kind = resolveLaunchKind(request, &launchError);
        if (!launchError.isEmpty()) {
            window.statusBar()->showMessage(launchError, 15000);
            focusWindow();
            return;
        }
        if (kind.isEmpty()) return;
        QStringList paths;
        if (!request.left.isEmpty() || !request.right.isEmpty()) paths << request.left << request.right;
        if (kind == "text-compare") {
            auto readText = [](const QString& path) {
                QFile file(path);
                return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
            };
            addTextView(request.left, request.right, readText(request.left), readText(request.right), nullptr);
        } else {
            home->onNewSession(kind, paths);
        }
        focusWindow();
    };
    auto handleLaunch = [&](const LaunchRequest& request) {
        if (request.hasContextAction()) {
            const bool folderAction = request.contextAction.endsWith("folder");
            const bool selectLeft = request.contextAction.startsWith("select-left-");
            const QFileInfo selected(request.contextPath);
            if ((folderAction && !selected.isDir()) || (!folderAction && !selected.isFile())) {
                window.statusBar()->showMessage(folderAction ? "Select a folder for this action"
                                                             : "Select a file for this action", 10000);
                focusWindow();
                return;
            }
            if (selectLeft) {
                preferences.setValue("context/leftKind", folderAction ? "folder" : "file");
                preferences.setValue("context/leftPath", request.contextPath);
                if (folderAction) {
                    openBlankFolderCompare();
                    if (auto* session = currentSession()) session->setPaths(request.contextPath, QString());
                }
                window.statusBar()->showMessage("Selected left " + QString(folderAction ? "folder: " : "file: ") +
                                                    QDir::toNativeSeparators(request.contextPath), 10000);
                focusWindow();
                return;
            }

            const QString expectedKind = folderAction ? "folder" : "file";
            const QString leftKind = preferences.value("context/leftKind").toString();
            const QString leftPath = preferences.value("context/leftPath").toString();
            if (leftKind != expectedKind || leftPath.isEmpty()) {
                window.statusBar()->showMessage("Select a left " + expectedKind + " first", 10000);
                focusWindow();
                return;
            }
            LaunchRequest comparison;
            comparison.left = leftPath;
            comparison.right = request.contextPath;
            dispatchComparison(comparison);
            return;
        }
        if (!request.hasPaths() && request.compareType.isEmpty()) {
            openBlankFolderCompare();
            return;
        }
        dispatchComparison(request);
    };
    window.show();
    QTimer::singleShot(0, &window, [&, launch]() { handleLaunch(launch); });
    singleInstance.setLaunchHandler(handleLaunch);

    return application.exec();
}

}  // namespace openbc::app
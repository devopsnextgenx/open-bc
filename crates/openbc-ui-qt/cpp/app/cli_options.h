// cli_options.h
// ---------------------------------------------------------------------------
// Command line of the `openbc` executable:
//
//   openbc [options] [LEFT [RIGHT]]
//     --compare <type>      force a viewer: folder | text | picture | 3d | table | registry | hex | media
//     --dependency-check    list/verify required .dll/.so files and exit (0 ok, 2 missing)
//     --version, --help
//
// Without --compare the viewer is chosen from the paths: two folders -> folder compare; two files
// -> by extension (picture, 3D model, media, csv/excel, .reg) otherwise text compare.
// Parsing is Qt-light on purpose (QString only) so it can run before QApplication exists.
// ---------------------------------------------------------------------------
#pragma once

#include <QString>
#include <QStringList>

namespace openbc::app {

struct LaunchRequest {
    QString left;
    QString right;
    QString compareType;          // as typed by the user; empty = auto-detect
    QString contextAction;
    QString contextPath;
    bool dependencyCheck = false;
    bool showHelp = false;
    bool showVersion = false;
    QString error;                // non-empty = invalid command line (message for the user)
    bool hasPaths() const { return !left.isEmpty() || !right.isEmpty(); }
    bool hasContextAction() const { return !contextAction.isEmpty(); }
};

// `arguments` excludes argv[0].
LaunchRequest parseLaunchArguments(const QStringList& arguments);

// Session kind understood by HomeView::onNewSession ("folder-compare", "text-compare", ...).
// Empty with `error` set when the request cannot be resolved (e.g. a folder paired with a file).
QString resolveLaunchKind(const LaunchRequest& request, QString* error);

QString cliHelpText();

// Absolute path of the running executable (works before QApplication exists).
QString currentExecutablePath();

// Console output that also works from a GUI-subsystem exe on Windows (attaches to the parent
// console). Use for --help / --version / --dependency-check / errors.
void printToConsole(const QString& text, bool isError = false);

// Windows only (no-op elsewhere): before QApplication exists, verify platforms/qwindows.dll is next
// to the executable and otherwise explain how to fix it. Returns false when startup must abort.
bool preflightQtRuntime();

}  // namespace openbc::app

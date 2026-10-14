// cli_options.cpp - see cli_options.h
#include "app/cli_options.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

#include "common/file_kinds.h"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace openbc::app {
namespace {

// Maps user-facing type names (and the aliases canonicalSessionKind knows) to a session kind.
QString kindForType(const QString& type) {
    const QString t = type.trimmed().toLower();
    if (t == "3d" || t == "model" || t == "model3d") return "model3d-compare";
    if (t == "image" || t == "picture" || t == "pictures") return "picture-compare";
    if (t == "excel" || t == "csv" || t == "table" || t == "spreadsheet") return "table-compare";
    if (t == "reg" || t == "registry") return "registry-compare";
    const QString canonical = canonicalSessionKind(t);
    // The version viewer does not exist yet; binaries fall back to hex.
    if (canonical == "version-compare") return "hex-compare";
    if (canonical == "folder-merge" || canonical == "folder-sync") return "folder-compare";
    if (canonical == "text-merge" || canonical == "text-edit") return "text-compare";
    return canonical;
}

bool looksRemote(const QString& path) { return path.contains("://"); }

bool isDirectory(const QString& path) {
    if (looksRemote(path)) return QFileInfo(path).suffix().isEmpty();  // best guess; remote not stat'ed here
    return QFileInfo(path).isDir();
}

}  // namespace

QString cliHelpText() {
    return QStringLiteral(
        "OpenBC - file and folder comparison\n\n"
        "Usage: openbc [options] [LEFT [RIGHT]]\n\n"
        "  LEFT, RIGHT           two files or two folders to compare (both optional)\n"
        "  --select-left-folder PATH   remember a folder as the left side\n"
        "  --compare-to-left-folder PATH   compare a folder to the remembered left folder\n"
        "  --select-left-file PATH   remember a file as the left side\n"
        "  --compare-to-left-file PATH   compare a file to the remembered left file\n"
        "  --compare <type>      force a viewer instead of auto-detecting from the paths:\n"
        "                        folder | text | picture | 3d | table | registry | hex | media\n"
        "  --dependency-check    verify required .dll/.so files and exit (0 = ok, 2 = missing)\n"
        "  --version             print the version and exit\n"
        "  -h, --help            show this help\n\n"
        "Auto-detection: two folders -> folder compare; two files -> picture / 3D model / media /\n"
        "table (csv, excel) / registry (.reg) by extension, otherwise text compare.\n"
        "Tip: if openbc will not start at all, run the Qt-free `openbc-depcheck` next to it.\n");
}

LaunchRequest parseLaunchArguments(const QStringList& arguments) {
    LaunchRequest r;
    QStringList positional;
    bool optionsDone = false;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString& a = arguments[i];
        if (optionsDone || !a.startsWith('-') || a == "-") {
            positional << a;
        } else if (a == "--") {
            optionsDone = true;
        } else if (a == "-h" || a == "--help") {
            r.showHelp = true;
        } else if (a == "--version") {
            r.showVersion = true;
        } else if (a == "--dependency-check") {
            r.dependencyCheck = true;
        } else if (a == "--select-left-folder" || a == "--compare-to-left-folder" ||
                   a == "--select-left-file" || a == "--compare-to-left-file") {
            if (i + 1 >= arguments.size()) {
                r.error = a + " needs a path";
                return r;
            }
            r.contextAction = a.mid(2);
            r.contextPath = arguments[++i];
        } else if (a == "--compare" || a.startsWith("--compare=")) {
            if (a.startsWith("--compare=")) {
                r.compareType = a.mid(10);
            } else if (i + 1 < arguments.size()) {
                r.compareType = arguments[++i];
            } else {
                r.error = "--compare needs a type (folder, text, picture, 3d, table, registry, hex, media)";
                return r;
            }
            if (kindForType(r.compareType).isEmpty()) {
                r.error = "unknown compare type '" + r.compareType + "'";
                return r;
            }
        } else {
            r.error = "unknown option '" + a + "' (see --help)";
            return r;
        }
    }
    if (positional.size() > 2) {
        r.error = "at most two paths can be given, got " + QString::number(positional.size());
        return r;
    }
    if (r.hasContextAction() && (!positional.isEmpty() || r.contextPath.isEmpty())) {
        r.error = "a file-manager action needs exactly one path";
        return r;
    }
    r.left = positional.value(0);
    r.right = positional.value(1);
    return r;
}

QString resolveLaunchKind(const LaunchRequest& request, QString* error) {
    if (!request.compareType.isEmpty()) return kindForType(request.compareType);
    const QString& l = request.left;
    const QString& r = request.right;
    if (l.isEmpty() && r.isEmpty()) return {};
    const bool leftDir = !l.isEmpty() && isDirectory(l);
    const bool rightDir = !r.isEmpty() && isDirectory(r);
    if (leftDir || rightDir) {
        if ((!l.isEmpty() && !leftDir) || (!r.isEmpty() && !rightDir)) {
            if (error) *error = "cannot compare a folder with a file: " + l + " <-> " + r;
            return {};
        }
        return "folder-compare";
    }
    const ViewerKind a = classifyFilePath(l);
    const ViewerKind b = r.isEmpty() ? a : classifyFilePath(r);
    ViewerKind chosen = ViewerKind::Text;
    if (a == b) chosen = a;  // differing kinds (e.g. .csv vs .txt) compare as plain text
    QString kind = sessionKindForViewer(chosen);
    if (kind == "version-compare") kind = "hex-compare";
    return kind;
}

QString currentExecutablePath() {
#ifdef _WIN32
    wchar_t module[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
    return n ? QString::fromWCharArray(module, int(n)) : QString();
#else
    return QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
#endif
}

void printToConsole(const QString& text, bool isError) {
#ifdef _WIN32
    // The exe is built for the GUI subsystem; borrow the parent's console when there is one.
    static bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    if (attached) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::fputs(text.toLocal8Bit().constData(), isError ? stderr : stdout);
    std::fflush(isError ? stderr : stdout);
}

bool preflightQtRuntime() {
#ifdef _WIN32
    wchar_t module[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return true;
    const QString exeDir = QFileInfo(QString::fromWCharArray(module)).absolutePath();
    if (QFileInfo::exists(exeDir + "/platforms/qwindows.dll")) return true;
    const QString message =
        "OpenBC cannot start: the Qt platform plugin is missing.\n\n"
        "Expected: " + QDir::toNativeSeparators(exeDir + "/platforms/qwindows.dll") + "\n\n"
        "It starts from MSYS2/git bash only because that shell puts the MSYS2 folders on PATH.\n"
        "Run windeployqt on openbc.exe (or install the MSI), and use `openbc-depcheck` to list\n"
        "every missing DLL.\n";
    printToConsole(message, true);
    MessageBoxW(nullptr, reinterpret_cast<LPCWSTR>(message.utf16()), L"OpenBC", MB_OK | MB_ICONERROR);
    return false;
#else
    return true;
#endif
}

}  // namespace openbc::app

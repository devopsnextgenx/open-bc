// context_menu_state.cpp - see context_menu_state.h
#include "app/context_menu_state.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

#include "app/cli_options.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace openbc::app {
namespace {

[[maybe_unused]] const char* kindName(ContextMenuState::Kind kind) {
    return kind == ContextMenuState::Kind::Folder ? "folder" : "file";
}

QString leftItemName(const QString& path) {
    QString name = QFileInfo(path).fileName();
    if (name.isEmpty()) name = QDir::toNativeSeparators(path);
    name.replace('\n', ' ');
    name.replace('\r', ' ');
    return name;
}

#ifdef Q_OS_WIN

// Verb keys mirror the MSI's naming (OpenBC.SelectLeftFolder, ...). The MSI installs only Select Left and
// Compare; these two are created here, per user, while a left item exists.
//   HKCU\Software\Classes\Directory\shell\OpenBC.CompareToLeftFolder
//   HKCU\Software\Classes\*\shell\OpenBC.CompareToLeftFile
std::wstring verbKey(const wchar_t* root, const wchar_t* verb) {
    return std::wstring(L"Software\\Classes\\") + root + L"\\shell\\" + verb;
}

void setDefaultValue(HKEY key, const QString& value) {
    const std::wstring w = value.toStdWString();
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
                   static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
}

void setStringValue(HKEY key, const wchar_t* name, const QString& value) {
    const std::wstring w = value.toStdWString();
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
                   static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
}

void writeVerb(const wchar_t* root, const wchar_t* verb, const QString& label, const QString& exe,
               const QString& argument) {
    const std::wstring key = verbKey(root, verb);
    HKEY handle = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle,
                        nullptr) != ERROR_SUCCESS)
        return;
    setStringValue(handle, L"MUIVerb", label);   // same value name the MSI verbs use
    setStringValue(handle, L"Icon", QDir::toNativeSeparators(exe) + ",0");
    RegCloseKey(handle);

    HKEY command = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (key + L"\\command").c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &command, nullptr) != ERROR_SUCCESS)
        return;
    setDefaultValue(command, "\"" + QDir::toNativeSeparators(exe) + "\" " + argument + " \"%1\"");
    RegCloseKey(command);
}

void removeVerbs() {
    RegDeleteTreeW(HKEY_CURRENT_USER, verbKey(L"Directory", L"OpenBC.CompareToLeftFolder").c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, verbKey(L"*", L"OpenBC.CompareToLeftFile").c_str());
}

void publish(ContextMenuState::Kind kind, const QString& path) {
    removeVerbs();
    const QString exe = currentExecutablePath();
    if (exe.isEmpty()) return;
    const QString label = "openbc: Compare To " + leftItemName(path);
    if (kind == ContextMenuState::Kind::Folder)
        writeVerb(L"Directory", L"OpenBC.CompareToLeftFolder", label, exe, "--compare-to-left-folder");
    else
        writeVerb(L"*", L"OpenBC.CompareToLeftFile", label, exe, "--compare-to-left-file");
}

void unpublish() { removeVerbs(); }

#else  // Linux and other desktops

QString stateFilePath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    return dir + "/openbc/left-selection";
}

QString dolphinMenuPath(const char* kind) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           "/kio/servicemenus/openbc-compare-to-left-" + kind + ".desktop";
}

// Command that starts OpenBC from a file manager: the line in ~/.config/openbc/launcher (written by
// install-nautilus.sh / install-dolphin.sh for AppImage and Flatpak installs), else $APPIMAGE, else this
// executable. Returned ready to put after "Exec=".
QString launcherCommand() {
    const QString config = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
    QFile file(config + "/openbc/launcher");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.isEmpty()) return line;
    }
    QString exe = qEnvironmentVariable("APPIMAGE");
    if (exe.isEmpty()) exe = currentExecutablePath();
    return exe.contains(' ') ? "\"" + exe + "\"" : exe;
}

// install-nautilus.sh installs "openbc: Select Left ..." scripts; "Compare To <left item name>" is a
// symlink to the matching one and is present only while a left item is selected.
QString nautilusScriptsDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/nautilus/scripts";
}

void removeNautilusCompareScripts() {
    const QString dir = nautilusScriptsDir();
    QDir scripts(dir);
    const QStringList entries = scripts.entryList({"openbc: Compare To *"}, QDir::Files | QDir::System);
    for (const QString& entry : entries) {
        if (QFileInfo(dir + "/" + entry).isSymLink()) QFile::remove(dir + "/" + entry);
    }
}

void publishNautilusScript(ContextMenuState::Kind kind, const QString& path) {
    const bool folder = kind == ContextMenuState::Kind::Folder;
    const QString dir = nautilusScriptsDir();
    const QString selectName = folder ? "openbc: Select Left Folder" : "openbc: Select Left File";
    removeNautilusCompareScripts();
    if (QFileInfo::exists(dir + "/" + selectName))
        QFile::link(selectName, dir + "/openbc: Compare To " + leftItemName(path));
}

void unpublishNautilusScripts() { removeNautilusCompareScripts(); }

void writeFile(const QString& path, const QByteArray& content, QFileDevice::Permissions perms) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(content);
    file.close();
    file.setPermissions(perms);
}

void publish(ContextMenuState::Kind kind, const QString& path) {
    const char* name = kindName(kind);
    writeFile(stateFilePath(), (QString(name) + "\n" + path + "\n").toUtf8(),
              QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    const bool folder = kind == ContextMenuState::Kind::Folder;
    const QString title = "Compare To " + leftItemName(path);
    const QString action = folder ? "OpenBCCompareToLeftFolder" : "OpenBCCompareToLeftFile";
    QString desktop;
    QTextStream out(&desktop);
    out << "[Desktop Entry]\n"
        << "Type=Service\n"
        << "ServiceTypes=KonqPopupMenu/Plugin\n"
        << "MimeType=" << (folder ? "inode/directory;" : "all/allfiles;") << "\n"
        << "Actions=" << action << ";\n"
        << "X-KDE-Submenu=openbc\n"
        << "Icon=io.github.devopsnextgenx.open-bc\n\n"
        << "[Desktop Action " << action << "]\n"
        << "Name=openbc: " << title << "\n"
        << "Icon=view-split-left-right\n"
        << "Exec=" << launcherCommand() << " --compare-to-left-" << name << " %f\n";
    // The other kind must not linger from an earlier selection.
    QFile::remove(dolphinMenuPath(folder ? "file" : "folder"));
    writeFile(dolphinMenuPath(name), desktop.toUtf8(),
              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                  QFileDevice::ReadGroup | QFileDevice::ReadOther);   // KDE trusts only executable menus
    publishNautilusScript(kind, path);
}

void unpublish() {
    QFile::remove(stateFilePath());
    QFile::remove(dolphinMenuPath("folder"));
    QFile::remove(dolphinMenuPath("file"));
    unpublishNautilusScripts();
}

#endif

}  // namespace

void ContextMenuState::setLeft(Kind kind, const QString& path) {
    if (kind == Kind::None || path.isEmpty()) {
        clear();
        return;
    }
    kind_ = kind;
    path_ = path;
    publish(kind, path);
}

void ContextMenuState::purgeStale() { unpublish(); }

void ContextMenuState::clear() {
    const bool wasSet = kind_ != Kind::None;
    kind_ = Kind::None;
    path_.clear();
    if (wasSet) unpublish();
}

}  // namespace openbc::app

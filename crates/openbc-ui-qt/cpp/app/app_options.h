// app_options.h
// ---------------------------------------------------------------------------
// AppOptions: the plain-data model behind Tools > Options. One field per
// control on the Options dialog pages (Startup, Tabs, Appearance, Text
// Editing, Next Difference, Backups, File Operations, Commands, Tweaks).
//
// OptionsStore: the single owner of the *committed* options.
//
// MOCK STAGE: nothing here talks to the backend yet. The dialog edits a copy
// of AppOptions; OK/Apply hands it to OptionsStore::commit(), which keeps it
// in memory and notifies listeners. The two seams to fill in later are
// clearly marked:
//   - OptionsStore::load()    -> read the user profile from preferences.ini
//   - OptionsStore::persist() -> write it back (same QSettings the window
//                                geometry and theme/dark already use)
// and listeners (addListener) are where tabs, the theme, backups, confirmation
// prompts, etc. will subscribe to reload when the options change. Per
// ARCHITECTURE.md the UI layer only consumes and emits owned values; nothing
// here blocks or touches the engine/VFS crates.
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>
#include <utility>
#include <vector>

#include "style/qt_style.h"

namespace openbc::app {

enum class OpenSessionsIn { NewWindow, NewTab };
enum class QuickCompareMode { Binary, RulesBased };
enum class ThemeChoice { FollowSystem, Light, Dark };
enum class SyncConfirmation { Prompt, YesToAll, NoToAll };

// "Back up abc.txt as:" - the seven naming schemes, plus "in this folder".
enum class BackupNaming {
    TildeInExtension,   // abc.~txt
    TildeAfterName,     // abc.txt~
    AppendBak,          // abc.txt.bak
    TildePrefix,        // ~abc.txt
    ReplaceWithBak,     // abc.bak
    BakBeforeExt,       // abc.bak.txt
    InFolder            // abc.txt in <backupFolder>
};

struct ItemColors {
    QColor text;
    QColor background;
    QColor folderInfo;   // Folder Views only
};

struct ExplorerCommand {
    QString command;
    QString location;   // "Main menu" / "Submenu" / "None"
};

struct CommandBinding {
    QString name;
    QString menu;
    bool inMenu = true;
    bool inToolbar = false;
    QString shortcut;
    QString defaultShortcut;
    QString description;
};

struct AppOptions {
    // ---- Startup ----------------------------------------------------------
    QString loadWorkspace;
    QString saveWorkspaceOnExit;
    bool showQuickCompareDialog = true;
    QuickCompareMode quickCompareMode = QuickCompareMode::Binary;
    bool openViewIfFilesDiffer = true;
    bool runClipboardCompareOnStartup = false;
    bool explorerIntegration = true;
    QList<ExplorerCommand> explorerCommands = {
        {"Compare", "Main menu"},          {"Compare Using", "None"},
        {"Compare to Clipboard", "None"},  {"Compare to Registry", "None"},
        {"Sync", "None"},                  {"Merge", "None"},
        {"Create Snapshot", "None"},
    };
    bool hideSelectLeftIfSaved = false;

    // ---- Tabs -------------------------------------------------------------
    OpenSessionsIn openSessionsIn = OpenSessionsIn::NewTab;
    OpenSessionsIn openChildSessionsIn = OpenSessionsIn::NewTab;
    bool openNewTabNextToActive = false;
    bool closingLastTabClosesWindow = true;
    bool warnClosingMultipleTabs = true;
    bool warnClosingMultipleWindows = true;
    bool hideTabBarIfSingleTab = false;
    bool showTabPreviewsInTaskbar = true;

    // ---- Appearance -------------------------------------------------------
    ThemeChoice theme = ThemeChoice::FollowSystem;
    bool previewTheme = false;
    bool useSystemFont = true;
    QString customFont;                 // QFont::toString(); only when !useSystemFont
    bool colorFoldersToReflectContent = true;
    bool useGradientForCenterBackground = true;
    bool useStripes = true;
    // Key: "<tab>/<item>", e.g. "Folder Views/Same". Missing key = default.
    QMap<QString, ItemColors> colors;

    // ---- Text Editing -----------------------------------------------------
    bool autoIndent = true;
    bool backspaceUnindents = true;
    bool initFindWithCurrentWord = true;
    bool showFilteredLineCounts = true;
    int contextLines = 6;

    // ---- Next Difference --------------------------------------------------
    bool goToFirstDifferenceOnLoad = false;
    bool goToNextDifferenceAfterCopy = false;
    bool limitNextDifferenceFilesToFolder = false;
    bool wrapAroundToFirstDifference = false;
    bool showMessagePanel = true;

    // ---- Backups ----------------------------------------------------------
    bool backupBeforeCopy = false;
    bool backupBeforeSave = false;
    BackupNaming backupNaming = BackupNaming::TildeInExtension;
    QString backupFolder;

    // ---- File Operations --------------------------------------------------
    bool confirmCopy = true;
    bool confirmMove = true;
    bool confirmReplaceReadOnly = true;
    bool confirmReplaceSystem = true;
    bool confirmOverwriteNewer = true;
    bool confirmReplaceDuringMove = true;
    bool confirmContentCompare = true;
    bool confirmDelete = true;
    bool requireExplicitSideInDelete = false;
    bool confirmMerge = true;
    SyncConfirmation syncConfirmation = SyncConfirmation::Prompt;
    bool includeHiddenItemsByDefault = false;

    // ---- Commands ---------------------------------------------------------
    bool largeToolbarButtonsWithText = true;
    // Key: view name ("Home", "Folder Compare", ...).
    QMap<QString, QList<CommandBinding>> commandsByView;

    // ---- Tweaks -----------------------------------------------------------
    bool checkForUpdates = true;
    int updateIntervalDays = 7;
    QString lastUpdateCheck = QStringLiteral("Never");
    bool syntaxHighlightOnDiffLines = false;
    bool crosshatchBeyondEndOfFile = true;
    bool rightGutterForLeftEditor = false;
    bool useOrphanColor = false;
    int extraLineSpacingPx = 0;
    int columnLineAt = 0;
    int dimInactivePanePercent = 20;
    bool useDifferentMergeInputFont = false;
    QString mergeInputFont;
    bool beepAfterLongFileOperations = false;

    // Defaults for the Appearance color editor, matching the folder-compare
    // palette in qt_style.h so "reset" shows what the app really uses today.
    static ItemColors defaultColors(const QString& tab, const QString& item) {
        namespace c = openbc::ui::color;
        ItemColors out{c::text(), c::windowBg(), c::dimText()};
        if (tab != QLatin1String("Folder Views")) return out;
        if (item == "Same") out.text = c::same();
        else if (item == "Orphan") out.text = c::orphan();
        else if (item == "Older") out.text = c::older();
        else if (item == "Newer") out.text = c::newer();
        else if (item == "Different") out.text = c::different();
        else if (item == "Selection") out.background = c::selection();
        return out;
    }

    ItemColors colorsFor(const QString& tab, const QString& item) const {
        const auto it = colors.constFind(tab + "/" + item);
        return it != colors.constEnd() ? it.value() : defaultColors(tab, item);
    }
};

class OptionsStore {
public:
    using Listener = std::function<void(const AppOptions&)>;

    OptionsStore() { load(); }

    const AppOptions& options() const { return options_; }

    // Called by the dialog on OK / Apply.
    void commit(const AppOptions& next) {
        options_ = next;
        persist();
        for (const auto& listener : listeners_) listener(options_);
    }

    // Subscribe to "options changed" (tabs, theme, backups, ...). Nothing
    // subscribes yet - this is the hook for the real configuration reload.
    void addListener(Listener listener) { listeners_.push_back(std::move(listener)); }

    // TODO(options): read the saved profile from preferences.ini; fall back
    // to AppOptions{} defaults for any missing key. Mock: defaults only.
    void load() { options_ = AppOptions{}; }

    // TODO(options): write options_ to the user profile in preferences.ini.
    // Mock: intentionally a no-op so nothing is written to disk yet.
    void persist() {}

private:
    AppOptions options_;
    std::vector<Listener> listeners_;
};

}  // namespace openbc::app
// app_options.h
// ---------------------------------------------------------------------------
// AppOptions: the plain-data model behind Tools > Options. One field per
// control on the Options dialog pages (Startup, Tabs, Appearance, Text
// Editing, Next Difference, Backups, File Operations, Archive Types, Commands, Tweaks).
//
// OptionsStore: the single owner of the *committed* options.
//
// PERSISTENCE: Appearance, syntaxMappings, Archive Types, and Folder Filters are saved/loaded (preferences.ini);
// other pages are still in-memory. The dialog edits a copy of AppOptions; OK/Apply hands it to
// OptionsStore::commit(), which persists implemented fields and notifies listeners.
// The persisted profile includes `syntaxMappings`: OptionsStore hands it to
// SyntaxMappingStore (options/syntax_mappings.h), which saves it to preferences.ini, applies it to
// the backend and re-highlights open comparisons. Archive Types settings are consumed by
// JavaDecompilerAdapter at runtime. Listeners (addListener) are where tabs, the theme, backups,
// confirmation prompts, etc. will subscribe to reload when the options change. Per
// ARCHITECTURE.md the UI layer only consumes and emits owned values; nothing
// here blocks or touches the engine/VFS crates.
// ---------------------------------------------------------------------------
#pragma once

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "options/syntax_mappings.h"
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

struct FolderFilterPreset {
    QString name;
    QString includeFiles = QStringLiteral("*.*");
    QString excludeFiles;
    QString includeFolders = QStringLiteral("*");
    QString excludeFolders;
    bool builtIn = false;
};

inline QList<FolderFilterPreset> defaultFolderFilterPresets() {
    return {
        {QStringLiteral("Source code"),
         QStringLiteral("*.c;*.cpp;*.h;*.hpp;*.cs;*.java;*.js;*.ts;*.tsx;*.py;*.rs;*.go"),
         {}, QStringLiteral("*"), {}, true},
        {QStringLiteral("Web"), QStringLiteral("*.html;*.htm;*.css;*.js;*.ts;*.json"),
         {}, QStringLiteral("*"), {}, true},
        {QStringLiteral("Exclude VCS folders"), QStringLiteral("*.*"), {},
         QStringLiteral("*"), QStringLiteral(".git;.svn;.hg"), true},
        {QStringLiteral("Exclude build output"), QStringLiteral("*.*"), {},
         QStringLiteral("*"),
         QStringLiteral("node_modules;target;dist;build;bin;obj"), true},
    };
}

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

    // ---- Archive Types ---------------------------------------------------
    bool javaDecompilerEnabled = true;
    QString decompilerJarPath;
    QString javaBinPath = QStringLiteral("java");

    // ---- Syntax Highlighting ----------------------------------------------
    // The user's extension / file name -> language rules, on top of the built-in table.
    QList<SyntaxMapping> syntaxMappings;

    // ---- Folder Compare Name Filters --------------------------------------
    QList<FolderFilterPreset> folderFilterPresets = defaultFolderFilterPresets();
    QString defaultFolderFilterPreset;

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

    // `preferences` (preferences.ini) must outlive the store; null = defaults only, nothing saved.
    explicit OptionsStore(QSettings* preferences = nullptr) : preferences_(preferences) { load(); }

    const AppOptions& options() const { return options_; }

    // Called by the dialog on OK / Apply.
    void commit(const AppOptions& next) {
        options_ = next;
        // Saved, applied to the backend and pushed to open editors by the store. Keep the
        // cleaned-up list (unknown languages and duplicate keys dropped) as the committed value.
        SyntaxMappingStore::instance().setMappings(options_.syntaxMappings);
        options_.syntaxMappings = SyntaxMappingStore::instance().mappings();
        persist();
        for (const auto& listener : listeners_) listener(options_);
    }

    // Subscribe to "options changed" (theme, fonts, colours, ...). Called after every commit.
    void addListener(Listener listener) { listeners_.push_back(std::move(listener)); }

    // Appearance > Preview: the dialog asks the app to show a theme without committing it.
    // Pass the committed theme to revert. Set by the app (main_window.h).
    std::function<void(ThemeChoice)> previewTheme;

    // Reads the saved profile from preferences.ini; missing keys keep the AppOptions{} defaults.
    // Appearance, Syntax Highlighting, Archive Types, and Folder Filters are persisted.
    void load() {
        options_ = AppOptions{};
        options_.syntaxMappings = SyntaxMappingStore::instance().mappings();
        if (!preferences_) return;
        QSettings& p = *preferences_;
        if (p.contains("appearance/theme")) {
            options_.theme = static_cast<ThemeChoice>(qBound(0, p.value("appearance/theme").toInt(), 2));
        } else if (p.contains("theme/dark")) {   // before Options existed: View > Dark Theme
            options_.theme = p.value("theme/dark").toBool() ? ThemeChoice::Dark : ThemeChoice::Light;
        }
        options_.useSystemFont = p.value("appearance/useDefaultFont", options_.useSystemFont).toBool();
        options_.customFont = p.value("appearance/customFont").toString();
        options_.colorFoldersToReflectContent =
            p.value("appearance/colorFolders", options_.colorFoldersToReflectContent).toBool();
        options_.useGradientForCenterBackground =
            p.value("appearance/gradientCenter", options_.useGradientForCenterBackground).toBool();
        options_.useStripes = p.value("appearance/stripes", options_.useStripes).toBool();
        const QJsonObject colors =
            QJsonDocument::fromJson(p.value("appearance/colors").toString().toUtf8()).object();
        for (auto it = colors.begin(); it != colors.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            ItemColors c;
            c.text = QColor(o.value("text").toString());
            c.background = QColor(o.value("background").toString());
            c.folderInfo = QColor(o.value("info").toString());
            if (c.text.isValid() && c.background.isValid() && c.folderInfo.isValid())
                options_.colors.insert(it.key(), c);
        }
        const QJsonArray filterPresets =
            QJsonDocument::fromJson(p.value("folderFilters/presets").toString().toUtf8())
                .array();
        if (!filterPresets.isEmpty()) {
            const QList<FolderFilterPreset> builtInPresets =
                defaultFolderFilterPresets();
            options_.folderFilterPresets.clear();
            for (const QJsonValue& value : filterPresets) {
                const QJsonObject preset = value.toObject();
                const QString name = preset.value("name").toString().trimmed();
                if (name.isEmpty())
                    continue;
                const bool duplicate = std::any_of(
                    options_.folderFilterPresets.cbegin(),
                    options_.folderFilterPresets.cend(),
                    [&name](const FolderFilterPreset& existing) {
                        return existing.name.compare(name, Qt::CaseInsensitive) == 0;
                    });
                if (duplicate)
                    continue;
                bool builtIn = preset.value("builtIn").toBool(false);
                if (!preset.contains("builtIn")) {
                    builtIn = std::any_of(
                        builtInPresets.cbegin(), builtInPresets.cend(),
                        [&name](const FolderFilterPreset& value) {
                            return value.name == name;
                        });
                }
                options_.folderFilterPresets.append(
                    {name, preset.value("includeFiles").toString(QStringLiteral("*.*")),
                     preset.value("excludeFiles").toString(),
                     preset.value("includeFolders").toString(QStringLiteral("*")),
                     preset.value("excludeFolders").toString(), builtIn});
            }
            if (options_.folderFilterPresets.isEmpty())
                options_.folderFilterPresets = defaultFolderFilterPresets();
        }
        options_.defaultFolderFilterPreset =
            p.value("folderFilters/defaultPreset").toString();
        if (std::none_of(options_.folderFilterPresets.cbegin(),
                         options_.folderFilterPresets.cend(),
                         [this](const FolderFilterPreset& preset) {
                             return preset.name ==
                                    options_.defaultFolderFilterPreset;
                         }))
            options_.defaultFolderFilterPreset.clear();
        options_.javaDecompilerEnabled =
            p.value("ArchiveTypes/JavaDecompilerEnabled", options_.javaDecompilerEnabled).toBool();
        options_.decompilerJarPath = p.value("ArchiveTypes/DecompilerJarPath").toString();
        options_.javaBinPath =
            p.value("ArchiveTypes/JavaBinPath", options_.javaBinPath).toString();
    }

    // Writes the saved profile to preferences.ini.
    void persist() {
        if (!preferences_) return;
        QSettings& p = *preferences_;
        p.setValue("appearance/theme", static_cast<int>(options_.theme));
        p.setValue("appearance/useDefaultFont", options_.useSystemFont);
        p.setValue("appearance/customFont", options_.customFont);
        p.setValue("appearance/colorFolders", options_.colorFoldersToReflectContent);
        p.setValue("appearance/gradientCenter", options_.useGradientForCenterBackground);
        p.setValue("appearance/stripes", options_.useStripes);
        p.setValue("ArchiveTypes/JavaDecompilerEnabled", options_.javaDecompilerEnabled);
        p.setValue("ArchiveTypes/DecompilerJarPath", options_.decompilerJarPath);
        p.setValue("ArchiveTypes/JavaBinPath", options_.javaBinPath);
        QJsonObject colors;
        for (auto it = options_.colors.constBegin(); it != options_.colors.constEnd(); ++it) {
            colors.insert(it.key(), QJsonObject{{"text", it.value().text.name(QColor::HexArgb)},
                                                {"background", it.value().background.name(QColor::HexArgb)},
                                                {"info", it.value().folderInfo.name(QColor::HexArgb)}});
        }
        p.setValue("appearance/colors",
                   QString::fromUtf8(QJsonDocument(colors).toJson(QJsonDocument::Compact)));
        QJsonArray filterPresets;
        for (const FolderFilterPreset& preset : options_.folderFilterPresets) {
            filterPresets.append(QJsonObject{
                {"name", preset.name},
                {"includeFiles", preset.includeFiles},
                {"excludeFiles", preset.excludeFiles},
                {"includeFolders", preset.includeFolders},
                {"excludeFolders", preset.excludeFolders},
                {"builtIn", preset.builtIn},
            });
        }
        p.setValue("folderFilters/presets",
                   QString::fromUtf8(QJsonDocument(filterPresets).toJson(QJsonDocument::Compact)));
        p.setValue("folderFilters/defaultPreset", options_.defaultFolderFilterPreset);
        p.remove("theme/dark");   // superseded by appearance/theme
        p.sync();
    }

private:
    QSettings* preferences_ = nullptr;
    AppOptions options_;
    std::vector<Listener> listeners_;
};

}  // namespace openbc::app

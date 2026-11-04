# Tools > Options: implementation status

Everything for Tools > Options lives in `cpp/options/` (UI, model, persistence, behaviour). Single source for the dialog: `cpp/options/options_status.h` (page status table, drives the grouped list and
the banner) and this file. When a page is finished, update both. Read this first when asked to
"work on option implementation", and continue with the first row of IN PROGRESS.

Files in this folder
| File | Role |
|---|---|
| `app_options.h` | `AppOptions` (one field per control) + `OptionsStore` (load / persist / commit / listeners) |
| `options_dialog.h`, `options_icons.h` | the dialog and its page UIs |
| `options_status.h` | which pages are Implemented / In progress / Pending |
| `appearance_controller.h`, `appearance_state.h` | Appearance behaviour (theme, colours) and the runtime table the style code reads |
| `text_font.h` | Text Compare font + change notifier |
| `syntax_mappings.h` | Syntax Highlighting rules store |
| `../common/compare_font.h` | Shared DejaVu Sans Mono default for file-compare viewers |
| `../common/java_class_support.*`, `../common/java_decompiler_adapter.*` | CFR-backed Java class decompilation and raw byte display |
Rule: a new setting = field in `AppOptions` + control in `options_dialog.h` + load/persist in `OptionsStore` + behaviour in a controller here. Do not add settings logic to `main_window.h` or the viewers beyond reading the result.

Definitions
* **Implemented**: persisted in `preferences.ini` AND applied (a listener changes behaviour).
* **In progress**: controls exist and edit the draft (`AppOptions`), but `OptionsStore::load()/persist()`
  are empty and nothing subscribes via `addListener`, so values are lost on restart and have no effect.
* **Pending**: placeholder page, no design.

Java `.class` files opened in File Compare, including classes opened from JAR archives, are decompiled to Java with CFR by default and shown read-only.
The **Java Source** toggle switches between decompiled source and a hex dump. Install the `cfr` command,
or configure a decompiler executable/JAR and Java binary in **Tools > Options > Archive Types**.
The adapter checks the saved settings for each decompilation. `CFR_JAR` / `OPENBC_CFR_JAR` and
Vineflower's `VINEFLOWER_JAR` / `OPENBC_VINEFLOWER_JAR` remain supported as environment fallbacks.
JAR-based decompilers use Java through `PATH`, `JAVA_HOME`, or the configured Java Runtime path.
The Flatpak package includes a compact Java runtime and configures it as `JAVA_HOME` for
decompilation when a host Java path is not available inside the sandbox.

To implement a page: (1) load/persist its fields in `OptionsStore`, (2) add a listener (or consume
`optionsStore.options()`) where the behaviour lives, (3) move its row in `options_status.h` and below.

## Implemented
| Page | Notes |
|---|---|
| Syntax Highlighting | `syntaxMappings` via `SyntaxMappingStore` (preferences.ini, backend, re-highlight) |
| Appearance | Saved by `OptionsStore` (`appearance/*` in preferences.ini), applied by `AppearanceController` (`appearance_controller.h`). Theme + Preview (`OptionsStore::previewTheme`, reverted on Cancel; `View > Dark Theme` commits the same option). File Compare viewer default font: DejaVu Sans Mono (system fixed-width fallback); Text Compare's custom font remains configurable via `QFontComboBox` and is pushed to `LineNumberEditor::applyEditorFont`. Colors go through `appearance::state()` (`appearance_state.h`, read by `style/qt_style.h`): Folder Views (Default text/Folder info, Selection, Unknown/Same/Orphan/Older/Newer/Different, Log panel), File Views (Default text, Line numbers, Selection, Different/Left only/Right only/Unimportant), stripes, center gradient (`#folderGutter`), folder-icon coloring. **No consumer yet** (kept, no effect): Folder Views "Filtered out"/"Merge colors", File Views "Same", all Picture Compare colors. |
| Archive Types | `javaDecompilerEnabled`, `decompilerJarPath`, `javaBinPath` (`ArchiveTypes/*` in preferences.ini); Java class decompilation reads the current values for each operation |
| Folder Filters | Named include/exclude file and folder masks plus the default preset for new folder comparisons (`folderFilters/*` in preferences.ini) |

## In progress (UI only)
| Page | Fields (AppOptions) | Where it should take effect |
|---|---|---|
| Startup | loadWorkspace, saveWorkspaceOnExit, showQuickCompareDialog, quickCompareMode, openViewIfFilesDiffer, runClipboardCompareOnStartup, explorerIntegration, explorerCommands, hideSelectLeftIfSaved | main_window.h startup; file-manager menus (see packaging/file-manager) |
| Tabs | openSessionsIn, openChildSessionsIn, openNewTabNextToActive, closingLastTabClosesWindow, warnClosingMultipleTabs/Windows, hideTabBarIfSingleTab, showTabPreviewsInTaskbar | main_window.h (addViewerTab, closeTab) |
| Text Editing | autoIndent, backspaceUnindents, initFindWithCurrentWord, showFilteredLineCounts, contextLines | line_number_editor.h, find_replace_bar.h, text viewer (context uses a fixed 2 today) |
| Next Difference | goToFirstDifferenceOnLoad, goToNextDifferenceAfterCopy, limitNextDifferenceFilesToFolder, wrapAroundToFirstDifference, showMessagePanel | text/table/registry viewers |
| Backups | backupBeforeCopy, backupBeforeSave, backupNaming, backupFolder | saveSide(), folder transferFile() |
| File Operations | confirmCopy/Move/ReplaceReadOnly/ReplaceSystem/OverwriteNewer/ReplaceDuringMove/ContentCompare/Delete/Merge, requireExplicitSideInDelete, syncConfirmation, includeHiddenItemsByDefault | folder_compare_viewer.h |
| Commands | largeToolbarButtonsWithText, commandsByView | toolbars/menus, main_window.h rebuildMenus |
| Tweaks | checkForUpdates, updateIntervalDays, syntaxHighlightOnDiffLines, crosshatchBeyondEndOfFile, rightGutterForLeftEditor, useOrphanColor, extraLineSpacingPx, columnLineAt, dimInactivePanePercent, useDifferentMergeInputFont/mergeInputFont, beepAfterLongFileOperations | various viewers |

## Pending
| Page | Notes |
|---|---|
| Open With | placeholder, no design |

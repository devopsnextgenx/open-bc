# Tools > Options: implementation status

Single source for the dialog: `cpp/app/options_status.h` (page status table, drives the grouped list and
the banner) and this file. When a page is finished, update both. Read this first when asked to
"work on option implementation", and continue with the first row of IN PROGRESS.

Definitions
* **Implemented**: persisted in `preferences.ini` AND applied (a listener changes behaviour).
* **In progress**: controls exist and edit the draft (`AppOptions`), but `OptionsStore::load()/persist()`
  are empty and nothing subscribes via `addListener`, so values are lost on restart and have no effect.
* **Pending**: placeholder page, no design.

To implement a page: (1) load/persist its fields in `OptionsStore`, (2) add a listener (or consume
`optionsStore.options()`) where the behaviour lives, (3) move its row in `options_status.h` and below.

## Implemented
| Page | Notes |
|---|---|
| Syntax Highlighting | `syntaxMappings` via `SyntaxMappingStore` (preferences.ini, backend, re-highlight) |

## In progress (UI only)
| Page | Fields (AppOptions) | Where it should take effect |
|---|---|---|
| Startup | loadWorkspace, saveWorkspaceOnExit, showQuickCompareDialog, quickCompareMode, openViewIfFilesDiffer, runClipboardCompareOnStartup, explorerIntegration, explorerCommands, hideSelectLeftIfSaved | main_window.h startup; file-manager menus (see packaging/file-manager) |
| Tabs | openSessionsIn, openChildSessionsIn, openNewTabNextToActive, closingLastTabClosesWindow, warnClosingMultipleTabs/Windows, hideTabBarIfSingleTab, showTabPreviewsInTaskbar | main_window.h (addViewerTab, closeTab) |
| Appearance | theme, previewTheme, useSystemFont, customFont, colorFoldersToReflectContent, useGradientForCenterBackground, useStripes, colors | style/qt_style.h, folder viewer palette |
| Text Editing | autoIndent, backspaceUnindents, initFindWithCurrentWord, showFilteredLineCounts, contextLines | line_number_editor.h, find_replace_bar.h, text viewer (context uses a fixed 2 today) |
| Next Difference | goToFirstDifferenceOnLoad, goToNextDifferenceAfterCopy, limitNextDifferenceFilesToFolder, wrapAroundToFirstDifference, showMessagePanel | text/table/registry viewers |
| Backups | backupBeforeCopy, backupBeforeSave, backupNaming, backupFolder | saveSide(), folder transferFile() |
| File Operations | confirmCopy/Move/ReplaceReadOnly/ReplaceSystem/OverwriteNewer/ReplaceDuringMove/ContentCompare/Delete/Merge, requireExplicitSideInDelete, syncConfirmation, includeHiddenItemsByDefault | folder_compare_viewer.h |
| Commands | largeToolbarButtonsWithText, commandsByView | toolbars/menus, main_window.h rebuildMenus |
| Tweaks | checkForUpdates, updateIntervalDays, syntaxHighlightOnDiffLines, crosshatchBeyondEndOfFile, rightGutterForLeftEditor, useOrphanColor, extraLineSpacingPx, columnLineAt, dimInactivePanePercent, useDifferentMergeInputFont/mergeInputFont, beepAfterLongFileOperations | various viewers |

## Pending
| Page | Notes |
|---|---|
| Archive Types | placeholder, no design |
| Open With | placeholder, no design |

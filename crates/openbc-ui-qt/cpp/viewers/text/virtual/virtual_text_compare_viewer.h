// virtual_text_compare_viewer.h
// ---------------------------------------------------------------------------
// The tab used when the Virtual editor is selected (Tools > Options > Text Compare). Same role as
// TextCompareViewer, built on the Qt-independent engine:
//
//   [ VirtualTextEditor (left) ] [ overview ] [ VirtualTextEditor (right) ]
//
// Both editors read ONE shared TextModel (memory-mapped files, hash index, global diff). The model
// is built on a worker thread (progress + cancel); nothing here ever loads a file into a QString.
//
// Opens READ-ONLY. A lock icon in the status bar (red lock = read-only, green open lock = editing)
// turns write mode on demand; trying to edit while locked flashes it red-blue-yellow-red (1.2 s).
// In write mode: typing / paste / cut / delete / Enter on either side, copy a difference block to the
// other side (arrow in the gutter or the Copy button), per-side undo / redo, Save (in place, atomic,
// encoding + BOM + line endings preserved), Save As, and the usual unsaved-changes prompt when the tab
// is closed or its content would be replaced. Edits live in the engine (piece-table overlay over the
// memory-mapped file), never in a QTextDocument.
//
// Also: line numbers, syntax highlighting, changed-line + inline-difference colouring, selection /
// copy, find, go to line, next/previous difference, synchronised scrolling, overview bar, file-change
// notices with explicit reload, Swap sides, and Rules (ignore samples or /regex/ parts of lines, same
// syntax as the Existing editor's Rules bar).
// Not supported (documented limitations): "Minor"/row filtering, word wrap, replace.
//
// Header/moc-free like the other viewers: notifications are std::function callbacks.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QProgressBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <functional>
#include <memory>

#include "common/diff_line_preview.h"
#include "common/diff_navigator.h"
#include "common/minimap.h"
#include "common/path_selector.h"
#include "common/unsaved_changes_dialog.h"
#include "common/file_change_controller.h"
#include "viewers/text/engine/text_model.h"
#include "viewers/text/virtual/virtual_text_editor.h"

namespace openbc::app {

class VirtualTextCompareViewer : public QWidget {
public:
    VirtualTextCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr);
    ~VirtualTextCompareViewer() override;

    QString title() const;
    QString leftText() const { return leftPath_; }
    QString rightText() const { return rightPath_; }
    // Drop target entry point: an empty argument leaves that side untouched.
    void applyDroppedPaths(const QString& left, const QString& right);

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> actionsMenuItems() const { return {reloadAction_, swapAction_}; }
    QList<QAction*> editMenuItems() const {
        return {undoAction_, redoAction_, saveAction_, saveAsAction_, nullptr, copyAction_, selectAllAction_};
    }
    QList<QAction*> searchMenuItems() const {
        return {findAction_, findNextAction_, findPrevAction_, gotoLineAction_, nextDiffAction_, prevDiffAction_};
    }
    QList<QAction*> viewMenuItems() const { return {minimapAction_, syntaxAction_}; }

    // For tests and status reporting.
    VirtualTextEditor* leftEditor() const { return left_; }
    VirtualTextEditor* rightEditor() const { return right_; }
    std::shared_ptr<TextModel> model() const { return model_; }
    bool loading() const { return loading_; }
    QString statusText() const { return status_->text(); }

    void reload(bool keepScroll = true);

    // ---- write mode -------------------------------------------------------------------------
    bool writeEnabled() const { return writeEnabled_; }
    // Lock (false) / unlock (true). Unlocking on Windows reopens the files with buffered I/O first,
    // because a memory-mapped file cannot be replaced there.
    void setWriteMode(bool enabled);
    // Lock icon introspection (tests, accessibility).
    QToolButton* lockButton() const { return lockButton_; }
    QToolButton* saveButton(bool left) const { return left ? leftSaveButton_ : rightSaveButton_; }
    QAction* copyDiffAction() const { return copyDiffAction_; }
    QAction* rulesAction() const { return rulesAction_; }
    QLabel* rulesStatusLabel() const { return rulesStatus_; }
    bool lockFlashing() const { return lockTimer_.isActive(); }
    QColor lockColor() const { return lockColor_; }
    int lockFlashStep() const { return lockFlashStep_; }   // 0..11 while flashing
    // Flash the lock: red-blue-yellow-red, 100 ms per colour, 3 rounds = 1.2 s. Restarts if running.
    void flashLock();
    // Copy the changed block containing `row` from `from` to the other side.
    void copyBlockAt(quint64 row, Side from);

    // ---- unsaved changes --------------------------------------------------------------------
    bool hasUnsavedChanges() const;
    // Shows the unsaved-changes dialog when needed. True = it is fine to close / replace the content.
    bool confirmClose() { return confirmDiscard(); }
    bool confirmDiscard();
    // Saves one side; `done` is called exactly once (false carries a reason). A side without a file
    // asks for a name (Save As).
    void saveSide(bool left, SaveDone done = {});
    bool taskBusy() const { return taskRunner_ && taskRunner_->busy(); }

    // ---- rules ------------------------------------------------------------------------------
    // Ignore rules (same syntax as the Existing editor: comma separated samples, `,,` = a comma,
    // `/regex/` entries). Empty clears them. Runs on a worker thread; edits and undo are kept.
    void applyRules(const QString& text);
    QString rulesText() const { return rulesText_; }
    bool rulesBarVisible() const { return rulesBar_ && !rulesBar_->isHidden(); }
    QLineEdit* rulesEdit() const { return rulesEdit_; }

private:
    void buildUi();
    void buildActions();
    void wireEditors();
    void startLoad(bool keepScroll);
    void onModelReady(std::shared_ptr<TextModel> model);
    void applyFont();
    void updateStatus();
    void updatePreview(quint64 row);
    void updateOverview();
    void refreshSideHeader(bool left);
    void commitPath(bool left, const QString& path);
    void handleEdited(const ModelEdit& edit);
    void copyCurrentBlock();
    void saveActive(bool saveAs);
    void refreshDirtyUi();
    void updateEditActions();
    void updateLockIcon();
    void showRulesBar(bool show);
    bool anyDirty() const;
    void showFindBar(bool show);
    void findNext(bool forward);
    VirtualTextEditor* activeEditor() const;

    QString leftPath_, rightPath_;
    std::shared_ptr<TextModel> model_;
    std::unique_ptr<TextModelLoader> loader_;
    bool loading_ = false;
    bool keepScrollOnLoad_ = false;
    quint64 savedTopRow_ = 0;

    VirtualTextEditor* left_ = nullptr;
    VirtualTextEditor* right_ = nullptr;
    DifferenceOverview* overview_ = nullptr;
    QToolBar* toolbar_ = nullptr;
    PathSelector* leftSelector_ = nullptr;
    PathSelector* rightSelector_ = nullptr;
    QLabel* leftAttrsLabel_ = nullptr;
    QLabel* rightAttrsLabel_ = nullptr;
    QToolButton* leftSaveButton_ = nullptr;
    QToolButton* rightSaveButton_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* editBadge_ = nullptr;             // EDITABLE / READ ONLY, as in the Existing editor's status bar
    DiffLinePreview* preview_ = nullptr;      // bottom line-by-line compare strip (shared with TextCompareViewer)
    DiffNavigator* diffNavigator_ = nullptr;  // up/down arrows in the right editor's bottom-right corner
    QProgressBar* progress_ = nullptr;
    QWidget* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QCheckBox* caseBox_ = nullptr;
    VirtualTextEditor* lastFocused_ = nullptr;
    bool syncing_ = false;

    // write mode / lock icon
    bool writeEnabled_ = false;
    bool bufferedIo_ = false;
    QToolButton* lockButton_ = nullptr;
    QTimer lockTimer_;
    int lockFlashStep_ = 0;
    QColor lockColor_;
    bool lastDirty_ = false;
    std::unique_ptr<ModelTaskRunner> taskRunner_;

    // rules
    QWidget* rulesBar_ = nullptr;
    QLineEdit* rulesEdit_ = nullptr;
    QLabel* rulesStatus_ = nullptr;
    QString rulesText_;

    QAction *homeAction_ = nullptr, *sessionsAction_ = nullptr, *minimapAction_ = nullptr,
            *copyDiffAction_ = nullptr, *reloadAction_ = nullptr, *swapAction_ = nullptr,
            *saveAction_ = nullptr, *saveAsAction_ = nullptr, *undoAction_ = nullptr, *redoAction_ = nullptr,
            *rulesAction_ = nullptr,
            *copyAction_ = nullptr, *selectAllAction_ = nullptr,
            *findAction_ = nullptr, *findNextAction_ = nullptr, *findPrevAction_ = nullptr,
            *gotoLineAction_ = nullptr, *nextDiffAction_ = nullptr, *prevDiffAction_ = nullptr,
            *syntaxAction_ = nullptr;

    std::unique_ptr<FileChangeController> changes_;  // destroy before the viewer state it calls back into
};

}  // namespace openbc::app

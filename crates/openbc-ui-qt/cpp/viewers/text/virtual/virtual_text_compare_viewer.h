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
// Read-only. Supported: line numbers, syntax highlighting, changed-line + inline-difference
// colouring, selection/copy, keyboard navigation, find / next / previous, go to line, next/previous
// difference, synchronised scrolling, overview bar, file-change notices with explicit reload, Swap sides.
// Not supported (documented limitations): editing, merge/copy-to-other-side, "ignore text" regex
// rules, replace. Use the Existing editor for those.
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
#include <QToolBar>
#include <QToolButton>
#include <QWidget>
#include <functional>
#include <memory>

#include "common/minimap.h"
#include "common/path_selector.h"
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
        return {undoAction_, redoAction_, saveAction_, nullptr, copyAction_, selectAllAction_};
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

private:
    void buildUi();
    void buildActions();
    void wireEditors();
    void startLoad(bool keepScroll);
    void onModelReady(std::shared_ptr<TextModel> model);
    void applyFont();
    void updateStatus();
    void updateOverview();
    void refreshSideHeader(bool left);
    void commitPath(bool left, const QString& path);
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
    QProgressBar* progress_ = nullptr;
    QWidget* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QCheckBox* caseBox_ = nullptr;
    VirtualTextEditor* lastFocused_ = nullptr;
    bool syncing_ = false;

    QAction *homeAction_ = nullptr, *sessionsAction_ = nullptr, *minimapAction_ = nullptr,
            *copyDiffAction_ = nullptr, *reloadAction_ = nullptr, *swapAction_ = nullptr,
            *saveAction_ = nullptr, *undoAction_ = nullptr, *redoAction_ = nullptr,
            *copyAction_ = nullptr, *selectAllAction_ = nullptr,
            *findAction_ = nullptr, *findNextAction_ = nullptr, *findPrevAction_ = nullptr,
            *gotoLineAction_ = nullptr, *nextDiffAction_ = nullptr, *prevDiffAction_ = nullptr,
            *syntaxAction_ = nullptr;

    std::unique_ptr<FileChangeController> changes_;  // destroy before the viewer state it calls back into
};

}  // namespace openbc::app

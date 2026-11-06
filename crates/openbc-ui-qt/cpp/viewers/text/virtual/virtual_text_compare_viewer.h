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
// difference, synchronised scrolling, overview bar, reload when a file changes on disk, Swap sides.
// Not supported (documented limitations): editing, merge/copy-to-other-side, "ignore text" regex
// rules, replace. Use the Existing editor for those.
//
// Header/moc-free like the other viewers: notifications are std::function callbacks.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QCheckBox>
#include <QFileSystemWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QProgressBar>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <memory>

#include "viewers/text/engine/text_model.h"
#include "viewers/text/virtual/virtual_text_editor.h"

namespace openbc::app {

// Whole-file difference strip. One pixel row summarises a slice of display rows (computed in Rust
// from the diff index in O(segments + pixels), no line data touched).
class ModelOverview : public QWidget {
public:
    explicit ModelOverview(QWidget* parent = nullptr);
    void setModel(std::shared_ptr<TextModel> model);
    void setViewport(quint64 firstRow, int visibleRows);
    std::function<void(quint64 row)> onRowRequested;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void request(int y);
    std::shared_ptr<TextModel> model_;
    QByteArray cells_;
    quint64 first_ = 0;
    int visible_ = 1;
};

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
    QList<QAction*> editMenuItems() const { return {copyAction_, selectAllAction_}; }
    QList<QAction*> searchMenuItems() const {
        return {findAction_, findNextAction_, findPrevAction_, gotoLineAction_, nextDiffAction_, prevDiffAction_};
    }
    QList<QAction*> viewMenuItems() const { return {syntaxAction_}; }

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
    void showFindBar(bool show);
    void findNext(bool forward);
    VirtualTextEditor* activeEditor() const;
    void watchFiles();

    QString leftPath_, rightPath_;
    std::shared_ptr<TextModel> model_;
    std::unique_ptr<TextModelLoader> loader_;
    bool loading_ = false;
    bool keepScrollOnLoad_ = false;
    quint64 savedTopRow_ = 0;

    VirtualTextEditor* left_ = nullptr;
    VirtualTextEditor* right_ = nullptr;
    ModelOverview* overview_ = nullptr;
    QLabel* leftLabel_ = nullptr;
    QLabel* rightLabel_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QWidget* findBar_ = nullptr;
    QLineEdit* findEdit_ = nullptr;
    QCheckBox* caseBox_ = nullptr;
    QFileSystemWatcher watcher_;
    QTimer reloadTimer_;
    VirtualTextEditor* lastFocused_ = nullptr;
    bool syncing_ = false;

    QAction *reloadAction_ = nullptr, *swapAction_ = nullptr, *copyAction_ = nullptr, *selectAllAction_ = nullptr,
            *findAction_ = nullptr, *findNextAction_ = nullptr, *findPrevAction_ = nullptr,
            *gotoLineAction_ = nullptr, *nextDiffAction_ = nullptr, *prevDiffAction_ = nullptr,
            *syntaxAction_ = nullptr;
};

}  // namespace openbc::app

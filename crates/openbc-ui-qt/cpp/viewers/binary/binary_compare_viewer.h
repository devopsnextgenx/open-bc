// binary_compare_viewer.h
// ---------------------------------------------------------------------------
// BinaryCompareViewer: read-only comparison of two versions of a compiled binary
// or shared library (.exe .dll .so .dylib, ELF / PE / Mach-O).
//
// Version 1 (baseline) is the left path, Version 2 (target) the right path. The Rust
// backend (openbc-engine binary actor -> openbc-compute LIEF extractor -> openbc-core diff)
// parses both files, hashes the file / sections / functions, classifies every element as
// unchanged, modified, removed or added, and hands back a pre-order list of tree rows.
// This class never opens or parses a binary itself and has no editing or patching feature.
//
// The tree is virtualized (BinaryTreeModel) so a result with hundreds of thousands of
// functions or strings stays responsive; it is rebuilt only when a new result arrives.
// ---------------------------------------------------------------------------
#pragma once

#include <QAbstractItemModel>
#include <QAction>
#include <QLabel>
#include <QList>
#include <QProgressBar>
#include <QString>
#include <QToolBar>
#include <QTreeView>
#include <QVector>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>

#include "backend/backend_channel.h"
#include "backend/compare_backend_api.h"
#include "common/path_selector.h"
#include "common/file_change_controller.h"

namespace openbc::app {

using BinarySummaryPtr = std::shared_ptr<OpenBcBinarySummary>;

// Change-state codes shared with openbc_core::binary_compare::ChangeState::code().
enum class BinaryState : std::uint8_t { Unchanged = 0, Modified = 1, Removed = 2, Added = 3, None = 255 };

// Read-only tree model over the backend's pre-order row list.
class BinaryTreeModel : public QAbstractItemModel {
public:
    enum Column { ColElement = 0, ColStatus, ColDetail, ColCount };

    explicit BinaryTreeModel(QObject* parent = nullptr);

    // Replaces the whole tree with `summary` (null clears it).
    void setSummary(BinarySummaryPtr summary);

    // Backend row index of `index` (the node's position in the pre-order list), or -1.
    int backendRow(const QModelIndex& index) const;
    BinaryState stateOf(const QModelIndex& index) const;
    bool isLeaf(const QModelIndex& index) const;
    // Model index for a backend row.
    QModelIndex indexForBackendRow(int row, int column = 0) const;
    int rowCountTotal() const { return parent_.size(); }

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override { Q_UNUSED(parent); return ColCount; }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    QString text(int row, bool detail) const;

    BinarySummaryPtr summary_;
    QVector<int> parent_;                  // backend row -> parent backend row (-1: top level)
    QVector<int> position_;                // backend row -> index among its siblings
    QVector<QVector<int>> children_;       // backend row -> child backend rows
    QVector<int> topLevel_;                // rows without a parent (normally just the root)
};

class BinaryCompareViewer : public QWidget {
public:
    BinaryCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr);
    ~BinaryCompareViewer() override;

    QString title() const;
    QString detailedTitle() const;
    QString leftText() const { return leftPath_; }
    QString rightText() const { return rightPath_; }

    // Drop target entry point (common/drop_router.h): an empty argument leaves that side untouched.
    void applyDroppedPaths(const QString& left, const QString& right);

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> actionsMenuItems() const { return {reloadAction_}; }
    QList<QAction*> searchMenuItems() const { return {nextAction_, prevAction_}; }
    QList<QAction*> viewMenuItems() const { return {showUnchangedAction_, expandAction_, collapseAction_}; }

private:
    static void destroyHandle(int kind, void* handle);
    static QString cleanPathInput(QString raw);

    void buildUi();
    void startSession();
    void stopSession();
    void commitPath(std::uint8_t side, const QString& raw);
    void reloadLatest();
    void onEvent(const BackendChannel::Event& event);
    void applySummary(const BinarySummaryPtr& summary);
    void jumpToChange(bool forward);

    QString leftPath_;
    QString rightPath_;
    OpenBcBinarySession* session_ = nullptr;
    BinarySummaryPtr summary_;

    QToolBar* toolbar_ = nullptr;
    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* nextAction_ = nullptr;
    QAction* prevAction_ = nullptr;
    QAction* reloadAction_ = nullptr;
    QAction* showUnchangedAction_ = nullptr;
    QAction* expandAction_ = nullptr;
    QAction* collapseAction_ = nullptr;
    PathSelector* selectors_[2] = {nullptr, nullptr};
    QTreeView* tree_ = nullptr;
    BinaryTreeModel* model_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    std::unique_ptr<FileChangeController> changes_;  // external-change notice + reload
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app

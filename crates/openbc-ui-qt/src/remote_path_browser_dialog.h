// remote_path_browser_dialog.h
// ---------------------------------------------------------------------------
// RemotePathBrowserDialog: what PathSelector's Browse button opens now that
// a path can come from "This computer" (a real QFileSystemModel tree) or
// from any saved remote connection (a lazily-populated tree of that host's
// folders). Folder-compare pickers restrict both trees to directories;
// TextCompareView's file pickers show files too - the same File/Folder
// split PathSelector already has.
//
// Talking to an actual remote host goes through RemoteBrowseBridge, an
// abstract two-method interface (connect + list a directory) rather than
// any networking code living in this file. That's the seam where this Qt
// layer should call into the openbc-vfs Rust crate's SftpVfs/FtpVfs/
// NetworkVfs (see sftp.rs/ftp.rs/network.rs) through whatever FFI/IPC this
// app already uses to cross that boundary - not shown here since it wasn't
// part of the files this dialog was built from. Until a bridge is wired up
// (pass nullptr, the default), the remote tab still lets the user pick a
// saved profile and type the remote path directly, so browsing degrades
// gracefully instead of being unusable.
// ---------------------------------------------------------------------------
#pragma once

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

#include "qt_style.h"
#include "remote_connections_dialog.h"
#include "remote_profile.h"
#include "remote_profile_store.h"

namespace openbc::app {

enum class PathPickMode { Folder, File };

// One entry returned by a remote directory listing.
struct RemoteEntry {
    QString name;
    bool isDir = false;
};

// Abstract bridge to real network I/O. Implement these two calls against
// the app's Rust bridge to make remote browsing live; everything else in
// this dialog is already wired to call through here.
class RemoteBrowseBridge {
public:
    virtual ~RemoteBrowseBridge() = default;
    virtual void connectProfile(const RemoteProfile& profile,
                                 std::function<void(bool ok, const QString& error)> done) = 0;
    virtual void listDirectory(
        const RemoteProfile& profile, const QString& path,
        std::function<void(bool ok, QList<RemoteEntry> entries, const QString& error)> done) = 0;
    virtual void disconnectProfile(const RemoteProfile&) {}
};

class RemotePathBrowserDialog : public QDialog {
public:
    explicit RemotePathBrowserDialog(PathPickMode mode, RemoteBrowseBridge* bridge = nullptr,
                                      QWidget* parent = nullptr)
        : QDialog(parent), mode_(mode), bridge_(bridge) {
        setWindowTitle(mode_ == PathPickMode::Folder ? "Select folder" : "Select file");
        resize(760, 540);
        auto* root = new QVBoxLayout(this);

        auto* sourceRow = new QHBoxLayout;
        sourceRow->addWidget(new QLabel("Location:", this));
        source_ = new QComboBox(this);
        sourceRow->addWidget(source_, 1);
        manage_ = new QToolButton(this);
        manage_->setText("Manage connections...");
        sourceRow->addWidget(manage_);
        root->addLayout(sourceRow);

        stack_ = new QStackedWidget(this);
        root->addWidget(stack_, 1);
        buildLocalPage();
        buildRemotePage();

        auto* pathRow = new QHBoxLayout;
        pathRow->addWidget(new QLabel("Path:", this));
        selectedPathEdit_ = new QLineEdit(this);
        pathRow->addWidget(selectedPathEdit_, 1);
        root->addLayout(pathRow);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText("Select");
        root->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() { attemptAccept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, [this]() { reject(); });

        connect(source_, &QComboBox::currentIndexChanged, this, [this](int) { handleSourceChanged(); });
        connect(manage_, &QToolButton::clicked, this, [this]() {
            RemoteConnectionsDialog dialog(this);
            dialog.exec();
            refreshSourceList();
        });

        refreshSourceList();
        handleSourceChanged();
    }

    // Seeds the path field with the row's current value, so opening the
    // dialog and clicking Select without picking anything new keeps it.
    void setInitialPath(const QString& path) { selectedPathEdit_->setText(path); }

    QString selectedPath() const { return selectedPathEdit_->text().trimmed(); }
    // Set only when the accepted path came from a remote connection; check
    // this to decide whether the caller should route reads through
    // openbc-vfs instead of the local filesystem.
    bool selectedIsRemote() const { return isRemoteResult_; }
    RemoteProfile selectedProfile() const { return currentProfile_; }

private:
    void buildLocalPage() {
        auto* page = new QWidget(stack_);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        localModel_ = new QFileSystemModel(page);
        localModel_->setRootPath(QString());
        const QDir::Filters filters = mode_ == PathPickMode::Folder
                                           ? (QDir::AllDirs | QDir::Drives | QDir::NoDotAndDotDot)
                                           : (QDir::AllDirs | QDir::Files | QDir::Drives | QDir::NoDotAndDotDot);
        localModel_->setFilter(filters);
        localTree_ = new QTreeView(page);
        localTree_->setModel(localModel_);
        localTree_->setRootIndex(localModel_->index(QString()));
        for (int column = 1; column < localModel_->columnCount(); ++column) localTree_->setColumnHidden(column, true);
        localTree_->header()->setStretchLastSection(true);
        layout->addWidget(localTree_, 1);
        connect(localTree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
            const bool isDir = localModel_->isDir(index);
            if (mode_ == PathPickMode::Folder && !isDir) return;
            isRemoteResult_ = false;
            currentProfile_ = {};
            selectedPathEdit_->setText(QDir::toNativeSeparators(localModel_->filePath(index)));
        });
        stack_->addWidget(page);
    }

    void buildRemotePage() {
        auto* page = new QWidget(stack_);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        remoteStatus_ = new QLabel("Pick a saved connection above, or add one with \"Manage connections...\".", page);
        remoteStatus_->setWordWrap(true);
        layout->addWidget(remoteStatus_);

        remoteTree_ = new QTreeWidget(page);
        remoteTree_->setHeaderHidden(true);
        layout->addWidget(remoteTree_, 1);
        connect(remoteTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { expandRemoteNode(item); });
        connect(remoteTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) { pickRemoteNode(item); });

        // Shown instead of the tree when no RemoteBrowseBridge is wired up:
        // the user can still type/adjust the remote path directly and pick
        // it, they just don't get live directory listings.
        manualRow_ = new QWidget(page);
        auto* manualLayout = new QHBoxLayout(manualRow_);
        manualLayout->setContentsMargins(0, 0, 0, 0);
        manualLayout->addWidget(new QLabel("Remote path:", manualRow_));
        manualPathEdit_ = new QLineEdit(manualRow_);
        manualLayout->addWidget(manualPathEdit_, 1);
        layout->addWidget(manualRow_);
        connect(manualPathEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
            isRemoteResult_ = true;
            selectedPathEdit_->setText(buildRemoteUri(currentProfile_, text));
        });

        stack_->addWidget(page);
    }

    void refreshSourceList() {
        const QSignalBlocker blocker(source_);
        const QString previous = source_->currentData().toString();
        source_->clear();
        source_->addItem("This computer", QString());
        for (const auto& node : RemoteProfileStore::loadTree()) addProfilesToCombo(node);
        const int restored = source_->findData(previous);
        source_->setCurrentIndex(restored >= 0 ? restored : 0);
    }

    void addProfilesToCombo(const RemoteProfileNode& node) {
        for (const auto& profile : node.profiles)
            source_->addItem(node.name + " / " + profile.name, node.path + QChar(0x1) + profile.name);
        for (const auto& child : node.children) addProfilesToCombo(child);
    }

    void handleSourceChanged() {
        const QString key = source_->currentData().toString();
        if (key.isEmpty()) {
            stack_->setCurrentIndex(0);
            return;
        }
        stack_->setCurrentIndex(1);
        const auto parts = key.split(QChar(0x1));
        if (parts.size() != 2) return;
        RemoteProfile profile;
        if (!RemoteProfileStore::findProfileByName(parts[0], parts[1], &profile)) return;
        currentProfile_ = profile;
        remoteTree_->clear();

        if (!bridge_) {
            remoteStatus_->setText("Remote browsing isn't wired up to a live backend in this build - type the path "
                                    "on \"" + profile.name + "\" below.");
            remoteTree_->setVisible(false);
            manualRow_->setVisible(true);
            manualPathEdit_->setText(profile.remoteRoot);
            return;
        }

        remoteTree_->setVisible(true);
        manualRow_->setVisible(false);
        remoteStatus_->setText("Connecting to " + profile.name + "...");
        bridge_->connectProfile(profile, [this, profile](bool ok, const QString& error) {
            if (currentProfile_.name != profile.name) return;  // user switched away meanwhile
            if (!ok) {
                remoteStatus_->setText("Couldn't connect to " + profile.name + ": " + error);
                return;
            }
            remoteStatus_->setText("Connected to " + profile.name + ".");
            auto* rootItem = new QTreeWidgetItem(remoteTree_, {profile.remoteRoot});
            rootItem->setIcon(0, openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow()));
            rootItem->setData(0, kPathRole, profile.remoteRoot);
            rootItem->setData(0, kIsDirRole, true);
            markExpandable(rootItem);
            remoteTree_->addTopLevelItem(rootItem);
            rootItem->setExpanded(true);
        });
    }

    void markExpandable(QTreeWidgetItem* item) {
        auto* placeholder = new QTreeWidgetItem(item, {"Loading..."});
        placeholder->setData(0, kDummyRole, true);
    }

    void expandRemoteNode(QTreeWidgetItem* item) {
        if (item->childCount() != 1 || !item->child(0)->data(0, kDummyRole).toBool()) return;  // already loaded
        if (!bridge_) return;
        const QString path = item->data(0, kPathRole).toString();
        bridge_->listDirectory(currentProfile_, path,
                               [this, item, path](bool ok, QList<RemoteEntry> entries, const QString& error) {
            delete item->takeChild(0);  // remove the "Loading..." placeholder
            if (!ok) {
                auto* errorItem = new QTreeWidgetItem(item, {"Couldn't list this folder: " + error});
                errorItem->setDisabled(true);
                return;
            }
            std::sort(entries.begin(), entries.end(), [](const RemoteEntry& a, const RemoteEntry& b) {
                if (a.isDir != b.isDir) return a.isDir;
                return a.name.localeAwareCompare(b.name) < 0;
            });
            for (const auto& entry : entries) {
                if (!entry.isDir && mode_ == PathPickMode::Folder) continue;
                auto* child = new QTreeWidgetItem(item, {entry.name});
                child->setIcon(0, entry.isDir ? openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow())
                                              : openbc::ui::icons::glyph(openbc::ui::icons::Glyph::FolderOpen));
                const QString childPath = path.endsWith('/') ? path + entry.name : path + "/" + entry.name;
                child->setData(0, kPathRole, childPath);
                child->setData(0, kIsDirRole, entry.isDir);
                if (entry.isDir) markExpandable(child);
            }
        });
    }

    void pickRemoteNode(QTreeWidgetItem* item) {
        if (!item || item->data(0, kDummyRole).toBool()) return;
        const bool isDir = item->data(0, kIsDirRole).toBool();
        if (mode_ == PathPickMode::Folder && !isDir) return;
        isRemoteResult_ = true;
        selectedPathEdit_->setText(buildRemoteUri(currentProfile_, item->data(0, kPathRole).toString()));
    }

    static QString buildRemoteUri(const RemoteProfile& profile, const QString& remotePath) {
        if (profile.name.isEmpty()) return remotePath;
        if (profile.protocol == RemoteProtocol::NetworkDrive) {
            const QString base = profile.mountPath;
            if (remotePath.isEmpty() || remotePath == "/") return base;
            return QDir(base).filePath(remotePath);
        }
        QString path = remotePath;
        if (!path.startsWith('/')) path.prepend('/');
        return profile.displayAddress() + path;
    }

    void attemptAccept() {
        if (selectedPath().isEmpty()) {
            QMessageBox::information(this, windowTitle(), "Pick a path before continuing.");
            return;
        }
        accept();
    }

    static constexpr int kPathRole = Qt::UserRole;
    static constexpr int kIsDirRole = Qt::UserRole + 1;
    static constexpr int kDummyRole = Qt::UserRole + 2;

    PathPickMode mode_;
    RemoteBrowseBridge* bridge_ = nullptr;
    QComboBox* source_ = nullptr;
    QToolButton* manage_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QFileSystemModel* localModel_ = nullptr;
    QTreeView* localTree_ = nullptr;
    QLabel* remoteStatus_ = nullptr;
    QTreeWidget* remoteTree_ = nullptr;
    QWidget* manualRow_ = nullptr;
    QLineEdit* manualPathEdit_ = nullptr;
    QLineEdit* selectedPathEdit_ = nullptr;
    RemoteProfile currentProfile_;
    bool isRemoteResult_ = false;
};

}  // namespace openbc::app

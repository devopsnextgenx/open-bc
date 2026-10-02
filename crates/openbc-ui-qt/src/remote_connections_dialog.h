// remote_connections_dialog.h
// ---------------------------------------------------------------------------
// RemoteConnectionsDialog: "Manage remote connections" - a saved-profile
// tree on the left (organize hosts under named groups, exactly like
// SessionHistory's saved-session nodes in HomeView) and a protocol-aware
// login form on the right, laid out the same way as the attached reference
// screenshot (protocol/host/port, username/password, a file picker for
// keys/certificates, a description box, "+"/"-" under the tree).
//
// Two ways to open it:
//   - From HomeView, as a plain manager: `RemoteConnectionsDialog(parent)`.
//   - From RemotePathBrowserDialog, as a picker: pass `pickerMode = true` to
//     get a "Connect" button that resolves `onProfileChosen` with the
//     selected/just-saved profile and closes the dialog, instead of the
//     browser having to duplicate this whole form itself.
//
// Pass a `RemoteBrowseBridge*` (VfsSessionBridge in practice) so "Test
// connection" can actually reach the host instead of only validating that
// the form is filled in; without one it falls back to that form-only check.
// ---------------------------------------------------------------------------
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>
#include <tuple>

#include "qt_style.h"
#include "remote_profile.h"
#include "remote_profile_store.h"

namespace openbc::app {

class RemoteConnectionsDialog : public QDialog {
public:
    // Fired only in picker mode, when the user clicks "Connect" (or presses
    // it right after "Save" for a brand-new profile). Not used in plain
    // manager mode.
    std::function<void(const RemoteProfile&)> onProfileChosen;

    // `bridge`, when supplied, lets "Test connection" actually reach the
    // host through openbc-vfs (VfsSessionBridge in practice) instead of
    // only validating that the form fields are filled in.
    explicit RemoteConnectionsDialog(QWidget* parent = nullptr, bool pickerMode = false,
                                      RemoteBrowseBridge* bridge = nullptr)
        : QDialog(parent), pickerMode_(pickerMode), bridge_(bridge) {
        setWindowTitle("Remote Connections");
        resize(760, 520);

        auto* root = new QVBoxLayout(this);
        auto* split = new QSplitter(Qt::Horizontal, this);
        split->setChildrenCollapsible(false);
        root->addWidget(split, 1);

        buildTree(split);
        buildForm(split);
        split->setSizes({260, 500});

        auto* bottomButtons = new QHBoxLayout;
        testConnection_ = new QPushButton("Test connection", this);
        save_ = new QPushButton("Save", this);
        saveAs_ = new QPushButton("Save As...", this);
        connect_ = new QPushButton("Connect", this);
        connect_->setVisible(pickerMode_);
        connect_->setDefault(pickerMode_);
        auto* close = new QPushButton(pickerMode_ ? "Cancel" : "Close", this);
        bottomButtons->addWidget(testConnection_);
        bottomButtons->addStretch();
        bottomButtons->addWidget(save_);
        bottomButtons->addWidget(saveAs_);
        if (pickerMode_) bottomButtons->addWidget(connect_);
        bottomButtons->addWidget(close);
        root->addLayout(bottomButtons);

        connect(testConnection_, &QPushButton::clicked, this, [this]() { testConnection(); });
        connect(save_, &QPushButton::clicked, this, [this]() { saveCurrent(false); });
        connect(saveAs_, &QPushButton::clicked, this, [this]() { saveCurrent(true); });
        connect(connect_, &QPushButton::clicked, this, [this]() {
            const RemoteProfile profile = collectForm();
            if (!profile.isValid()) {
                QMessageBox::warning(this, "Remote Connections", "Fill in the required fields before connecting.");
                return;
            }
            if (onProfileChosen) onProfileChosen(profile);
            accept();
        });
        connect(close, &QPushButton::clicked, this, [this]() { reject(); });

        refreshTree();
        clearForm();
    }

    // Convenience for callers that just want "pick or create a profile and
    // hand it back" without wiring onProfileChosen themselves.
    static bool pickProfile(QWidget* parent, RemoteProfile* outProfile) {
        RemoteConnectionsDialog dialog(parent, /*pickerMode=*/true);
        bool picked = false;
        dialog.onProfileChosen = [&](const RemoteProfile& profile) {
            if (outProfile) *outProfile = profile;
            picked = true;
        };
        dialog.exec();
        return picked;
    }

private:
    void buildTree(QSplitter* split) {
        auto* left = new QWidget(split);
        auto* layout = new QVBoxLayout(left);
        layout->setContentsMargins(0, 0, 4, 0);
        tree_ = new QTreeWidget(left);
        tree_->setHeaderHidden(true);
        tree_->setRootIsDecorated(true);
        tree_->setUniformRowHeights(true);
        tree_->setSelectionMode(QAbstractItemView::SingleSelection);
        tree_->setEditTriggers(QAbstractItemView::EditKeyPressed);
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(tree_, 1);

        auto* rowButtons = new QHBoxLayout;
        add_ = new QToolButton(left);
        add_->setText("+");
        add_->setPopupMode(QToolButton::InstantPopup);
        auto* addMenu = new QMenu(add_);
        addMenu->addAction("New connection", this, [this]() { addProfile(); });
        addMenu->addAction("New folder", this, [this]() { addFolder(); });
        add_->setMenu(addMenu);
        remove_ = new QToolButton(left);
        remove_->setText("-");
        rowButtons->addWidget(add_);
        rowButtons->addWidget(remove_);
        rowButtons->addStretch();
        layout->addLayout(rowButtons);

        connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this]() { loadSelection(); });
        connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) { handleRename(item); });
        connect(tree_, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& position) { showContextMenu(position); });
        connect(remove_, &QToolButton::clicked, this, [this]() { removeSelected(); });
    }

    void buildForm(QSplitter* split) {
        auto* right = new QWidget(split);
        auto* layout = new QVBoxLayout(right);
        tabs_ = new QTabWidget(right);
        layout->addWidget(tabs_, 1);

        auto* login = new QWidget(tabs_);
        auto* form = new QFormLayout(login);
        nameEdit_ = new QLineEdit(login);
        form->addRow("Name", nameEdit_);

        protocolCombo_ = new QComboBox(login);
        for (auto protocol : allRemoteProtocols()) protocolCombo_->addItem(protocolLabel(protocol), int(protocol));
        form->addRow("Protocol", protocolCombo_);

        hostLabel_ = new QLabel("Host", login);
        hostEdit_ = new QLineEdit(login);
        hostEdit_->setPlaceholderText("host name or IP address");
        form->addRow(hostLabel_, hostEdit_);

        portLabel_ = new QLabel("Port", login);
        portSpin_ = new QSpinBox(login);
        portSpin_->setRange(0, 65535);
        form->addRow(portLabel_, portSpin_);

        userLabel_ = new QLabel("Username", login);
        userEdit_ = new QLineEdit(login);
        form->addRow(userLabel_, userEdit_);

        passLabel_ = new QLabel("Password", login);
        auto* passRow = new QWidget(login);
        auto* passLayout = new QHBoxLayout(passRow);
        passLayout->setContentsMargins(0, 0, 0, 0);
        passEdit_ = new QLineEdit(passRow);
        passEdit_->setEchoMode(QLineEdit::Password);
        savePasswordCheck_ = new QCheckBox("Save", passRow);
        savePasswordCheck_->setChecked(true);
        passLayout->addWidget(passEdit_, 1);
        passLayout->addWidget(savePasswordCheck_);
        form->addRow(passLabel_, passRow);

        keyFileLabel_ = new QLabel("Private key file", login);
        auto [keyRow, keyEdit, keyBrowse] = makeFileRow(login, QFileDialog::AcceptOpen, false);
        keyFileEdit_ = keyEdit;
        form->addRow(keyFileLabel_, keyRow);
        Q_UNUSED(keyBrowse);

        certFileLabel_ = new QLabel("SSL client certificate", login);
        auto [certRow, certEdit, certBrowse] = makeFileRow(login, QFileDialog::AcceptOpen, false);
        certFileEdit_ = certEdit;
        form->addRow(certFileLabel_, certRow);
        Q_UNUSED(certBrowse);

        shareLabel_ = new QLabel("Share name", login);
        shareEdit_ = new QLineEdit(login);
        shareEdit_->setPlaceholderText("e.g. Shared");
        form->addRow(shareLabel_, shareEdit_);

        domainLabel_ = new QLabel("Domain", login);
        domainEdit_ = new QLineEdit(login);
        domainEdit_->setPlaceholderText("optional");
        form->addRow(domainLabel_, domainEdit_);

        mountPathLabel_ = new QLabel("Mount path", login);
        auto [mountRow, mountEdit, mountBrowse] = makeFileRow(login, QFileDialog::AcceptOpen, true);
        mountPathEdit_ = mountEdit;
        form->addRow(mountPathLabel_, mountRow);
        Q_UNUSED(mountBrowse);

        descriptionEdit_ = new QTextEdit(login);
        descriptionEdit_->setMaximumHeight(90);
        form->addRow("Description", descriptionEdit_);
        tabs_->addTab(login, "Login");

        auto* advanced = new QWidget(tabs_);
        auto* advancedForm = new QFormLayout(advanced);
        remoteRootEdit_ = new QLineEdit(advanced);
        remoteRootEdit_->setPlaceholderText("/");
        advancedForm->addRow("Initial remote folder", remoteRootEdit_);
        passiveModeCheck_ = new QCheckBox("Use passive mode", advanced);
        passiveModeCheck_->setChecked(true);
        advancedForm->addRow(QString(), passiveModeCheck_);
        tabs_->addTab(advanced, "Advanced");

        connect(protocolCombo_, &QComboBox::currentIndexChanged, this, [this](int) { updateFieldVisibility(); });
    }

    // Returns {row widget, line edit, browse button}. `folder` picks a
    // directory instead of a file - used for the NetworkDrive mount path.
    std::tuple<QWidget*, QLineEdit*, QToolButton*> makeFileRow(QWidget* parent, QFileDialog::AcceptMode, bool folder) {
        auto* row = new QWidget(parent);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* edit = new QLineEdit(row);
        auto* browse = new QToolButton(row);
        browse->setText("...");
        layout->addWidget(edit, 1);
        layout->addWidget(browse);
        connect(browse, &QToolButton::clicked, this, [edit, folder, row]() {
            const QString picked = folder ? QFileDialog::getExistingDirectory(row, "Select folder", edit->text())
                                           : QFileDialog::getOpenFileName(row, "Select file", edit->text());
            if (!picked.isEmpty()) edit->setText(picked);
        });
        return {row, edit, browse};
    }

    void updateFieldVisibility() {
        const auto protocol = RemoteProtocol(protocolCombo_->currentData().toInt());
        const bool hostPort = protocolUsesHostPort(protocol);
        hostLabel_->setVisible(hostPort);
        hostEdit_->setVisible(hostPort);
        portLabel_->setVisible(hostPort);
        portSpin_->setVisible(hostPort);
        const bool credentials = protocolUsesCredentials(protocol);
        userLabel_->setVisible(credentials);
        userEdit_->setVisible(credentials);
        passLabel_->setVisible(credentials);
        passEdit_->parentWidget()->setVisible(credentials);
        const bool keyFile = protocolUsesKeyFile(protocol);
        keyFileLabel_->setVisible(keyFile);
        keyFileEdit_->parentWidget()->setVisible(keyFile);
        const bool certFile = protocolUsesCertFile(protocol);
        certFileLabel_->setVisible(certFile);
        certFileEdit_->parentWidget()->setVisible(certFile);
        const bool shareName = protocolUsesShareName(protocol);
        shareLabel_->setVisible(shareName && protocol == RemoteProtocol::Smb);
        shareEdit_->setVisible(shareName && protocol == RemoteProtocol::Smb);
        domainLabel_->setVisible(protocol == RemoteProtocol::Smb);
        domainEdit_->setVisible(protocol == RemoteProtocol::Smb);
        const bool mountPath = protocol == RemoteProtocol::NetworkDrive;
        mountPathLabel_->setVisible(mountPath);
        mountPathEdit_->parentWidget()->setVisible(mountPath);
        passiveModeCheck_->setVisible(protocolUsesPassiveMode(protocol));
        if (!portSpin_->isVisible() || portSpin_->value() == 0) portSpin_->setValue(defaultPortFor(protocol));
    }

    void refreshTree() {
        const QSignalBlocker blocker(tree_);
        tree_->clear();
        for (const auto& node : RemoteProfileStore::loadTree()) addNodeItem(nullptr, node);
        tree_->expandAll();
    }

    void addNodeItem(QTreeWidgetItem* parent, const RemoteProfileNode& node) {
        auto* item = parent ? new QTreeWidgetItem(parent, {node.name}) : new QTreeWidgetItem(tree_, {node.name});
        item->setIcon(0, openbc::ui::icons::groupNodeIcon(RemoteProfileStore::colorForNode(node.path)));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsDropEnabled);
        item->setData(0, Qt::UserRole, node.path);
        item->setData(0, Qt::UserRole + 1, true);  // isGroup
        for (int i = 0; i < node.profiles.size(); ++i) {
            auto* profileItem = new QTreeWidgetItem(item, {node.profiles[i].name});
            profileItem->setIcon(0, openbc::ui::icons::glyph(openbc::ui::icons::Glyph::FolderOpen));
            profileItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
            profileItem->setData(0, Qt::UserRole, node.path);
            profileItem->setData(0, Qt::UserRole + 1, false);  // isGroup
            profileItem->setData(0, Qt::UserRole + 2, i);
        }
        for (const auto& child : node.children) addNodeItem(item, child);
        item->setExpanded(true);
    }

    void loadSelection() {
        auto* item = tree_->currentItem();
        if (!item || item->data(0, Qt::UserRole + 1).toBool()) {
            clearForm();
            return;
        }
        currentNodePath_ = item->data(0, Qt::UserRole).toString();
        currentIndex_ = item->data(0, Qt::UserRole + 2).toInt();
        const auto nodes = RemoteProfileStore::loadTree();
        const auto* node = findConst(nodes, currentNodePath_);
        if (!node || currentIndex_ >= node->profiles.size()) {
            clearForm();
            return;
        }
        populateForm(node->profiles[currentIndex_]);
        setFormEnabled(true);
    }

    static const RemoteProfileNode* findConst(const QList<RemoteProfileNode>& nodes, const QString& path) {
        for (const auto& node : nodes) {
            if (node.path == path) return &node;
            if (const auto* found = findConst(node.children, path)) return found;
        }
        return nullptr;
    }

    void populateForm(const RemoteProfile& profile) {
        nameEdit_->setText(profile.name);
        protocolCombo_->setCurrentIndex(protocolCombo_->findData(int(profile.protocol)));
        hostEdit_->setText(profile.host);
        portSpin_->setValue(profile.port);
        userEdit_->setText(profile.username);
        passEdit_->setText(profile.password);
        savePasswordCheck_->setChecked(profile.savePassword);
        keyFileEdit_->setText(profile.keyFile);
        certFileEdit_->setText(profile.certFile);
        shareEdit_->setText(profile.shareName);
        domainEdit_->setText(profile.domain);
        mountPathEdit_->setText(profile.mountPath);
        remoteRootEdit_->setText(profile.remoteRoot);
        passiveModeCheck_->setChecked(profile.passiveMode);
        descriptionEdit_->setPlainText(profile.description);
        updateFieldVisibility();
    }

    RemoteProfile collectForm() const {
        RemoteProfile profile;
        profile.name = nameEdit_->text().trimmed();
        profile.protocol = RemoteProtocol(protocolCombo_->currentData().toInt());
        profile.host = hostEdit_->text().trimmed();
        profile.port = portSpin_->value();
        profile.username = userEdit_->text();
        profile.password = passEdit_->text();
        profile.savePassword = savePasswordCheck_->isChecked();
        profile.keyFile = keyFileEdit_->text();
        profile.certFile = certFileEdit_->text();
        profile.shareName = shareEdit_->text();
        profile.domain = domainEdit_->text();
        profile.mountPath = mountPathEdit_->text();
        profile.remoteRoot = remoteRootEdit_->text().isEmpty() ? "/" : remoteRootEdit_->text();
        profile.passiveMode = passiveModeCheck_->isChecked();
        profile.description = descriptionEdit_->toPlainText();
        return profile;
    }

    void clearForm() {
        currentNodePath_.clear();
        currentIndex_ = -1;
        nameEdit_->clear();
        hostEdit_->clear();
        portSpin_->setValue(defaultPortFor(RemoteProtocol::Sftp));
        userEdit_->clear();
        passEdit_->clear();
        savePasswordCheck_->setChecked(true);
        keyFileEdit_->clear();
        certFileEdit_->clear();
        shareEdit_->clear();
        domainEdit_->clear();
        mountPathEdit_->clear();
        remoteRootEdit_->setText("/");
        passiveModeCheck_->setChecked(true);
        descriptionEdit_->clear();
        updateFieldVisibility();
        setFormEnabled(false);
    }

    void setFormEnabled(bool enabled) {
        // Name/protocol stay editable even with nothing selected, so "New
        // connection" can be filled in and Saved without a prior selection.
        save_->setEnabled(true);
        saveAs_->setEnabled(enabled);
        connect_->setEnabled(true);
        testConnection_->setEnabled(true);
    }

    QString currentGroupPath() const {
        if (!currentNodePath_.isEmpty()) return currentNodePath_;
        auto* item = tree_->currentItem();
        if (item && item->data(0, Qt::UserRole + 1).toBool()) return item->data(0, Qt::UserRole).toString();
        const auto roots = RemoteProfileStore::loadTree();
        return roots.isEmpty() ? QString() : roots.first().path;
    }

    void addProfile() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "New connection", "Connection name", QLineEdit::Normal,
                                                    "New connection", &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        RemoteProfile profile;
        profile.name = name.trimmed();
        RemoteProfileStore::saveProfile(currentGroupPath(), -1, profile);
        refreshTree();
        selectProfile(currentGroupPath(), name.trimmed());
    }

    void addFolder() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, "New folder", "Folder name", QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty() && RemoteProfileStore::createNode(currentGroupPath(), name)) refreshTree();
    }

    void selectProfile(const QString& nodePath, const QString& name) {
        std::function<QTreeWidgetItem*(QTreeWidgetItem*)> search = [&](QTreeWidgetItem* item) -> QTreeWidgetItem* {
            if (!item) return nullptr;
            for (int i = 0; i < item->childCount(); ++i) {
                auto* child = item->child(i);
                if (!child->data(0, Qt::UserRole + 1).toBool() && child->data(0, Qt::UserRole).toString() == nodePath &&
                    child->text(0) == name)
                    return child;
                if (auto* found = search(child)) return found;
            }
            return nullptr;
        };
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            if (auto* found = search(tree_->topLevelItem(i))) {
                tree_->setCurrentItem(found);
                return;
            }
        }
    }

    void removeSelected() {
        auto* item = tree_->currentItem();
        if (!item) return;
        const bool isGroup = item->data(0, Qt::UserRole + 1).toBool();
        const auto answer = QMessageBox::question(this, "Remove", isGroup ? "Remove this folder and everything in it?"
                                                                           : "Remove this connection?");
        if (answer != QMessageBox::Yes) return;
        if (isGroup) {
            RemoteProfileStore::removeNode(item->data(0, Qt::UserRole).toString());
        } else {
            RemoteProfileStore::removeProfile(item->data(0, Qt::UserRole).toString(),
                                              item->data(0, Qt::UserRole + 2).toInt());
        }
        refreshTree();
        clearForm();
    }

    void handleRename(QTreeWidgetItem* item) {
        if (!item) return;
        const QString value = item->text(0).trimmed();
        if (value.isEmpty()) return;
        if (item->data(0, Qt::UserRole + 1).toBool()) {
            RemoteProfileStore::renameNode(item->data(0, Qt::UserRole).toString(), value);
            refreshTree();
            return;
        }
        const QString nodePath = item->data(0, Qt::UserRole).toString();
        const int index = item->data(0, Qt::UserRole + 2).toInt();
        auto nodes = RemoteProfileStore::loadTree();
        if (const auto* node = findConst(nodes, nodePath); node && index < node->profiles.size()) {
            RemoteProfile profile = node->profiles[index];
            profile.name = value;
            RemoteProfileStore::saveProfile(nodePath, index, profile);
        }
        refreshTree();
    }

    void showContextMenu(const QPoint& position) {
        auto* item = tree_->itemAt(position);
        if (!item) return;
        tree_->setCurrentItem(item);
        QMenu menu(tree_);
        menu.addAction("Rename", this, [this, item]() { tree_->editItem(item, 0); });
        menu.addAction("Remove", this, [this]() { removeSelected(); });
        if (item->data(0, Qt::UserRole + 1).toBool()) menu.addAction("New connection here", this, [this]() { addProfile(); });
        menu.exec(tree_->viewport()->mapToGlobal(position));
    }

    void saveCurrent(bool asNew) {
        const RemoteProfile profile = collectForm();
        if (!profile.isValid()) {
            QMessageBox::warning(this, "Remote Connections",
                                 "Give the connection a name and fill in its address before saving.");
            return;
        }
        const QString nodePath = currentGroupPath().isEmpty() ? "Personal" : currentGroupPath();
        const int index = asNew ? -1 : currentIndex_;
        RemoteProfileStore::saveProfile(nodePath, index, profile);
        refreshTree();
        selectProfile(nodePath, profile.name);
    }

    // Actually reaches the host through openbc-vfs (RemoteBrowseBridge,
    // VfsSessionBridge in practice) and reports whatever the provider
    // itself says (bad credentials, unreachable host, SFTP handshake
    // failure, ...) instead of only validating that the form is filled in.
    // The test connection is torn down again immediately afterwards - this
    // button only ever proves reachability, it doesn't keep a session
    // around for browsing.
    void testConnection() {
        const RemoteProfile profile = collectForm();
        if (!profile.isValid()) {
            QMessageBox::warning(this, "Test connection", "Fill in the required fields first.");
            return;
        }
        if (!bridge_) {
            QMessageBox::information(this, "Test connection",
                                     "Connection details look complete for " + profile.displayAddress() +
                                         ".\n\nRemote browsing isn't wired up in this build, so the connection "
                                         "itself can't be tested here.");
            return;
        }
        testConnection_->setEnabled(false);
        testConnection_->setText("Testing...");
        QPointer<RemoteConnectionsDialog> self(this);
        RemoteBrowseBridge* bridge = bridge_;
        bridge->connectProfile(profile, [self, bridge, profile](bool ok, const QString& error) {
            if (!self) return;
            self->testConnection_->setEnabled(true);
            self->testConnection_->setText("Test connection");
            if (ok) {
                bridge->disconnectProfile(profile);
                QMessageBox::information(self, "Test connection",
                                         "Connected successfully to " + profile.displayAddress() + ".");
            } else {
                QMessageBox::warning(self, "Test connection",
                                     "Couldn't connect to " + profile.displayAddress() + ":\n\n" + error);
            }
        });
    }

    bool pickerMode_ = false;
    RemoteBrowseBridge* bridge_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QToolButton* add_ = nullptr;
    QToolButton* remove_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* protocolCombo_ = nullptr;
    QLabel* hostLabel_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QLabel* portLabel_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLabel* userLabel_ = nullptr;
    QLineEdit* userEdit_ = nullptr;
    QLabel* passLabel_ = nullptr;
    QLineEdit* passEdit_ = nullptr;
    QCheckBox* savePasswordCheck_ = nullptr;
    QLabel* keyFileLabel_ = nullptr;
    QLineEdit* keyFileEdit_ = nullptr;
    QLabel* certFileLabel_ = nullptr;
    QLineEdit* certFileEdit_ = nullptr;
    QLabel* shareLabel_ = nullptr;
    QLineEdit* shareEdit_ = nullptr;
    QLabel* domainLabel_ = nullptr;
    QLineEdit* domainEdit_ = nullptr;
    QLabel* mountPathLabel_ = nullptr;
    QLineEdit* mountPathEdit_ = nullptr;
    QLineEdit* remoteRootEdit_ = nullptr;
    QCheckBox* passiveModeCheck_ = nullptr;
    QTextEdit* descriptionEdit_ = nullptr;
    QPushButton* testConnection_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* saveAs_ = nullptr;
    QPushButton* connect_ = nullptr;
    QString currentNodePath_;
    int currentIndex_ = -1;
};

}  // namespace openbc::app

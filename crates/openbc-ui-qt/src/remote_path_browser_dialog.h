// remote_path_browser_dialog.h
// ---------------------------------------------------------------------------
// RemotePathBrowserDialog: pick a folder/file from either the local machine
// or a saved remote connection, all in one tree:
//
//   This Computer         -> rooted at "/" (QDir::rootPath()). On open the
//                            tree auto-expands down to the user's home dir,
//                            so there's always something useful visible.
//                            Well-known FHS folder names (home, etc, opt,
//                            media, usr, var, ...) get their own coloured
//                            *symbol* icons, not just a tinted folder, so
//                            they're identifiable at a glance.
//   Remote                -> parent of every saved connection (flattened
//                            across groups). Each connection shows a protocol
//                            coloured icon; expanding one connects through
//                            RemoteBrowseBridge and lists its root folder
//                            ("/"). Subfolders are listed lazily as they are
//                            expanded.
// ---------------------------------------------------------------------------
#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
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

struct RemoteEntry {
    QString name;
    bool isDir = false;
};

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
        resize(780, 580);
        auto* root = new QVBoxLayout(this);

        tree_ = new QTreeWidget(this);
        tree_->setHeaderHidden(true);
        tree_->setUniformRowHeights(true);
        tree_->setIconSize(QSize(16, 16));
        tree_->setSelectionMode(QAbstractItemView::SingleSelection);
        tree_->setExpandsOnDoubleClick(false);
        root->addWidget(tree_, 1);

        status_ = new QLabel(this);
        status_->setWordWrap(true);
        root->addWidget(status_);

        auto* pathRow = new QHBoxLayout;
        pathRow->addWidget(new QLabel("Path:", this));
        selectedPathEdit_ = new QLineEdit(this);
        pathRow->addWidget(selectedPathEdit_, 1);
        manage_ = new QToolButton(this);
        manage_->setText("Manage connections...");
        pathRow->addWidget(manage_);
        root->addLayout(pathRow);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText("Select");
        root->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() { attemptAccept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, [this]() { reject(); });

        connect(tree_, &QTreeWidget::itemExpanded, this,
                [this](QTreeWidgetItem* item) { onItemExpanded(item); });
        connect(tree_, &QTreeWidget::itemClicked, this,
                [this](QTreeWidgetItem* item, int) { onItemClicked(item); });
        connect(manage_, &QToolButton::clicked, this, [this]() {
            RemoteConnectionsDialog dialog(this);
            dialog.exec();
            refreshRemoteProfiles();
        });

        buildTree();
    }

    void setInitialPath(const QString& path) { selectedPathEdit_->setText(path); }

    QString selectedPath() const { return selectedPathEdit_->text().trimmed(); }
    bool selectedIsRemote() const { return isRemoteResult_; }
    RemoteProfile selectedProfile() const { return currentProfile_; }

private:
    enum class NodeKind {
        LocalRoot, LocalDir, LocalFile,
        RemoteRoot, RemoteProfile, RemoteDir, RemoteFile,
    };

    static constexpr int kPathRole       = Qt::UserRole;
    static constexpr int kIsDirRole      = Qt::UserRole + 1;
    static constexpr int kDummyRole      = Qt::UserRole + 2;
    static constexpr int kNodeKindRole   = Qt::UserRole + 3;
    static constexpr int kGroupRole      = Qt::UserRole + 4;
    static constexpr int kNameRole       = Qt::UserRole + 5;
    static constexpr int kLoadedRole     = Qt::UserRole + 6;
    static constexpr int kConnectingRole = Qt::UserRole + 7;

    // =====================================================================
    // Icons
    // =====================================================================

    // A coloured rounded-square badge with a white symbol drawn inside. Each
    // well-known FHS folder gets its own glyph so shape, not just hue,
    // distinguishes them.
    static void drawSymbol(QPainter& p, const QString& name, const QRectF& r, const QColor& color) {
        if (name == "home") {
            QPolygonF roof;
            roof << QPointF(r.left(), r.center().y())
                 << QPointF(r.center().x(), r.top())
                 << QPointF(r.right(), r.center().y());
            p.drawPolygon(roof);
            p.drawRect(QRectF(r.left() + r.width() * 0.2, r.center().y(),
                              r.width() * 0.6, r.height() * 0.5));
        } else if (name == "root") {
            QPolygonF sh;
            sh << QPointF(r.center().x(), r.top())
               << QPointF(r.right(), r.top() + r.height() * 0.25)
               << QPointF(r.right(), r.center().y())
               << QPointF(r.center().x(), r.bottom())
               << QPointF(r.left(), r.center().y())
               << QPointF(r.left(), r.top() + r.height() * 0.25);
            p.drawPolygon(sh);
        } else if (name == "etc" || name == "bin" || name == "sbin" ||
                   name == "sys" || name == "srv") {
            p.drawEllipse(r.adjusted(1, 1, -1, -1));
            p.setBrush(color);
            p.drawEllipse(r.adjusted(r.width() * 0.32, r.height() * 0.32,
                                     -r.width() * 0.32, -r.height() * 0.32));
        } else if (name == "opt") {
            QPolygonF hex;
            hex << QPointF(r.center().x(), r.top())
                << QPointF(r.right(), r.top() + r.height() * 0.25)
                << QPointF(r.right(), r.bottom() - r.height() * 0.25)
                << QPointF(r.center().x(), r.bottom())
                << QPointF(r.left(), r.bottom() - r.height() * 0.25)
                << QPointF(r.left(), r.top() + r.height() * 0.25);
            p.drawPolygon(hex);
        } else if (name == "media") {
            QPolygonF tri;
            tri << QPointF(r.left() + 1, r.top())
                << QPointF(r.right() - 1, r.center().y())
                << QPointF(r.left() + 1, r.bottom());
            p.drawPolygon(tri);
        } else if (name == "mnt") {
            p.drawEllipse(r.adjusted(r.width() * 0.1, r.height() * 0.15,
                                     -r.width() * 0.1, -r.height() * 0.15));
            p.setBrush(color);
            p.drawEllipse(r.adjusted(r.width() * 0.4, r.height() * 0.45,
                                     -r.width() * 0.4, -r.height() * 0.4));
        } else if (name == "usr") {
            p.drawEllipse(QRectF(r.center().x() - r.width() * 0.18, r.top(),
                                 r.width() * 0.36, r.height() * 0.4));
            p.drawChord(QRectF(r.left(), r.center().y(), r.width(), r.height() * 0.7),
                        0, 180 * 16);
        } else if (name == "var") {
            for (int i = 0; i < 3; ++i) {
                QRectF e(r.left(), r.top() + i * (r.height() * 0.25),
                         r.width(), r.height() * 0.32);
                p.drawEllipse(e);
            }
        } else if (name == "tmp") {
            QPolygonF top, bot;
            top << QPointF(r.left(), r.top()) << QPointF(r.right(), r.top())
                << QPointF(r.center().x(), r.center().y());
            bot << QPointF(r.left(), r.bottom()) << QPointF(r.right(), r.bottom())
                << QPointF(r.center().x(), r.center().y());
            p.drawPolygon(top);
            p.drawPolygon(bot);
        } else if (name == "boot") {
            p.drawArc(QRectF(r.left(), r.top() + 1, r.width(), r.height() - 1),
                      45 * 16, 270 * 16);
            p.drawLine(QPointF(r.center().x(), r.top() - 1),
                       QPointF(r.center().x(), r.center().y()));
        } else if (name == "dev") {
            p.drawRect(r.adjusted(2, 2, -2, -2));
            for (int i = 0; i < 3; ++i) {
                const qreal x = r.left() + 2 + i * (r.width() - 4) / 2.0;
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.top() + 2));
                p.drawLine(QPointF(x, r.bottom() - 2), QPointF(x, r.bottom()));
            }
        } else if (name == "proc" || name == "run") {
            QPolygonF bolt;
            bolt << QPointF(r.center().x() + 2, r.top())
                 << QPointF(r.left() + 2, r.center().y() + 1)
                 << QPointF(r.center().x(), r.center().y() + 1)
                 << QPointF(r.center().x() - 2, r.bottom())
                 << QPointF(r.right() - 2, r.center().y() - 1)
                 << QPointF(r.center().x(), r.center().y() - 1);
            p.drawPolygon(bolt);
        } else if (name == "lib" || name == "lib64") {
            p.drawRect(QRectF(r.left() + 1, r.top(), r.width() * 0.38, r.height()));
            p.drawRect(QRectF(r.center().x() + 1, r.top() + 1,
                              r.width() * 0.38, r.height() - 2));
        }
    }

    static QIcon buildSymbolIcon(const QString& name, const QColor& color) {
        const int S = 16;
        QPixmap pm(S, S);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawRoundedRect(QRectF(0.5, 0.5, S - 1.0, S - 1.0), 3, 3);
            p.setBrush(Qt::white);
            p.setPen(QPen(Qt::white, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            drawSymbol(p, name, QRectF(3.0, 3.0, S - 6.0, S - 6.0), color);
        }
        return QIcon(pm);
    }

    static QIcon folderSymbolIcon(const QString& name) {
        static const QHash<QString, QColor> kColors = {
            {"home",  QColor(0x26, 0xA6, 0x9A)}, {"root",  QColor(0xC6, 0x28, 0x28)},
            {"etc",   QColor(0x39, 0x49, 0xAB)}, {"opt",   QColor(0xEF, 0x6C, 0x00)},
            {"media", QColor(0x8E, 0x24, 0xAA)}, {"mnt",   QColor(0x7B, 0x1F, 0xA2)},
            {"usr",   QColor(0x00, 0x89, 0x7B)}, {"var",   QColor(0xF9, 0xA8, 0x25)},
            {"tmp",   QColor(0x75, 0x75, 0x75)}, {"bin",   QColor(0xD3, 0x2F, 0x2F)},
            {"sbin",  QColor(0xB7, 0x1C, 0x1C)}, {"lib",   QColor(0x6D, 0x4C, 0x41)},
            {"lib64", QColor(0x6D, 0x4C, 0x41)}, {"boot",  QColor(0x2E, 0x7D, 0x32)},
            {"dev",   QColor(0x45, 0x4A, 0x4E)}, {"proc",  QColor(0x37, 0x47, 0x4F)},
            {"sys",   QColor(0x37, 0x47, 0x4F)}, {"run",   QColor(0x45, 0x4A, 0x4E)},
            {"srv",   QColor(0x00, 0x89, 0x7B)},
        };
        const auto it = kColors.constFind(name);
        if (it == kColors.constEnd())
            return openbc::ui::icons::folderIcon(openbc::ui::color::folderYellow());

        static QHash<QString, QIcon> cache;
        auto cached = cache.constFind(name);
        if (cached != cache.constEnd()) return cached.value();
        const QIcon icon = buildSymbolIcon(name, it.value());
        cache.insert(name, icon);
        return icon;
    }

    static QIcon protocolIcon(RemoteProtocol protocol) {
        QColor color;
        switch (protocol) {
            case RemoteProtocol::Sftp:         color = QColor(0x4C, 0xAF, 0x50); break;
            case RemoteProtocol::Ftp:          color = QColor(0xFF, 0x98, 0x00); break;
            case RemoteProtocol::Smb:          color = QColor(0x21, 0x96, 0xF3); break;
            case RemoteProtocol::NetworkDrive: color = QColor(0xAB, 0x47, 0xBC); break;
            default:                           color = QColor(0x78, 0x90, 0x9C); break;
        }
        return openbc::ui::icons::groupNodeIcon(color);
    }

    // =====================================================================
    // Path helpers
    // =====================================================================

    static bool isPathPrefix(const QString& parent, const QString& child) {
        if (parent.isEmpty() || child.isEmpty()) return false;
        if (child == parent) return true;
        if (parent.endsWith('/')) return child.startsWith(parent);
        return child.startsWith(parent + '/');
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

    // =====================================================================
    // Tree construction
    // =====================================================================

    void buildTree() {
        tree_->clear();
        const QString localRoot = QDir::cleanPath(QDir::rootPath());

        topLocal_ = new QTreeWidgetItem(tree_, {"This Computer"});
        topLocal_->setIcon(0, style()->standardIcon(QStyle::SP_ComputerIcon));
        topLocal_->setData(0, kNodeKindRole, int(NodeKind::LocalRoot));
        topLocal_->setData(0, kPathRole, localRoot);
        topLocal_->setData(0, kIsDirRole, true);
        markLocalExpandable(topLocal_);

        topRemote_ = new QTreeWidgetItem(tree_, {"Remote"});
        topRemote_->setIcon(0, style()->standardIcon(QStyle::SP_DriveNetIcon));
        topRemote_->setData(0, kNodeKindRole, int(NodeKind::RemoteRoot));
        for (const auto& node : RemoteProfileStore::loadTree()) addProfilesToTree(topRemote_, node);

        loadLocalChildren(topLocal_);
        topLocal_->setExpanded(true);
        expandLocalTo(QDir::homePath());
        topRemote_->setExpanded(true);
    }

    void addProfilesToTree(QTreeWidgetItem* parent, const RemoteProfileNode& node) {
        for (const auto& profile : node.profiles) {
            auto* item = new QTreeWidgetItem(parent, {profile.name});
            item->setIcon(0, protocolIcon(profile.protocol));
            item->setData(0, kNodeKindRole, int(NodeKind::RemoteProfile));
            // The connection node stands for the remote root. Its children
            // are the entries in "/", listed on first expand.
            item->setData(0, kPathRole, QString("/"));
            item->setData(0, kIsDirRole, true);
            item->setData(0, kGroupRole, node.path);
            item->setData(0, kNameRole, profile.name);
            item->setToolTip(0, QString("%1 \u2014 %2").arg(protocolLabel(profile.protocol),
                                                            profile.displayAddress()));
            markRemoteExpandable(item);
        }
        for (const auto& child : node.children) addProfilesToTree(parent, child);
    }

    void refreshRemoteProfiles() {
        if (!topRemote_) return;
        topRemote_->takeChildren();
        for (const auto& node : RemoteProfileStore::loadTree()) addProfilesToTree(topRemote_, node);
        topRemote_->setExpanded(true);
    }

    static void markLocalExpandable(QTreeWidgetItem* item) {
        auto* placeholder = new QTreeWidgetItem(item, {"Loading..."});
        placeholder->setData(0, kDummyRole, true);
    }
    static void markRemoteExpandable(QTreeWidgetItem* item) {
        auto* placeholder = new QTreeWidgetItem(item, {"Loading..."});
        placeholder->setData(0, kDummyRole, true);
    }

    // =====================================================================
    // Local side
    // =====================================================================

    void loadLocalChildren(QTreeWidgetItem* item) {
        const bool hasDummy = item->childCount() == 1 && item->child(0)->data(0, kDummyRole).toBool();
        if (item->childCount() > 0 && !hasDummy) return;
        if (hasDummy) delete item->takeChild(0);

        const QString path = item->data(0, kPathRole).toString();
        QDir dir(path);
        const QDir::Filters filters = mode_ == PathPickMode::Folder
                                          ? (QDir::AllDirs | QDir::NoDotAndDotDot)
                                          : (QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
        const auto entries = dir.entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
        for (const auto& info : entries) {
            const bool isDir = info.isDir();
            auto* child = new QTreeWidgetItem(item, {info.fileName()});
            child->setData(0, kPathRole, QDir::cleanPath(info.absoluteFilePath()));
            child->setData(0, kIsDirRole, isDir);
            child->setData(0, kNodeKindRole, int(isDir ? NodeKind::LocalDir : NodeKind::LocalFile));
            if (isDir) {
                child->setIcon(0, folderSymbolIcon(info.fileName()));
                markLocalExpandable(child);
            } else {
                child->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            }
        }
    }

    void expandLocalTo(const QString& targetPath) {
        if (!topLocal_) return;
        const QString cleanTarget = QDir::cleanPath(targetPath);
        QTreeWidgetItem* item = topLocal_;
        while (item) {
            const QString itemPath = QDir::cleanPath(item->data(0, kPathRole).toString());
            if (itemPath == cleanTarget) break;
            loadLocalChildren(item);
            item->setExpanded(true);
            QTreeWidgetItem* next = nullptr;
            for (int i = 0; i < item->childCount(); ++i) {
                auto* child = item->child(i);
                if (!child->data(0, kIsDirRole).toBool()) continue;
                const QString childPath = QDir::cleanPath(child->data(0, kPathRole).toString());
                if (isPathPrefix(childPath, cleanTarget)) { next = child; break; }
            }
            item = next;
        }
        if (item) {
            item->setExpanded(true);
            tree_->setCurrentItem(item);
            tree_->scrollToItem(item);
        }
    }

    // =====================================================================
    // Remote side
    // =====================================================================

    void connectAndLoadRemoteProfile(QTreeWidgetItem* item) {
        if (item->data(0, kLoadedRole).toBool()) return;
        if (item->data(0, kConnectingRole).toBool()) return;
        item->setData(0, kConnectingRole, true);

        RemoteProfile profile;
        if (!profileForItem(item, &profile)) {
            item->setData(0, kConnectingRole, false);
            item->takeChildren();
            auto* err = new QTreeWidgetItem(item, {"This connection no longer exists."});
            err->setDisabled(true);
            return;
        }

        if (!bridge_) {
            item->setData(0, kConnectingRole, false);
            item->setData(0, kLoadedRole, true);
            item->takeChildren();
            auto* msg = new QTreeWidgetItem(
                item, {"Remote browsing isn't wired up in this build - type the path below."});
            msg->setDisabled(true);
            isRemoteResult_ = true;
            currentProfile_ = profile;
            selectedPathEdit_->setText(buildRemoteUri(
                profile, profile.remoteRoot.isEmpty() ? QString("/") : profile.remoteRoot));
            return;
        }

        status_->setText("Connecting to " + profile.name + "...");
        bridge_->connectProfile(profile, [this, item, profile](bool ok, const QString& error) {
            item->setData(0, kConnectingRole, false);
            if (!ok) {
                item->takeChildren();
                auto* err = new QTreeWidgetItem(item, {"Couldn't connect: " + error});
                err->setDisabled(true);
                status_->setText("Couldn't connect to " + profile.name + ": " + error);
                return;
            }
            status_->setText("Connected to " + profile.name + ".");
            // Just list the root ("/"). Deeper levels are pulled in as the
            // user expands them. The item's kLoadedRole is set by
            // loadRemoteDirForProfile once the listing actually arrives -
            // setting it here would leave the node stuck on "Loading...".
            loadRemoteDirForProfile(item, profile, {});
        });
    }

    void loadRemoteDir(QTreeWidgetItem* item) {
        if (item->data(0, kLoadedRole).toBool()) return;
        if (item->data(0, kConnectingRole).toBool()) return;
        RemoteProfile profile;
        if (!profileForItem(item, &profile)) return;
        item->setData(0, kConnectingRole, true);
        loadRemoteDirForProfile(item, profile, [item](bool) {
            item->setData(0, kConnectingRole, false);
        });
    }

    void loadRemoteDirForProfile(QTreeWidgetItem* item, const RemoteProfile& profile,
                                  std::function<void(bool ok)> done) {
        if (!bridge_) { if (done) done(false); return; }
        if (item->data(0, kLoadedRole).toBool()) { if (done) done(true); return; }

        const bool hasDummy = item->childCount() == 1 && item->child(0)->data(0, kDummyRole).toBool();
        if (hasDummy) delete item->takeChild(0);

        const QString path = item->data(0, kPathRole).toString();
        bridge_->listDirectory(profile, path,
                               [this, item, path, done](bool ok, QList<RemoteEntry> entries,
                                                         const QString& error) {
            if (!item) { if (done) done(false); return; }
            if (!ok) {
                auto* err = new QTreeWidgetItem(item, {"Couldn't list this folder: " + error});
                err->setDisabled(true);
                if (done) done(false);
                return;
            }
            std::sort(entries.begin(), entries.end(), [](const RemoteEntry& a, const RemoteEntry& b) {
                if (a.isDir != b.isDir) return a.isDir;
                return a.name.localeAwareCompare(b.name) < 0;
            });
            for (const auto& entry : entries) {
                if (!entry.isDir && mode_ == PathPickMode::Folder) continue;
                auto* child = new QTreeWidgetItem(item, {entry.name});
                const QString childPath = path.endsWith('/') ? path + entry.name : path + "/" + entry.name;
                child->setData(0, kPathRole, childPath);
                child->setData(0, kIsDirRole, entry.isDir);
                child->setData(0, kNodeKindRole,
                               int(entry.isDir ? NodeKind::RemoteDir : NodeKind::RemoteFile));
                child->setData(0, kGroupRole, item->data(0, kGroupRole));
                child->setData(0, kNameRole, item->data(0, kNameRole));
                if (entry.isDir) {
                    child->setIcon(0, folderSymbolIcon(entry.name));
                    markRemoteExpandable(child);
                } else {
                    child->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
                }
            }
            item->setData(0, kLoadedRole, true);
            if (done) done(true);
        });
    }

    // =====================================================================
    // Events
    // =====================================================================

    void onItemExpanded(QTreeWidgetItem* item) {
        if (!item) return;
        switch (NodeKind(item->data(0, kNodeKindRole).toInt())) {
            case NodeKind::LocalRoot:
            case NodeKind::LocalDir:      loadLocalChildren(item);            break;
            case NodeKind::RemoteProfile: connectAndLoadRemoteProfile(item);  break;
            case NodeKind::RemoteDir:     loadRemoteDir(item);                break;
            default: break;
        }
    }

    void onItemClicked(QTreeWidgetItem* item) {
        if (!item) return;
        const auto kind = NodeKind(item->data(0, kNodeKindRole).toInt());
        switch (kind) {
            case NodeKind::LocalRoot:
            case NodeKind::LocalDir:
            case NodeKind::LocalFile: {
                if (kind == NodeKind::LocalFile && mode_ == PathPickMode::Folder) return;
                isRemoteResult_ = false;
                currentProfile_ = {};
                selectedPathEdit_->setText(
                    QDir::toNativeSeparators(item->data(0, kPathRole).toString()));
                break;
            }
            case NodeKind::RemoteProfile:
            case NodeKind::RemoteDir:
            case NodeKind::RemoteFile: {
                if (kind == NodeKind::RemoteFile && mode_ == PathPickMode::Folder) return;
                RemoteProfile profile;
                if (!profileForItem(item, &profile)) return;
                isRemoteResult_ = true;
                currentProfile_ = profile;
                selectedPathEdit_->setText(
                    buildRemoteUri(profile, item->data(0, kPathRole).toString()));
                break;
            }
            case NodeKind::RemoteRoot:
            default: break;
        }
    }

    bool profileForItem(QTreeWidgetItem* item, RemoteProfile* out) const {
        const QString group = item->data(0, kGroupRole).toString();
        const QString name = item->data(0, kNameRole).toString();
        if (group.isEmpty() || name.isEmpty()) return false;
        return RemoteProfileStore::findProfileByName(group, name, out);
    }

    void attemptAccept() {
        if (selectedPath().isEmpty()) {
            QMessageBox::information(this, windowTitle(), "Pick a path before continuing.");
            return;
        }
        accept();
    }

    PathPickMode mode_;
    RemoteBrowseBridge* bridge_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;
    QToolButton* manage_ = nullptr;
    QLineEdit* selectedPathEdit_ = nullptr;
    RemoteProfile currentProfile_;
    bool isRemoteResult_ = false;
    QTreeWidgetItem* topLocal_ = nullptr;
    QTreeWidgetItem* topRemote_ = nullptr;
};

}  // namespace openbc::app
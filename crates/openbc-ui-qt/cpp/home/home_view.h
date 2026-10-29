#pragma once

#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QFileInfo>
#include <QHash>
#include <QPaintEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <memory>

#include "common/file_kinds.h"
#include "home/session_history.h"
#include "home/update_notice.h"
#include "style/qt_style.h"
#include "remote/remote_connections_dialog.h"
#include "remote/remote_vfs_bridge.h"

namespace openbc::app {

class HistoryTree : public QTreeWidget {
public:
    std::function<void(QTreeWidgetItem*, QTreeWidgetItem*)> onSessionDropped;

    explicit HistoryTree(QWidget* parent = nullptr) : QTreeWidget(parent) {}

protected:
    // Whatever is under the cursor (the node row, one of its session rows, or the
    // gap between two session rows) resolves to the saved-session node that owns
    // it, by walking up to the nearest ancestor flagged as a node (UserRole + 4).
    // Auto-saved rows, date buckets, "New" and empty space resolve to nullptr.
    QTreeWidgetItem* dropNodeAt(const QPoint& position) const {
        for (auto* item = itemAt(position); item; item = item->parent()) {
            if (item->data(0, Qt::UserRole + 4).toBool()) return item;
        }
        return nullptr;
    }

    bool dragSourceIsSession() const {
        auto* source = currentItem();
        return source && source->data(0, Qt::UserRole + 3).toBool();
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        QTreeWidget::dragMoveEvent(event);
        auto* node = dragSourceIsSession() ? dropNodeAt(event->position().toPoint()) : nullptr;
        if (node) event->acceptProposedAction();  // base rejects hovering over plain session rows
        else event->ignore();
        if (node != dropHighlight_) {
            dropHighlight_ = node;
            viewport()->update();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        dropHighlight_ = nullptr;
        viewport()->update();
        QTreeWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        dropHighlight_ = nullptr;
        auto* source = currentItem();
        auto* target = dragSourceIsSession() ? dropNodeAt(event->position().toPoint()) : nullptr;
        if (source && target && onSessionDropped) {
            onSessionDropped(source, target);
            // The model is persisted and the tree rebuilt by the callback, so the
            // view must not do its own item move/removal. A copy action stops
            // QAbstractItemView from deleting the dragged row after a "move".
            stopAutoScroll();
            setState(NoState);
            event->setDropAction(Qt::CopyAction);
            event->accept();
            viewport()->update();
            return;
        }
        // Never fall back to QTreeWidget::dropEvent: it would reshuffle the visible
        // items without touching the saved data.
        stopAutoScroll();
        setState(NoState);
        event->ignore();
        viewport()->update();
    }

    void paintEvent(QPaintEvent* event) override {
        QTreeWidget::paintEvent(event);
        if (!dropHighlight_) return;
        QPainter painter(viewport());
        painter.setPen(QPen(palette().highlight().color(), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(visualItemRect(dropHighlight_).adjusted(1, 1, -1, -1));
    }

private:
    QTreeWidgetItem* dropHighlight_ = nullptr;
};

inline QIcon historyFileIcon(const QString& path) {
    const QString extension = QFileInfo(path).suffix().toLower();
    QColor color(0x8f, 0x9b, 0xb3);
    if (extension == "rs") color = QColor(0xe5, 0x7a, 0x44);
    else if (extension == "cpp" || extension == "h") color = QColor(0x4f, 0x9d, 0xde);
    else if (extension == "py") color = QColor(0xe5, 0xc0, 0x7b);
    else if (extension == "js" || extension == "ts") color = QColor(0xe8, 0xc5, 0x47);
    else if (extension == "json" || extension == "toml" || extension == "yaml") color = QColor(0x68, 0xc0, 0x9a);
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setPen(QPen(color, 1.2));
    painter.setBrush(color.darker(185));
    painter.drawRoundedRect(QRectF(3, 1.5, 10, 13), 1, 1);
    painter.setPen(QPen(color.lighter(145), 1));
    painter.drawLine(QPointF(5, 6), QPointF(11, 6));
    painter.drawLine(QPointF(5, 9), QPointF(11, 9));
    painter.drawLine(QPointF(5, 12), QPointF(9, 12));
    return QIcon(pixmap);
}

// ---------------------------------------------------------------------------
// Session types shown both in the "New" tree node and as the big icon grid.
// ---------------------------------------------------------------------------
struct SessionTypeSpec {
    QString kind;
    QString label;
    bool primary;  // first icon row (large icons)
    bool inTree;   // listed under the "New" node
};

inline const QList<SessionTypeSpec>& sessionTypes() {
    static const QList<SessionTypeSpec> types = {
        {"folder-compare", "Folder Compare", true, true},
        {"folder-merge", "Folder Merge", true, true},
        {"folder-sync", "Folder Sync", true, true},
        {"text-compare", "Text Compare", true, true},
        {"text-merge", "Text Merge", true, true},
        {"text-edit", "Text Edit", true, false},
        {"hex-compare", "Hex Compare", false, true},
        {"media-compare", "Media Compare", false, true},
        {"picture-compare", "Picture Compare", false, true},
        {"model3d-compare", "3D Compare", false, true},
        {"registry-compare", "Registry Compare", false, true},
        {"table-compare", "Table Compare", false, true},
        {"version-compare", "Version Compare", false, true},
    };
    return types;
}

namespace detail {

inline void paintPage(QPainter& p, const QColor& body) {
    QPainterPath path;
    path.moveTo(3, 1);
    path.lineTo(10.5, 1);
    path.lineTo(13, 3.5);
    path.lineTo(13, 15);
    path.lineTo(3, 15);
    path.closeSubpath();
    p.setPen(QPen(QColor(0x7d, 0x82, 0x94), 0.7));
    p.setBrush(body);
    p.drawPath(path);
    p.setBrush(QColor(0xb8, 0xbc, 0xcc));
    p.drawPolygon(QPolygonF({QPointF(10.5, 1), QPointF(10.5, 3.5), QPointF(13, 3.5)}));
}

inline void paintLines(QPainter& p, const QColor& ink, int count = 4) {
    p.setPen(QPen(ink, 0.9));
    for (int i = 0; i < count; ++i) {
        const double y = 5.4 + i * 2.3;
        p.drawLine(QPointF(4.8, y), QPointF(i % 2 ? 9.6 : 11.2, y));
    }
}

}  // namespace detail

inline QIcon sessionTypeIcon(const QString& kind) {
    using namespace openbc::ui;
    if (kind == "folder-compare") return icons::folderIcon(color::folderYellow());
    if (kind == "folder-merge")
        return icons::folderIcon(QColor(0xf0, 0xc0, 0x70), QColor(0xd9, 0x9a, 0x4a));
    if (kind == "folder-sync") {
        const QIcon folder = icons::folderIcon(color::folderYellow());
        return icons::make([folder](QPainter& p, QIcon::Mode, QIcon::State) {
            folder.paint(&p, QRect(0, 0, 16, 16), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
            p.setBrush(QColor(0xf4, 0xf6, 0xff));
            p.setPen(QPen(QColor(0x3b, 0x6f, 0xd8), 0.8));
            p.drawEllipse(QPointF(10.2, 10.2), 4.4, 4.4);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0x3b, 0x6f, 0xd8), 1.3, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRectF(7.8, 7.8, 4.8, 4.8), 20 * 16, 140 * 16);
            p.drawArc(QRectF(7.8, 7.8, 4.8, 4.8), 200 * 16, 140 * 16);
        });
    }
    if (kind == "text-compare" || kind == "text-merge") {
        const bool merge = kind == "text-merge";
        return icons::make([merge](QPainter& p, QIcon::Mode, QIcon::State) {
            detail::paintPage(p, merge ? QColor(0xe4, 0xee, 0xff) : QColor(0xf2, 0xf2, 0xf4));
            detail::paintLines(p, merge ? QColor(0x3b, 0x6f, 0xd8) : QColor(0x80, 0x84, 0x94));
        });
    }
    if (kind == "text-edit") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            detail::paintPage(p, QColor(0xf2, 0xf2, 0xf4));
            detail::paintLines(p, QColor(0x80, 0x84, 0x94), 3);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xf0, 0x8a, 0x30));
            p.drawPolygon(QPolygonF({QPointF(14.6, 3.2), QPointF(15.6, 4.2), QPointF(9.6, 10.6),
                                     QPointF(8.4, 10.8), QPointF(8.6, 9.6)}));
        });
    }
    if (kind == "hex-compare") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            detail::paintPage(p, QColor(0xf2, 0xf2, 0xf4));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x3b, 0x6f, 0xd8));
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    if ((row + col) % 2 == 0) p.drawRect(QRectF(4.6 + col * 2.6, 5.4 + row * 2.8, 1.6, 1.8));
        });
    }
    if (kind == "media-compare") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            detail::paintPage(p, QColor(0xf2, 0xee, 0xfa));
            p.setPen(QPen(QColor(0x8a, 0x4f, 0xd8), 1.2));
            p.drawLine(QPointF(9.6, 5.0), QPointF(9.6, 10.6));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x8a, 0x4f, 0xd8));
            p.drawEllipse(QPointF(8.2, 10.8), 1.6, 1.2);
            p.drawRect(QRectF(9.6, 5.0, 2.4, 1.2));
        });
    }
    if (kind == "picture-compare") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            detail::paintPage(p, QColor(0xdd, 0xec, 0xff));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x4c, 0xb0, 0x5c));
            p.drawPolygon(QPolygonF({QPointF(3.4, 14.6), QPointF(7.0, 9.0), QPointF(9.6, 12.0),
                                     QPointF(11.0, 10.4), QPointF(12.6, 14.6)}));
            p.setBrush(QColor(0xf4, 0xc0, 0x40));
            p.drawEllipse(QPointF(10.6, 6.2), 1.4, 1.4);
        });
    }
    if (kind == "model3d-compare") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(QColor(0x7d, 0x82, 0x94), 0.7));
            p.setBrush(QColor(0xdd, 0xec, 0xff));   // top
            p.drawPolygon(QPolygonF({QPointF(8, 1.5), QPointF(14, 4.5), QPointF(8, 7.5), QPointF(2, 4.5)}));
            p.setBrush(QColor(0x8f, 0xb8, 0xf0));   // left
            p.drawPolygon(QPolygonF({QPointF(2, 4.5), QPointF(8, 7.5), QPointF(8, 14.5), QPointF(2, 11.5)}));
            p.setBrush(QColor(0x4e, 0x9c, 0xff));   // right
            p.drawPolygon(QPolygonF({QPointF(14, 4.5), QPointF(8, 7.5), QPointF(8, 14.5), QPointF(14, 11.5)}));
        });
    }
    if (kind == "registry-compare") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            const QColor teal(0x3d, 0xc4, 0xc0);
            p.setPen(QPen(teal.darker(160), 0.6));
            const QRectF cubes[] = {QRectF(2.0, 8.4, 5.2, 5.2), QRectF(8.4, 8.4, 5.2, 5.2),
                                    QRectF(5.2, 2.4, 5.2, 5.2)};
            for (const QRectF& r : cubes) {
                p.setBrush(teal);
                p.drawRoundedRect(r, 0.8, 0.8);
                p.setBrush(teal.lighter(145));
                p.drawRect(QRectF(r.x(), r.y(), r.width(), 1.6));
            }
        });
    }
    if (kind == "table-compare") {
        return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
            p.setPen(QPen(QColor(0x6b, 0x5f, 0xa8), 0.8));
            p.setBrush(QColor(0xf2, 0xf0, 0xfa));
            p.drawRect(QRectF(2.0, 2.4, 12.0, 11.2));
            p.setBrush(QColor(0x8a, 0x7a, 0xd8));
            p.drawRect(QRectF(2.0, 2.4, 12.0, 2.6));
            for (int i = 1; i < 3; ++i) p.drawLine(QPointF(2.0 + i * 4.0, 2.4), QPointF(2.0 + i * 4.0, 13.6));
            for (int i = 1; i < 4; ++i) p.drawLine(QPointF(2.0, 2.4 + 2.6 + i * 2.2), QPointF(14.0, 2.4 + 2.6 + i * 2.2));
        });
    }
    // version-compare: page with a gear badge
    return icons::make([](QPainter& p, QIcon::Mode, QIcon::State) {
        detail::paintPage(p, QColor(0xf2, 0xf2, 0xf4));
        p.setPen(QPen(QColor(0x4a, 0x4f, 0x66), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(8.0, 8.6), 2.4, 2.4);
        p.setPen(QPen(QColor(0x4a, 0x4f, 0x66), 1.6, Qt::SolidLine, Qt::FlatCap));
        for (int i = 0; i < 8; ++i) {
            QTransform t;
            t.translate(8.0, 8.6);
            t.rotate(i * 45.0);
            p.drawLine(t.map(QPointF(0, -3.2)), t.map(QPointF(0, -4.4)));
        }
    });
}

// sessionTypeIcon() paints a fresh pixmap per call; the sessions tree can hold
// hundreds of rows, so hand out cached copies.
inline QIcon cachedSessionTypeIcon(const QString& kind) {
    static QHash<QString, QIcon> cache;
    auto it = cache.find(kind);
    if (it == cache.end()) it = cache.insert(kind, sessionTypeIcon(kind));
    return it.value();
}

// Which node icon a recorded session gets. The recorded kind wins; a plain
// text session (or an unknown / legacy kind) is re-checked against the file
// extension so pictures, 3D models, tables, media, .reg and executables get
// their own icon instead of the generic text-file page.
inline QIcon sessionEntryIcon(const SessionHistoryEntry& entry) {
    QString kind = canonicalSessionKind(entry.kind);
    if (kind.isEmpty() || kind == "text-compare") {
        const QString path = entry.left.isEmpty() ? entry.right : entry.left;
        const QString byFile = sessionKindForViewer(classifyFilePath(path));
        if (byFile != "text-compare") kind = byFile;
    }
    if (kind.isEmpty() || kind == "text-compare") return historyFileIcon(entry.left.isEmpty() ? entry.right : entry.left);
    return cachedSessionTypeIcon(kind);
}

// Big clickable icon that also accepts dropped folders / files.
class SessionTile : public QToolButton {
public:
    std::function<void(const QStringList&)> onPathsDropped;

    explicit SessionTile(QWidget* parent = nullptr) : QToolButton(parent) { setAcceptDrops(true); }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent* event) override {
        QStringList paths;
        for (const QUrl& url : event->mimeData()->urls())
            if (url.isLocalFile()) paths << url.toLocalFile();
        if (paths.isEmpty() || !onPathsDropped) return;
        event->acceptProposedAction();
        onPathsDropped(paths);
    }
};

class HomeView : public QWidget {
public:
    std::function<void()> onNewFolderCompare;
    std::function<void()> onNewTextCompare;
    // Generic launcher for every session type; `paths` holds dropped folders/files (may be empty).
    // When unset, only folder-compare / text-compare fall back to the two callbacks above.
    std::function<void(const QString& kind, const QStringList& paths)> onNewSession;
    std::function<void(const SessionHistoryEntry&)> onOpenHistory;

    // Turns on the bottom-right release notices ("update available" with change log, and a
    // once-per-install "what's new"). `preferences` must outlive the view. See home/update_notice.h.
    void enableUpdateNotices(QSettings* preferences) {
        if (updates_ || !preferences) return;
        updates_ = std::make_unique<UpdateNotifier>(
            this, preferences, [this]() { return footer_ && footer_->isVisibleTo(this) ? footer_->height() : 0; });
        updates_->start();
    }

    explicit HomeView(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("homeView");
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        auto* split = new QSplitter(Qt::Horizontal, this);
        split->setObjectName("homeSplit");
        split->setChildrenCollapsible(false);
        outer->addWidget(split, 1);

        buildSidebar(split);
        buildContent(split);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({383, 1537});

        buildFooter(outer);

        connect(history_, &QTreeWidget::itemSelectionChanged, this, [this]() { showSelected(); });
        connect(history_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item) return;
                const QString newKind = item->data(0, Qt::UserRole + 6).toString();
                if (!newKind.isEmpty()) {
                    launchNew(newKind, {});
                    return;
                }
                if (!item->data(0, Qt::UserRole + 3).toBool() || !onOpenHistory) return;
                history_->setCurrentItem(item);
                openSelected();
            });
        connect(history_, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem* item, int column) { handleItemRenamed(item, column); });
        history_->onSessionDropped = [this](QTreeWidgetItem* source, QTreeWidgetItem* target) {
            moveSession(source, target);
        };
        connect(open_, &QPushButton::clicked, this, [this]() { openSelected(); });
        connect(edit_, &QPushButton::clicked, this, [this]() { toggleEditing(); });
        connect(history_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
            contextMenu(position);
        });
        connect(addButton_, &QToolButton::clicked, this, [this]() { addPressed(); });
        connect(removeButton_, &QToolButton::clicked, this, [this]() { removePressed(); });
        connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) { applyFilter(text); });

        // Remember which tree nodes the user has open, across refreshes and launches.
        treeState_ = SessionHistory::loadTreeState();
        connect(history_, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem* item) { rememberExpanded(item, true); });
        connect(history_, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem* item) { rememberExpanded(item, false); });

        refreshHistory();
        clearDetails();
    }

    void manageRemoteConnections() {
        RemoteConnectionsDialog dialog(this, /*pickerMode=*/false, &sharedVfsBridge());
        dialog.exec();
    }

    void refreshHistory() {
        const QSignalBlocker blocker(history_);
        history_->clear();

        // "New": one row per session type, same icons as the big tiles.
        auto* newRoot = new QTreeWidgetItem(history_, {"New"});
        newRoot->setIcon(0, openbc::ui::icons::groupNodeIcon(openbc::ui::color::folderGreen()));
        newRoot->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        newRoot->setData(0, Qt::UserRole + 7, true);
        for (const auto& spec : sessionTypes()) {
            if (!spec.inTree) continue;
            auto* child = new QTreeWidgetItem(newRoot, {spec.label});
            child->setIcon(0, sessionTypeIcon(spec.kind));
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            child->setData(0, Qt::UserRole + 6, spec.kind);
        }
        restoreExpanded(newRoot, "new", true);

        // "Auto-saved": recent history grouped into date buckets. Only buckets
        // that hold a session are shown; just "Today" starts expanded.
        auto* recent = new QTreeWidgetItem(history_, {"Auto-saved"});
        recent->setIcon(0, openbc::ui::icons::groupNodeIcon(QColor(0xc9, 0xa8, 0x6a)));
        recent->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);

        const auto entries = SessionHistory::load();
        QList<QList<int>> buckets(kRecentBucketCount);
        for (int index = 0; index < entries.size(); ++index) {
            buckets[static_cast<int>(SessionHistory::bucketFor(entries[index].openedAt))].append(index);
        }
        for (int ordinal = 0; ordinal < kRecentBucketCount; ++ordinal) {
            if (buckets[ordinal].isEmpty()) continue;
            const auto bucket = static_cast<RecentBucket>(ordinal);
            const QString label = bucket == RecentBucket::Older ? QString("More than 6 days ago")
                                                                 : SessionHistory::bucketLabel(bucket);
            auto* bucketItem = new QTreeWidgetItem(recent, {label});
            bucketItem->setIcon(0, openbc::ui::icons::groupNodeIcon(bucketColor(bucket)));
            bucketItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            bucketItem->setData(0, Qt::UserRole + 5, true);
            for (int index : buckets[ordinal]) addSessionItem(bucketItem, entries[index], false, {}, index);
            restoreExpanded(bucketItem, QString("auto/%1").arg(ordinal), bucket == RecentBucket::Today);
        }
        restoreExpanded(recent, "auto", true);

        for (const auto& node : SessionHistory::loadSavedNodes()) addNodeItem(nullptr, node);
        clearDetails();  // the old selected item was destroyed by clear()
        applyFilter(search_->text());
    }

private:
    // ----- construction helpers -------------------------------------------
    void buildSidebar(QSplitter* split) {
        auto* sidebar = new QWidget(split);
        sidebar->setObjectName("homeSidebar");
        auto* layout = new QVBoxLayout(sidebar);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto* header = new QLabel("Sessions", sidebar);
        header->setObjectName("homeSessionsHeader");
        layout->addWidget(header);

        history_ = new HistoryTree(sidebar);
        history_->setObjectName("homeHistory");
        history_->setHeaderHidden(true);
        history_->setRootIsDecorated(true);
        history_->setUniformRowHeights(true);
        history_->setIndentation(19);
        history_->setIconSize(QSize(16, 16));
        history_->setSelectionMode(QAbstractItemView::SingleSelection);
        history_->setEditTriggers(QAbstractItemView::EditKeyPressed);
        history_->setDragEnabled(true);
        history_->setAcceptDrops(true);
        history_->setDropIndicatorShown(true);
        history_->setDragDropMode(QAbstractItemView::InternalMove);
        history_->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(history_, 1);

        auto* bar = new QWidget(sidebar);
        bar->setObjectName("homeSidebarBar");
        auto* barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(4, 4, 4, 4);
        barLayout->setSpacing(4);
        addButton_ = new QToolButton(bar);
        addButton_->setObjectName("homeBarButton");
        addButton_->setText("+");
        addButton_->setToolTip("New saved session folder / save selected session");
        removeButton_ = new QToolButton(bar);
        removeButton_->setObjectName("homeBarButton");
        removeButton_->setText(QString::fromUtf8("\xE2\x88\x92"));
        removeButton_->setToolTip("Remove selected");
        search_ = new QLineEdit(bar);
        search_->setObjectName("homeSearch");
        search_->setClearButtonEnabled(true);
        search_->addAction(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Find), QLineEdit::LeadingPosition);
        barLayout->addWidget(addButton_);
        barLayout->addWidget(removeButton_);
        barLayout->addWidget(search_, 1);
        layout->addWidget(bar);
    }

    void buildContent(QSplitter* split) {
        auto* content = new QWidget(split);
        content->setObjectName("homeContent");
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(11, 10, 11, 0);
        layout->setSpacing(0);

        // Session information: only visible while a session node is selected.
        // It keeps its space when hidden so the icon grid never jumps.
        info_ = new QWidget(content);
        info_->setObjectName("homeInfo");
        info_->setFixedHeight(132);
        auto policy = info_->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        info_->setSizePolicy(policy);
        auto* infoLayout = new QVBoxLayout(info_);
        infoLayout->setContentsMargins(0, 0, 0, 0);
        infoLayout->setSpacing(4);

        auto* nameRow = new QHBoxLayout;
        nameRow->setSpacing(6);
        infoIcon_ = new QLabel(info_);
        infoIcon_->setFixedSize(16, 16);
        infoName_ = new QLabel(info_);
        infoName_->setObjectName("homeInfoName");
        nameRow->addWidget(infoIcon_);
        nameRow->addWidget(infoName_, 1);
        infoLayout->addLayout(nameRow);

        leftLabel_ = new QLabel(info_);
        rightLabel_ = new QLabel(info_);
        for (auto* label : {leftLabel_, rightLabel_}) {
            label->setObjectName("homeInfoPath");
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        leftEdit_ = new QLineEdit(info_);
        rightEdit_ = new QLineEdit(info_);
        infoLayout->addWidget(leftLabel_);
        infoLayout->addWidget(leftEdit_);
        infoLayout->addWidget(rightLabel_);
        infoLayout->addWidget(rightEdit_);
        infoLayout->addSpacing(14);

        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        open_ = new QPushButton("Open", info_);
        open_->setObjectName("homeOpen");
        open_->setDefault(true);
        edit_ = new QPushButton("Edit", info_);
        edit_->setObjectName("homeEdit");
        buttons->addWidget(open_);
        buttons->addWidget(edit_);
        buttons->addStretch();
        infoLayout->addLayout(buttons);
        infoLayout->addStretch();
        layout->addWidget(info_);

        layout->addStretch(1);

        auto* center = new QWidget(content);
        auto* centerLayout = new QVBoxLayout(center);
        centerLayout->setContentsMargins(0, 0, 0, 0);
        centerLayout->setSpacing(0);
        auto* hint = new QLabel("Drag folders or files onto session icon\nor click a session icon to begin:", center);
        hint->setObjectName("homeHint");
        hint->setAlignment(Qt::AlignHCenter);
        centerLayout->addWidget(hint);
        centerLayout->addSpacing(22);

        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(0);
        grid->setVerticalSpacing(14);
        int primaryColumn = 0;
        int secondaryColumn = 0;
        for (const auto& spec : sessionTypes()) {
            auto* tile = new SessionTile(center);
            tile->setObjectName("homeTile");
            tile->setText(spec.label);
            tile->setIcon(sessionTypeIcon(spec.kind));
            tile->setIconSize(spec.primary ? QSize(64, 64) : QSize(32, 32));
            tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            tile->setFixedSize(138, spec.primary ? 96 : 66);
            tile->setCursor(Qt::PointingHandCursor);
            const QString kind = spec.kind;
            connect(tile, &QToolButton::clicked, this, [this, kind]() { launchNew(kind, {}); });
            tile->onPathsDropped = [this, kind](const QStringList& paths) { launchNew(kind, paths); };
            grid->addWidget(tile, spec.primary ? 0 : 1, spec.primary ? primaryColumn++ : secondaryColumn++);
        }
        centerLayout->addLayout(grid);
        layout->addWidget(center, 0, Qt::AlignHCenter);
        layout->addStretch(2);
    }

    void buildFooter(QVBoxLayout* outer) {
        footer_ = new QFrame(this);
        footer_->setObjectName("homeFooter");
        footer_->setFixedHeight(117);
        auto* row = new QHBoxLayout(footer_);
        row->setContentsMargins(18, 18, 8, 6);
        row->setSpacing(58);

        auto column = [this](const QString& title) {
            auto* box = new QVBoxLayout;
            box->setSpacing(4);
            auto* head = new QLabel(title, footer_);
            head->setObjectName("homeFooterTitle");
            box->addWidget(head);
            return box;
        };

        auto* kb = column("KNOWLEDGE BASE");
        auto* kbRow = new QHBoxLayout;
        auto* kbEdit = new QLineEdit(footer_);
        kbEdit->setObjectName("homeFooterSearch");
        kbEdit->setFixedWidth(167);
        auto* kbButton = new QToolButton(footer_);
        kbButton->setObjectName("homeFooterIcon");
        kbButton->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Find));
        kbRow->addWidget(kbEdit);
        kbRow->addWidget(kbButton);
        kbRow->addStretch();
        kb->addLayout(kbRow);
        kb->addStretch();
        row->addLayout(kb);

        auto* forums = column("SUPPORT FORUMS");
        auto* forumsText = new QLabel("Visit our <b><u>online forums</u></b> for<br>technical support.", footer_);
        forumsText->setObjectName("homeFooterText");
        forums->addWidget(forumsText);
        forums->addStretch();
        row->addLayout(forums);

        auto* casts = column("SCREENCASTS");
        auto* castsText = new QLabel("Watch our <b><u>online videos</u></b> for tips<br>on getting the most out of OpenBC.", footer_);
        castsText->setObjectName("homeFooterText");
        casts->addWidget(castsText);
        casts->addStretch();
        row->addLayout(casts);

        row->addStretch(1);
        auto* close = new QToolButton(footer_);
        close->setObjectName("homeFooterIcon");
        close->setIcon(openbc::ui::icons::glyph(openbc::ui::icons::Glyph::Close));
        close->setAutoRaise(true);
        connect(close, &QToolButton::clicked, this, [this]() { footer_->hide(); });
        row->addWidget(close, 0, Qt::AlignTop);
        outer->addWidget(footer_);
    }

    // ----- launching ------------------------------------------------------
    void launchNew(const QString& kind, const QStringList& paths) {
        if (onNewSession) {
            onNewSession(kind, paths);
            return;
        }
        if (kind == "folder-compare" && onNewFolderCompare) onNewFolderCompare();
        else if (kind == "text-compare" && onNewTextCompare) onNewTextCompare();
    }

    void openSelected() {
        if (!hasSelection() || selected_.kind.isEmpty() || !onOpenHistory) return;
        selected_.label = selectedItem_->text(0);
        if (leftEdit_->isVisible()) {
            selected_.left = leftEdit_->text();
            selected_.right = rightEdit_->text();
        }
        // Reopening from any node other than "Today" (older buckets or a saved
        // node) records a fresh entry for today; already-Today items are a no-op.
        SessionHistory::record(selected_.kind, selected_.left, selected_.right);
        const SessionHistoryEntry entry = selected_;
        refreshHistory();
        onOpenHistory(entry);
    }

    void toggleEditing() {
        if (!hasSelection()) return;
        const bool startEditing = !leftEdit_->isVisible();
        if (startEditing) {
            leftEdit_->setText(selected_.left);
            rightEdit_->setText(selected_.right);
        } else {
            selected_.left = leftEdit_->text();
            selected_.right = rightEdit_->text();
            if (selectedSaved_) {
                SessionHistory::updateSaved(selected_.savedNode, selected_.savedIndex, selected_,
                                            selectedItem_->text(0));
            }
            leftLabel_->setText(selected_.left);
            rightLabel_->setText(selected_.right);
        }
        setEditing(startEditing);
    }

    void setEditing(bool editing) {
        leftLabel_->setVisible(!editing);
        rightLabel_->setVisible(!editing);
        leftEdit_->setVisible(editing);
        rightEdit_->setVisible(editing);
        edit_->setText(editing ? "Done" : "Edit");
    }

    // ----- sidebar bar buttons / filter -----------------------------------
    void addPressed() {
        QTreeWidgetItem* item = history_->currentItem();
        if (item && item->data(0, Qt::UserRole + 3).toBool()) {
            saveSelected();
            return;
        }
        QString parentNode = "Personal";
        if (item && item->data(0, Qt::UserRole + 4).toBool()) parentNode = item->data(0, Qt::UserRole + 2).toString();
        bool ok = false;
        const QString name = QInputDialog::getText(this, "New saved session folder", "Name",
                                                   QLineEdit::Normal, QString(), &ok);
        if (ok && SessionHistory::createNode(parentNode, name)) refreshHistory();
    }

    void removePressed() {
        QTreeWidgetItem* item = history_->currentItem();
        if (!item) return;
        if (item->data(0, Qt::UserRole + 4).toBool()) {
            SessionHistory::removeNode(item->data(0, Qt::UserRole + 2).toString());
            refreshHistory();
        } else {
            removeSession(item);
        }
    }

    void removeSession(QTreeWidgetItem* item) {
        if (!item || !item->data(0, Qt::UserRole + 3).toBool()) return;
        const bool saved = item->data(0, Qt::UserRole).toBool();
        const QString node = item->data(0, Qt::UserRole + 2).toString();
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (saved) SessionHistory::removeSaved(node, index); else SessionHistory::removeAt(index);
        refreshHistory();
    }

    bool filterItem(QTreeWidgetItem* item, const QString& needle, bool ancestorMatches) {
        const bool self = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
        bool anyChild = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (filterItem(item->child(i), needle, ancestorMatches || self)) anyChild = true;
        }
        const bool show = self || anyChild || ancestorMatches;
        item->setHidden(!show);
        if (!needle.isEmpty() && anyChild) item->setExpanded(true);
        return show;
    }

    void applyFilter(const QString& text) {
        const QString needle = text.trimmed();
        applyingFilter_ = true;  // filterItem() expands matches; don't persist that
        for (int i = 0; i < history_->topLevelItemCount(); ++i) filterItem(history_->topLevelItem(i), needle, false);
        applyingFilter_ = false;
    }

    // ----- tree items -----------------------------------------------------
    static QColor bucketColor(RecentBucket bucket) {
        switch (bucket) {
        case RecentBucket::Today: return openbc::ui::color::groupToday();
        case RecentBucket::Yesterday: return openbc::ui::color::groupYesterday();
        case RecentBucket::ThisWeek: return openbc::ui::color::groupThisWeek();
        case RecentBucket::LastWeek: return openbc::ui::color::groupLastWeek();
        case RecentBucket::Older:
        default: return openbc::ui::color::groupOlder();
        }
    }

    // Colour for a named saved-session node: "Personal" gets its own fixed
    // colour, nodes nested under it get a persisted random colour each, and
    // everything else falls back to a shared neutral colour.
    static QColor nodeGroupColor(QTreeWidgetItem* parent, const SavedSessionNode& node) {
        if (!parent && node.name == "Personal") return openbc::ui::color::groupPersonal();
        const QColor persisted = SessionHistory::colorForNode(node.path);
        return persisted.isValid() ? persisted : openbc::ui::color::groupDefault();
    }

    void addSessionItem(QTreeWidgetItem* parent, const SessionHistoryEntry& entry, bool saved,
                        const QString& node, int index) {
        const QString label = entry.label.isEmpty() ? SessionHistory::defaultLabel(entry.kind, entry.left, entry.right) : entry.label;
        auto* item = new QTreeWidgetItem(parent, {label});
        item->setIcon(0, sessionEntryIcon(entry));
        item->setToolTip(0, entry.left + "\n<->\n" + entry.right);
        item->setData(0, Qt::UserRole, saved);
        item->setData(0, Qt::UserRole + 1, index);
        item->setData(0, Qt::UserRole + 2, node);
        item->setData(0, Qt::UserRole + 3, true);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDragEnabled);
    }

    void addNodeItem(QTreeWidgetItem* parent, const SavedSessionNode& node) {
        auto* item = parent ? new QTreeWidgetItem(parent, {node.name}) : new QTreeWidgetItem(history_, {node.name});
        item->setIcon(0, openbc::ui::icons::groupNodeIcon(nodeGroupColor(parent, node)));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                       Qt::ItemIsDropEnabled);
        item->setData(0, Qt::UserRole + 2, node.path);
        item->setData(0, Qt::UserRole + 4, true);
        for (int index = 0; index < node.sessions.size(); ++index) {
            addSessionItem(item, node.sessions[index], true, node.path, index);
        }
        for (const auto& child : node.children) addNodeItem(item, child);
        restoreExpanded(item, "saved/" + node.path, false);
    }

    // ----- tree expand/collapse persistence -------------------------------
    // Tags `item` with a stable key and applies its remembered state (or the
    // default when the user never toggled it). Call after its children exist.
    void restoreExpanded(QTreeWidgetItem* item, const QString& key, bool defaultExpanded) {
        item->setData(0, Qt::UserRole + 8, key);
        item->setExpanded(treeState_.value(key, defaultExpanded));
    }

    void rememberExpanded(QTreeWidgetItem* item, bool expanded) {
        if (applyingFilter_ || !item) return;  // search auto-expands are temporary
        const QString key = item->data(0, Qt::UserRole + 8).toString();
        if (key.isEmpty() || treeState_.value(key, !expanded) == expanded) return;
        treeState_.insert(key, expanded);
        SessionHistory::saveTreeState(treeState_);
    }

    // Keeps saved keys attached to a node (and its descendants) when it is renamed.
    void remapTreeKeys(const QString& from, const QString& to) {
        QHash<QString, bool> updated;
        for (auto it = treeState_.constBegin(); it != treeState_.constEnd(); ++it) {
            QString key = it.key();
            if (key == from || key.startsWith(from + "/")) key = to + key.mid(from.size());
            updated.insert(key, it.value());
        }
        treeState_ = updated;
        SessionHistory::saveTreeState(treeState_);
    }

    // ----- selection / details --------------------------------------------
    bool hasSelection() const { return selectedItem_ && selectedItem_->data(0, Qt::UserRole + 3).toBool(); }

    void showSelected() {
        selectedItem_ = history_->currentItem();
        if (!hasSelection()) {
            clearDetails();
            return;
        }
        const bool saved = selectedItem_->data(0, Qt::UserRole).toBool();
        const int index = selectedItem_->data(0, Qt::UserRole + 1).toInt();
        const QString node = selectedItem_->data(0, Qt::UserRole + 2).toString();
        selected_ = saved ? SessionHistory::loadSaved(node).value(index) : SessionHistory::load().value(index);
        selected_.savedNode = node;
        selected_.savedIndex = index;
        selectedSaved_ = saved;
        infoIcon_->setPixmap(selectedItem_->icon(0).pixmap(16, 16));
        infoName_->setText(selectedItem_->text(0));
        leftLabel_->setText(selected_.left);
        rightLabel_->setText(selected_.right);
        setEditing(false);
        info_->setVisible(true);
    }

    void clearDetails() {
        selected_ = {};
        selectedItem_ = nullptr;
        leftEdit_->clear();
        rightEdit_->clear();
        leftLabel_->clear();
        rightLabel_->clear();
        setEditing(false);
        info_->setVisible(false);
    }

    void saveSelected() {
        if (!hasSelection()) return;
        selected_.label = selectedItem_->text(0).trimmed();
        if (leftEdit_->isVisible()) {
            selected_.left = leftEdit_->text();
            selected_.right = rightEdit_->text();
        }
        if (selectedSaved_) {
            SessionHistory::updateSaved(selected_.savedNode, selected_.savedIndex, selected_, selected_.label);
        } else {
            bool ok = false;
            const QString node = QInputDialog::getText(this, "Save session", "Named node", QLineEdit::Normal,
                                                       "Personal", &ok);
            if (ok && !node.trimmed().isEmpty()) SessionHistory::save(selected_, node, selected_.label);
        }
        refreshHistory();
    }

    // ----- context menus / rename / move ----------------------------------
    void contextMenu(const QPoint& position) {
        auto* item = history_->itemAt(position);
        if (!item) return;
        history_->setCurrentItem(item);
        if (item->data(0, Qt::UserRole + 7).toBool()) {  // "New" root
            QMenu menu(history_);
            QAction* remote = menu.addAction("Manage remote connections...");
            if (menu.exec(history_->viewport()->mapToGlobal(position)) == remote) manageRemoteConnections();
            return;
        }
        if (!item->data(0, Qt::UserRole + 6).toString().isEmpty()) return;  // "New" children: no menu
        if (item->data(0, Qt::UserRole + 5).toBool()) return;  // date-bucket header: no menu
        const bool isNode = item->data(0, Qt::UserRole + 4).toBool();
        const QString node = item->data(0, Qt::UserRole + 2).toString();
        if (isNode) {
            showNodeMenu(item, node, position);
            return;
        }
        if (!item->data(0, Qt::UserRole + 3).toBool()) return;  // only real session rows below
        QMenu menu(history_);
        QAction* rename = menu.addAction("Rename session");
        QAction* save = menu.addAction("Save to named node");
        QAction* remove = menu.addAction("Remove from history");
        QAction* chosen = menu.exec(history_->viewport()->mapToGlobal(position));
        if (chosen == rename) {
            history_->editItem(item, 0);
        } else if (chosen == save) {
            history_->setCurrentItem(item);
            saveSelected();
        } else if (chosen == remove) {
            removeSession(item);
        }
    }

    void showNodeMenu(QTreeWidgetItem* item, const QString& node, const QPoint& position) {
        QMenu menu(history_);
        QAction* create = menu.addAction("New saved session folder");
        QAction* rename = menu.addAction("Rename");
        QAction* remove = menu.addAction("Remove named node");
        QAction* chosen = menu.exec(history_->viewport()->mapToGlobal(position));
        if (chosen == create) {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "New saved session folder", "Name",
                                                       QLineEdit::Normal, QString(), &ok);
            if (ok && SessionHistory::createNode(node, name)) refreshHistory();
        } else if (chosen == rename) {
            history_->editItem(item, 0);
        } else if (chosen == remove) {
            SessionHistory::removeNode(node);
            refreshHistory();
        }
    }

    void handleItemRenamed(QTreeWidgetItem* item, int column) {
        if (!item || column != 0) return;
        const QString value = item->text(0).trimmed();
        if (item->data(0, Qt::UserRole + 4).toBool()) {
            const QString oldNode = item->data(0, Qt::UserRole + 2).toString();
            if (value.isEmpty() || !SessionHistory::renameNode(oldNode, value)) {
                const QSignalBlocker blocker(history_);
                item->setText(0, QFileInfo(oldNode).fileName());
                return;
            }
            const QString parentPath = QFileInfo(oldNode).path();
            remapTreeKeys("saved/" + oldNode,
                          "saved/" + (parentPath == "." ? value : parentPath + "/" + value));
            refreshHistory();
            return;
        }
        if (!item->data(0, Qt::UserRole + 3).toBool() || value.isEmpty()) return;
        const bool saved = item->data(0, Qt::UserRole).toBool();
        const QString node = item->data(0, Qt::UserRole + 2).toString();
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (saved) SessionHistory::renameSaved(node, index, value);
        else SessionHistory::renameRecent(index, value);
        refreshHistory();
    }

    void moveSession(QTreeWidgetItem* source, QTreeWidgetItem* target) {
        if (!source || !target || !source->data(0, Qt::UserRole + 3).toBool() ||
            !target->data(0, Qt::UserRole + 4).toBool()) return;
        const bool saved = source->data(0, Qt::UserRole).toBool();
        const QString sourceNode = source->data(0, Qt::UserRole + 2).toString();
        const int index = source->data(0, Qt::UserRole + 1).toInt();
        const QString targetNode = target->data(0, Qt::UserRole + 2).toString();
        if (SessionHistory::moveSession(saved, sourceNode, index, targetNode)) refreshHistory();
    }

    HistoryTree* history_ = nullptr;
    QTreeWidgetItem* selectedItem_ = nullptr;
    QWidget* info_ = nullptr;
    QFrame* footer_ = nullptr;
    QLabel* infoIcon_ = nullptr;
    QLabel* infoName_ = nullptr;
    QLabel* leftLabel_ = nullptr;
    QLabel* rightLabel_ = nullptr;
    QLineEdit* leftEdit_ = nullptr;
    QLineEdit* rightEdit_ = nullptr;
    QLineEdit* search_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* edit_ = nullptr;
    SessionHistoryEntry selected_;
    bool selectedSaved_ = false;
    QHash<QString, bool> treeState_;  // tree key -> expanded, mirrored to tree_state.json
    bool applyingFilter_ = false;
    std::unique_ptr<UpdateNotifier> updates_;  // declared last: destroyed first (release notices, home/update_notice.h)
};

}  // namespace openbc::app
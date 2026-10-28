// drop_filter.h
// ---------------------------------------------------------------------------
// ONE reusable drag-and-drop entry point. Installed on a tab's root widget it
// watches the root and every descendant (including scroll-area viewports and
// line edits, which would otherwise swallow a file drop as text) and turns file /
// folder URL drags into a single callback. Widgets no longer need their own
// dragEnter/dropEvent logic.
//
//   dragEnter : accepted for 1-2 URLs; refused (forbidden cursor + hint) for more,
//               matching the router policy. Internal drags (no URLs: session tree
//               re-ordering, text selections) are not touched.
//   dragMove  : keeps the drop accepted; no file-system access while moving.
//   drop      : parses/validates (drop_payload.h) and calls onDrop on the GUI thread
//               with the side under the cursor as a hint.
// ---------------------------------------------------------------------------
#pragma once

#include <QChildEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>

#include "common/drop_payload.h"
#include "common/drop_router.h"

namespace openbc::app {

class DropRouteFilter : public QObject {
public:
    // Called for every drop that carried URLs (even if every entry was rejected: parse.problems).
    std::function<void(const DropParse&, std::optional<drop::Side>)> onDrop;
    // Status-bar style feedback while dragging (e.g. "Drop one or two items at a time").
    std::function<void(const QString&)> onHint;

    // `skip` names objects that keep their own, more specific drop behaviour (e.g. Home's
    // per-type tiles); it must be known up front because the filter installs itself right away.
    explicit DropRouteFilter(QWidget* root, std::function<bool(QObject*)> skip = {})
        : QObject(root), skip_(std::move(skip)), root_(root) {
        root->setAcceptDrops(true);
        install(root);
    }

    void install(QWidget* widget) {
        if (!widget || (skip_ && skip_(widget))) return;
        widget->installEventFilter(this);  // installing twice is a no-op in Qt
        const auto children = widget->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
        for (QWidget* child : children) install(child);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::ChildAdded: {
            // The child is not fully constructed yet; wire it once the event loop turns.
            auto* child = static_cast<QChildEvent*>(event)->child();
            if (child && child->isWidgetType()) {
                const QPointer<QObject> guard(child);
                QTimer::singleShot(0, this, [this, guard]() {
                    if (guard) install(qobject_cast<QWidget*>(guard.data()));
                });
            }
            return false;
        }
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto* drag = static_cast<QDropEvent*>(event);
            if (!drag->mimeData() || !drag->mimeData()->hasUrls()) return false;  // not ours
            const int count = droppedUrlCount(drag->mimeData());
            if (count < 1 || count > 2) {
                if (event->type() == QEvent::DragEnter && onHint)
                    onHint(QString("Drop one or two items at a time to compare (%1 dragged).").arg(count));
                drag->ignore();
                return true;
            }
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        case QEvent::Drop: {
            auto* dropEv = static_cast<QDropEvent*>(event);
            if (!dropEv->mimeData() || !dropEv->mimeData()->hasUrls()) return false;
            const DropParse parsed = parseDropMime(dropEv->mimeData());
            std::optional<drop::Side> hint;
            if (auto* w = qobject_cast<QWidget*>(watched); w && root_) {
                const QPoint inRoot = w->mapTo(root_, dropEv->position().toPoint());
                hint = inRoot.x() < root_->width() / 2 ? drop::Side::Left : drop::Side::Right;
            }
            dropEv->setDropAction(Qt::CopyAction);
            dropEv->accept();
            // Handled after the event returns so a modal dialog / tab swap never runs inside the drag.
            QTimer::singleShot(0, this, [this, parsed, hint]() {
                if (onDrop) onDrop(parsed, hint);
            });
            return true;
        }
        default:
            return false;
        }
    }

private:
    std::function<bool(QObject*)> skip_;
    QPointer<QWidget> root_;
};

}  // namespace openbc::app

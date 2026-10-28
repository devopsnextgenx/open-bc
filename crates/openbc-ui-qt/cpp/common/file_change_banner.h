// file_change_banner.h
// ---------------------------------------------------------------------------
// Popup notice for "a loaded file changed on disk / remotely".
//
// A floating card parented to the viewer (NOT part of its layout): it sits over the top-right of
// the tab content, above every child, with an accent border and a drop shadow, so it cannot be
// scrolled away or overlooked while the user keeps working. It follows the viewer's resizes.
// If the viewer's tab is in the background the card shows when the tab is next opened; if the
// whole window is inactive the taskbar/dock entry is flashed (QApplication::alert).
//
//   Full    : "<Left file 'a.txt' was changed externally. The version currently displayed may be
//             outdated.>"                      [Keep Current] [Reload Latest]   (x)
//   Compact : a small "outdated" pill with [Reload Latest]. Reached by Keep Current or by
//             dismissing (x). Dismissing never reloads; the pill stays while the side is stale.
//
// A genuinely newer version (engine sends a new Stale) expands it to Full again; repeated events
// for the same version never reach this widget (the engine de-duplicates them).
// Pure presentation: no file access, no engine calls (see file_change_controller.h).
// ---------------------------------------------------------------------------
#pragma once

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <functional>

namespace openbc::app {

class FileChangeBanner : public QFrame {
public:
    // Sides (0 = left, 1 = right) the user asked to reload / to keep as they are.
    std::function<void(const QList<std::uint8_t>&)> onReload;
    std::function<void(const QList<std::uint8_t>&)> onKeep;

    explicit FileChangeBanner(QWidget* parent) : QFrame(parent), host_(parent) {
        setObjectName("fileChangeBanner");
        setFrameShape(QFrame::NoFrame);
        setAutoFillBackground(true);  // opaque: it floats over content
        setStyleSheet(
            "QFrame#fileChangeBanner { background: palette(window); border: 2px solid #d9a31b; border-radius: 6px; }"
            " QFrame#fileChangeBanner QLabel { background: transparent; }");
        auto* shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(18);
        shadow->setOffset(0, 3);
        shadow->setColor(QColor(0, 0, 0, 110));
        setGraphicsEffect(shadow);

        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(10, 8, 8, 8);
        col->setSpacing(8);
        auto* top = new QHBoxLayout();
        top->setSpacing(8);
        icon_ = new QLabel(QString::fromUtf8("\xE2\x9A\xA0"), this);  // warning sign
        QFont iconFont = icon_->font();
        iconFont.setPointSizeF(iconFont.pointSizeF() * 1.4);
        icon_->setFont(iconFont);
        text_ = new QLabel(this);
        text_->setTextFormat(Qt::PlainText);
        text_->setWordWrap(true);
        text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        dismiss_ = new QToolButton(this);
        dismiss_->setText(QString::fromUtf8("\xC3\x97"));  // multiplication sign
        dismiss_->setAutoRaise(true);
        dismiss_->setToolTip("Dismiss (does not reload; the file stays marked as outdated)");
        top->addWidget(icon_, 0, Qt::AlignTop);
        top->addWidget(text_, 1);
        top->addWidget(dismiss_, 0, Qt::AlignTop);
        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(8);
        keep_ = new QPushButton("Keep Current", this);
        reload_ = new QPushButton("Reload Latest", this);
        reload_->setDefault(true);
        for (auto* b : {keep_, reload_}) b->setFocusPolicy(Qt::NoFocus);  // never steals typing focus
        buttons->addStretch(1);
        buttons->addWidget(keep_);
        buttons->addWidget(reload_);
        col->addLayout(top);
        col->addLayout(buttons);

        connect(reload_, &QPushButton::clicked, this, [this]() {
            const QList<std::uint8_t> sides = reloadableSides();
            if (!sides.isEmpty() && onReload) onReload(sides);
        });
        connect(keep_, &QPushButton::clicked, this, [this]() {
            if (onKeep) onKeep(staleSides());
            collapse();
        });
        connect(dismiss_, &QToolButton::clicked, this, [this]() { collapse(); });
        if (host_) host_->installEventFilter(this);  // follow the viewer's size
        hide();
    }

    bool isStale(std::uint8_t side) const { return side < 2 && stale_[side]; }

    // A side's loaded version is outdated. `change`: "modified" | "replaced" | "deleted" | "created".
    void showStale(std::uint8_t side, const QString& path, const QString& change) {
        if (side > 1) return;
        stale_[side] = true;
        change_[side] = change;
        name_[side] = QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName();
        unavailable_[side].clear();
        compact_ = false;  // a (new) version always gets the full popup
        refresh();
        if (QWidget* top = window(); top && !top->isActiveWindow()) QApplication::alert(top, 0);
    }

    // The side matches what is displayed again (reloaded, or the file was reverted).
    void clearSide(std::uint8_t side) {
        if (side > 1) return;
        stale_[side] = false;
        unavailable_[side].clear();
        if (!stale_[0] && !stale_[1]) compact_ = false;
        refresh();
    }

    // The resource could not be checked (permissions, network). Information only; not "stale".
    void showUnavailable(std::uint8_t side, const QString& path, const QString& message) {
        if (side > 1 || stale_[side]) return;  // a stale notice is more important
        name_[side] = QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName();
        unavailable_[side] = message;
        refresh();
    }

    QString message() const { return text_->text(); }  // for tests / status bars

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == host_ && isVisibleTo(host_) &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest))
            reposition();
        return false;
    }

private:
    static QString sideName(std::uint8_t side) { return side == 0 ? "Left" : "Right"; }

    static QString changeWords(const QString& change) {
        if (change == "deleted") return "was deleted or moved";
        if (change == "replaced") return "was replaced";
        if (change == "created") return "now exists";
        return "was changed";
    }

    QList<std::uint8_t> staleSides() const {
        QList<std::uint8_t> out;
        for (std::uint8_t s = 0; s < 2; ++s) if (stale_[s]) out << s;
        return out;
    }

    // A deleted file cannot be reloaded: loaded content is never replaced by "nothing".
    QList<std::uint8_t> reloadableSides() const {
        QList<std::uint8_t> out;
        for (std::uint8_t s = 0; s < 2; ++s) if (stale_[s] && change_[s] != "deleted") out << s;
        return out;
    }

    void collapse() {
        compact_ = true;
        refresh();
    }

    // Pins the card to the top-right corner of the viewer, above all of its children.
    void reposition() {
        if (!host_) return;
        const int margin = 10;
        const int width = std::max(240, std::min(compact_ ? 360 : 460, host_->width() - 2 * margin));
        setFixedWidth(width);
        QLayout* lay = layout();
        const int height = lay->hasHeightForWidth() ? lay->heightForWidth(width) : lay->sizeHint().height();
        setFixedHeight(std::max(height, lay->minimumSize().height()));
        move(std::max(margin, host_->width() - width - margin), margin);
        raise();
    }

    void refresh() {
        const QList<std::uint8_t> stale = staleSides();
        if (stale.isEmpty()) {
            QString info;
            for (std::uint8_t s = 0; s < 2; ++s)
                if (!unavailable_[s].isEmpty())
                    info += (info.isEmpty() ? "" : "  ") + QString("Cannot check %1 file %2: %3.").arg(sideName(s), name_[s], unavailable_[s]);
            text_->setText(info);
            reload_->hide();
            keep_->hide();
            dismiss_->setVisible(!info.isEmpty());
            setVisible(!info.isEmpty());
            if (!info.isEmpty()) reposition();
            return;
        }
        const bool canReload = !reloadableSides().isEmpty();
        if (compact_) {
            text_->setText(stale.size() == 2 ? "Both displayed files are outdated."
                                             : sideName(stale[0]) + " file is outdated.");
            keep_->hide();
            dismiss_->hide();
        } else {
            QStringList parts;
            for (std::uint8_t s : stale) parts << QString("%1 file '%2' %3").arg(sideName(s), name_[s], changeWords(change_[s]));
            text_->setText(parts.join(" and ") + " externally. The version currently displayed may be outdated.");
            keep_->show();
            dismiss_->show();
        }
        reload_->show();
        reload_->setEnabled(canReload);
        reload_->setToolTip(canReload ? QString() : "The file no longer exists, so there is nothing to reload.");
        show();
        reposition();
    }

    QWidget* host_ = nullptr;
    QLabel* icon_ = nullptr;
    QLabel* text_ = nullptr;
    QPushButton* reload_ = nullptr;
    QPushButton* keep_ = nullptr;
    QToolButton* dismiss_ = nullptr;
    bool stale_[2] = {false, false};
    bool compact_ = false;
    QString change_[2], name_[2], unavailable_[2];
};

}  // namespace openbc::app

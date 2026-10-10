// unsaved_changes_dialog.h  (put in src/common/)
// ---------------------------------------------------------------------------
// One dialog for "this tab has unsaved edits" - used when closing a tab and by
// the viewers' Reload / Swap / change-path guards.
//
//   * Both sides are always listed. A side without changes has its Save button
//     disabled; a side with changes has it enabled.
//   * Clicking Save writes that side (via the viewer's SaveSideFn). On success
//     the button goes inactive. If it was the last side with changes the dialog
//     closes by itself - nothing is left for the user to decide.
//   * Continue proceeds WITHOUT saving whatever is still unsaved (the caption
//     says exactly what will be discarded). Cancel / Esc keeps the tab open.
//   * Saves are asynchronous (backend sessions answer on a later event), so the
//     viewer reports completion through SaveDone. Nothing can be clicked while a
//     save is in flight, and a watchdog frees the dialog if no answer arrives.
//
// Returns true  -> caller may go on (close the tab / reload / swap).
// Returns false -> caller must stay as it is.
// ---------------------------------------------------------------------------
#pragma once

#include <QDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

#include "style/qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

struct UnsavedSide {
    QString path;
    int changes = 0;  // 0 = clean
};

// Call exactly once per save request: ok=false carries a user-readable reason.
using SaveDone = std::function<void(bool ok, const QString& error)>;
using SaveSideFn = std::function<void(bool left, SaveDone done)>;

class UnsavedChangesDialog : public QDialog {
public:
    UnsavedChangesDialog(QWidget* parent, const UnsavedSide& left, const UnsavedSide& right, SaveSideFn save)
        : QDialog(parent), save_(std::move(save)) {
        setObjectName("unsavedChangesDialog");
        setWindowTitle("Unsaved changes");
        setModal(true);
        side_[0] = left;
        side_[1] = right;

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(16, 14, 16, 12);
        root->setSpacing(10);

        auto* heading = new QLabel("Save your changes before continuing?", this);
        QFont headingFont = heading->font();
        headingFont.setBold(true);
        heading->setFont(headingFont);
        root->addWidget(heading);

        for (int s = 0; s < 2; ++s) {
            auto* row = new QFrame(this);
            row->setObjectName("sideHeader");  // same dark card the compare tabs use for a side
            auto* line = new QHBoxLayout(row);
            line->setContentsMargins(10, 6, 10, 6);
            line->setSpacing(10);

            auto* text = new QVBoxLayout;
            text->setSpacing(1);
            name_[s] = new QLabel(row);
            name_[s]->setObjectName("sidePath");
            info_[s] = new QLabel(row);
            info_[s]->setObjectName("sideAttrs");
            text->addWidget(name_[s]);
            text->addWidget(info_[s]);
            line->addLayout(text, 1);

            save_btn_[s] = new QPushButton(s == 0 ? "Save Left" : "Save Right", row);
            save_btn_[s]->setMinimumWidth(104);
            line->addWidget(save_btn_[s]);
            root->addWidget(row);

            QObject::connect(save_btn_[s], &QPushButton::clicked, this, [this, s]() { saveClicked(s == 0); });
        }

        error_ = new QLabel(this);
        error_->setWordWrap(true);
        error_->setStyleSheet("color:#e5534b;");
        error_->hide();
        root->addWidget(error_);

        caption_ = new QLabel(this);
        caption_->setObjectName("sideAttrs");
        caption_->setWordWrap(true);
        root->addWidget(caption_);

        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        buttons->addStretch(1);
        cancel_ = new QPushButton("Cancel", this);
        cancel_->setAutoDefault(false);
        continue_ = new QPushButton("Continue", this);
        continue_->setAutoDefault(false);
        continue_->setMinimumWidth(96);
        buttons->addWidget(cancel_);
        buttons->addWidget(continue_);
        root->addLayout(buttons);

        QObject::connect(cancel_, &QPushButton::clicked, this, [this]() { reject(); });
        QObject::connect(continue_, &QPushButton::clicked, this, [this]() { accept(); });

        watchdog_ = new QTimer(this);
        watchdog_->setSingleShot(true);
        watchdog_->setInterval(30000);
        QObject::connect(watchdog_, &QTimer::timeout, this, [this]() {
            ++token_;  // a late answer from the backend is now ignored
            busy_ = false;
            error_->setText("Timed out waiting for the save to finish.");
            error_->show();
            refresh();
        });

        setMinimumWidth(460);
        refresh();
    }

    // Closing the dialog with the window's X while a save runs would leave the
    // viewer with a dangling callback target; ignore it until the save answers.
    void reject() override {
        if (busy_) return;
        QDialog::reject();
    }

private:
    void saveClicked(bool left) {
        if (busy_ || !save_) return;
        const int s = left ? 0 : 1;
        if (side_[s].changes <= 0) return;
        busy_ = true;
        savingSide_ = s;
        error_->hide();
        const int token = ++token_;
        watchdog_->start();
        refresh();
        const QPointer<UnsavedChangesDialog> self(this);
        save_(left, [self, token, s](bool ok, const QString& error) {
            if (!self || token != self->token_ || !self->busy_) return;
            self->saveFinished(s, ok, error);
        });
    }

    void saveFinished(int s, bool ok, const QString& error) {
        watchdog_->stop();
        busy_ = false;
        savingSide_ = -1;
        if (!ok) {
            error_->setText(error.isEmpty() ? QString("Could not save %1.").arg(s == 0 ? "the left file" : "the right file") : error);
            error_->show();
            refresh();
            return;
        }
        side_[s].changes = 0;
        if (side_[0].changes <= 0 && side_[1].changes <= 0) {
            accept();  // that was the last unsaved side: nothing left to ask
            return;
        }
        refresh();
    }

    void refresh() {
        static const QColor kBlue(0x4e, 0x9c, 0xff);
        static const QColor kInk(0x8b, 0x90, 0xa6);
        QStringList leftOver;
        for (int s = 0; s < 2; ++s) {
            const bool dirty = side_[s].changes > 0;
            const QString file = side_[s].path.isEmpty() ? QString("(no file)") : QFileInfo(side_[s].path).fileName();
            name_[s]->setText((s == 0 ? "Left: " : "Right: ") + file);
            name_[s]->setToolTip(side_[s].path);
            info_[s]->setText(dirty ? QString("%1 unsaved %2").arg(side_[s].changes).arg(side_[s].changes == 1 ? "change" : "changes")
                                    : QString("No unsaved changes"));
            save_btn_[s]->setEnabled(dirty && !busy_);
            save_btn_[s]->setIcon(icons::saveIcon(dirty && !busy_ ? kBlue : kInk));
            save_btn_[s]->setText(busy_ && savingSide_ == s ? "Saving..." : (s == 0 ? "Save Left" : "Save Right"));
            if (dirty) leftOver << (s == 0 ? "left" : "right");
        }
        // Enter saves the first side that still needs it; Continue is never the default while it would discard work.
        const int first = side_[0].changes > 0 ? 0 : (side_[1].changes > 0 ? 1 : -1);
        for (int s = 0; s < 2; ++s) {
            save_btn_[s]->setAutoDefault(s == first);
            save_btn_[s]->setDefault(s == first);
        }
        cancel_->setEnabled(!busy_);
        continue_->setEnabled(!busy_);
        caption_->setText(leftOver.isEmpty()
                              ? QString("Everything is saved.")
                              : QString("Continue discards the unsaved changes on the %1 side.").arg(leftOver.join(" and ")));
    }

    SaveSideFn save_;
    UnsavedSide side_[2];
    QLabel* name_[2] = {nullptr, nullptr};
    QLabel* info_[2] = {nullptr, nullptr};
    QPushButton* save_btn_[2] = {nullptr, nullptr};
    QLabel* error_ = nullptr;
    QLabel* caption_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* continue_ = nullptr;
    QTimer* watchdog_ = nullptr;
    bool busy_ = false;
    int savingSide_ = -1;
    int token_ = 0;
};

// Returns true when the caller may proceed.
inline bool confirmUnsavedChanges(QWidget* parent, const UnsavedSide& left, const UnsavedSide& right, SaveSideFn save) {
    if (left.changes <= 0 && right.changes <= 0) return true;
    UnsavedChangesDialog dialog(parent, left, right, std::move(save));
    return dialog.exec() == QDialog::Accepted;
}

}  // namespace openbc::app
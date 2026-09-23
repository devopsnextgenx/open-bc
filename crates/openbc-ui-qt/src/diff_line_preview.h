// diff_line_preview.h
// ---------------------------------------------------------------------------
// Bottom "console" strip of TextCompareView: shows the current row's left
// and right line, editable in place via double-click. Extracted unchanged
// from the original monolithic qt_app.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <functional>

#include "diff_algorithms.h"

namespace openbc::app {

// Bottom "console" strip, editable in place: double-clicking the top row's
// text (the left file's current line) lets you type a replacement that is
// written straight back into the left editor's current line; double-clicking
// the bottom row's text edits the right editor's current line the same way.
// Single click / normal display stays plain read-only, same coloring as
// before, so this only changes behaviour once the user explicitly asks to
// edit by double-clicking.
//
// The line number is its own fixed-width, always-read-only label to the left
// of each row's text field - it is metadata about the line, not part of the
// line's content, so it is never selected, edited, or sent back in
// onLeftEdited/onRightEdited. Only the QLineEdit holding the actual text
// becomes editable.
class DiffLinePreview : public QFrame {
public:
    explicit DiffLinePreview(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("diffLinePreview");
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);

        auto makeRow = [this, layout](QLabel*& numberLabel, QLineEdit*& textEdit) {
            auto* row = new QWidget(this);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(0);

            numberLabel = new QLabel(row);
            numberLabel->setObjectName("previewLineNo");
            numberLabel->setFixedHeight(20);
            numberLabel->setMinimumWidth(56);
            numberLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            textEdit = new QLineEdit(row);
            textEdit->setObjectName("previewText");
            textEdit->setFrame(false);
            textEdit->setReadOnly(true);
            textEdit->setFixedHeight(20);
            textEdit->setTextMargins(8, 0, 8, 0);
            textEdit->installEventFilter(this);
            connect(textEdit, &QLineEdit::returnPressed, this,
                    [this, textEdit]() { commitEdit(textEdit); });

            rowLayout->addWidget(numberLabel);
            rowLayout->addWidget(textEdit, 1);
            layout->addWidget(row);
        };
        makeRow(topNumber_, topEdit_);
        makeRow(bottomNumber_, bottomEdit_);
        applyStyle(topEdit_, topNumber_);
        applyStyle(bottomEdit_, bottomNumber_);
    }

    void setRow(const TextDiffLine& row) {
        row_ = row;
        background_ = !row.changed          ? "transparent"
                      : row.whitespaceOnly   ? "#2d5a87"
                                             : "#7a2d2d";
        topNumber_->setText(row.leftNumber ? QString::number(row.leftNumber) : QString());
        bottomNumber_->setText(row.rightNumber ? QString::number(row.rightNumber) : QString());
        topEdit_->setText(row.leftNumber ? row.left : QString());
        bottomEdit_->setText(row.rightNumber ? row.right : QString());
        applyStyle(topEdit_, topNumber_);
        applyStyle(bottomEdit_, bottomNumber_);
    }

    void clear() {
        row_ = TextDiffLine();
        background_ = "transparent";
        topNumber_->clear();
        bottomNumber_->clear();
        topEdit_->clear();
        bottomEdit_->clear();
        applyStyle(topEdit_, topNumber_);
        applyStyle(bottomEdit_, bottomNumber_);
    }

    // Fired once the user finishes editing (Enter, or clicking away). The
    // text passed back is exactly the line's content - no line-number prefix
    // is ever mixed into it, since the number was never part of the edited
    // widget to begin with.
    std::function<void(QString)> onLeftEdited;
    std::function<void(QString)> onRightEdited;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* edit = qobject_cast<QLineEdit*>(watched);
        if (!edit) return QFrame::eventFilter(watched, event);
        if (event->type() == QEvent::MouseButtonDblClick && edit->isReadOnly() &&
            !edit->text().isEmpty()) {
            edit->setReadOnly(false);
            edit->setStyleSheet(edit->styleSheet() + "border:1px solid #6f9fdb;");
            edit->setFocus();
            edit->selectAll();
            return true;
        }
        if (event->type() == QEvent::FocusOut && !edit->isReadOnly()) {
            commitEdit(edit);
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    void applyStyle(QLineEdit* edit, QLabel* numberLabel) {
        edit->setStyleSheet(
            QString("background:%1; color:#f8f8f2; border:1px solid transparent;").arg(background_));
        numberLabel->setStyleSheet(
            QString("background:%1; border:1px solid transparent; border-right:1px solid #3a3a3a;")
                .arg(background_ == "transparent" ? "#1b1b1b" : background_));
    }

    void commitEdit(QLineEdit* edit) {
        if (edit->isReadOnly()) return;
        edit->setReadOnly(true);
        applyStyle(topEdit_ == edit ? topEdit_ : bottomEdit_, topEdit_ == edit ? topNumber_ : bottomNumber_);
        if (edit == topEdit_ && onLeftEdited) {
            onLeftEdited(edit->text());
        } else if (edit == bottomEdit_ && onRightEdited) {
            onRightEdited(edit->text());
        }
    }

    QLabel* topNumber_ = nullptr;
    QLabel* bottomNumber_ = nullptr;
    QLineEdit* topEdit_ = nullptr;
    QLineEdit* bottomEdit_ = nullptr;
    TextDiffLine row_;
    QString background_ = "transparent";
};

}  // namespace openbc::app
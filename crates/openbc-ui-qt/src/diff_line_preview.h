// diff_line_preview.h
// ---------------------------------------------------------------------------
// Bottom "console" strip of TextCompareView: shows the current row's left
// and right line, editable in place via double-click. Extracted unchanged
// from the original monolithic qt_app.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
#include <QFrame>
#include <QLineEdit>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <functional>

#include "diff_algorithms.h"

namespace openbc::app {

// Bottom "console" strip, now editable in place: double-clicking the top row
// (the left file's current line) lets you type a replacement that is written
// straight back into the left editor's current line; double-clicking the
// bottom row edits the right editor's current line the same way. Single
// click / normal display stays a plain read-only line, same coloring as
// before, so this only changes behaviour once the user explicitly asks to
// edit by double-clicking.
class DiffLinePreview : public QFrame {
public:
    explicit DiffLinePreview(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName("diffLinePreview");
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);
        topEdit_ = new QLineEdit(this);
        bottomEdit_ = new QLineEdit(this);
        for (QLineEdit* edit : {topEdit_, bottomEdit_}) {
            edit->setFrame(false);
            edit->setReadOnly(true);
            edit->setFixedHeight(20);
            edit->setTextMargins(8, 0, 8, 0);
            edit->installEventFilter(this);
            connect(edit, &QLineEdit::returnPressed, this, [this, edit]() { commitEdit(edit); });
        }
        layout->addWidget(topEdit_);
        layout->addWidget(bottomEdit_);
        applyStyle(topEdit_);
        applyStyle(bottomEdit_);
    }

    void setRow(const TextDiffLine& row) {
        row_ = row;
        background_ = !row.changed          ? "transparent"
                      : row.whitespaceOnly   ? "#2d5a87"
                                             : "#7a2d2d";
        topEdit_->setText(row.leftNumber ? QString("%1:  %2").arg(row.leftNumber).arg(row.left)
                                         : QString());
        bottomEdit_->setText(row.rightNumber ? QString("%1:  %2").arg(row.rightNumber).arg(row.right)
                                             : QString());
        leftPrefixLength_ = row.leftNumber ? QString("%1:  ").arg(row.leftNumber).length() : 0;
        rightPrefixLength_ = row.rightNumber ? QString("%1:  ").arg(row.rightNumber).length() : 0;
        applyStyle(topEdit_);
        applyStyle(bottomEdit_);
    }

    void clear() {
        row_ = TextDiffLine();
        background_ = "transparent";
        topEdit_->clear();
        bottomEdit_->clear();
        applyStyle(topEdit_);
        applyStyle(bottomEdit_);
    }

    // Fired once the user finishes editing (Enter, or clicking away). The
    // text passed back has the "N:  " line-number prefix already stripped.
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
    void applyStyle(QLineEdit* edit) {
        edit->setStyleSheet(
            QString("background:%1; color:#f8f8f2; border:1px solid transparent;").arg(background_));
    }

    void commitEdit(QLineEdit* edit) {
        if (edit->isReadOnly()) return;
        edit->setReadOnly(true);
        applyStyle(edit);
        const int prefixLength = edit == topEdit_ ? leftPrefixLength_ : rightPrefixLength_;
        const QString newText = edit->text().mid(prefixLength);
        if (edit == topEdit_ && onLeftEdited) {
            onLeftEdited(newText);
        } else if (edit == bottomEdit_ && onRightEdited) {
            onRightEdited(newText);
        }
    }

    QLineEdit* topEdit_ = nullptr;
    QLineEdit* bottomEdit_ = nullptr;
    TextDiffLine row_;
    QString background_ = "transparent";
    int leftPrefixLength_ = 0;
    int rightPrefixLength_ = 0;
};

}  // namespace openbc::app

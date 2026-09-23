// find_replace_bar.h
// ---------------------------------------------------------------------------
// A slim, dismissable find/replace bar for TextCompareView. Ctrl+F (wired by
// TextCompareView) opens and focuses it; Escape closes it. A "Scope" combo
// lets the search run against just the pane that had focus when the bar was
// opened, or "Both panes" at once - so the same bar covers both the
// per-editor and the global (both sides) cases the user asked for, instead
// of needing two separate widgets.
// ---------------------------------------------------------------------------
#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVector>
#include <QWidget>
#include <functional>

namespace openbc::app {

class FindReplaceBar : public QWidget {
public:
    enum class Scope { FocusedPane, BothPanes };

    explicit FindReplaceBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("findReplaceBar");
        setStyleSheet(
            "#findReplaceBar { background:#2b2b2b; border-top:1px solid #505050; }"
            "#findReplaceBar QLineEdit { background:#1e1e1e; color:#f8f8f2; border:1px solid "
            "#505050; border-radius:3px; padding:2px 6px; }"
            "#findReplaceBar QLabel { color:#c7cbe0; }");
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(6);

        findEdit_ = new QLineEdit(this);
        findEdit_->setPlaceholderText("Find");
        findEdit_->setFixedWidth(200);
        replaceEdit_ = new QLineEdit(this);
        replaceEdit_->setPlaceholderText("Replace with");
        replaceEdit_->setFixedWidth(200);

        auto* prev = new QToolButton(this);
        prev->setText(QString::fromUtf8("\u2191"));
        prev->setToolTip("Previous match (Shift+Enter)");
        auto* next = new QToolButton(this);
        next->setText(QString::fromUtf8("\u2193"));
        next->setToolTip("Next match (Enter)");
        auto* replaceOne = new QPushButton("Replace", this);
        auto* replaceAll = new QPushButton("Replace All", this);

        caseSensitive_ = new QCheckBox("Aa", this);
        caseSensitive_->setToolTip("Case sensitive");
        wholeWord_ = new QCheckBox("Word", this);
        wholeWord_->setToolTip("Whole word only");

        scopeCombo_ = new QComboBox(this);
        scopeCombo_->addItem("This pane", static_cast<int>(Scope::FocusedPane));
        scopeCombo_->addItem("Both panes", static_cast<int>(Scope::BothPanes));

        status_ = new QLabel(this);
        status_->setMinimumWidth(90);

        auto* close = new QToolButton(this);
        close->setText(QString::fromUtf8("\u2715"));
        close->setToolTip("Close (Esc)");
        close->setAutoRaise(true);

        layout->addWidget(new QLabel("Find:", this));
        layout->addWidget(findEdit_);
        layout->addWidget(prev);
        layout->addWidget(next);
        layout->addWidget(caseSensitive_);
        layout->addWidget(wholeWord_);
        layout->addSpacing(8);
        layout->addWidget(new QLabel("Replace:", this));
        layout->addWidget(replaceEdit_);
        layout->addWidget(replaceOne);
        layout->addWidget(replaceAll);
        layout->addSpacing(8);
        layout->addWidget(scopeCombo_);
        layout->addWidget(status_, 1);
        layout->addWidget(close);

        connect(findEdit_, &QLineEdit::returnPressed, this, [this]() { findNext(); });
        connect(replaceEdit_, &QLineEdit::returnPressed, this, [this]() { replaceCurrent(); });
        connect(next, &QToolButton::clicked, this, [this]() { findNext(); });
        connect(prev, &QToolButton::clicked, this, [this]() { findNext(/*backward=*/true); });
        connect(replaceOne, &QPushButton::clicked, this, [this]() { replaceCurrent(); });
        connect(replaceAll, &QPushButton::clicked, this, [this]() { replaceAllMatches(); });
        connect(close, &QToolButton::clicked, this, [this]() { hideBar(); });
        connect(findEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
            status_->clear();
        });

        hide();
    }

    // Editors this bar searches. `focused` is whichever editor currently has
    // (or last had) keyboard focus and is what "This pane" scope targets.
    void setEditors(QPlainTextEdit* left, QPlainTextEdit* right) {
        leftEditor_ = left;
        rightEditor_ = right;
    }

    void openFor(QPlainTextEdit* focusedEditor) {
        if (focusedEditor) focused_ = focusedEditor;
        show();
        findEdit_->setFocus();
        findEdit_->selectAll();
        if (!focused_) focused_ = leftEditor_;
    }

    void notifyFocused(QPlainTextEdit* editor) { focused_ = editor; }

    bool isOpen() const { return isVisible(); }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape) {
            hideBar();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            findNext(event->modifiers() & Qt::ShiftModifier);
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    Scope scope() const {
        return static_cast<Scope>(scopeCombo_->currentData().toInt());
    }

    QTextDocument::FindFlags flags(bool backward) const {
        QTextDocument::FindFlags f;
        if (backward) f |= QTextDocument::FindBackward;
        if (caseSensitive_->isChecked()) f |= QTextDocument::FindCaseSensitively;
        if (wholeWord_->isChecked()) f |= QTextDocument::FindWholeWords;
        return f;
    }

    QVector<QPlainTextEdit*> targets() const {
        if (scope() == Scope::BothPanes) return {leftEditor_, rightEditor_};
        return {focused_ ? focused_ : leftEditor_};
    }

    void findNext(bool backward = false) {
        const QString term = findEdit_->text();
        if (term.isEmpty()) return;
        for (QPlainTextEdit* editor : targets()) {
            if (!editor) continue;
            if (editor->find(term, flags(backward))) {
                editor->setFocus();
                status_->setText("Found");
                return;
            }
            // Wrap around: retry from the top/bottom of this editor once.
            QTextCursor cursor = editor->textCursor();
            cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
            editor->setTextCursor(cursor);
            if (editor->find(term, flags(backward))) {
                editor->setFocus();
                status_->setText("Wrapped");
                return;
            }
        }
        status_->setText("Not found");
    }

    void replaceCurrent() {
        for (QPlainTextEdit* editor : targets()) {
            if (!editor) continue;
            QTextCursor cursor = editor->textCursor();
            if (cursor.hasSelection() &&
                cursor.selectedText().compare(
                    findEdit_->text(),
                    caseSensitive_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive) == 0) {
                cursor.insertText(replaceEdit_->text());
            }
        }
        findNext();
    }

    void replaceAllMatches() {
        const QString term = findEdit_->text();
        const QString replacement = replaceEdit_->text();
        if (term.isEmpty()) return;
        int total = 0;
        for (QPlainTextEdit* editor : targets()) {
            if (!editor) continue;
            QTextCursor cursor(editor->document());
            cursor.movePosition(QTextCursor::Start);
            editor->setTextCursor(cursor);
            while (editor->find(term, flags(false))) {
                QTextCursor match = editor->textCursor();
                match.insertText(replacement);
                editor->setTextCursor(match);
                ++total;
            }
        }
        status_->setText(QString("Replaced %1").arg(total));
    }

    void hideBar() {
        hide();
        if (focused_) focused_->setFocus();
    }

    QLineEdit* findEdit_ = nullptr;
    QLineEdit* replaceEdit_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QCheckBox* wholeWord_ = nullptr;
    QComboBox* scopeCombo_ = nullptr;
    QLabel* status_ = nullptr;

    QPlainTextEdit* leftEditor_ = nullptr;
    QPlainTextEdit* rightEditor_ = nullptr;
    QPlainTextEdit* focused_ = nullptr;
};

}  // namespace openbc::app

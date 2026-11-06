// plain_text_editor_adapter.h
// ---------------------------------------------------------------------------
// Adapts the EXISTING editor (a QPlainTextEdit: LineNumberEditor) to ITextCompareEditor, so callers
// can drive either implementation through one interface (composition, not inheritance: the two
// editors have unrelated Qt base classes).
//
// The existing editor owns its text inside a QTextDocument, so the TextModel is only remembered
// (for line <-> row mapping); diff navigation is supplied by the owning viewer, which already knows
// its own difference list.
// ---------------------------------------------------------------------------
#pragma once

#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <functional>

#include "viewers/text/text_compare_editor.h"

namespace openbc::app {

class PlainTextEditorAdapter : public ITextCompareEditor {
public:
    explicit PlainTextEditorAdapter(QPlainTextEdit* editor) : edit_(editor) {}

    // Supplied by the viewer: next/previous difference start rows (0-based document blocks).
    std::function<std::optional<quint64>(quint64 fromRow)> nextDifferenceRow;
    std::function<std::optional<quint64>(quint64 fromRow)> prevDifferenceRow;
    // Optional: one-based file line -> document block. Defaults to the model's alignment when set,
    // otherwise block == line - 1 (correct when the document has no filler rows).
    std::function<std::optional<quint64>(quint64 line1)> rowForLine;

    void setModel(std::shared_ptr<TextModel> model, Side side) override {
        model_ = std::move(model);
        side_ = side;
    }
    void setReadOnly(bool readOnly) override { edit_->setReadOnly(readOnly); }
    bool isReadOnly() const override { return edit_->isReadOnly(); }

    void goToLine(std::uint64_t line) override {
        if (line == 0) return;
        std::optional<quint64> row;
        if (rowForLine) row = rowForLine(line);
        else if (model_) row = model_->rowForLine(side_, line - 1);
        else row = line - 1;
        if (!row) return;
        const QTextBlock block = edit_->document()->findBlockByNumber(static_cast<int>(qMin<quint64>(*row, INT_MAX)));
        if (!block.isValid()) return;
        QTextCursor cursor(block);
        edit_->setTextCursor(cursor);
        edit_->centerCursor();
    }

    void goToNextDifference() override { jump(nextDifferenceRow); }
    void goToPreviousDifference() override { jump(prevDifferenceRow); }

    bool findText(const QString& text, bool caseSensitive, bool forward) override {
        if (text.isEmpty()) return false;
        QTextDocument::FindFlags flags;
        if (caseSensitive) flags |= QTextDocument::FindCaseSensitively;
        if (!forward) flags |= QTextDocument::FindBackward;
        if (edit_->find(text, flags)) return true;
        // Wrap around, like the virtual editor.
        QTextCursor cursor = edit_->textCursor();
        cursor.movePosition(forward ? QTextCursor::Start : QTextCursor::End);
        edit_->setTextCursor(cursor);
        return edit_->find(text, flags);
    }

    void setEditorFont(const QFont& font) override { edit_->setFont(font); }
    QString selectedText() const override {
        // QTextCursor uses U+2029 for paragraph breaks.
        return edit_->textCursor().selectedText().replace(QChar(0x2029), QLatin1Char('\n'));
    }
    void copySelection() override { edit_->copy(); }
    void selectAllText() override { edit_->selectAll(); }
    std::uint64_t currentRow() const override { return static_cast<std::uint64_t>(edit_->textCursor().blockNumber()); }
    std::uint64_t firstVisibleRow() const override {
        // firstVisibleBlock() is protected; the block under the viewport's top-left corner is public API.
        return static_cast<std::uint64_t>(edit_->cursorForPosition(QPoint(0, 0)).blockNumber());
    }
    void setFirstVisibleRow(std::uint64_t row) override { edit_->verticalScrollBar()->setValue(static_cast<int>(qMin<quint64>(row, INT_MAX))); }
    QWidget* widget() override { return edit_; }

private:
    void jump(const std::function<std::optional<quint64>(quint64)>& fn) {
        if (!fn) return;
        if (const auto row = fn(currentRow())) {
            const QTextBlock block = edit_->document()->findBlockByNumber(static_cast<int>(qMin<quint64>(*row, INT_MAX)));
            if (!block.isValid()) return;
            edit_->setTextCursor(QTextCursor(block));
            edit_->centerCursor();
        }
    }

    QPlainTextEdit* edit_;
    std::shared_ptr<TextModel> model_;
    Side side_ = Side::Left;
};

}  // namespace openbc::app

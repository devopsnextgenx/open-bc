// text_compare_editor.h
// ---------------------------------------------------------------------------
// The surface the rest of the application uses to drive a text-compare editor, independent of
// which implementation is behind it:
//
//   * VirtualTextEditor        (QAbstractScrollArea over the engine; viewers/text/virtual/)
//   * PlainTextEditorAdapter   (wraps the existing LineNumberEditor / QPlainTextEdit)
//
// It is deliberately NOT a QWidget: the two editors have unrelated Qt base classes, so the
// interface is implemented by composition / multiple inheritance and exposes its widget through
// widget(). Positions are zero-based "display rows" unless stated otherwise; `goToLine` is the
// one-based line number of this editor's own file, like the editor's gutter shows.
// ---------------------------------------------------------------------------
#pragma once

#include <QFont>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>

#include "viewers/text/engine/text_model.h"

namespace openbc::app {

class ITextCompareEditor {
public:
    virtual ~ITextCompareEditor() = default;

    // Attach the comparison model and choose which file of the pair this editor shows. Editors
    // that own their text (QPlainTextEdit) may ignore the model and only remember it.
    virtual void setModel(std::shared_ptr<TextModel> model, Side side) = 0;

    virtual void setReadOnly(bool readOnly) = 0;
    virtual bool isReadOnly() const = 0;

    // One-based line number of this editor's file.
    virtual void goToLine(std::uint64_t line) = 0;
    virtual void goToNextDifference() = 0;
    virtual void goToPreviousDifference() = 0;

    // Finds the next/previous occurrence starting at the cursor and selects it. Returns false when
    // there is no (further) match.
    virtual bool findText(const QString& text, bool caseSensitive, bool forward) = 0;

    virtual void setEditorFont(const QFont& font) = 0;
    virtual QString selectedText() const = 0;
    virtual void copySelection() = 0;
    virtual void selectAllText() = 0;

    // Display row (0-based) of the caret / first visible row.
    virtual std::uint64_t currentRow() const = 0;
    virtual std::uint64_t firstVisibleRow() const = 0;
    virtual void setFirstVisibleRow(std::uint64_t row) = 0;

    virtual QWidget* widget() = 0;
};

}  // namespace openbc::app

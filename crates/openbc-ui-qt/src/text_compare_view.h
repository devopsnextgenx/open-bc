// text_compare_view.h
// ---------------------------------------------------------------------------
// TextCompareView: the two-pane diff editor tab (toolbar, minimap, the two
// LineNumberEditor panes, the bottom DiffLinePreview console, and now the
// FindReplaceBar). This is the file that wires together everything the
// other new headers provide:
//   - computeInlineDiff() results feed both editors' whitespace-glyph
//     overlay (LineNumberEditor::setInlineDiffs) and each side's
//     SyntaxHighlighter (red/whitespace diff colouring).
//   - SyntaxHighlighter also gets a language selected from the file
//     extension via syntax::forExtension().
//   - FindReplaceBar is created once, hidden until Ctrl+F/the toolbar
//     button opens it.
//   - editMenuItems()/searchMenuItems()/viewMenuItems()/actionsMenuItems()
//     expose this view's QActions so main_window.h's rebuildMenus() can
//     populate the top menu bar for a text-compare tab, not just a
//     folder-compare (CompareSession) tab.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <functional>

#include "diff_algorithms.h"
#include "diff_line_preview.h"
#include "find_replace_bar.h"
#include "line_number_editor.h"
#include "minimap.h"
#include "qt_style.h"
#include "syntax_highlighter.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

class TextCompareView : public QWidget {
public:
    TextCompareView(const QString& leftPath, const QString& rightPath,
                    const QString& leftText, const QString& rightText, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          leftOriginal_(leftText.split('\n')), rightOriginal_(rightText.split('\n')) {
        setObjectName("textCompareView");
        buildUi();
        connectSignals();
        rebuild();
    }

    QString title() const {
        return QFileInfo(leftPath_).fileName() + " <-> " + QFileInfo(rightPath_).fileName();
    }

    // Wired by main() so the toolbar's Home / Sessions buttons behave like
    // Beyond Compare's: both return the user to the session (folder-compare)
    // tab this text view was opened from.
    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    // Menu item accessors - main_window.h's rebuildMenus() reads these
    // when this view is the active tab, the same way it already reads
    // CompareSession's for a folder-compare tab.
    QList<QAction*> editMenuItems() const { return {undoAction_, redoAction_, saveAction_}; }
    QList<QAction*> searchMenuItems() const { return {findAction_, nextSectionAction_, prevSectionAction_}; }
    QList<QAction*> viewMenuItems() const {
        return {showAllAction_, showDiffsAction_, contextAction_, minorAction_,
                pixelMinimapAction_, formatAction_};
    }
    QList<QAction*> actionsMenuItems() const { return {copyAction_, swapAction_, reloadAction_}; }

private:
    // Whichever editor pane last had keyboard focus - used as the default
    // target for Ctrl+Z/Ctrl+Y and for the find bar's "This pane" scope.
    LineNumberEditor* focusedEditor() const {
        return (rightEditor_ && rightEditor_->hasFocus()) ? rightEditor_ : leftEditor_;
    }

    // Picks each side's syntax-highlighting language from its own file
    // extension (so comparing a .cpp to a .h, say, highlights each pane
    // correctly rather than forcing both to match).
    void applyLanguageHighlighting() {
        leftHighlighter_->setLanguage(syntax::forExtension(QFileInfo(leftPath_).suffix()));
        rightHighlighter_->setLanguage(syntax::forExtension(QFileInfo(rightPath_).suffix()));
    }

    // Char-level diff (red mismatches / tinted+glyphed whitespace) for every
    // row that has real content on both sides; pure insert/delete rows (one
    // side missing entirely) are left empty here since the hashed-background
    // ExtraSelection already marks those as "no counterpart", not "differs".
    void computeInlineDiffs(const QVector<TextDiffLine>& rows) {
        leftInline_.clear();
        rightInline_.clear();
        leftInline_.reserve(rows.size());
        rightInline_.reserve(rows.size());
        for (const TextDiffLine& row : rows) {
            if (row.changed && row.leftNumber > 0 && row.rightNumber > 0) {
                const InlineDiff diff = computeInlineDiff(row.left, row.right);
                leftInline_.push_back(diff.left);
                rightInline_.push_back(diff.right);
            } else {
                leftInline_.push_back({});
                rightInline_.push_back({});
            }
        }
    }

    void buildUi() {
        using icons::Glyph;
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        // ---- toolbar: mirrors Beyond Compare's Text Compare toolbar ----
        toolbar_ = new QToolBar(this);
        toolbar_->setObjectName("textCompareToolbar");
        toolbar_->setMovable(false);
        toolbar_->setFloatable(false);
        toolbar_->setIconSize(QSize(20, 20));
        toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

        homeAction_ = toolbar_->addAction(icons::glyph(Glyph::Home), "Home");
        sessionsAction_ = toolbar_->addAction(icons::glyph(Glyph::Sessions), "Sessions");
        toolbar_->addSeparator();

        auto* viewGroup = new QActionGroup(this);
        viewGroup->setExclusive(true);
        showAllAction_ = toolbar_->addAction(icons::glyph(Glyph::ShowAll), "All");
        showDiffsAction_ = toolbar_->addAction(icons::glyph(Glyph::ShowDiffs), "Diffs");
        contextAction_ = toolbar_->addAction(icons::glyph(Glyph::Context), "Context");
        for (QAction* action : {showAllAction_, showDiffsAction_, contextAction_}) {
            action->setCheckable(true);
            viewGroup->addAction(action);
        }
        contextAction_->setChecked(true);
        toolbar_->addSeparator();

        minorAction_ = toolbar_->addAction(icons::glyph(Glyph::Minor), "Minor");
        minorAction_->setCheckable(true);
        minorAction_->setToolTip("Treat whitespace-only differences as unimportant");
        rulesAction_ = toolbar_->addAction(icons::glyph(Glyph::Rules), "Rules");
        rulesAction_->setCheckable(true);
        formatAction_ = toolbar_->addAction(icons::glyph(Glyph::Format), "Format");
        formatAction_->setCheckable(true);
        formatAction_->setToolTip("Wrap long lines");
        toolbar_->addSeparator();

        pixelMinimapAction_ = toolbar_->addAction(icons::glyph(Glyph::MinimapPixel), "Minimap");
        pixelMinimapAction_->setCheckable(true);
        pixelMinimapAction_->setToolTip(
            "Minimap: one pixel-thin line per differing row, instead of one bar per change block");
        findAction_ = toolbar_->addAction(icons::glyph(Glyph::Find), "Find");
        findAction_->setShortcut(QKeySequence::Find);
        findAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        findAction_->setToolTip("Find / Replace (Ctrl+F)");
        // WidgetWithChildrenShortcut fires when the associated widget or one
        // of its *children* has focus; the action's owner (toolbar_) is a
        // sibling of the editors, not their parent, so it must also be
        // registered on `this` (matching saveAction_ below) or Ctrl+F would
        // only fire while the toolbar itself had focus.
        addAction(findAction_);
        toolbar_->addSeparator();

        copyAction_ = toolbar_->addAction(icons::glyph(Glyph::CopyLine), "Copy");
        copyAction_->setToolTip("Copy the current change to the other side");
        toolbar_->addSeparator();

        nextSectionAction_ = toolbar_->addAction(icons::glyph(Glyph::NextSection), "Next Section");
        prevSectionAction_ = toolbar_->addAction(icons::glyph(Glyph::PrevSection), "Prev Section");
        toolbar_->addSeparator();

        swapAction_ = toolbar_->addAction(icons::glyph(Glyph::Swap), "Swap");
        reloadAction_ = toolbar_->addAction(icons::glyph(Glyph::Refresh), "Reload");
        root->addWidget(toolbar_);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(8, 5, 8, 5);
        leftLabel_ = new QLabel(QFileInfo(leftPath_).fileName(), this);
        rightLabel_ = new QLabel(QFileInfo(rightPath_).fileName(), this);
        leftLabel_->setStyleSheet("font-weight:700; color:#f8f8f2;");
        rightLabel_->setStyleSheet("font-weight:700; color:#f8f8f2;");
        header->addWidget(leftLabel_, 1);
        header->addWidget(rightLabel_, 1);
        root->addLayout(header);

        auto* editors = new QSplitter(Qt::Horizontal, this);
        editors->setChildrenCollapsible(false);

        auto* leftPane = new QWidget(editors);
        auto* leftLayout = new QHBoxLayout(leftPane);
        leftLayout->setContentsMargins(0, 0, 0, 0);
        leftLayout->setSpacing(0);
        miniMap_ = new DifferenceOverview(leftPane, true);
        // Left editor: numbers on the outer (left) edge, arrows on the inner
        // edge next to the splitter, pointing right toward the other file.
        leftEditor_ = new LineNumberEditor(LineNumberEditor::GutterSide::Left,
                                           LineNumberEditor::GutterSide::Right, leftPane);
        leftLayout->addWidget(miniMap_);
        leftLayout->addWidget(leftEditor_, 1);

        auto* rightPane = new QWidget(editors);
        auto* rightLayout = new QHBoxLayout(rightPane);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);
        // Right editor: numbers on the outer (right) edge, arrows on the
        // inner edge next to the splitter, pointing left.
        rightEditor_ = new LineNumberEditor(LineNumberEditor::GutterSide::Right,
                                            LineNumberEditor::GutterSide::Left, rightPane);
        rightLayout->addWidget(rightEditor_, 1);

        // One QSyntaxHighlighter per editor: it carries both the language
        // syntax colouring (picked from the file extension, below) and the
        // char-level diff overlay (red mismatches / tinted whitespace) fed
        // to it from rebuild() - Qt does not support layering two
        // independent highlighters on the same document, so both concerns
        // share this one highlighter per side.
        leftHighlighter_ = new SyntaxHighlighter(leftEditor_->document());
        rightHighlighter_ = new SyntaxHighlighter(rightEditor_->document());
        leftHighlighter_->setInlineDiffProvider([this](int block) {
            return block >= 0 && block < leftInline_.size() ? leftInline_[block]
                                                             : QVector<CharSegment>();
        });
        rightHighlighter_->setInlineDiffProvider([this](int block) {
            return block >= 0 && block < rightInline_.size() ? rightInline_[block]
                                                              : QVector<CharSegment>();
        });
        applyLanguageHighlighting();

        editors->addWidget(leftPane);
        editors->addWidget(rightPane);
        editors->setStretchFactor(0, 1);
        editors->setStretchFactor(1, 1);
        root->addWidget(editors, 1);

        // Find/Replace bar: hidden until Ctrl+F (or the toolbar's Find
        // button) opens it; see FindReplaceBar for the per-pane/both-panes
        // scope selector.
        findBar_ = new FindReplaceBar(this);
        findBar_->setEditors(leftEditor_, rightEditor_);
        root->addWidget(findBar_);

        preview_ = new DiffLinePreview(this);
        root->addWidget(preview_);

        status_ = new QLabel(this);
        status_->setStyleSheet("background:#3e3e3e; border-top:1px solid #505050; padding:3px 8px;");
        root->addWidget(status_);

        // Right-click context menu on each editor pane (Align Manually,
        // Isolate, Copy to Other Side, Edit Line, indent, Compare to
        // Clipboard, plus the usual clipboard/Open With actions).
        leftEditor_->setContextMenuPolicy(Qt::CustomContextMenu);
        rightEditor_->setContextMenuPolicy(Qt::CustomContextMenu);

        // Ctrl+S saves whichever editor pane currently has keyboard focus,
        // to its own file on disk - not both sides at once, so the user
        // always saves the side they are actually looking at/editing.
        saveAction_ = new QAction(this);
        saveAction_->setShortcut(QKeySequence::Save);
        saveAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(saveAction_);

        // Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) undo/redo whichever pane has
        // focus. QPlainTextEdit already binds these itself, but declaring
        // them explicitly here means they also show up in the Edit menu
        // (editMenuItems()) and keeps working even if a future ancestor
        // widget installs its own shortcut on the same key sequence.
        undoAction_ = new QAction("Undo", this);
        undoAction_->setShortcut(QKeySequence::Undo);
        undoAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        redoAction_ = new QAction("Redo", this);
        redoAction_->setShortcut(QKeySequence::Redo);
        redoAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(undoAction_);
        addAction(redoAction_);
    }

    void connectSignals() {
        connect(leftEditor_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int value) {
                    if (!syncing_) {
                        syncing_ = true;
                        rightEditor_->verticalScrollBar()->setValue(value);
                        syncing_ = false;
                    }
                    updateMinimapViewport();
                });
        connect(rightEditor_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this](int value) {
                    if (!syncing_) {
                        syncing_ = true;
                        leftEditor_->verticalScrollBar()->setValue(value);
                        syncing_ = false;
                    }
                    updateMinimapViewport();
                });
        // Both editors' rows line up (filler blanks keep them aligned), so
        // whichever side the caret moves in, mirror the same row number as
        // the "current line" on both panes - VS Code style highlight, kept
        // in sync across the split the way the minimap/preview already are.
        const auto updateCurrentRow = [this](const QTextCursor& cursor) {
            currentRow_ = cursor.blockNumber();
            miniMap_->setCurrentRow(currentRow_);
            if (currentRow_ >= 0 && currentRow_ < rows_.size()) {
                preview_->setRow(rows_[currentRow_]);
            }
            updateExtraSelections();
        };
        connect(leftEditor_, &QPlainTextEdit::cursorPositionChanged, this,
                [this, updateCurrentRow]() { updateCurrentRow(leftEditor_->textCursor()); });
        connect(rightEditor_, &QPlainTextEdit::cursorPositionChanged, this,
                [this, updateCurrentRow]() { updateCurrentRow(rightEditor_->textCursor()); });
        miniMap_->onRowClicked = [this](int row) { jumpToRow(row); };
        leftEditor_->onArrowClicked = [this](int start, int end) { copyGroup(start, end, true); };
        rightEditor_->onArrowClicked = [this](int start, int end) { copyGroup(start, end, false); };

        connect(findAction_, &QAction::triggered, this,
                [this]() { findBar_->openFor(focusedEditor()); });
        connect(pixelMinimapAction_, &QAction::toggled, this,
                [this](bool on) { miniMap_->setPixelLineMode(on); });
        connect(undoAction_, &QAction::triggered, this, [this]() { focusedEditor()->undo(); });
        connect(redoAction_, &QAction::triggered, this, [this]() { focusedEditor()->redo(); });
        // Keeps the find bar's "This pane" scope pointed at whichever
        // editor the user actually last clicked/typed into.
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
            if (now == leftEditor_ || now == leftEditor_->viewport()) {
                findBar_->notifyFocused(leftEditor_);
            } else if (now == rightEditor_ || now == rightEditor_->viewport()) {
                findBar_->notifyFocused(rightEditor_);
            }
        });

        // Track structural edits (a newline typed or removed) separately per
        // side: those change how many blocks exist, so the row <-> block
        // mapping the diff arrows/highlights rely on is no longer valid
        // until the next Reload. In-place edits that don't add or remove a
        // line keep the mapping intact and stay fully interactive.
        connect(leftEditor_->document(), &QTextDocument::blockCountChanged, this,
                [this](int count) {
                    if (applyingProgrammaticUpdate_ || count == rows_.size()) return;
                    leftStructurallyEdited_ = true;
                    leftEditor_->setDiffData({}, {});
                });
        connect(rightEditor_->document(), &QTextDocument::blockCountChanged, this,
                [this](int count) {
                    if (applyingProgrammaticUpdate_ || count == rows_.size()) return;
                    rightStructurallyEdited_ = true;
                    rightEditor_->setDiffData({}, {});
                });

        connect(homeAction_, &QAction::triggered, this,
                [this]() { if (onHomeRequested) onHomeRequested(); });
        connect(sessionsAction_, &QAction::triggered, this,
                [this]() { if (onSessionsRequested) onSessionsRequested(); });
        connect(showAllAction_, &QAction::triggered, this, [this]() { rebuild(); });
        connect(showDiffsAction_, &QAction::triggered, this, [this]() { rebuild(); });
        connect(contextAction_, &QAction::triggered, this, [this]() { rebuild(); });
        connect(minorAction_, &QAction::toggled, this, [this](bool) { rebuild(); });
        connect(formatAction_, &QAction::toggled, this, [this](bool checked) {
            const auto mode = checked ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap;
            leftEditor_->setLineWrapMode(mode);
            rightEditor_->setLineWrapMode(mode);
        });
        connect(copyAction_, &QAction::triggered, this, [this]() {
            const bool fromLeft = !rightEditor_->hasFocus();
            const int row = (fromLeft ? leftEditor_ : rightEditor_)->textCursor().blockNumber();
            const DiffGroup* group = groupContainingRow(row);
            if (group) copyGroup(group->start, group->end, fromLeft);
        });
        connect(nextSectionAction_, &QAction::triggered, this, [this]() { jumpToSection(1); });
        connect(prevSectionAction_, &QAction::triggered, this, [this]() { jumpToSection(-1); });
        connect(swapAction_, &QAction::triggered, this, [this]() {
            syncFromEditors();
            std::swap(leftOriginal_, rightOriginal_);
            std::swap(leftPath_, rightPath_);
            std::swap(leftDirtyLines_, rightDirtyLines_);
            leftLabel_->setText(QFileInfo(leftPath_).fileName());
            rightLabel_->setText(QFileInfo(rightPath_).fileName());
            applyLanguageHighlighting();
            if (onTitleChanged) onTitleChanged();
            rebuild();
        });
        connect(reloadAction_, &QAction::triggered, this, [this]() {
            syncFromEditors();
            rebuild();
        });

        connect(leftEditor_, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) { showEditorContextMenu(leftEditor_, pos, true); });
        connect(rightEditor_, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) { showEditorContextMenu(rightEditor_, pos, false); });
        connect(saveAction_, &QAction::triggered, this, [this]() { saveFocusedSide(); });

        // Bottom preview strip: top row edits the left editor's current
        // line, bottom row edits the right editor's - wired straight
        // through to whichever row the caret is currently on.
        preview_->onLeftEdited = [this](const QString& text) { applyPreviewEdit(true, text); };
        preview_->onRightEdited = [this](const QString& text) { applyPreviewEdit(false, text); };
    }

    static QTextCursor cursorAtRow(QPlainTextEdit* editor, int row) {
        return QTextCursor(editor->document()->findBlockByNumber(row));
    }

    void jumpToRow(int row) {
        leftEditor_->setTextCursor(cursorAtRow(leftEditor_, row));
        rightEditor_->setTextCursor(cursorAtRow(rightEditor_, row));
    }

    void jumpToSection(int direction) {
        if (groups_.isEmpty()) return;
        const int cursorRow = leftEditor_->textCursor().blockNumber();
        if (direction > 0) {
            for (const auto& group : groups_) {
                if (group.start > cursorRow) {
                    jumpToRow(group.start);
                    return;
                }
            }
        } else {
            for (auto it = groups_.crbegin(); it != groups_.crend(); ++it) {
                if (it->start < cursorRow) {
                    jumpToRow(it->start);
                    return;
                }
            }
        }
    }

    const DiffGroup* groupContainingRow(int row) const {
        for (const auto& group : groups_) {
            if (row >= group.start && row <= group.end) return &group;
        }
        return nullptr;
    }

    // Right-click menu for an editor pane, matching Beyond Compare's text
    // compare context menu: alignment/section actions on top, line-editing
    // actions in the middle, then the usual clipboard actions and Open With.
    void showEditorContextMenu(LineNumberEditor* editor, const QPoint& pos, bool isLeft) {
        // `pos` arrives in the editor widget's own coordinates (that is what
        // customContextMenuRequested delivers); cursorForPosition and the
        // global-position lookup both need viewport coordinates instead,
        // since the gutters shift the viewport away from (0, 0).
        const QPoint viewportPos = editor->viewport()->mapFrom(editor, pos);
        const int row = editor->cursorForPosition(viewportPos).blockNumber();
        const DiffGroup* group = groupContainingRow(row);

        QMenu menu(this);
        QAction* alignManually = menu.addAction("Align Manually");
        QAction* isolate = menu.addAction("Isolate");
        QAction* copyToOther = menu.addAction(isLeft ? "Copy to Right" : "Copy to Left");
        copyToOther->setEnabled(group != nullptr);
        menu.addSeparator();
        QAction* editLine = menu.addAction("Edit Line");
        QAction* insertBlank = menu.addAction("Insert Blank Line");
        QAction* increaseIndent = menu.addAction("Increase Indent");
        QAction* decreaseIndent = menu.addAction("Decrease Indent");
        menu.addSeparator();
        QAction* compareClipboard = menu.addAction("Compare to Clipboard");
        menu.addSeparator();
        QAction* cutAction = menu.addAction("Cut");
        QAction* copyAction = menu.addAction("Copy");
        QAction* pasteAction = menu.addAction("Paste");
        QAction* deleteAction = menu.addAction("Delete");
        const bool hasSelection = editor->textCursor().hasSelection();
        cutAction->setEnabled(hasSelection);
        copyAction->setEnabled(hasSelection);
        deleteAction->setEnabled(hasSelection);
        menu.addSeparator();
        QMenu* openWith = menu.addMenu("Open With");
        QAction* openDefault = openWith->addAction("Default Application");
        QAction* openChoose = openWith->addAction("Choose Program...");

        QAction* chosen = menu.exec(editor->viewport()->mapToGlobal(viewportPos));
        if (!chosen) return;

        if (chosen == alignManually) {
            status_->setText("Align Manually: pick the matching line on the other side.");
        } else if (chosen == isolate) {
            status_->setText("Isolate: showing only this difference section.");
            if (!showDiffsAction_->isChecked() && !contextAction_->isChecked()) {
                showDiffsAction_->setChecked(true);
            } else {
                contextAction_->setChecked(true);
            }
            rebuild();
        } else if (chosen == copyToOther && group) {
            copyGroup(group->start, group->end, isLeft);
        } else if (chosen == editLine) {
            editor->setTextCursor(cursorAtRow(editor, row));
            editor->setFocus();
        } else if (chosen == insertBlank) {
            QTextCursor cursor = cursorAtRow(editor, row);
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.insertBlock();
        } else if (chosen == increaseIndent) {
            indentRow(editor, row, true);
        } else if (chosen == decreaseIndent) {
            indentRow(editor, row, false);
        } else if (chosen == compareClipboard) {
            compareToClipboard(isLeft);
        } else if (chosen == cutAction) {
            editor->cut();
        } else if (chosen == copyAction) {
            editor->copy();
        } else if (chosen == pasteAction) {
            editor->paste();
        } else if (chosen == deleteAction) {
            QTextCursor cursor = editor->textCursor();
            if (cursor.hasSelection()) cursor.removeSelectedText();
        } else if (chosen == openDefault || chosen == openChoose) {
            status_->setText("Open With: " + (isLeft ? leftPath_ : rightPath_));
        }
    }

    static void indentRow(LineNumberEditor* editor, int row, bool increase) {
        QTextCursor cursor = cursorAtRow(editor, row);
        cursor.movePosition(QTextCursor::StartOfBlock);
        if (increase) {
            cursor.insertText("    ");
            return;
        }
        for (int i = 0; i < 4; ++i) {
            QTextCursor probe = cursorAtRow(editor, row);
            probe.movePosition(QTextCursor::StartOfBlock);
            probe.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 1);
            if (probe.selectedText() == " " || probe.selectedText() == "\t") {
                probe.removeSelectedText();
            } else {
                break;
            }
        }
    }

    void compareToClipboard(bool isLeft) {
        const QString clipboard = QApplication::clipboard()->text();
        if (clipboard.isEmpty()) {
            status_->setText("Clipboard is empty - nothing to compare.");
            return;
        }
        const QStringList clipboardLines = clipboard.split('\n');
        const QStringList& fileLines = isLeft ? leftOriginal_ : rightOriginal_;
        const QVector<TextDiffLine> comparison = alignTextLines(fileLines, clipboardLines);
        int differences = 0;
        for (const auto& line : comparison) {
            if (line.changed) ++differences;
        }
        status_->setText(QString("Compare to Clipboard: %1 differing line(s) against the %2 pane.")
                             .arg(differences)
                             .arg(isLeft ? "left" : "right"));
    }

    // Pulls whatever the user has actually typed back into the plain
    // left/right line arrays so Reload, Swap and group-copy all operate on
    // current content rather than the text captured when the tab opened.
    // Rows still in sync with the editors (no lines added/removed) are read
    // back precisely, filler alignment rows included; once a side has had a
    // structural edit (a newline typed or deleted) its row mapping can no
    // longer be trusted, so that side falls back to the editor's raw text.
    void syncFromEditors() {
        if (!leftStructurallyEdited_) {
            for (int i = 0; i < rows_.size() && i < leftEditor_->document()->blockCount(); ++i) {
                rows_[i].left = leftEditor_->document()->findBlockByNumber(i).text();
            }
            leftOriginal_ = materializeSide(true);
        } else {
            leftOriginal_ = leftEditor_->toPlainText().split('\n');
        }
        if (!rightStructurallyEdited_) {
            for (int i = 0; i < rows_.size() && i < rightEditor_->document()->blockCount(); ++i) {
                rows_[i].right = rightEditor_->document()->findBlockByNumber(i).text();
            }
            rightOriginal_ = materializeSide(false);
        } else {
            rightOriginal_ = rightEditor_->toPlainText().split('\n');
        }
    }

    // A row belongs in the real file if it was a real line to begin with, or
    // if the user typed something into what used to be an alignment filler
    // (which makes it a genuine new line). Untouched filler rows are dropped.
    QStringList materializeSide(bool left) const {
        QStringList result;
        for (const auto& row : rows_) {
            const int number = left ? row.leftNumber : row.rightNumber;
            const QString& text = left ? row.left : row.right;
            if (number > 0 || !text.isEmpty()) result << text;
        }
        return result;
    }

    // Copies every row in [startRow, endRow] to the other side in one shot.
    // `row` indices are into rows_ (the currently displayed/filtered list);
    // the edit itself targets the untouched original line arrays so a
    // subsequent rebuild() re-diffs cleanly. `shift` accounts for lines this
    // same call has already inserted earlier in the loop. Every line the
    // copy lands on is marked dirty on the destination side (a "needs
    // saving" flag independent of the diff itself) so it can be picked out
    // with a green highlight until that side is saved with Ctrl+S.
    void copyGroup(int startRow, int endRow, bool leftToRight) {
        syncFromEditors();
        startRow = qMax(0, startRow);
        endRow = qMin(rows_.size() - 1, endRow);
        if (startRow > endRow) return;
        int shift = 0;
        for (int row = startRow; row <= endRow; ++row) {
            const TextDiffLine& copied = rows_[row];
            if (leftToRight) {
                if (copied.leftNumber == 0) continue;
                if (copied.rightNumber > 0) {
                    const int lineNumber = copied.rightNumber + shift;
                    rightOriginal_[lineNumber - 1] = copied.left;
                    rightDirtyLines_.insert(lineNumber);
                } else {
                    const int insertAt = insertionIndex(row, /*forRight=*/true) + shift;
                    rightOriginal_.insert(insertAt, copied.left);
                    markDirtyForInsert(rightDirtyLines_, insertAt + 1);
                    ++shift;
                }
            } else {
                if (copied.rightNumber == 0) continue;
                if (copied.leftNumber > 0) {
                    const int lineNumber = copied.leftNumber + shift;
                    leftOriginal_[lineNumber - 1] = copied.right;
                    leftDirtyLines_.insert(lineNumber);
                } else {
                    const int insertAt = insertionIndex(row, /*forRight=*/false) + shift;
                    leftOriginal_.insert(insertAt, copied.right);
                    markDirtyForInsert(leftDirtyLines_, insertAt + 1);
                    ++shift;
                }
            }
        }
        rebuild();
    }

    // Shifts every previously-recorded dirty (1-based) line number at or
    // after `insertedAt1Based` up by one to account for a freshly-inserted
    // line, then marks the new line itself dirty too.
    static void markDirtyForInsert(QSet<int>& dirty, int insertedAt1Based) {
        QSet<int> shifted;
        for (int number : dirty) {
            shifted.insert(number >= insertedAt1Based ? number + 1 : number);
        }
        shifted.insert(insertedAt1Based);
        dirty = shifted;
    }

    // Position to insert a brand-new line at, found by walking back to the
    // closest earlier row that still has a real line number on that side.
    int insertionIndex(int fromRow, bool forRight) const {
        for (int i = fromRow - 1; i >= 0; --i) {
            const int number = forRight ? rows_[i].rightNumber : rows_[i].leftNumber;
            if (number > 0) return number;
        }
        return 0;
    }

    QVector<TextDiffLine> visibleRows(const QVector<TextDiffLine>& all) const {
        if (showAllAction_->isChecked()) {
            return all;
        }
        QVector<bool> keep(all.size(), false);
        const int context = contextAction_->isChecked() ? 2 : 0;
        for (int i = 0; i < all.size(); ++i) {
            if (!all[i].changed) continue;
            for (int j = qMax(0, i - context); j <= qMin(all.size() - 1, i + context); ++j) {
                keep[j] = true;
            }
        }
        QVector<TextDiffLine> result;
        for (int i = 0; i < all.size(); ++i) {
            if (keep[i]) result.push_back(all[i]);
        }
        return result;
    }

    void rebuild() {
        QVector<TextDiffLine> all = alignTextLines(leftOriginal_, rightOriginal_);
        if (minorAction_->isChecked()) {
            for (auto& row : all) {
                if (row.whitespaceOnly) row.changed = false;
            }
        }
        rows_ = visibleRows(all);
        groups_ = groupDiffRows(rows_);
        computeInlineDiffs(rows_);

        QStringList leftLines;
        QStringList rightLines;
        QVector<int> leftNumbers;
        QVector<int> rightNumbers;
        int differenceCount = 0;
        int sectionCount = 0;
        bool inSection = false;
        for (const auto& row : rows_) {
            leftLines << row.left;
            rightLines << row.right;
            leftNumbers << row.leftNumber;
            rightNumbers << row.rightNumber;
            if (row.changed) {
                ++differenceCount;
                if (!inSection) ++sectionCount;
                inSection = true;
            } else {
                inSection = false;
            }
        }

        applyingProgrammaticUpdate_ = true;
        leftEditor_->setPlainText(leftLines.join('\n'));
        rightEditor_->setPlainText(rightLines.join('\n'));
        applyingProgrammaticUpdate_ = false;
        leftStructurallyEdited_ = false;
        rightStructurallyEdited_ = false;

        leftEditor_->setLineNumbers(leftNumbers);
        rightEditor_->setLineNumbers(rightNumbers);
        leftEditor_->setDiffData(rows_, groups_);
        rightEditor_->setDiffData(rows_, groups_);
        leftEditor_->setInlineDiffs(leftInline_);
        rightEditor_->setInlineDiffs(rightInline_);
        computeDiffSelections(rows_);
        miniMap_->setRows(rows_);
        preview_->clear();
        currentRow_ = -1;
        refreshDirtyIndicators();
        updateMinimapViewport();
        status_->setText(QString("%1 difference section(s)   |   %2 line(s)   |   %3 changed row(s)")
                             .arg(sectionCount)
                             .arg(rows_.size())
                             .arg(differenceCount));
    }

    // Colours each changed row to say what actually happened to it, matching
    // Beyond Compare's convention: green on the side a line was added to,
    // red/pink on the side a line was removed from, amber where the same
    // line exists on both sides but its text differs, blue for a
    // whitespace-only difference, and a diagonal hatch on whichever side has
    // no counterpart at all (a pure filler/alignment row).
    void computeDiffSelections(const QVector<TextDiffLine>& rows) {
        leftDiffSelections_.clear();
        rightDiffSelections_.clear();
        const auto addSelection = [this](LineNumberEditor* editor, int row, const QBrush& brush) {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(brush);
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = cursorAtRow(editor, row);
            (editor == leftEditor_ ? leftDiffSelections_ : rightDiffSelections_).append(selection);
        };
        static const QBrush kHatch(QColor(0x50, 0x50, 0x50, 130), Qt::BDiagPattern);
        for (int row = 0; row < rows.size(); ++row) {
            const TextDiffLine& line = rows[row];
            if (!line.changed) continue;
            if (line.whitespaceOnly) {
                addSelection(leftEditor_, row, QColor(45, 90, 135, 150));
                addSelection(rightEditor_, row, QColor(45, 90, 135, 150));
            } else if (line.leftNumber > 0 && line.rightNumber > 0) {
                addSelection(leftEditor_, row, QColor(150, 120, 30, 150));
                addSelection(rightEditor_, row, QColor(150, 120, 30, 150));
            } else if (line.leftNumber > 0) {
                addSelection(leftEditor_, row, QColor(40, 140, 70, 150));
                addSelection(rightEditor_, row, kHatch);
            } else if (line.rightNumber > 0) {
                addSelection(rightEditor_, row, QColor(150, 45, 45, 150));
                addSelection(leftEditor_, row, kHatch);
            }
        }
    }

    // Row(s), by index into rows_, that a side's copied-but-unsaved lines
    // land on right now.
    QSet<int> dirtyRowIndices(bool left) const {
        const QSet<int>& dirty = left ? leftDirtyLines_ : rightDirtyLines_;
        QSet<int> result;
        if (dirty.isEmpty()) return result;
        for (int row = 0; row < rows_.size(); ++row) {
            const int number = left ? rows_[row].leftNumber : rows_[row].rightNumber;
            if (number > 0 && dirty.contains(number)) result.insert(row);
        }
        return result;
    }

    // Recomputes the green "copied, not yet saved" overlay from
    // left/rightDirtyLines_ and pushes it both into the editors' extra
    // selections (full-row highlight) and into the arrow-gutter marker.
    void refreshDirtyIndicators() {
        leftDirtyRows_ = dirtyRowIndices(true);
        rightDirtyRows_ = dirtyRowIndices(false);
        updateExtraSelections();
        leftEditor_->setDirtyRows(leftDirtyRows_);
        rightEditor_->setDirtyRows(rightDirtyRows_);
    }

    // VS Code-style current-line highlight, plus the green "needs saving"
    // overlay for copied lines - both drawn on top of the base diff
    // colouring, kept on the same row in both editors regardless of which
    // pane the caret or a mouse click actually landed in.
    void updateExtraSelections() {
        const auto withOverlays = [this](LineNumberEditor* editor,
                                         QVector<QTextEdit::ExtraSelection> selections,
                                         const QSet<int>& dirtyRows) {
            static const QColor kDirty(40, 170, 90, 170);
            for (int row : dirtyRows) {
                if (row < 0 || row >= editor->document()->blockCount()) continue;
                QTextEdit::ExtraSelection dirty;
                dirty.format.setBackground(kDirty);
                dirty.format.setProperty(QTextFormat::FullWidthSelection, true);
                dirty.cursor = cursorAtRow(editor, row);
                selections.append(dirty);
            }
            if (currentRow_ >= 0 && currentRow_ < editor->document()->blockCount()) {
                QTextEdit::ExtraSelection current;
                current.format.setBackground(QColor(255, 255, 255, 22));
                current.format.setProperty(QTextFormat::FullWidthSelection, true);
                current.cursor = cursorAtRow(editor, currentRow_);
                selections.append(current);
            }
            return selections;
        };
        leftEditor_->setExtraSelections(withOverlays(leftEditor_, leftDiffSelections_, leftDirtyRows_));
        rightEditor_->setExtraSelections(
            withOverlays(rightEditor_, rightDiffSelections_, rightDirtyRows_));
    }

    // Maps the editor's current vertical-scrollbar position onto the
    // fraction-of-whole-file the mini-map needs, so its black viewport
    // rectangle always matches what is actually on screen.
    void updateMinimapViewport() {
        const int total = qMax(1, rows_.size());
        const QScrollBar* bar = leftEditor_->verticalScrollBar();
        const qreal start = static_cast<qreal>(bar->value()) / total;
        const qreal span = qMax(1, bar->pageStep()) / static_cast<qreal>(total);
        miniMap_->setViewport(start, span);
    }

    // Writes whichever editor currently has focus back to its file on disk.
    void saveFocusedSide() {
        syncFromEditors();
        if (leftEditor_->hasFocus()) {
            saveSide(true);
        } else if (rightEditor_->hasFocus()) {
            saveSide(false);
        } else {
            status_->setText("Click into a pane first, then Ctrl+S saves that side.");
        }
    }

    void saveSide(bool left) {
        const QString& path = left ? leftPath_ : rightPath_;
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            status_->setText("Could not save " + (path.isEmpty() ? QString("(no file)") : path));
            return;
        }
        QTextStream out(&file);
        out << (left ? leftOriginal_ : rightOriginal_).join('\n');
        file.close();
        (left ? leftDirtyLines_ : rightDirtyLines_).clear();
        refreshDirtyIndicators();
        status_->setText(QFileInfo(path).fileName() + " saved.");
    }

    // Applies an edit made in the bottom preview strip straight back into
    // the corresponding editor's current line (in place - this never adds
    // or removes a line, so the row <-> block mapping stays valid).
    void applyPreviewEdit(bool left, const QString& text) {
        if (currentRow_ < 0 || currentRow_ >= rows_.size()) return;
        LineNumberEditor* editor = left ? leftEditor_ : rightEditor_;
        const int number = left ? rows_[currentRow_].leftNumber : rows_[currentRow_].rightNumber;
        if (number == 0) return;  // filler row on this side - nothing to edit
        QTextCursor cursor = cursorAtRow(editor, currentRow_);
        cursor.select(QTextCursor::LineUnderCursor);
        cursor.insertText(text);
        editor->setFocus();
    }

    QString leftPath_;
    QString rightPath_;
    QStringList leftOriginal_;
    QStringList rightOriginal_;
    QVector<TextDiffLine> rows_;    // currently displayed rows (post All/Diffs/Context filter)
    QVector<DiffGroup> groups_;     // contiguous change runs within rows_

    QToolBar* toolbar_ = nullptr;
    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* showAllAction_ = nullptr;
    QAction* showDiffsAction_ = nullptr;
    QAction* contextAction_ = nullptr;
    QAction* minorAction_ = nullptr;
    QAction* rulesAction_ = nullptr;
    QAction* formatAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* nextSectionAction_ = nullptr;
    QAction* prevSectionAction_ = nullptr;
    QAction* swapAction_ = nullptr;
    QAction* reloadAction_ = nullptr;

    QLabel* leftLabel_ = nullptr;
    QLabel* rightLabel_ = nullptr;
    LineNumberEditor* leftEditor_ = nullptr;
    LineNumberEditor* rightEditor_ = nullptr;
    DifferenceOverview* miniMap_ = nullptr;
    DiffLinePreview* preview_ = nullptr;
    QLabel* status_ = nullptr;
    bool syncing_ = false;

    int currentRow_ = -1;
    QVector<QTextEdit::ExtraSelection> leftDiffSelections_;
    QVector<QTextEdit::ExtraSelection> rightDiffSelections_;

    bool applyingProgrammaticUpdate_ = false;
    bool leftStructurallyEdited_ = false;
    bool rightStructurallyEdited_ = false;

    // 1-based line numbers (into leftOriginal_/rightOriginal_) that were
    // written by a copy-to-other-side action and have not been saved since.
    QSet<int> leftDirtyLines_;
    QSet<int> rightDirtyLines_;
    // Same information translated into row indices (into rows_) for the
    // currently displayed/filtered document - what actually gets painted.
    QSet<int> leftDirtyRows_;
    QSet<int> rightDirtyRows_;

    QAction* saveAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* pixelMinimapAction_ = nullptr;

    SyntaxHighlighter* leftHighlighter_ = nullptr;
    SyntaxHighlighter* rightHighlighter_ = nullptr;
    QVector<QVector<CharSegment>> leftInline_;
    QVector<QVector<CharSegment>> rightInline_;
    FindReplaceBar* findBar_ = nullptr;
};

}  // namespace openbc::app

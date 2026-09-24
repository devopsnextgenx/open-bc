// text_compare_view.h
// ---------------------------------------------------------------------------
// TextCompareView: the two-pane diff editor tab (toolbar, minimap, the two
// LineNumberEditor panes, the bottom DiffLinePreview console, and the
// FindReplaceBar). This revision gives each side its own undo/redo history
// and routes Ctrl+Z / Ctrl+Y to whichever pane currently has focus.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSet>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>
#include <functional>

#include "diff_algorithms.h"
#include "diff_line_preview.h"
#include "find_replace_bar.h"
#include "line_number_editor.h"
#include "minimap.h"
#include "path_selector.h"
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
        // Each side starts with its own single-entry history ("as loaded").
        pushSideHistory(/*left=*/true);
        pushSideHistory(/*left=*/false);
    }

    QString title() const {
        const QString leftName = leftPath_.isEmpty() ? "(missing)" : QFileInfo(leftPath_).fileName();
        const QString rightName = rightPath_.isEmpty() ? "(missing)" : QFileInfo(rightPath_).fileName();
        return leftName + " <-> " + rightName;
    }

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> editMenuItems() const { return {undoAction_, redoAction_, saveAction_}; }
    QList<QAction*> searchMenuItems() const { return {findAction_, nextSectionAction_, prevSectionAction_}; }
    QList<QAction*> viewMenuItems() const {
        return {showAllAction_, showDiffsAction_, contextAction_, minorAction_,
                pixelMinimapAction_, formatAction_};
    }
    QAction* syntaxStyleMenuAction() const { return syntaxStyleMenu_->menuAction(); }
    QList<QAction*> actionsMenuItems() const { return {copyAction_, swapAction_, reloadAction_}; }

protected:
    // Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y are intercepted here *before*
    // QPlainTextEdit's own keyPressEvent can swallow them. They are then
    // routed to the focused pane's own history, so undoing the left pane
    // never touches the right pane's state.
    bool eventFilter(QObject* watched, QEvent* event) override {
        if ((watched == leftEditor_ || watched == rightEditor_) &&
            event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            const QKeySequence seq(ke->key() | ke->modifiers());
            if (seq == QKeySequence::Undo) {
                undoFocusedSide();
                return true;
            }
            if (seq == QKeySequence::Redo ||
                seq == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)) {
                redoFocusedSide();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    // Whichever editor pane last actually held keyboard focus. Falls back
    // to the left pane if neither has ever been focused (e.g. only the
    // toolbar has focus), so Ctrl+Z still does something sensible.
    LineNumberEditor* focusedEditor() const {
        if (rightEditor_ && rightEditor_->hasFocus()) return rightEditor_;
        if (leftEditor_ && leftEditor_->hasFocus()) return leftEditor_;
        if (lastFocusedEditor_) return lastFocusedEditor_;
        return leftEditor_;
    }

    void applyLanguageHighlighting() {
        leftHighlighter_->setLanguage(QFileInfo(leftPath_).suffix());
        rightHighlighter_->setLanguage(QFileInfo(rightPath_).suffix());
    }

    void setHighlightStyle(SyntaxHighlighter::Style style) {
        applyingProgrammaticUpdate_ = true;
        leftHighlighter_->setStyle(style);
        rightHighlighter_->setStyle(style);
        applyingProgrammaticUpdate_ = false;
    }

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

    QWidget* buildSideHeader(bool left) {
        auto* frame = new QFrame(this);
        frame->setObjectName("sideHeader");
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(2);

        auto* pathRow = new QHBoxLayout;
        pathRow->setContentsMargins(0, 0, 0, 0);
        pathRow->setSpacing(4);
        PathSelector*& selector = left ? leftSelector_ : rightSelector_;
        QToolButton*& saveButton = left ? leftSaveButton_ : rightSaveButton_;
        selector = new PathSelector(PathSelector::Mode::File, left ? "Left" : "Right", frame);
        selector->combo()->setObjectName("sidePath");
        selector->onBrowsed = [this, left](const QString& path) { loadSide(left, path); };
        pathRow->addWidget(selector, 1);
        saveButton = new QToolButton(frame);
        saveButton->setObjectName("saveSideBtn");
        saveButton->setIcon(icons::glyph(icons::Glyph::Save));
        saveButton->setIconSize(QSize(15, 15));
        saveButton->setToolTip(left ? "Save left file (Ctrl+S while focused)"
                                    : "Save right file (Ctrl+S while focused)");
        saveButton->setCursor(Qt::PointingHandCursor);
        connect(saveButton, &QToolButton::clicked, this, [this, left]() {
            syncFromEditors();
            saveSide(left);
        });
        pathRow->addWidget(saveButton, 0);
        layout->addLayout(pathRow);

        QLabel*& attrsLabel = left ? leftAttrsLabel_ : rightAttrsLabel_;
        attrsLabel = new QLabel(frame);
        attrsLabel->setObjectName("sideAttrs");
        layout->addWidget(attrsLabel);

        refreshSideHeader(left);
        return frame;
    }

    void refreshSideHeader(bool left) {
        const QString& path = left ? leftPath_ : rightPath_;
        PathSelector* selector = left ? leftSelector_ : rightSelector_;
        QLabel* attrsLabel = left ? leftAttrsLabel_ : rightAttrsLabel_;
        QToolButton* saveButton = left ? leftSaveButton_ : rightSaveButton_;
        if (!selector || !attrsLabel) return;

        const QFileInfo info(path);
        selector->setText(path);
        selector->combo()->setToolTip(path);

        if (info.exists()) {
            const QString modified = info.lastModified().toString("M/d/yyyy h:mm:ss AP");
            const QString size = QLocale().toString(info.size()) + " bytes";
            const QString lineEnding = detectLineEnding(left);
            attrsLabel->setText(QString("%1    %2    %3    %4")
                                     .arg(modified, size, "UTF-8", lineEnding));
        } else {
            attrsLabel->setText(path.isEmpty() ? "(missing on this side)" : "(unavailable)");
        }
        if (saveButton) {
            // A side is "unsaved" if it has copied-but-not-saved rows *or* if the
            // user has edited it by hand at all since the last load/save. The
            // leftDirtyLines_/rightDirtyLines_ sets only cover the copy-to-other
            // -side path (they drive the green row highlight), so without the
            // extra unsaved flags manual typing would leave the button grey.
            const bool dirty = (left ? leftUnsaved_ : rightUnsaved_) ||
                            !(left ? leftDirtyLines_ : rightDirtyLines_).isEmpty();
            static const QColor kDirtyBlue(0x4e, 0x9c, 0xff);
            static const QColor kCleanInk(0xdd, 0xe0, 0xee);
            saveButton->setIcon(icons::saveIcon(dirty ? kDirtyBlue : kCleanInk));
            saveButton->setProperty("dirty", dirty);
            saveButton->setToolTip(
                dirty ? "Unsaved changes on this side - click or Ctrl+S to write to disk"
                    : (left ? "Save left file (Ctrl+S while focused)"
                            : "Save right file (Ctrl+S while focused)"));
            saveButton->style()->unpolish(saveButton);
            saveButton->style()->polish(saveButton);
        }
    }

    QString detectLineEnding(bool left) const {
        const QStringList& lines = left ? leftOriginal_ : rightOriginal_;
        for (const QString& line : lines) {
            if (line.endsWith('\r')) return "PC";
        }
        return lines.size() > 1 ? "Unix" : "PC";
    }

    void buildUi() {
        using icons::Glyph;
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

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
        showAllAction_->setChecked(true);
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
        header->setContentsMargins(0, 0, 0, 0);
        header->setSpacing(1);
        header->addWidget(buildSideHeader(/*left=*/true), 1);
        header->addWidget(buildSideHeader(/*left=*/false), 1);
        root->addLayout(header);

        auto* editors = new QSplitter(Qt::Horizontal, this);
        editors->setChildrenCollapsible(false);

        auto* leftPane = new QWidget(editors);
        auto* leftLayout = new QHBoxLayout(leftPane);
        leftLayout->setContentsMargins(0, 0, 0, 0);
        leftLayout->setSpacing(0);
        miniMap_ = new DifferenceOverview(leftPane, true);
        leftEditor_ = new LineNumberEditor(LineNumberEditor::GutterSide::Left,
                                           LineNumberEditor::GutterSide::Right, leftPane);
        leftLayout->addWidget(miniMap_);
        leftLayout->addWidget(leftEditor_, 1);

        auto* rightPane = new QWidget(editors);
        auto* rightLayout = new QHBoxLayout(rightPane);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->setSpacing(0);
        rightEditor_ = new LineNumberEditor(LineNumberEditor::GutterSide::Right,
                                            LineNumberEditor::GutterSide::Left, rightPane);
        rightEditor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        rightLayout->addWidget(rightEditor_, 1);

        // Intercept Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z on both editors before
        // their own keyPressEvent can swallow them.
        leftEditor_->installEventFilter(this);
        rightEditor_->installEventFilter(this);

        // Undo/redo is driven by this view's own per-side snapshot
        // histories, which survive rebuild()'s setPlainText() calls.
        // Disable the native stacks so the two systems don't fight.
        leftEditor_->setUndoRedoEnabled(false);
        rightEditor_->setUndoRedoEnabled(false);

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

        syntaxStyleMenu_ = new QMenu("Syntax & Style", this);
        auto* syntaxMenu = syntaxStyleMenu_->addMenu("Syntax Highlighting");
        auto* syntaxGroup = new QActionGroup(this);
        syntaxGroup->setExclusive(true);
        syntaxAutoAction_ = syntaxMenu->addAction("Automatic");
        syntaxAutoAction_->setCheckable(true);
        syntaxAutoAction_->setChecked(true);
        syntaxPlainAction_ = syntaxMenu->addAction("Plain Text");
        syntaxPlainAction_->setCheckable(true);
        syntaxGroup->addAction(syntaxAutoAction_);
        syntaxGroup->addAction(syntaxPlainAction_);

        auto* styleMenu = syntaxStyleMenu_->addMenu("Editor Style");
        auto* styleGroup = new QActionGroup(this);
        styleGroup->setExclusive(true);
        vscodeDarkStyleAction_ = styleMenu->addAction("VS Code Dark");
        vibrantStyleAction_ = styleMenu->addAction("Vibrant");
        classicStyleAction_ = styleMenu->addAction("Classic");
        contrastStyleAction_ = styleMenu->addAction("High Contrast");
        for (QAction* action : {vibrantStyleAction_, classicStyleAction_, contrastStyleAction_,
                    vscodeDarkStyleAction_}) {
            action->setCheckable(true);
            styleGroup->addAction(action);
        }
        vscodeDarkStyleAction_->setChecked(true);

        editors->addWidget(leftPane);
        editors->addWidget(rightPane);
        editors->setStretchFactor(0, 1);
        editors->setStretchFactor(1, 1);
        root->addWidget(editors, 1);

        findBar_ = new FindReplaceBar(this);
        findBar_->setEditors(leftEditor_, rightEditor_);
        root->addWidget(findBar_);

        preview_ = new DiffLinePreview(this);
        root->addWidget(preview_);

        status_ = new QLabel(this);
        status_->setObjectName("compareStatus");
        root->addWidget(status_);

        leftEditor_->setContextMenuPolicy(Qt::CustomContextMenu);
        rightEditor_->setContextMenuPolicy(Qt::CustomContextMenu);

        saveAction_ = new QAction(this);
        saveAction_->setShortcut(QKeySequence::Save);
        saveAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(saveAction_);

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

        // Menu / toolbar triggers route to the focused pane's history.
        connect(undoAction_, &QAction::triggered, this, [this]() { undoFocusedSide(); });
        connect(redoAction_, &QAction::triggered, this, [this]() { redoFocusedSide(); });

        // One debounce timer per side: typing in the left pane only ever
        // pushes onto the left history, and vice versa.
        leftEditTimer_ = new QTimer(this);
        leftEditTimer_->setSingleShot(true);
        leftEditTimer_->setInterval(400);
        connect(leftEditTimer_, &QTimer::timeout, this, [this]() {
            syncFromEditors();
            pushSideHistory(/*left=*/true);
        });
        rightEditTimer_ = new QTimer(this);
        rightEditTimer_->setSingleShot(true);
        rightEditTimer_->setInterval(400);
        connect(rightEditTimer_, &QTimer::timeout, this, [this]() {
            syncFromEditors();
            pushSideHistory(/*left=*/false);
        });
        connect(leftEditor_->document(), &QTextDocument::contentsChanged, this, [this]() {
            if (applyingProgrammaticUpdate_ || restoringHistory_) return;
            leftUnsaved_ = true;
            refreshSideHeader(true);
            if (leftEditTimer_) leftEditTimer_->start();
        });
        connect(rightEditor_->document(), &QTextDocument::contentsChanged, this, [this]() {
            if (applyingProgrammaticUpdate_ || restoringHistory_) return;
            rightUnsaved_ = true;
            refreshSideHeader(false);
            if (rightEditTimer_) rightEditTimer_->start();
        });

        connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
            if (now == leftEditor_ || now == leftEditor_->viewport()) {
                lastFocusedEditor_ = leftEditor_;
                findBar_->notifyFocused(leftEditor_);
            } else if (now == rightEditor_ || now == rightEditor_->viewport()) {
                lastFocusedEditor_ = rightEditor_;
                findBar_->notifyFocused(rightEditor_);
            }
            // Undo/redo enable-state depends on which pane is focused.
            updateUndoRedoActions();

            const bool leftBlurred = (old == leftEditor_ || old == leftEditor_->viewport()) &&
                                      now != leftEditor_ && now != leftEditor_->viewport();
            const bool rightBlurred = (old == rightEditor_ || old == rightEditor_->viewport()) &&
                                       now != rightEditor_ && now != rightEditor_->viewport();
            if ((leftBlurred && leftEditor_->document()->isModified()) ||
                (rightBlurred && rightEditor_->document()->isModified())) {
                scheduleAutoReload();
            }
        });

        connect(leftEditor_->document(), &QTextDocument::blockCountChanged, this,
                [this](int count) {
                    if (applyingProgrammaticUpdate_ || count == rows_.size()) return;
                    leftStructurallyEdited_ = true;
                    leftEditor_->setDiffData({}, {});
                    scheduleAutoReload();
                });
        connect(rightEditor_->document(), &QTextDocument::blockCountChanged, this,
                [this](int count) {
                    if (applyingProgrammaticUpdate_ || count == rows_.size()) return;
                    rightStructurallyEdited_ = true;
                    rightEditor_->setDiffData({}, {});
                    scheduleAutoReload();
                });

        connect(homeAction_, &QAction::triggered, this,
                [this]() { if (onHomeRequested) onHomeRequested(); });
        connect(sessionsAction_, &QAction::triggered, this,
                [this]() { if (onSessionsRequested) onSessionsRequested(); });
        connect(showAllAction_, &QAction::triggered, this, [this]() { rebuild(); });
        connect(showDiffsAction_, &QAction::triggered, this, [this]() { rebuild(); });
        connect(contextAction_, &QAction::triggered, this, [this]() { rebuild(); });
        connect(syntaxAutoAction_, &QAction::triggered, this, [this]() {
            applyingProgrammaticUpdate_ = true;
            leftHighlighter_->setEnabled(true);
            rightHighlighter_->setEnabled(true);
            applyingProgrammaticUpdate_ = false;
        });
        connect(syntaxPlainAction_, &QAction::triggered, this, [this]() {
            applyingProgrammaticUpdate_ = true;
            leftHighlighter_->setEnabled(false);
            rightHighlighter_->setEnabled(false);
            applyingProgrammaticUpdate_ = false;
        });
        connect(vibrantStyleAction_, &QAction::triggered, this, [this]() {
            setHighlightStyle(SyntaxHighlighter::Style::Vibrant);
        });
        connect(classicStyleAction_, &QAction::triggered, this, [this]() {
            setHighlightStyle(SyntaxHighlighter::Style::Classic);
        });
        connect(contrastStyleAction_, &QAction::triggered, this, [this]() {
            setHighlightStyle(SyntaxHighlighter::Style::HighContrast);
        });
        connect(vscodeDarkStyleAction_, &QAction::triggered, this, [this]() {
            setHighlightStyle(SyntaxHighlighter::Style::VsCodeDark);
        });
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
            flushPendingEdits();
            syncFromEditors();
            std::swap(leftOriginal_, rightOriginal_);
            std::swap(leftPath_, rightPath_);
            std::swap(leftDirtyLines_, rightDirtyLines_);
            std::swap(leftUnsaved_, rightUnsaved_);                      // <-- new
            refreshSideHeader(true);
            refreshSideHeader(false);
            applyLanguageHighlighting();
            if (onTitleChanged) onTitleChanged();
            rebuild();
            // Post-swap state goes into *both* histories, so undoing either
            // side restores that side's pre-swap content.
            pushSideHistory(true);
            pushSideHistory(false);
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

        preview_->onLeftEdited = [this](const QString& text) { applyPreviewEdit(true, text); };
        preview_->onRightEdited = [this](const QString& text) { applyPreviewEdit(false, text); };
    }

    static QTextCursor cursorAtRow(QPlainTextEdit* editor, int row) {
        return QTextCursor(editor->document()->findBlockByNumber(row));
    }

    void jumpToRow(int row) {
        leftEditor_->setTextCursor(cursorAtRow(leftEditor_, row));
        rightEditor_->setTextCursor(cursorAtRow(rightEditor_, row));
        leftEditor_->ensureCursorVisible();
        rightEditor_->ensureCursorVisible();
        updateMinimapViewport();
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

    void showEditorContextMenu(LineNumberEditor* editor, const QPoint& pos, bool isLeft) {
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

        // Every branch that modifies the editor pushes the pre-edit state
        // onto *that side's* history before applying, then the post-edit
        // state after - so Ctrl+Z from that pane can always walk back.
        const bool sideIsLeft = (editor == leftEditor_);
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
            flushPendingEdits();
            syncFromEditors();
            QTextCursor cursor = cursorAtRow(editor, row);
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.insertBlock();
            syncFromEditors();
            pushSideHistory(sideIsLeft);
        } else if (chosen == increaseIndent || chosen == decreaseIndent) {
            flushPendingEdits();
            syncFromEditors();
            indentRow(editor, row, chosen == increaseIndent);
            syncFromEditors();
            pushSideHistory(sideIsLeft);
        } else if (chosen == compareClipboard) {
            compareToClipboard(isLeft);
        } else if (chosen == cutAction) {
            flushPendingEdits();
            syncFromEditors();
            editor->cut();
            syncFromEditors();
            pushSideHistory(sideIsLeft);
        } else if (chosen == copyAction) {
            editor->copy();
        } else if (chosen == pasteAction) {
            flushPendingEdits();
            syncFromEditors();
            editor->paste();
            syncFromEditors();
            pushSideHistory(sideIsLeft);
        } else if (chosen == deleteAction) {
            QTextCursor cursor = editor->textCursor();
            if (cursor.hasSelection()) {
                flushPendingEdits();
                syncFromEditors();
                cursor.removeSelectedText();
                syncFromEditors();
                pushSideHistory(sideIsLeft);
            }
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

    QStringList materializeSide(bool left) const {
        QStringList result;
        for (const auto& row : rows_) {
            const int number = left ? row.leftNumber : row.rightNumber;
            const QString& text = left ? row.left : row.right;
            if (number > 0 || !text.isEmpty()) result << text;
        }
        return result;
    }

    // ---------------------------------------------------------------------
    // Per-side undo / redo history.
    //
    // rebuild() replaces both editors' text via setPlainText(), which wipes
    // the per-document undo stacks. Instead of one both-sides history, each
    // pane now owns its own stack: any operation that changes a side pushes
    // that side's new state onto its history, and Ctrl+Z / Ctrl+Y walk only
    // the *focused* pane's stack. Undoing the left pane therefore never
    // touches the right pane's content or its own undo position.
    //
    // Each push captures the *current* state, so the history is a list of
    // historical versions of one side, with `*HistoryIndex_` pointing at the
    // version currently displayed. Entries survive saves (a save only writes
    // to disk) and only die when the tab is closed, bounded to
    // kMaxHistoryEntries snapshots per side.
    // ---------------------------------------------------------------------
    struct SideSnapshot {
        QStringList lines;
        QSet<int> dirty;
        QString path;
        bool unsaved = false;   // <-- new
    };
    static constexpr int kMaxHistoryEntries = 200;

    // Appends a snapshot of one side to that side's history. No-op if the
    // side's current state already matches the snapshot at the current
    // index (which happens routinely for the unchanged side in operations
    // that push both sides).
    void pushSideHistory(bool left) {
        if (restoringHistory_) return;
        QVector<SideSnapshot>& history = left ? leftHistory_ : rightHistory_;
        int& index = left ? leftHistoryIndex_ : rightHistoryIndex_;
        SideSnapshot snap;
        if (left) {
            snap.lines = leftOriginal_;
            snap.dirty = leftDirtyLines_;
            snap.path = leftPath_;
            snap.unsaved = leftUnsaved_;       // <-- new
        } else {
            snap.lines = rightOriginal_;
            snap.dirty = rightDirtyLines_;
            snap.path = rightPath_;
            snap.unsaved = rightUnsaved_;      // <-- new
        }
        if (index >= 0 && index < history.size()) {
            const SideSnapshot& cur = history[index];
            if (cur.lines == snap.lines && cur.dirty == snap.dirty &&
                cur.path == snap.path && cur.unsaved == snap.unsaved) {   // <-- new
                updateUndoRedoActions();
                return;
            }
        }
        if (index >= 0 && index + 1 < history.size()) {
            history.resize(index + 1);
        }
        history.append(std::move(snap));
        if (history.size() > kMaxHistoryEntries) history.removeFirst();
        index = history.size() - 1;
        updateUndoRedoActions();
    }

    // Flushes any debounced typing on *both* sides into their respective
    // histories, so hitting Ctrl+Z immediately after typing (or starting a
    // copy / load / swap) still has the pre-edit state as its own entry.
    void flushPendingEdits() {
        bool leftDirty = false;
        bool rightDirty = false;
        if (leftEditTimer_ && leftEditTimer_->isActive()) {
            leftEditTimer_->stop();
            leftDirty = true;
        }
        if (rightEditTimer_ && rightEditTimer_->isActive()) {
            rightEditTimer_->stop();
            rightDirty = true;
        }
        if (!leftDirty && !rightDirty) return;
        syncFromEditors();
        if (leftDirty) pushSideHistory(true);
        if (rightDirty) pushSideHistory(false);
    }

    void undoFocusedSide() {
        flushPendingEdits();
        LineNumberEditor* editor = focusedEditor();
        const bool left = (editor == leftEditor_);
        QVector<SideSnapshot>& history = left ? leftHistory_ : rightHistory_;
        int& index = left ? leftHistoryIndex_ : rightHistoryIndex_;
        if (index <= 0) return;
        --index;
        applySideSnapshot(left);
        status_->setText(QString("Undo %1  (%2 / %3)")
                             .arg(left ? "left" : "right")
                             .arg(index + 1)
                             .arg(history.size()));
    }

    void redoFocusedSide() {
        flushPendingEdits();
        LineNumberEditor* editor = focusedEditor();
        const bool left = (editor == leftEditor_);
        QVector<SideSnapshot>& history = left ? leftHistory_ : rightHistory_;
        int& index = left ? leftHistoryIndex_ : rightHistoryIndex_;
        if (index < 0 || index >= history.size() - 1) return;
        ++index;
        applySideSnapshot(left);
        status_->setText(QString("Redo %1  (%2 / %3)")
                             .arg(left ? "left" : "right")
                             .arg(index + 1)
                             .arg(history.size()));
    }

    // Restores one side from its current history entry and rebuilds the
    // whole view (so the diff colouring, minimap, arrow gutters, dirty
    // indicators and side headers are all recomputed). The *other* side is
    // left completely untouched.
    void applySideSnapshot(bool left) {
        QVector<SideSnapshot>& history = left ? leftHistory_ : rightHistory_;
        const int index = left ? leftHistoryIndex_ : rightHistoryIndex_;
        if (index < 0 || index >= history.size()) return;
        restoringHistory_ = true;
        const SideSnapshot& snap = history[index];
        bool pathChanged = false;
        if (left) {
            leftOriginal_ = snap.lines;
            leftDirtyLines_ = snap.dirty;
            leftUnsaved_ = snap.unsaved;       // <-- new
            pathChanged = (leftPath_ != snap.path);
            leftPath_ = snap.path;
            leftStructurallyEdited_ = false;
        } else {
            rightOriginal_ = snap.lines;
            rightDirtyLines_ = snap.dirty;
            rightUnsaved_ = snap.unsaved;     // <-- new
            pathChanged = (rightPath_ != snap.path);
            rightPath_ = snap.path;
            rightStructurallyEdited_ = false;
        }
        if (pathChanged) applyLanguageHighlighting();
        rebuild();
        restoringHistory_ = false;
        if (pathChanged && onTitleChanged) onTitleChanged();
        updateUndoRedoActions();
    }

    // Enabled-state of the Edit-menu Undo/Redo entries follows whichever
    // pane currently has focus.
    void updateUndoRedoActions() {
        LineNumberEditor* editor = focusedEditor();
        const bool left = (editor == leftEditor_);
        const int index = left ? leftHistoryIndex_ : rightHistoryIndex_;
        const int size = left ? leftHistory_.size() : rightHistory_.size();
        if (undoAction_) undoAction_->setEnabled(index > 0);
        if (redoAction_) redoAction_->setEnabled(index >= 0 && index < size - 1);
    }

    void copyGroup(int startRow, int endRow, bool leftToRight) {
        flushPendingEdits();
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
        // Mark the destination side unsaved *before* rebuild() so its
        // refreshSideHeader() picks up the new flag and recolours the icon.
        if (leftToRight) rightUnsaved_ = true;
        else              leftUnsaved_ = true;
        rebuild();
        pushSideHistory(/*left=*/!leftToRight);
    }

    static void markDirtyForInsert(QSet<int>& dirty, int insertedAt1Based) {
        QSet<int> shifted;
        for (int number : dirty) {
            shifted.insert(number >= insertedAt1Based ? number + 1 : number);
        }
        shifted.insert(insertedAt1Based);
        dirty = shifted;
    }

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

    struct CursorSnapshot {
        int block = 0;
        int column = 0;
        int scroll = 0;
    };

    static CursorSnapshot captureCursor(LineNumberEditor* editor) {
        const QTextCursor cursor = editor->textCursor();
        return {cursor.blockNumber(), cursor.positionInBlock(), editor->verticalScrollBar()->value()};
    }

    static void restoreCursor(LineNumberEditor* editor, const CursorSnapshot& snap) {
        const QTextBlock block =
            editor->document()->findBlockByNumber(qBound(0, snap.block, editor->document()->blockCount() - 1));
        QTextCursor cursor(block);
        cursor.setPosition(block.position() + qBound(0, snap.column, qMax(0, block.length() - 1)));
        editor->setTextCursor(cursor);
        editor->verticalScrollBar()->setValue(qMin(snap.scroll, editor->verticalScrollBar()->maximum()));
    }

    void scheduleAutoReload() {
        if (autoReloadPending_) return;
        autoReloadPending_ = true;
        QTimer::singleShot(0, this, [this]() {
            autoReloadPending_ = false;
            syncFromEditors();
            rebuild();
            // Only the side that was actually edited will get a real push;
            // the other side dedups against its current head.
            pushSideHistory(true);
            pushSideHistory(false);
        });
    }

    void rebuild() {
        const CursorSnapshot leftSnap = captureCursor(leftEditor_);
        const CursorSnapshot rightSnap = captureCursor(rightEditor_);

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
        leftStructurallyEdited_ = false;
        rightStructurallyEdited_ = false;
        leftEditor_->document()->setModified(false);
        rightEditor_->document()->setModified(false);
        currentRow_ = -1;
        preview_->clear();
        restoreCursor(leftEditor_, leftSnap);
        restoreCursor(rightEditor_, rightSnap);

        leftEditor_->setLineNumbers(leftNumbers);
        rightEditor_->setLineNumbers(rightNumbers);
        leftEditor_->setDiffData(rows_, groups_);
        rightEditor_->setDiffData(rows_, groups_);
        leftEditor_->setInlineDiffs(leftInline_);
        rightEditor_->setInlineDiffs(rightInline_);
        leftHighlighter_->refreshInlineDiffs();
        rightHighlighter_->refreshInlineDiffs();
        applyingProgrammaticUpdate_ = false;
        computeDiffSelections(rows_);
        miniMap_->setRows(rows_);
        miniMap_->setCurrentRow(currentRow_);
        refreshDirtyIndicators();
        updateMinimapViewport();
        refreshSideHeader(true);
        refreshSideHeader(false);
        status_->setText(QString("%1 difference section(s)   |   %2 line(s)   |   %3 changed row(s)")
                             .arg(sectionCount)
                             .arg(rows_.size())
                             .arg(differenceCount));
    }

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
                addSelection(leftEditor_, row, QColor(70, 70, 70, 90));
                addSelection(rightEditor_, row, kHatch);
            } else if (line.rightNumber > 0) {
                addSelection(rightEditor_, row, QColor(70, 70, 70, 90));
                addSelection(leftEditor_, row, kHatch);
            }
        }
    }

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

    void refreshDirtyIndicators() {
        leftDirtyRows_ = dirtyRowIndices(true);
        rightDirtyRows_ = dirtyRowIndices(false);
        updateExtraSelections();
        leftEditor_->setDirtyRows(leftDirtyRows_);
        rightEditor_->setDirtyRows(rightDirtyRows_);
        QSet<int> bothDirty = leftDirtyRows_;
        bothDirty.unite(rightDirtyRows_);
        miniMap_->setDirtyRows(bothDirty);
    }

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

    void updateMinimapViewport() {
        const QScrollBar* bar = leftEditor_->verticalScrollBar();
        const qreal pageStep = qMax(1, bar->pageStep());
        const qreal total = qMax<qreal>(1.0, bar->maximum() + pageStep);
        const qreal start = static_cast<qreal>(bar->value()) / total;
        const qreal span = pageStep / total;
        miniMap_->setViewport(start, span);
    }

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

    void loadSide(bool left, const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            status_->setText("Could not open " + path);
            return;
        }
        const QString text = QTextStream(&file).readAll();
        file.close();

        flushPendingEdits();
        syncFromEditors();
        (left ? leftPath_ : rightPath_) = path;
        (left ? leftOriginal_ : rightOriginal_) = text.split('\n');
        (left ? leftDirtyLines_ : rightDirtyLines_).clear();
        if (left) leftUnsaved_ = false; else rightUnsaved_ = false;
        (left ? leftStructurallyEdited_ : rightStructurallyEdited_) = false;

        applyLanguageHighlighting();
        rebuild();
        refreshSideHeader(true);
        refreshSideHeader(false);
        refreshDirtyIndicators();
        if (onTitleChanged) onTitleChanged();
        status_->setText(QFileInfo(path).fileName() + " loaded.");
        // Only the side that was reloaded gets a history entry - Ctrl+Z
        // there will bring back the file that was previously compared.
        pushSideHistory(left);
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
        if (left) leftUnsaved_ = false; else rightUnsaved_ = false; 
        refreshDirtyIndicators();
        refreshSideHeader(left);
        status_->setText(QFileInfo(path).fileName() + " saved.");
        // Deliberately no history push: saving must not wipe the undo/redo
        // stacks, otherwise Ctrl+Z after a save would have nothing to walk
        // back to.
    }

    void applyPreviewEdit(bool left, const QString& text) {
        if (currentRow_ < 0 || currentRow_ >= rows_.size()) return;
        LineNumberEditor* editor = left ? leftEditor_ : rightEditor_;
        const int number = left ? rows_[currentRow_].leftNumber : rows_[currentRow_].rightNumber;
        if (number == 0) return;
        // Flush any debounced typing so the pre-edit state is its own entry.
        flushPendingEdits();
        QTextCursor cursor = cursorAtRow(editor, currentRow_);
        cursor.select(QTextCursor::LineUnderCursor);
        cursor.insertText(text);
        editor->setFocus();
        syncFromEditors();
        if (left) leftUnsaved_ = true; else rightUnsaved_ = true;
        pushSideHistory(left);
        refreshSideHeader(left);
    }

    QString leftPath_;
    QString rightPath_;
    QStringList leftOriginal_;
    QStringList rightOriginal_;
    QVector<TextDiffLine> rows_;
    QVector<DiffGroup> groups_;

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

    PathSelector* leftSelector_ = nullptr;
    PathSelector* rightSelector_ = nullptr;
    QLabel* leftAttrsLabel_ = nullptr;
    QLabel* rightAttrsLabel_ = nullptr;
    QToolButton* leftSaveButton_ = nullptr;
    QToolButton* rightSaveButton_ = nullptr;
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
    bool autoReloadPending_ = false;
    bool leftStructurallyEdited_ = false;
    bool rightStructurallyEdited_ = false;

    QSet<int> leftDirtyLines_;
    QSet<int> rightDirtyLines_;
    QSet<int> leftDirtyRows_;
    QSet<int> rightDirtyRows_;
    
    // Per-side "has unsaved edits of any kind" flag. Distinct from the
    // leftDirtyLines_/rightDirtyLines_ sets, which only cover copy-to-other
    // -side rows (and drive the green row highlight). Set on any content
    // change; cleared on save and on loadSide. Carried in SideSnapshot so
    // undo/redo restores it.
    bool leftUnsaved_ = false;                                   // <-- new
    bool rightUnsaved_ = false;                                  // <-- new

    QAction* saveAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* pixelMinimapAction_ = nullptr;
    QMenu* syntaxStyleMenu_ = nullptr;
    QAction* syntaxAutoAction_ = nullptr;
    QAction* syntaxPlainAction_ = nullptr;
    QAction* vibrantStyleAction_ = nullptr;
    QAction* classicStyleAction_ = nullptr;
    QAction* contrastStyleAction_ = nullptr;
    QAction* vscodeDarkStyleAction_ = nullptr;

    SyntaxHighlighter* leftHighlighter_ = nullptr;
    SyntaxHighlighter* rightHighlighter_ = nullptr;
    QVector<QVector<CharSegment>> leftInline_;
    QVector<QVector<CharSegment>> rightInline_;
    FindReplaceBar* findBar_ = nullptr;

    // Per-side undo/redo history. Each side is an independent stack of
    // historical versions of that side; Ctrl+Z / Ctrl+Y walk only the
    // focused pane's stack (see undoFocusedSide / redoFocusedSide).
    QVector<SideSnapshot> leftHistory_;
    int leftHistoryIndex_ = -1;
    QVector<SideSnapshot> rightHistory_;
    int rightHistoryIndex_ = -1;
    bool restoringHistory_ = false;
    QTimer* leftEditTimer_ = nullptr;
    QTimer* rightEditTimer_ = nullptr;
    // Remembers which pane last actually held focus, so Ctrl+Z still has a
    // target when focus is temporarily on the toolbar or a menu.
    LineNumberEditor* lastFocusedEditor_ = nullptr;
};

}  // namespace openbc::app
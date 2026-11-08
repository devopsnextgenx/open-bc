// virtual_text_compare_viewer.cpp — see the header.
#include "viewers/text/virtual/virtual_text_compare_viewer.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QToolBar>
#include <QVBoxLayout>

#include "common/compare_status.h"
#include "options/appearance_state.h"
#include "options/text_font.h"
#include "style/qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

namespace {

// Lock states. Red = read-only, green = editing; the flash cycles red, blue, yellow, red.
const QColor kLockRed(0xe5, 0x39, 0x35);
const QColor kLockGreen(0x2e, 0xa0, 0x43);
const QColor kLockBlue(0x1e, 0x88, 0xe5);
const QColor kLockYellow(0xfd, 0xd8, 0x35);
constexpr int kLockFlashStepMs = 100;   // 4 colours x 3 rounds x 100 ms = 1.2 s per flash
constexpr int kLockFlashSteps = 12;

// Painted instead of loaded: the icon set has no lock glyph, and this keeps the colour exact.
QIcon lockIcon(bool unlocked, const QColor& color) {
    const qreal size = 18, dpr = 2;
    QPixmap pm(QSize(static_cast<int>(size * dpr), static_cast<int>(size * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen shackle(color, size * 0.11, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(shackle);
    p.setBrush(Qt::NoBrush);
    QPainterPath arc;
    const QRectF top(size * 0.29, size * 0.10, size * 0.42, size * 0.42);
    arc.moveTo(size * 0.29, size * 0.48);
    arc.lineTo(size * 0.29, size * 0.31);
    arc.arcTo(top, 180, -180);
    if (unlocked) arc.lineTo(size * 0.71, size * 0.20);   // shackle lifted on one side
    else arc.lineTo(size * 0.71, size * 0.48);
    p.drawPath(arc);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(size * 0.17, size * 0.45, size * 0.66, size * 0.44), 2.2, 2.2);
    p.setBrush(QColor(0, 0, 0, 120));
    p.drawEllipse(QPointF(size * 0.5, size * 0.63), size * 0.07, size * 0.07);
    p.drawRect(QRectF(size * 0.5 - size * 0.025, size * 0.63, size * 0.05, size * 0.14));
    return QIcon(pm);
}

}  // namespace

// ---- VirtualTextCompareViewer ---------------------------------------------------------------------

VirtualTextCompareViewer::VirtualTextCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent)
    : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath) {
    buildUi();
    buildActions();
    wireEditors();
    applyFont();
    TextFont::instance().subscribe(this, [this]() { applyFont(); });
    // A change on disk asks before it throws unsaved edits away (the controller shows its banner first).
    changes_ = std::make_unique<FileChangeController>(
        this, [this](std::uint8_t) { startLoad(true); }, /*wholeTab=*/true,
        [this]() { return confirmDiscard(); });
    changes_->track(0, leftPath_);
    changes_->track(1, rightPath_);
    lockTimer_.setInterval(kLockFlashStepMs);
    lockTimer_.callOnTimeout([this] {
        ++lockFlashStep_;
        if (lockFlashStep_ >= kLockFlashSteps) {
            lockTimer_.stop();
            lockColor_ = writeEnabled_ ? kLockGreen : kLockRed;
        } else {
            static const QColor sequence[4] = {kLockRed, kLockBlue, kLockYellow, kLockRed};
            lockColor_ = sequence[lockFlashStep_ % 4];
        }
        updateLockIcon();
    });
    updateLockIcon();
    updateEditActions();
    startLoad(false);
}

VirtualTextCompareViewer::~VirtualTextCompareViewer() {
    if (loader_) loader_->cancel();
    if (taskRunner_) taskRunner_->cancel();
}

QString VirtualTextCompareViewer::title() const {
    const QString l = leftPath_.isEmpty() ? QStringLiteral("(missing)") : QFileInfo(leftPath_).fileName();
    const QString r = rightPath_.isEmpty() ? QStringLiteral("(missing)") : QFileInfo(rightPath_).fileName();
    return l + QStringLiteral(" <-> ") + r + (anyDirty() ? QStringLiteral(" *") : QString());
}

void VirtualTextCompareViewer::buildUi() {
    setObjectName(QStringLiteral("textCompareView"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    toolbar_ = new QToolBar(this);
    toolbar_->setObjectName(QStringLiteral("textCompareToolbar"));
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(20, 20));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    root->addWidget(toolbar_);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(1);
    const auto buildSideHeader = [this](bool left) {
        auto* frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("sideHeader"));
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(8, 4, 8, 4);
        layout->setSpacing(2);
        auto* pathRow = new QHBoxLayout;
        pathRow->setContentsMargins(0, 0, 0, 0);
        pathRow->setSpacing(4);
        PathSelector*& selector = left ? leftSelector_ : rightSelector_;
        selector = makeCommitSelector(
            frame, left ? tr("Left") : tr("Right"), left ? leftPath_ : rightPath_,
            QStringLiteral("All files (*.*)"),
            [this, left](QString path) { commitPath(left, path); });
        selector->setObjectName(QStringLiteral("sidePathSelector"));
        selector->combo()->setObjectName(QStringLiteral("sidePath"));
        selector->combo()->setToolTip(left ? leftPath_ : rightPath_);
        pathRow->addWidget(selector, 1);

        QToolButton*& saveButton = left ? leftSaveButton_ : rightSaveButton_;
        saveButton = new QToolButton(frame);
        saveButton->setObjectName(QStringLiteral("saveSideBtn"));
        saveButton->setIcon(icons::glyph(icons::Glyph::Save));
        saveButton->setIconSize(QSize(15, 15));
        saveButton->setToolTip(tr("Save this side (no unsaved changes)"));
        saveButton->setEnabled(false);
        QObject::connect(saveButton, &QToolButton::clicked, this, [this, left] { saveSide(left); });
        pathRow->addWidget(saveButton, 0);
        layout->addLayout(pathRow);

        QLabel*& attrs = left ? leftAttrsLabel_ : rightAttrsLabel_;
        attrs = new QLabel(frame);
        attrs->setObjectName(QStringLiteral("sideAttrs"));
        layout->addWidget(attrs);
        refreshSideHeader(left);
        return frame;
    };
    header->addWidget(buildSideHeader(true), 1);
    header->addWidget(buildSideHeader(false), 1);
    root->addLayout(header);

    auto* editors = new QSplitter(Qt::Horizontal, this);
    editors->setChildrenCollapsible(false);

    auto* leftPane = new QWidget(editors);
    auto* leftLayout = new QHBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    left_ = new VirtualTextEditor;
    overview_ = new DifferenceOverview(leftPane, true);
    right_ = new VirtualTextEditor;
    left_->setGutterSides(VirtualTextEditor::GutterSide::Left, VirtualTextEditor::GutterSide::Right);
    right_->setGutterSides(VirtualTextEditor::GutterSide::Right, VirtualTextEditor::GutterSide::Left);
    right_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftLayout->addWidget(overview_);
    leftLayout->addWidget(left_, 1);
    editors->addWidget(leftPane);
    auto* rightPane = new QWidget(editors);
    auto* rightLayout = new QHBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(right_, 1);
    editors->addWidget(rightPane);
    root->addWidget(editors, 1);

    // Sidebar on the right edge of the right editor with the previous / next section buttons.
    diffNavigator_ = new DiffNavigator(rightPane);
    rightLayout->addWidget(diffNavigator_);
    diffNavigator_->onPrevious = [this] { if (auto* e = activeEditor()) e->goToPreviousDifference(); };
    diffNavigator_->onNext = [this] { if (auto* e = activeEditor()) e->goToNextDifference(); };
    diffNavigator_->setEnabled(false);

    findBar_ = new QWidget;
    auto* fl = new QHBoxLayout(findBar_);
    fl->setContentsMargins(6, 3, 6, 3);
    fl->addWidget(new QLabel(tr("Find:")));
    findEdit_ = new QLineEdit;
    caseBox_ = new QCheckBox(tr("Match case"));
    auto* prev = new QPushButton(tr("Previous"));
    auto* next = new QPushButton(tr("Next"));
    auto* close = new QPushButton(tr("Close"));
    fl->addWidget(findEdit_, 1);
    fl->addWidget(caseBox_);
    fl->addWidget(prev);
    fl->addWidget(next);
    fl->addWidget(close);
    QObject::connect(findEdit_, &QLineEdit::returnPressed, this, [this] { findNext(true); });
    QObject::connect(next, &QPushButton::clicked, this, [this] { findNext(true); });
    QObject::connect(prev, &QPushButton::clicked, this, [this] { findNext(false); });
    QObject::connect(close, &QPushButton::clicked, this, [this] { showFindBar(false); });
    findBar_->hide();
    root->addWidget(findBar_);

    // Line-by-line compare strip: same widget, font and arrows as the Existing editor's.
    preview_ = new DiffLinePreview(this);
    preview_->setEditingEnabled(false);  // display only here; edits go through the editors
    preview_->onCopyLeftToRight = [this] { copyBlockAt(left_->currentRow(), Side::Left); };
    preview_->onCopyRightToLeft = [this] { copyBlockAt(left_->currentRow(), Side::Right); };
    root->addWidget(preview_);

    rulesBar_ = new QWidget;
    rulesBar_->setObjectName(QStringLiteral("rulesBar"));
    auto* rl = new QHBoxLayout(rulesBar_);
    rl->setContentsMargins(6, 3, 6, 3);
    rl->addWidget(new QLabel(tr("Rules:")));
    rulesEdit_ = new QLineEdit;
    rulesEdit_->setObjectName(QStringLiteral("rulesEdit"));
    rulesEdit_->setPlaceholderText(tr("timestamp, correlation-id, pod-123, /regex/, or a,,b for a literal comma"));
    rulesEdit_->setToolTip(tr("Parts of lines matching these samples or /regular expressions/ are ignored, so lines that "
                              "differ only there count as matched."));
    auto* applyRulesButton = new QPushButton(tr("Apply"));
    auto* clearRulesButton = new QPushButton(tr("Clear"));
    rulesStatus_ = new QLabel;
    rl->addWidget(rulesEdit_, 1);
    rl->addWidget(applyRulesButton);
    rl->addWidget(clearRulesButton);
    rl->addWidget(rulesStatus_);
    QObject::connect(rulesEdit_, &QLineEdit::returnPressed, this, [this] { applyRules(rulesEdit_->text()); });
    QObject::connect(applyRulesButton, &QPushButton::clicked, this, [this] { applyRules(rulesEdit_->text()); });
    QObject::connect(clearRulesButton, &QPushButton::clicked, this, [this] {
        rulesEdit_->clear();
        applyRules(QString());
    });
    rulesBar_->hide();
    root->addWidget(rulesBar_);

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("compareStatus"));
    status_->setContentsMargins(6, 2, 6, 2);
    lockButton_ = new QToolButton(this);
    lockButton_->setObjectName(QStringLiteral("lockButton"));
    lockButton_->setAutoRaise(true);
    lockButton_->setIconSize(QSize(18, 18));
    lockButton_->setFocusPolicy(Qt::NoFocus);
    QObject::connect(lockButton_, &QToolButton::clicked, this, [this] { setWriteMode(!writeEnabled_); });
    editBadge_ = new QLabel;
    editBadge_->setObjectName(QStringLiteral("textEditabilityStatus"));
    editBadge_->setText(compare_status::badgeText(false));
    editBadge_->setStyleSheet(compare_status::badgeStyle(false, false));
    auto* statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(0, 0, 4, 0);
    statusRow->setSpacing(8);
    statusRow->addWidget(status_, 1);
    statusRow->addWidget(editBadge_, 0);
    statusRow->addWidget(lockButton_, 0);
    root->addLayout(statusRow);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(3);
    progress_->hide();
    root->addWidget(progress_);
}

void VirtualTextCompareViewer::buildActions() {
    auto make = [this](const QString& text, const QKeySequence& key, std::function<void()> fn) {
        auto* a = new QAction(text, this);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        QObject::connect(a, &QAction::triggered, this, [fn = std::move(fn)](bool) { fn(); });
        addAction(a);
        return a;
    };
    homeAction_ = toolbar_->addAction(icons::glyph(icons::Glyph::Home), tr("Home"));
    sessionsAction_ = toolbar_->addAction(icons::glyph(icons::Glyph::Sessions), tr("Sessions"));
    toolbar_->addSeparator();
    QObject::connect(homeAction_, &QAction::triggered, this,
                     [this] { if (onHomeRequested) onHomeRequested(); });
    QObject::connect(sessionsAction_, &QAction::triggered, this,
                     [this] { if (onSessionsRequested) onSessionsRequested(); });

    const auto unavailable = [this](icons::Glyph glyph, const QString& text, const QString& reason) {
        QAction* action = toolbar_->addAction(icons::glyph(glyph), text);
        action->setEnabled(false);
        action->setToolTip(reason);
        return action;
    };
    QAction* showAllAction =
        unavailable(icons::Glyph::ShowAll, tr("All"), tr("Row filtering is unavailable in the Virtual editor"));
    showAllAction->setCheckable(true);
    showAllAction->setChecked(true);
    QAction* showDiffsAction =
        unavailable(icons::Glyph::ShowDiffs, tr("Diffs"), tr("Row filtering is unavailable in the Virtual editor"));
    showDiffsAction->setCheckable(true);
    QAction* contextAction =
        unavailable(icons::Glyph::Context, tr("Context"), tr("Row filtering is unavailable in the Virtual editor"));
    contextAction->setCheckable(true);
    toolbar_->addSeparator();
    unavailable(icons::Glyph::Minor, tr("Minor"), tr("Whitespace filtering is unavailable in the Virtual editor"));
    rulesAction_ = toolbar_->addAction(icons::glyph(icons::Glyph::Rules), tr("Rules"));
    rulesAction_->setCheckable(true);
    rulesAction_->setToolTip(tr("Ignore parts of lines that match samples or /regular expressions/"));
    QObject::connect(rulesAction_, &QAction::toggled, this, [this](bool on) { showRulesBar(on); });
    unavailable(icons::Glyph::Format, tr("Word Wrap"), tr("Word wrapping is unavailable in the Virtual editor"));
    toolbar_->addSeparator();

    minimapAction_ = toolbar_->addAction(icons::glyph(icons::Glyph::MinimapPixel), tr("Minimap"));
    minimapAction_->setCheckable(true);
    minimapAction_->setChecked(true);
    minimapAction_->setToolTip(tr("Toggle pixel-line versus block markers in the minimap"));
    findAction_ = make(tr("&Find..."), QKeySequence::Find, [this] { showFindBar(true); });
    findAction_->setIcon(icons::glyph(icons::Glyph::Find));
    toolbar_->addAction(findAction_);
    toolbar_->addSeparator();

    copyDiffAction_ = toolbar_->addAction(icons::glyph(icons::Glyph::CopyLine), tr("Copy"));
    copyDiffAction_->setToolTip(tr("Copy the difference at the caret to the other side (needs write mode)"));
    QObject::connect(copyDiffAction_, &QAction::triggered, this, [this] { copyCurrentBlock(); });
    toolbar_->addSeparator();
    nextDiffAction_ = make(tr("&Next Difference"), QKeySequence(Qt::Key_F8),
                           [this] { if (auto* e = activeEditor()) e->goToNextDifference(); });
    nextDiffAction_->setIcon(icons::glyph(icons::Glyph::NextSection));
    toolbar_->addAction(nextDiffAction_);
    prevDiffAction_ = make(tr("P&revious Difference"), QKeySequence(Qt::SHIFT | Qt::Key_F8),
                           [this] { if (auto* e = activeEditor()) e->goToPreviousDifference(); });
    prevDiffAction_->setIcon(icons::glyph(icons::Glyph::PrevSection));
    toolbar_->addAction(prevDiffAction_);
    toolbar_->addSeparator();

    swapAction_ = make(tr("S&wap Sides"), QKeySequence(), [this] {
        if (!confirmDiscard()) return;
        std::swap(leftPath_, rightPath_);
        changes_->track(0, leftPath_);
        changes_->track(1, rightPath_);
        if (onTitleChanged) onTitleChanged();
        startLoad(false);
    });
    swapAction_->setIcon(icons::glyph(icons::Glyph::Swap));
    toolbar_->addAction(swapAction_);
    reloadAction_ = make(tr("&Reload"), QKeySequence(Qt::Key_F5), [this] {
        if (confirmDiscard()) reload(true);
    });
    reloadAction_->setIcon(icons::glyph(icons::Glyph::Refresh));
    toolbar_->addAction(reloadAction_);

    saveAction_ = make(tr("&Save"), QKeySequence::Save, [this] { saveActive(false); });
    saveAsAction_ = make(tr("Save &As..."), QKeySequence::SaveAs, [this] { saveActive(true); });
    undoAction_ = make(tr("&Undo"), QKeySequence::Undo, [this] { activeEditor()->undo(); });
    redoAction_ = make(tr("&Redo"), QKeySequence::Redo, [this] { activeEditor()->redo(); });

    copyAction_ = make(tr("&Copy"), QKeySequence::Copy, [this] { if (auto* e = activeEditor()) e->copySelection(); });
    selectAllAction_ = make(tr("Select &All"), QKeySequence::SelectAll, [this] { if (auto* e = activeEditor()) e->selectAllText(); });
    findNextAction_ = make(tr("Find &Next"), QKeySequence(Qt::Key_F3), [this] { findNext(true); });
    findPrevAction_ = make(tr("Find &Previous"), QKeySequence(Qt::SHIFT | Qt::Key_F3), [this] { findNext(false); });
    gotoLineAction_ = make(tr("&Go to Line..."), QKeySequence(Qt::CTRL | Qt::Key_G), [this] {
        VirtualTextEditor* e = activeEditor();
        if (!e || !model_) return;
        bool ok = false;
        const quint64 max = qMax<quint64>(1, model_->lineCount(e->side()));
        const qlonglong v = QInputDialog::getInt(this, tr("Go to Line"), tr("Line number (1 - %1):").arg(max), 1, 1,
                                                 static_cast<int>(qMin<quint64>(max, 2147483647)), 1, &ok);
        if (ok) e->goToLine(static_cast<quint64>(v));
    });
    syntaxAction_ = make(tr("Syntax &Highlighting"), QKeySequence(), [this] {
        left_->setSyntaxEnabled(syntaxAction_->isChecked());
        right_->setSyntaxEnabled(syntaxAction_->isChecked());
    });
    syntaxAction_->setCheckable(true);
    syntaxAction_->setChecked(true);

    QObject::connect(minimapAction_, &QAction::toggled, this,
                     [this](bool on) { overview_->setPixelLineMode(on); });
}

void VirtualTextCompareViewer::wireEditors() {
    auto link = [this](VirtualTextEditor* self, VirtualTextEditor* peer) {
        self->onTopRowChanged = [this, peer, self](quint64 row) {
            if (!syncing_) { syncing_ = true; peer->setFirstVisibleRow(row); syncing_ = false; }
            overview_->setViewportRows(row, static_cast<quint64>(self->visibleRowCount()));
        };
        self->onHorizontalOffsetChanged = [this, peer](int px) {
            if (!syncing_) { syncing_ = true; peer->setHorizontalPixelOffset(px); syncing_ = false; }
        };
        self->onCurrentRowChanged = [this, peer](quint64 row) {
            peer->setCurrentRowQuietly(row);
            overview_->setVirtualCurrentRow(row);
            updatePreview(row);
        };
        self->onFocused = [this, self]() {
            lastFocused_ = self;
            updateEditActions();
        };
        self->onEditBlocked = [this]() { flashLock(); };
        self->onEdited = [this](const ModelEdit& edit) { handleEdited(edit); };
        self->onCopyBlockRequested = [this, self](quint64 row) { copyBlockAt(row, self->side()); };
        self->onStatusMessage = [this](const QString& text) { status_->setText(text); };
    };
    link(left_, right_);
    link(right_, left_);
    overview_->onVirtualRowClicked = [this](quint64 row) {
        const quint64 halfViewport = static_cast<quint64>(left_->visibleRowCount() / 2);
        left_->setFirstVisibleRow(row > halfViewport ? row - halfViewport : 0);
    };
    overview_->onResized = [this] { updateOverview(); };
}

VirtualTextEditor* VirtualTextCompareViewer::activeEditor() const {
    if (right_->hasFocus()) return right_;
    if (left_->hasFocus()) return left_;
    return lastFocused_ ? lastFocused_ : left_;
}

void VirtualTextCompareViewer::applyFont() {
    const QFont f = TextFont::instance().font();
    left_->setEditorFont(f);
    right_->setEditorFont(f);
    if (preview_) preview_->setPreviewFont(f);  // the strip uses exactly the editors' font and size
}

void VirtualTextCompareViewer::updateOverview() {
    if (!model_ || !overview_) return;
    overview_->setVirtualOverview(model_->overview(qMax(1, overview_->height())), model_->rowCount());
    overview_->setViewportRows(left_->firstVisibleRow(),
                               static_cast<quint64>(left_->visibleRowCount()));
    overview_->setVirtualCurrentRow(left_->currentRow());
}

void VirtualTextCompareViewer::refreshSideHeader(bool left) {
    PathSelector* selector = left ? leftSelector_ : rightSelector_;
    QLabel* attrs = left ? leftAttrsLabel_ : rightAttrsLabel_;
    const QString& path = left ? leftPath_ : rightPath_;
    if (!selector || !attrs) return;
    selector->setText(path);
    selector->combo()->setToolTip(path);

    const QFileInfo info(path);
    if (!info.exists()) {
        attrs->setText(path.isEmpty() ? tr("(missing on this side)") : tr("(unavailable)"));
        return;
    }

    static const char* const encodings[] = {"UTF-8", "UTF-16 LE", "UTF-16 BE", "?"};
    QString encoding = tr("UTF-8");
    if (model_) {
        const int code = model_->encodingCode(left ? Side::Left : Side::Right);
        encoding = QString::fromLatin1(encodings[qMin(code & 0x7f, 3)]);
        if (code & 0x80) encoding += tr(" (binary?)");
    }
    QString line = QStringLiteral("%1    %2    %3")
                       .arg(info.lastModified().toString(QStringLiteral("M/d/yyyy h:mm:ss AP")),
                            QLocale().toString(info.size()) + tr(" bytes"), encoding);
    if (model_ && model_->isDirty(left ? Side::Left : Side::Right)) line += tr("    ● modified");
    attrs->setText(line);
}

void VirtualTextCompareViewer::commitPath(bool left, const QString& path) {
    const QString selectedPath = path.trimmed();
    if (selectedPath.isEmpty()) return;
    QString& currentPath = left ? leftPath_ : rightPath_;
    PathSelector* selector = left ? leftSelector_ : rightSelector_;
    if (currentPath == selectedPath) {
        selector->setText(selectedPath);
        return;
    }
    if (!confirmDiscard()) {                 // keep the edits: put the old path back
        selector->setText(currentPath);
        return;
    }
    currentPath = selectedPath;
    changes_->track(left ? 0 : 1, selectedPath);
    selector->remember(selectedPath);
    selector->setText(selectedPath);
    if (onTitleChanged) onTitleChanged();
    startLoad(false);
}

void VirtualTextCompareViewer::applyDroppedPaths(const QString& left, const QString& right) {
    if (left.isEmpty() && right.isEmpty()) return;
    if (!confirmDiscard()) return;
    if (!left.isEmpty()) {
        leftPath_ = left;
        changes_->track(0, leftPath_);
        leftSelector_->remember(left);
        leftSelector_->setText(left);
    }
    if (!right.isEmpty()) {
        rightPath_ = right;
        changes_->track(1, rightPath_);
        rightSelector_->remember(right);
        rightSelector_->setText(right);
    }
    if (onTitleChanged) onTitleChanged();
    startLoad(false);
}

void VirtualTextCompareViewer::reload(bool keepScroll) {
    changes_->track(0, leftPath_);
    changes_->track(1, rightPath_);
    startLoad(keepScroll);
}

void VirtualTextCompareViewer::startLoad(bool keepScroll) {
    refreshSideHeader(true);
    refreshSideHeader(false);
    if (leftPath_.isEmpty() && rightPath_.isEmpty()) {
        status_->setText(tr("Drop two files here to compare them."));
        return;
    }
    keepScrollOnLoad_ = keepScroll;
    savedTopRow_ = keepScroll ? left_->firstVisibleRow() : 0;
    loading_ = true;
    progress_->setValue(0);
    progress_->show();
    status_->setText(tr("Comparing..."));
    if (!loader_) loader_ = std::make_unique<TextModelLoader>();
    loader_->onProgress = [this](const QString& phase, quint64 done, quint64 total) {
        progress_->setValue(total ? static_cast<int>(done * 100 / total) : 0);
        status_->setText(tr("Comparing (%1)...").arg(phase));
    };
    loader_->onFinished = [this](std::shared_ptr<TextModel> model) { onModelReady(std::move(model)); };
    loader_->onFailed = [this](const QString& message) {
        loading_ = false;
        progress_->hide();
        status_->setText(message == QLatin1String("cancelled") ? tr("Cancelled.") : tr("Could not compare: %1").arg(message));
    };
    ModelOptions options;
    options.bufferedIo = bufferedIo_;
    loader_->start(leftPath_, rightPath_, options);
}

void VirtualTextCompareViewer::onModelReady(std::shared_ptr<TextModel> model) {
    loading_ = false;
    progress_->hide();
    model_ = std::move(model);
    const QString name = QFileInfo(rightPath_.isEmpty() ? leftPath_ : rightPath_).fileName();
    const bool dark = palette().color(QPalette::Base).lightness() < 128;
    for (VirtualTextEditor* e : {left_, right_}) {
        e->setLanguage(syntaxAction_->isChecked() ? name : QString());
        e->setDarkSyntax(dark);
    }
    left_->setModel(model_, Side::Left);
    right_->setModel(model_, Side::Right);
    updateOverview();
    refreshSideHeader(true);
    refreshSideHeader(false);
    if (keepScrollOnLoad_) left_->setFirstVisibleRow(savedTopRow_);
    overview_->setViewportRows(left_->firstVisibleRow(),
                               static_cast<quint64>(left_->visibleRowCount()));
    overview_->setVirtualCurrentRow(left_->currentRow());
    for (VirtualTextEditor* e : {left_, right_}) e->setReadOnly(!writeEnabled_);
    updateStatus();
    refreshDirtyUi();
    updateEditActions();
    if (onTitleChanged) onTitleChanged();
    if (!rulesText_.isEmpty()) {            // rules survive reloads, swaps and path changes
        const QString text = rulesText_;
        rulesText_.clear();
        applyRules(text);
    }
}

void VirtualTextCompareViewer::updateStatus() {
    if (editBadge_) {
        editBadge_->setText(compare_status::badgeText(writeEnabled_));
        editBadge_->setStyleSheet(compare_status::badgeStyle(writeEnabled_, false));
    }
    if (!model_) {
        if (preview_) preview_->clear();
        return;
    }
    const OpenBcTmStats s = model_->stats();
    static const char* const kEnc[] = {"UTF-8", "UTF-16 LE", "UTF-16 BE", "?"};
    auto enc = [this](Side side) {
        const int code = model_->encodingCode(side);
        return QString::fromLatin1(kEnc[qMin(code & 0x7f, 3)]) + ((code & 0x80) ? tr(" (binary?)") : QString());
    };
    // Same wording as the Existing editor: "N difference section(s) | M line(s) | K changed row(s)".
    const quint64 changedRows = s.insertedRows + s.deletedRows + s.replacedRows;
    QString text = compare_status::summaryText(s.blocks, s.rows, changedRows);
    if (anyDirty()) text += tr("   |   unsaved changes");
    if (!rulesText_.isEmpty()) text += tr("   |   rules active");
    if (s.blocks == 0 && s.rows > 0) text = tr("Files are identical   |   ") + text;
    if (s.degraded) text += tr("   |   coarse alignment used for a very large unmatched region");
    status_->setText(text);
    status_->setToolTip(tr("%1 / %2 · Virtual editor (%3)")
                            .arg(enc(Side::Left), enc(Side::Right), writeEnabled_ ? tr("editing") : tr("read-only")));
    if (diffNavigator_) diffNavigator_->setEnabled(s.blocks > 0);
    updatePreview(left_->currentRow());
}

// Fills the line-by-line strip from the model's row (the lines are read straight from the engine).
void VirtualTextCompareViewer::updatePreview(quint64 row) {
    if (!preview_) return;
    if (!model_ || row >= model_->rowCount()) {
        preview_->clear();
        return;
    }
    const ModelRow m = model_->row(row);
    const auto toSegments = [](const QVector<InlineRange>& ranges) {
        QVector<CharSegment> out;
        for (const InlineRange& r : ranges)
            out.push_back({r.start, r.length, r.whitespaceOnly ? CharSegmentKind::WhitespaceMismatch
                                                               : CharSegmentKind::Mismatch});
        return out;
    };
    TextDiffLine line;
    line.changed = m.changed();
    QVector<InlineRange> leftRanges, rightRanges;
    if (m.left >= 0) {
        line.leftNumber = static_cast<int>(m.left + 1);
        line.left = model_->window(Side::Left, static_cast<quint64>(m.left), 0, VirtualTextEditor::kMaxRenderChars).text;
        if (m.kind == RowKind::Replace) leftRanges = model_->inlineRanges(row, Side::Left, line.left);
    }
    if (m.right >= 0) {
        line.rightNumber = static_cast<int>(m.right + 1);
        line.right = model_->window(Side::Right, static_cast<quint64>(m.right), 0, VirtualTextEditor::kMaxRenderChars).text;
        if (m.kind == RowKind::Replace) rightRanges = model_->inlineRanges(row, Side::Right, line.right);
    }
    bool onlyWhitespace = m.kind == RowKind::Replace && !(leftRanges.isEmpty() && rightRanges.isEmpty());
    for (const auto* ranges : {&leftRanges, &rightRanges})
        for (const InlineRange& r : *ranges) onlyWhitespace = onlyWhitespace && r.whitespaceOnly;
    line.whitespaceOnly = onlyWhitespace;
    preview_->setRow(line, toSegments(leftRanges), toSegments(rightRanges));
}

// ---- write mode and the lock icon -----------------------------------------------------------------

void VirtualTextCompareViewer::updateLockIcon() {
    if (!lockFlashing()) lockColor_ = writeEnabled_ ? kLockGreen : kLockRed;
    if (!lockButton_) return;
    lockButton_->setIcon(lockIcon(writeEnabled_, lockColor_));
    lockButton_->setToolTip(writeEnabled_ ? tr("Editing is enabled - click to lock (read-only)")
                                          : tr("Read-only - click to unlock and enable editing"));
}

void VirtualTextCompareViewer::flashLock() {
    lockFlashStep_ = 0;
    lockColor_ = kLockRed;            // first colour of red-blue-yellow-red
    lockTimer_.start();               // restarts the 1.2 s sequence when a flash is already running
    updateLockIcon();
    status_->setText(tr("Read-only: click the lock icon to enable editing."));
}

void VirtualTextCompareViewer::setWriteMode(bool enabled) {
    if (writeEnabled_ == enabled) return;
    writeEnabled_ = enabled;
    for (VirtualTextEditor* e : {left_, right_}) e->setReadOnly(!enabled);
    if (lockTimer_.isActive()) lockTimer_.stop();
    updateLockIcon();
    updateEditActions();
    updateStatus();
#ifdef Q_OS_WIN
    // A memory-mapped file cannot be replaced on Windows. Nothing can be dirty before the first
    // unlock, so reopen with buffered I/O once (it keeps the scroll position).
    if (enabled && !bufferedIo_ && model_ && !anyDirty()) {
        bufferedIo_ = true;
        startLoad(true);
    }
#endif
}

void VirtualTextCompareViewer::updateEditActions() {
    VirtualTextEditor* e = activeEditor();
    // While locked the actions stay enabled so using them flashes the lock; in write mode they
    // follow what can actually be undone.
    undoAction_->setEnabled(!writeEnabled_ || (model_ && e->canUndo()));
    redoAction_->setEnabled(!writeEnabled_ || (model_ && e->canRedo()));
    saveAction_->setEnabled(model_ && model_->isDirty(e->side()));
    saveAsAction_->setEnabled(model_ != nullptr);
    copyDiffAction_->setEnabled(model_ != nullptr);
}

bool VirtualTextCompareViewer::anyDirty() const {
    return model_ && (model_->isDirty(Side::Left) || model_->isDirty(Side::Right));
}

bool VirtualTextCompareViewer::hasUnsavedChanges() const { return anyDirty(); }

void VirtualTextCompareViewer::refreshDirtyUi() {
    for (const bool left : {true, false}) {
        const Side side = left ? Side::Left : Side::Right;
        const bool dirty = model_ && model_->isDirty(side);
        QToolButton* button = left ? leftSaveButton_ : rightSaveButton_;
        button->setEnabled(dirty);
        const QString path = left ? leftPath_ : rightPath_;
        button->setToolTip(dirty ? (path.isEmpty() ? tr("Save this side as...") : tr("Save %1").arg(path))
                                 : tr("Save this side (no unsaved changes)"));
        refreshSideHeader(left);
    }
    const bool dirty = anyDirty();
    if (dirty != lastDirty_) {
        lastDirty_ = dirty;
        if (onTitleChanged) onTitleChanged();
    }
}

void VirtualTextCompareViewer::handleEdited(const ModelEdit& edit) {
    left_->modelEdited(edit);
    right_->modelEdited(edit);
    updateOverview();
    updateStatus();
    refreshDirtyUi();
    updateEditActions();
}

void VirtualTextCompareViewer::copyBlockAt(quint64 row, Side from) {
    if (!model_) return;
    if (!writeEnabled_) {
        flashLock();
        return;
    }
    QString error;
    const auto edit = model_->copyBlock(from, row, &error);
    if (!edit) {
        status_->setText(error.isEmpty() ? tr("Nothing to copy here.") : error);
        return;
    }
    handleEdited(*edit);
    VirtualTextEditor* destination = edit->side == Side::Left ? left_ : right_;
    VirtualTextEditor* source = from == Side::Left ? left_ : right_;
    destination->setCurrentRowQuietly(qMin<quint64>(edit->firstRow, model_->rowCount() ? model_->rowCount() - 1 : 0));
    source->setCurrentRowQuietly(destination->currentRow());
}

void VirtualTextCompareViewer::copyCurrentBlock() {
    if (!model_) return;
    if (!writeEnabled_) {
        flashLock();
        return;
    }
    VirtualTextEditor* e = activeEditor();
    if (!model_->blockAt(e->currentRow())) {
        status_->setText(tr("Place the caret in a difference (or press F8) to copy it to the other side."));
        return;
    }
    copyBlockAt(e->currentRow(), e->side());
}

// ---- saving ---------------------------------------------------------------------------------------

void VirtualTextCompareViewer::saveActive(bool saveAs) {
    VirtualTextEditor* e = activeEditor();
    const bool left = e->side() == Side::Left;
    if (!saveAs) {
        saveSide(left);
        return;
    }
    if (!model_ || taskBusy()) return;
    const QString current = left ? leftPath_ : rightPath_;
    const QString target = QFileDialog::getSaveFileName(this, tr("Save As"), current, tr("All files (*.*)"));
    if (target.isEmpty()) return;
    if (!taskRunner_) taskRunner_ = std::make_unique<ModelTaskRunner>();
    const Side side = e->side();
    taskRunner_->onProgress = {};
    taskRunner_->onFailed = [this](const QString& message) {
        progress_->hide();
        status_->setText(tr("Could not save: %1").arg(message));
    };
    taskRunner_->onFinished = [this, left, target](const QString&) {
        (left ? leftPath_ : rightPath_) = target;
        changes_->track(left ? 0 : 1, target);   // our own write is not an external change
        (left ? leftSelector_ : rightSelector_)->remember(target);
        progress_->hide();
        refreshDirtyUi();
        updateEditActions();
        status_->setText(tr("Saved %1").arg(target));
        if (onTitleChanged) onTitleChanged();
    };
    progress_->setValue(0);
    progress_->show();
    status_->setText(tr("Saving..."));
    taskRunner_->start(model_, ModelTaskRunner::Task::SaveAs, side, target);
}

void VirtualTextCompareViewer::saveSide(bool left, SaveDone done) {
    const auto finish = [done](bool ok, const QString& error) { if (done) done(ok, error); };
    if (!model_) { finish(false, tr("Nothing is loaded.")); return; }
    const Side side = left ? Side::Left : Side::Right;
    if (!model_->isDirty(side)) { finish(true, QString()); return; }
    if (taskBusy()) { finish(false, tr("Another operation is still running.")); return; }
    QString saveAsPath;
    if ((left ? leftPath_ : rightPath_).isEmpty()) {          // a side that had no file yet
        saveAsPath = QFileDialog::getSaveFileName(this, left ? tr("Save left side as") : tr("Save right side as"),
                                                  QString(), tr("All files (*.*)"));
        if (saveAsPath.isEmpty()) { finish(false, tr("Save was cancelled.")); return; }
    }
    if (!taskRunner_) taskRunner_ = std::make_unique<ModelTaskRunner>();
    taskRunner_->onProgress = {};
    taskRunner_->onFailed = [this, finish](const QString& message) {
        progress_->hide();
        status_->setText(tr("Could not save: %1").arg(message));
        finish(false, message);
    };
    taskRunner_->onFinished = [this, left, saveAsPath, finish](const QString& written) {
        QString& path = left ? leftPath_ : rightPath_;
        if (!saveAsPath.isEmpty()) {
            path = saveAsPath;
            (left ? leftSelector_ : rightSelector_)->remember(saveAsPath);
        }
        changes_->track(left ? 0 : 1, path);       // re-baseline: our own write must not raise "changed on disk"
        progress_->hide();
        refreshDirtyUi();
        updateEditActions();
        updateStatus();
        status_->setText(tr("Saved %1").arg(written.isEmpty() ? path : written));
        if (onTitleChanged) onTitleChanged();
        finish(true, QString());
    };
    progress_->setValue(0);
    progress_->show();
    status_->setText(tr("Saving..."));
    taskRunner_->start(model_, saveAsPath.isEmpty() ? ModelTaskRunner::Task::Save : ModelTaskRunner::Task::SaveAs, side,
                       saveAsPath);
}

bool VirtualTextCompareViewer::confirmDiscard() {
    if (!anyDirty()) return true;
    const auto info = [this](Side side, const QString& path) {
        return UnsavedSide{path, model_->isDirty(side) ? static_cast<int>(qMax<quint64>(1, model_->editCount(side))) : 0};
    };
    return confirmUnsavedChanges(this, info(Side::Left, leftPath_), info(Side::Right, rightPath_),
                                 [this](bool left, SaveDone done) { saveSide(left, std::move(done)); });
}

// ---- rules ------------------------------------------------------------------------------------------

void VirtualTextCompareViewer::showRulesBar(bool show) {
    rulesBar_->setVisible(show);
    if (show) rulesEdit_->setFocus();
    else if (VirtualTextEditor* e = activeEditor()) e->setFocus();
}

void VirtualTextCompareViewer::applyRules(const QString& text) {
    if (rulesEdit_->text() != text) rulesEdit_->setText(text);
    if (!model_) {
        rulesText_ = text;
        return;
    }
    if (text == model_->rulesText() && text == rulesText_) return;
    if (taskBusy()) {
        rulesStatus_->setText(tr("Busy - try again in a moment."));
        return;
    }
    if (!taskRunner_) taskRunner_ = std::make_unique<ModelTaskRunner>();
    taskRunner_->onProgress = [this](const QString&, quint64 done, quint64 total) {
        progress_->setValue(total ? static_cast<int>(done * 100 / total) : 0);
    };
    taskRunner_->onFailed = [this](const QString& message) {
        progress_->hide();
        rulesStatus_->setStyleSheet(QStringLiteral("color: #e53935;"));
        rulesStatus_->setText(message == QLatin1String("cancelled") ? tr("Cancelled.") : message);
        status_->setText(tr("Rules not applied: %1").arg(message));
    };
    taskRunner_->onFinished = [this, text](const QString&) {
        rulesText_ = text;
        progress_->hide();
        rulesStatus_->setStyleSheet(QString());
        rulesStatus_->setText(text.isEmpty() ? QString() : tr("active"));
        left_->invalidateAll();
        right_->invalidateAll();
        updateOverview();
        updateStatus();
        updateEditActions();
    };
    progress_->setValue(0);
    progress_->show();
    taskRunner_->start(model_, ModelTaskRunner::Task::SetRules, Side::Left, text);
}

void VirtualTextCompareViewer::showFindBar(bool show) {
    findBar_->setVisible(show);
    if (show) { findEdit_->setFocus(); findEdit_->selectAll(); }
    else if (VirtualTextEditor* e = activeEditor()) e->setFocus();
}

void VirtualTextCompareViewer::findNext(bool forward) {
    const QString text = findEdit_->text();
    if (text.isEmpty()) { showFindBar(true); return; }
    VirtualTextEditor* e = activeEditor();
    if (!e->findText(text, caseBox_->isChecked(), forward)) return;
    e->setFocus();
}

}  // namespace openbc::app

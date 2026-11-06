// virtual_text_compare_viewer.cpp — see the header.
#include "viewers/text/virtual/virtual_text_compare_viewer.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QToolBar>
#include <QVBoxLayout>

#include "options/appearance_state.h"
#include "options/text_font.h"
#include "style/qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

// ---- VirtualTextCompareViewer ---------------------------------------------------------------------

VirtualTextCompareViewer::VirtualTextCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent)
    : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath) {
    buildUi();
    buildActions();
    wireEditors();
    applyFont();
    TextFont::instance().subscribe(this, [this]() { applyFont(); });
    changes_ = std::make_unique<FileChangeController>(
        this, [this](std::uint8_t) { startLoad(true); }, /*wholeTab=*/true);
    changes_->track(0, leftPath_);
    changes_->track(1, rightPath_);
    startLoad(false);
}

VirtualTextCompareViewer::~VirtualTextCompareViewer() {
    if (loader_) loader_->cancel();
}

QString VirtualTextCompareViewer::title() const {
    const QString l = leftPath_.isEmpty() ? QStringLiteral("(missing)") : QFileInfo(leftPath_).fileName();
    const QString r = rightPath_.isEmpty() ? QStringLiteral("(missing)") : QFileInfo(rightPath_).fileName();
    return l + QStringLiteral(" <-> ") + r;
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
        saveButton->setToolTip(tr("Saving is unavailable in the Virtual editor (read-only)"));
        saveButton->setEnabled(false);
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
    editors->addWidget(right_);
    root->addWidget(editors, 1);

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

    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("compareStatus"));
    status_->setContentsMargins(6, 2, 6, 2);
    root->addWidget(status_);

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
    unavailable(icons::Glyph::Rules, tr("Rules"), tr("Ignore rules are unavailable in the Virtual editor"));
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
    copyDiffAction_->setToolTip(tr("Copy changes to the other side (unavailable in the read-only Virtual editor)"));
    copyDiffAction_->setEnabled(false);
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
        std::swap(leftPath_, rightPath_);
        changes_->track(0, leftPath_);
        changes_->track(1, rightPath_);
        if (onTitleChanged) onTitleChanged();
        startLoad(false);
    });
    swapAction_->setIcon(icons::glyph(icons::Glyph::Swap));
    toolbar_->addAction(swapAction_);
    reloadAction_ = make(tr("&Reload"), QKeySequence(Qt::Key_F5), [this] { reload(true); });
    reloadAction_->setIcon(icons::glyph(icons::Glyph::Refresh));
    toolbar_->addAction(reloadAction_);

    saveAction_ = make(tr("&Save"), QKeySequence::Save, [] {});
    undoAction_ = make(tr("&Undo"), QKeySequence::Undo, [] {});
    redoAction_ = make(tr("&Redo"), QKeySequence::Redo, [] {});
    for (QAction* readOnlyAction : {saveAction_, undoAction_, redoAction_}) {
        readOnlyAction->setEnabled(false);
        readOnlyAction->setToolTip(tr("Unavailable in the Virtual editor (read-only)"));
    }

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
        };
        self->onFocused = [this, self]() { lastFocused_ = self; };
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
    attrs->setText(QStringLiteral("%1    %2    %3")
                       .arg(info.lastModified().toString(QStringLiteral("M/d/yyyy h:mm:ss AP")),
                            QLocale().toString(info.size()) + tr(" bytes"), encoding));
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
    currentPath = selectedPath;
    changes_->track(left ? 0 : 1, selectedPath);
    selector->remember(selectedPath);
    selector->setText(selectedPath);
    if (onTitleChanged) onTitleChanged();
    startLoad(false);
}

void VirtualTextCompareViewer::applyDroppedPaths(const QString& left, const QString& right) {
    if (left.isEmpty() && right.isEmpty()) return;
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
    loader_->start(leftPath_, rightPath_, ModelOptions{});
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
    updateStatus();
    if (onTitleChanged) onTitleChanged();
}

void VirtualTextCompareViewer::updateStatus() {
    if (!model_) return;
    const OpenBcTmStats s = model_->stats();
    static const char* const kEnc[] = {"UTF-8", "UTF-16 LE", "UTF-16 BE", "?"};
    auto enc = [this](Side side) {
        const int code = model_->encodingCode(side);
        return QString::fromLatin1(kEnc[qMin(code & 0x7f, 3)]) + ((code & 0x80) ? tr(" (binary?)") : QString());
    };
    QString text = tr("%1 rows · %2 difference blocks · %3 / %4 · Virtual editor (read-only)")
                       .arg(s.rows).arg(s.blocks).arg(enc(Side::Left), enc(Side::Right));
    if (s.blocks == 0 && s.rows > 0) text = tr("Files are identical · ") + text;
    if (s.degraded) text += tr(" · coarse alignment used for a very large unmatched region");
    status_->setText(text);
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

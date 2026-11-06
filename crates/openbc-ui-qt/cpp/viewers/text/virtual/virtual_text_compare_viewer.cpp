// virtual_text_compare_viewer.cpp — see the header.
#include "viewers/text/virtual/virtual_text_compare_viewer.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include "options/appearance_state.h"
#include "options/text_font.h"

namespace openbc::app {

// ---- ModelOverview ----------------------------------------------------------------------------

ModelOverview::ModelOverview(QWidget* parent) : QWidget(parent) {
    setFixedWidth(22);
    setCursor(Qt::PointingHandCursor);
}

void ModelOverview::setModel(std::shared_ptr<TextModel> model) {
    model_ = std::move(model);
    cells_ = model_ ? model_->overview(qMax(1, height())) : QByteArray();
    update();
}

void ModelOverview::setViewport(quint64 firstRow, int visibleRows) {
    first_ = firstRow;
    visible_ = qMax(1, visibleRows);
    update();
}

void ModelOverview::resizeEvent(QResizeEvent*) {
    if (model_) cells_ = model_->overview(qMax(1, height()));
}

void ModelOverview::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Window));
    if (!model_ || model_->rowCount() == 0) return;
    for (int y = 0; y < cells_.size() && y < height(); ++y) {
        const quint8 f = static_cast<quint8>(cells_[y]);
        if (!f) continue;
        const QColor c = (f & 4) ? QColor(0xe0, 0xa0, 0x30) : ((f & 1) ? QColor(0x50, 0xc8, 0x78) : QColor(0xe0, 0x60, 0x60));
        p.fillRect(QRect(3, y, width() - 6, 1), c);
    }
    const double rows = static_cast<double>(model_->rowCount());
    const int y0 = static_cast<int>(first_ / rows * height());
    const int h = qMax(3, static_cast<int>(visible_ / rows * height()));
    p.setPen(QPen(palette().color(QPalette::Text), 1));
    p.setBrush(QColor(255, 255, 255, 40));
    p.drawRect(QRect(0, y0, width() - 1, h));
}

void ModelOverview::request(int y) {
    if (!model_ || model_->rowCount() == 0 || !onRowRequested) return;
    const double ratio = qBound(0.0, static_cast<double>(y) / qMax(1, height()), 1.0);
    const quint64 row = static_cast<quint64>(ratio * static_cast<double>(model_->rowCount()));
    onRowRequested(row > static_cast<quint64>(visible_ / 2) ? row - static_cast<quint64>(visible_ / 2) : 0);
}

void ModelOverview::mousePressEvent(QMouseEvent* e) { request(e->pos().y()); }
void ModelOverview::mouseMoveEvent(QMouseEvent* e) { if (e->buttons() & Qt::LeftButton) request(e->pos().y()); }

// ---- VirtualTextCompareViewer ---------------------------------------------------------------------

VirtualTextCompareViewer::VirtualTextCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent)
    : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath) {
    buildUi();
    buildActions();
    wireEditors();
    applyFont();
    TextFont::instance().subscribe(this, [this]() { applyFont(); });
    reloadTimer_.setSingleShot(true);
    reloadTimer_.setInterval(400);
    reloadTimer_.callOnTimeout([this] { reload(true); });
    QObject::connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
        reloadTimer_.start();          // debounce bursts of writes
        watchFiles();                  // editors that replace the file drop the watch; re-add it
    });
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
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(6, 4, 6, 4);
    leftLabel_ = new QLabel;
    rightLabel_ = new QLabel;
    for (QLabel* l : {leftLabel_, rightLabel_}) l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(leftLabel_, 1);
    header->addSpacing(22);
    header->addWidget(rightLabel_, 1);
    root->addLayout(header);

    progress_ = new QProgressBar;
    progress_->setRange(0, 100);
    progress_->setTextVisible(true);
    progress_->setMaximumHeight(14);
    progress_->hide();
    root->addWidget(progress_);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    left_ = new VirtualTextEditor;
    right_ = new VirtualTextEditor;
    overview_ = new ModelOverview;
    body->addWidget(left_, 1);
    body->addWidget(overview_);
    body->addWidget(right_, 1);
    root->addLayout(body, 1);

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
    status_->setContentsMargins(6, 2, 6, 2);
    root->addWidget(status_);
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
    reloadAction_ = make(tr("&Reload"), QKeySequence(Qt::Key_F5), [this] { reload(true); });
    swapAction_ = make(tr("S&wap Sides"), QKeySequence(), [this] {
        std::swap(leftPath_, rightPath_);
        if (onTitleChanged) onTitleChanged();
        startLoad(false);
    });
    copyAction_ = make(tr("&Copy"), QKeySequence::Copy, [this] { if (auto* e = activeEditor()) e->copySelection(); });
    selectAllAction_ = make(tr("Select &All"), QKeySequence::SelectAll, [this] { if (auto* e = activeEditor()) e->selectAllText(); });
    findAction_ = make(tr("&Find..."), QKeySequence::Find, [this] { showFindBar(true); });
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
    nextDiffAction_ = make(tr("&Next Difference"), QKeySequence(Qt::Key_F8), [this] { if (auto* e = activeEditor()) e->goToNextDifference(); });
    prevDiffAction_ = make(tr("P&revious Difference"), QKeySequence(Qt::SHIFT | Qt::Key_F8), [this] { if (auto* e = activeEditor()) e->goToPreviousDifference(); });
    syntaxAction_ = make(tr("Syntax &Highlighting"), QKeySequence(), [this] {
        left_->setSyntaxEnabled(syntaxAction_->isChecked());
        right_->setSyntaxEnabled(syntaxAction_->isChecked());
    });
    syntaxAction_->setCheckable(true);
    syntaxAction_->setChecked(true);
}

void VirtualTextCompareViewer::wireEditors() {
    auto link = [this](VirtualTextEditor* self, VirtualTextEditor* peer) {
        self->onTopRowChanged = [this, peer, self](quint64 row) {
            if (!syncing_) { syncing_ = true; peer->setFirstVisibleRow(row); syncing_ = false; }
            overview_->setViewport(row, self->visibleRowCount());
        };
        self->onHorizontalOffsetChanged = [this, peer](int px) {
            if (!syncing_) { syncing_ = true; peer->setHorizontalPixelOffset(px); syncing_ = false; }
        };
        self->onCurrentRowChanged = [peer](quint64 row) { peer->setCurrentRowQuietly(row); };
        self->onFocused = [this, self]() { lastFocused_ = self; };
        self->onStatusMessage = [this](const QString& text) { status_->setText(text); };
    };
    link(left_, right_);
    link(right_, left_);
    overview_->onRowRequested = [this](quint64 row) { left_->setFirstVisibleRow(row); };
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

void VirtualTextCompareViewer::watchFiles() {
    const QStringList watched = watcher_.files();
    if (!watched.isEmpty()) watcher_.removePaths(watched);
    for (const QString& p : {leftPath_, rightPath_})
        if (!p.isEmpty() && QFileInfo::exists(p)) watcher_.addPath(p);
}

void VirtualTextCompareViewer::applyDroppedPaths(const QString& left, const QString& right) {
    if (left.isEmpty() && right.isEmpty()) return;
    if (!left.isEmpty()) leftPath_ = left;
    if (!right.isEmpty()) rightPath_ = right;
    if (onTitleChanged) onTitleChanged();
    startLoad(false);
}

void VirtualTextCompareViewer::reload(bool keepScroll) { startLoad(keepScroll); }

void VirtualTextCompareViewer::startLoad(bool keepScroll) {
    leftLabel_->setText(leftPath_.isEmpty() ? tr("(none)") : leftPath_);
    rightLabel_->setText(rightPath_.isEmpty() ? tr("(none)") : rightPath_);
    leftLabel_->setToolTip(leftPath_);
    rightLabel_->setToolTip(rightPath_);
    watchFiles();
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
    overview_->setModel(model_);
    if (keepScrollOnLoad_) left_->setFirstVisibleRow(savedTopRow_);
    overview_->setViewport(left_->firstVisibleRow(), left_->visibleRowCount());
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

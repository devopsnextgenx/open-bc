// diff_line_preview.h
// ---------------------------------------------------------------------------
// Bottom line-by-line compare strip shared by TextCompareViewer and VirtualTextCompareViewer: shows the
// current row's left and right line one above the other, in the SAME font and size as the editors,
// with the editors' copy arrows, line numbers, character-level differences (mismatched characters
// highlighted, mismatched whitespace shown as visible dots/arrows) like Beyond Compare's
// "line compare" pane. A line can be edited in place by double-clicking its text when editing is enabled;
// editable lines carry a slightly lowered (sunken) bevel so they read as editable. While a line is being
// edited the strip never switches to another row: the line shown when the edit began stays put until the
// edit is committed or cancelled.
// ---------------------------------------------------------------------------
#pragma once

#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QVBoxLayout>
#include <functional>

#include "common/diff_algorithms.h"
#include "common/diff_arrow.h"
#include "options/appearance_state.h"
#include "i18n/i18n.h"

namespace openbc::app {

// Copy-arrow gutter of one strip row; same width and arrow shape as the editors' gutters.
class PreviewArrow : public QWidget {
public:
    static constexpr int kWidth = 20;
    explicit PreviewArrow(bool pointsRight, QWidget* parent = nullptr)
        : QWidget(parent), pointsRight_(pointsRight) {
        setFixedWidth(kWidth);
    }
    void setShown(bool shown) { if (shown_ != shown) { shown_ = shown; update(); } }
    std::function<void()> onClicked;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        if (shown_) paintCopyArrow(p, 0, kWidth, height() / 2.0, pointsRight_);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (shown_ && onClicked && e->button() == Qt::LeftButton) onClicked();
    }

private:
    bool pointsRight_ = true;
    bool shown_ = false;
};

// One line of text drawn with highlighted character differences; becomes a QLineEdit on double-click.
class PreviewText : public QWidget {
public:
    explicit PreviewText(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("previewText"));
        edit_ = new QLineEdit(this);
        edit_->setFrame(false);
        // QLineEdit already insets its text by 2px; the app-wide style sheet also gives line edits
        // padding, borders and a min-height, which pushed the text right and down. Pin all of it so the
        // editing text sits exactly where the painted text was.
        edit_->setTextMargins(kMargin - 2 - kBevel, 0, kMargin, 0);   // the edit is inset by the bevel
        edit_->setContentsMargins(0, 0, 0, 0);
        edit_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        applyEditStyle();
        edit_->hide();
        edit_->installEventFilter(this);
        connect(edit_, &QLineEdit::returnPressed, this, [this]() { finishEdit(true); });
    }

    // `hasLine`: this side has a real line (it may be empty); a filler side is neither editable nor bevelled.
    void setContent(const QString& text, const QVector<CharSegment>& segments, const QColor& background,
                    bool showWhitespace, bool hasLine = true) {
        hasLine_ = hasLine;
        text_ = text;
        segments_ = segments;
        background_ = background;
        showWhitespace_ = showWhitespace;
        applyEditStyle();
        if (edit_->isVisible()) finishEdit(false);
        updateAffordance();
        update();
    }
    void setEditingEnabled(bool enabled) {
        editingEnabled_ = enabled;
        if (!enabled && edit_->isVisible()) finishEdit(false);
        updateAffordance();
        update();
    }
    bool isEditing() const { return edit_->isVisible(); }
    void setTextFont(const QFont& f) {
        setFont(f);
        edit_->setFont(f);
        setFixedHeight(QFontMetrics(f).lineSpacing() + 4);
        update();
    }
    std::function<void(const QString&)> onCommit;
    std::function<void()> onEditFinished;   // after every edit ends (committed or cancelled)

protected:
    void resizeEvent(QResizeEvent*) override { edit_->setGeometry(editRect()); }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), background_.alpha() ? background_ : palette().color(QPalette::Base));
        if (text_.isEmpty()) {
            paintBevel(p);
            return;
        }
        p.setFont(font());
        const QFontMetrics fm(font());
        const int baseline = (height() - fm.height()) / 2 + fm.ascent();
        const QColor normal = palette().color(QPalette::Text);

        QString shown = text_;
        if (showWhitespace_) {
            shown.replace(QLatin1Char(' '), QChar(0x00b7));
            shown.replace(QLatin1Char('\t'), QChar(0x2192));
        }
        // Per-character kind from the segments; everything else is equal text.
        QVector<CharSegmentKind> kinds(shown.size(), CharSegmentKind::Equal);
        for (const CharSegment& s : segments_) {
            if (s.kind == CharSegmentKind::Equal) continue;
            for (int i = s.start; i < s.start + s.length && i < kinds.size(); ++i)
                if (i >= 0) kinds[i] = s.kind;
        }
        int x = kMargin;
        int i = 0;
        while (i < shown.size() && x < width()) {
            int j = i + 1;
            while (j < shown.size() && kinds[j] == kinds[i]) ++j;
            const QString run = shown.mid(i, j - i);
            const int w = fm.horizontalAdvance(run);
            switch (kinds[i]) {
            case CharSegmentKind::Mismatch:
                p.fillRect(QRect(x, 1, w, height() - 2), QColor(0xe5, 0x39, 0x35, 170));
                p.setPen(QColor(0xff, 0xff, 0xff));
                break;
            case CharSegmentKind::WhitespaceMismatch:
                p.fillRect(QRect(x, 1, w, height() - 2), QColor(0x4f, 0x8f, 0xd8, 170));
                p.setPen(QColor(0xff, 0xff, 0xff));
                break;
            default:
                p.setPen(normal);
                break;
            }
            p.drawText(x, baseline, run);
            x += w;
            i = j;
        }
        paintBevel(p);
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (!editable() || e->button() != Qt::LeftButton) return;
        edit_->setGeometry(editRect());
        edit_->setText(text_);
        edit_->setCursorPosition(edit_->text().size());
        edit_->show();
        edit_->setFocus();
        edit_->selectAll();
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == edit_) {
            if (event->type() == QEvent::FocusOut && edit_->isVisible()) finishEdit(true);
            if (event->type() == QEvent::KeyPress &&
                static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                finishEdit(false);
                return true;  // Esc cancels this edit, it does not close the tab
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    static constexpr int kMargin = 8;
    static constexpr int kBevel = 2;   // width of the lowered bevel; the edit control sits inside it

    bool editable() const { return editingEnabled_ && hasLine_; }
    QRect editRect() const { return rect().adjusted(kBevel, kBevel, -kBevel, -kBevel); }

    void updateAffordance() {
        setCursor(editable() ? Qt::IBeamCursor : Qt::ArrowCursor);
        setToolTip(editable() ? ::openbc::i18n::t("common.double_click_to_edit_this_line") : QString());
    }

    // Slightly lowered (sunken) frame: dark on the top/left edges, light on the bottom/right ones.
    void paintBevel(QPainter& p) const {
        if (!editable()) return;
        const QColor shadow = palette().color(QPalette::Dark);
        QColor softShadow = shadow;
        softShadow.setAlpha(90);
        const QColor light = palette().color(QPalette::Light);
        const int w = width(), h = height();
        p.setPen(shadow);
        p.drawLine(0, 0, w - 1, 0);
        p.drawLine(0, 0, 0, h - 1);
        p.setPen(softShadow);
        p.drawLine(1, 1, w - 2, 1);
        p.drawLine(1, 1, 1, h - 2);
        p.setPen(light);
        p.drawLine(0, h - 1, w - 1, h - 1);
        p.drawLine(w - 1, 0, w - 1, h - 1);
    }

    void applyEditStyle() {
        const QColor bg = background_.alpha() ? background_ : palette().color(QPalette::Base);
        edit_->setStyleSheet(
            QStringLiteral("QLineEdit { background: %1; color: %2; border: 0px; margin: 0px; padding: 0px;"
                           " min-height: 0px; selection-background-color: #44475a; }")
                .arg(bg.name(), palette().color(QPalette::Text).name()));
    }

    void finishEdit(bool commit) {
        if (!edit_->isVisible() || finishing_) return;
        finishing_ = true;   // hiding the edit sends FocusOut, which must not finish it a second time
        const QString value = edit_->text();
        edit_->hide();
        if (commit && value != text_ && onCommit) onCommit(value);
        finishing_ = false;
        if (onEditFinished) onEditFinished();
    }

    QLineEdit* edit_ = nullptr;
    QString text_;
    QVector<CharSegment> segments_;
    QColor background_ = Qt::transparent;
    bool showWhitespace_ = false;
    bool editingEnabled_ = true;
    bool hasLine_ = true;
    bool finishing_ = false;
};

class DiffLinePreview : public QFrame {
public:
    explicit DiffLinePreview(QWidget* parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("diffLinePreview"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);

        const auto makeRow = [this, layout](bool leftSide, PreviewArrow*& arrow, QLabel*& number,
                                            PreviewText*& text) {
            auto* row = new QWidget(this);
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(0);
            arrow = new PreviewArrow(leftSide, row);
            number = new QLabel(row);
            number->setObjectName(QStringLiteral("previewLineNo"));
            number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            text = new PreviewText(row);
            rowLayout->addWidget(arrow);
            rowLayout->addWidget(number);
            rowLayout->addWidget(text, 1);
            layout->addWidget(row);
        };
        makeRow(true, topArrow_, topNumber_, topText_);
        makeRow(false, bottomArrow_, bottomNumber_, bottomText_);
        topText_->onCommit = [this](const QString& t) { if (onLeftEdited) onLeftEdited(t); };
        bottomText_->onCommit = [this](const QString& t) { if (onRightEdited) onRightEdited(t); };
        topText_->onEditFinished = [this]() { editFinished(); };
        bottomText_->onEditFinished = [this]() { editFinished(); };
        // The arrows copy this row's line to the other side.
        topArrow_->onClicked = [this]() { if (onCopyLeftToRight && row_.leftNumber) onCopyLeftToRight(); };
        bottomArrow_->onClicked = [this]() { if (onCopyRightToLeft && row_.rightNumber) onCopyRightToLeft(); };
        setPreviewFont(QFont(QStringLiteral("monospace")));
        clear();
    }

    // Same font as the editors (TextFont); keeps the strip's row height and number column in step.
    void setPreviewFont(const QFont& font) {
        const QFontMetrics fm(font);
        for (QLabel* n : {topNumber_, bottomNumber_}) {
            n->setFont(font);
            n->setFixedWidth(fm.horizontalAdvance(QString(6, QLatin1Char('0'))) + 16);
            n->setFixedHeight(fm.lineSpacing() + 4);
        }
        topText_->setTextFont(font);
        bottomText_->setTextFont(font);
        topArrow_->setFixedHeight(fm.lineSpacing() + 4);
        bottomArrow_->setFixedHeight(fm.lineSpacing() + 4);
    }

    // `leftSegments` / `rightSegments` are the character-level differences of the two lines; when both
    // are empty for a changed row they are computed here. `contextRow` identifies the row in the caller's
    // list (-1 = unknown); it is what contextRow() reports and what an edit applies to.
    // While a line is being edited the request is parked, never shown: the edit keeps its row.
    void setRow(const TextDiffLine& row, const QVector<CharSegment>& leftSegments = {},
                const QVector<CharSegment>& rightSegments = {}, int contextRow = -1) {
        if (isEditing()) {
            pendingRow_ = row;
            pendingLeft_ = leftSegments;
            pendingRight_ = rightSegments;
            pendingContext_ = contextRow;
            hasPending_ = true;
            return;
        }
        hasPending_ = false;
        contextRow_ = contextRow;
        QVector<CharSegment> l = leftSegments, r = rightSegments;
        if (row.changed && l.isEmpty() && r.isEmpty() && row.leftNumber && row.rightNumber) {
            const InlineDiff d = computeInlineDiff(row.left, row.right, sessionId_);
            l = d.left;
            r = d.right;
        }
        row_ = row;
        const QColor background = !row.changed         ? QColor(Qt::transparent)
                                  : row.whitespaceOnly ? QColor(0x2d, 0x5a, 0x87)
                                                       : QColor(0x7a, 0x2d, 0x2d);
        const QColor numberText =
            openbc::ui::appearance::text("File Views/Line numbers", QGuiApplication::palette().color(QPalette::Text));
        const auto show = [&](PreviewArrow* arrow, QLabel* number, PreviewText* text, int lineNumber,
                              const QString& content, const QVector<CharSegment>& segments) {
            number->setText(lineNumber ? QString::number(lineNumber) : QString());
            number->setStyleSheet(QStringLiteral("color:%1; padding-right:8px; background:%2;")
                                      .arg(numberText.name(),
                                           row.changed ? background.name() : QStringLiteral("transparent")));
            text->setContent(lineNumber ? content : QString(), segments, background, row.changed, lineNumber != 0);
            arrow->setShown(row.changed && lineNumber != 0);
        };
        show(topArrow_, topNumber_, topText_, row.leftNumber, row.left, l);
        show(bottomArrow_, bottomNumber_, bottomText_, row.rightNumber, row.right, r);
    }

    void clear() {
        setRow(TextDiffLine());
    }

    void setEditingEnabled(bool enabled) {
        editingEnabled_ = enabled;
        topText_->setEditingEnabled(enabled);
        bottomText_->setEditingEnabled(enabled);
    }
    bool editingEnabled() const { return editingEnabled_; }
    // True while either line is being edited in place.
    bool isEditing() const { return topText_->isEditing() || bottomText_->isEditing(); }
    // The caller's row index of the line pair currently shown (as passed to setRow), -1 if unknown.
    // Stays fixed while a line is being edited, so commits and copies target the row on screen.
    int contextRow() const { return contextRow_; }
    void setSessionId(const QString& id) { sessionId_ = id; }

    // Fired once the user finishes editing (Enter, or clicking away). The text is exactly the line's
    // content; the line number is never part of it.
    std::function<void(QString)> onLeftEdited;
    std::function<void(QString)> onRightEdited;
    // Fired by clicking the arrow of a changed row.
    std::function<void()> onCopyLeftToRight;
    std::function<void()> onCopyRightToLeft;

private:
    // An edit ended. A refresh of the SAME row that arrived meanwhile is shown now; a request for any
    // other row is dropped, so the strip does not jump to a different line.
    void editFinished() {
        if (isEditing() || !hasPending_) return;
        hasPending_ = false;
        if (pendingContext_ >= 0 && pendingContext_ == contextRow_)
            setRow(pendingRow_, pendingLeft_, pendingRight_, pendingContext_);
    }

    PreviewArrow* topArrow_ = nullptr;
    PreviewArrow* bottomArrow_ = nullptr;
    QLabel* topNumber_ = nullptr;
    QLabel* bottomNumber_ = nullptr;
    PreviewText* topText_ = nullptr;
    PreviewText* bottomText_ = nullptr;
    TextDiffLine row_;
    int contextRow_ = -1;
    TextDiffLine pendingRow_;
    QVector<CharSegment> pendingLeft_, pendingRight_;
    int pendingContext_ = -1;
    bool hasPending_ = false;
    QString sessionId_;
    bool editingEnabled_ = true;
};

}  // namespace openbc::app

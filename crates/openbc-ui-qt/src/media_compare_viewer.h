// media_compare_viewer.h
// ---------------------------------------------------------------------------
// MediaCompareViewer: compares the METADATA of two media files (mp3, wav, flac,
// ogg, m4a, mp4, mov, avi, mkv ...): container, codec, duration, dimensions,
// bitrate, channels, tags. The audio/video payload is never decoded or compared.
//
// Presentation only: header parsing and property alignment happen in
// openbc-compute (media) / openbc-core (meta_compare) behind the
// openbc_media_* C ABI. Rows whose values differ are highlighted.
//
// Playback is NOT implemented yet: the Play buttons exist but are disabled so
// the layout and actions are final; see kPlaybackImplemented below.
// ---------------------------------------------------------------------------
#pragma once

#include <QAction>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <memory>

#include "backend_channel.h"
#include "compare_backend_api.h"
#include "qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

using MediaViewPtr = std::shared_ptr<OpenBcMediaView>;

class MediaCompareViewer : public QWidget {
public:
    // Flip once a player (e.g. QMediaPlayer, one-at-a-time or in parallel) is wired in.
    static constexpr bool kPlaybackImplemented = false;

    MediaCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &MediaCompareViewer::destroyHandle, [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("mediaCompareView");
        buildUi();
        startSession();
    }

    ~MediaCompareViewer() override { stopSession(); }

    QString title() const {
        const auto name = [](const QString& p) { return p.isEmpty() ? QString("(missing)") : QFileInfo(p).fileName(); };
        return name(leftPath_) + " <-> " + name(rightPath_);
    }
    QString detailedTitle() const { return leftPath_ + "  <->  " + rightPath_; }
    QString leftText() const { return leftPath_; }
    QString rightText() const { return rightPath_; }

    std::function<void()> onHomeRequested;
    std::function<void()> onSessionsRequested;
    std::function<void()> onTitleChanged;

    QList<QAction*> actionsMenuItems() const { return {playLeftAction_, playRightAction_, playBothAction_, reloadAction_}; }

private:
    static void destroyHandle(int kind, void* handle) {
        if (kind == OPENBC_EV_RESULT) openbc_media_view_destroy(static_cast<OpenBcMediaView*>(handle));
    }

    void startSession() {
        stopSession();
        view_.reset();
        const QByteArray left = leftPath_.toUtf8(), right = rightPath_.toUtf8();
        session_ = openbc_media_session_create(left.constData(), right.constData(), BackendChannel::callback(), channel_.user());
        summary_->setText("Reading media headers...");
        if (onTitleChanged) onTitleChanged();
    }

    void stopSession() {
        if (!session_) return;
        openbc_media_session_destroy(session_);
        session_ = nullptr;
    }

    void onEvent(const BackendChannel::Event& event) {
        if (event.kind == OPENBC_EV_STATUS) summary_->setText(event.message);
        else if (event.kind == OPENBC_EV_RESULT) {
            view_ = std::static_pointer_cast<OpenBcMediaView>(event.handle);
            rebuild();
        }
    }

    QString text(std::size_t row, std::uint8_t field) const {
        std::size_t length = 0;
        return backendString(openbc_media_row_text(view_.get(), row, field, &length), length);
    }

    void rebuild() {
        tree_->clear();
        plain_.clear();
        const OpenBcMediaView* v = view_.get();
        QStringList problems;
        for (std::uint8_t side = 0; side < 2; ++side) {
            std::size_t length = 0;
            const QString error = backendString(openbc_media_side_error(v, side, &length), length);
            if (!error.isEmpty()) problems << error;
        }
        errors_->setText(problems.join("\n"));
        errors_->setVisible(!problems.isEmpty());

        const QString dash = QStringLiteral("-");
        const QBrush differBrush(QColor(0xe5, 0x53, 0x4b, 56));
        const bool onlyDifferences = onlyDifferences_->isChecked();
        int compared = 0, differing = 0;
        QStringList lines{"Property\tLeft\tRight"};
        QTreeWidgetItem* top = nullptr;
        QString currentGroup;
        const std::size_t rows = openbc_media_view_rows(v);
        for (std::size_t i = 0; i < rows; ++i) {
            const std::uint32_t flags = openbc_media_row_flags(v, i);
            const bool differs = flags & 1, informational = flags & 2;
            if (!informational) {
                ++compared;
                differing += differs;
            }
            if (onlyDifferences && !(differs && !informational)) continue;
            const QString group = text(i, 0), key = text(i, 1);
            const bool hasLeft = openbc_media_row_text(v, i, 2, nullptr) != nullptr;
            const bool hasRight = openbc_media_row_text(v, i, 3, nullptr) != nullptr;
            const QString l = hasLeft ? text(i, 2) : dash, r = hasRight ? text(i, 3) : dash;
            if (!top || group != currentGroup) {
                top = new QTreeWidgetItem(tree_, {group});
                QFont bold = top->font(0);
                bold.setBold(true);
                top->setFont(0, bold);
                top->setFirstColumnSpanned(true);
                top->setExpanded(true);
                currentGroup = group;
            }
            auto* item = new QTreeWidgetItem(top, {key, l, r});
            item->setToolTip(1, l);
            item->setToolTip(2, r);
            if (differs && !informational) {
                QFont bold = item->font(0);
                bold.setBold(true);
                item->setFont(0, bold);
                for (int c = 0; c < 3; ++c) item->setBackground(c, differBrush);
            }
            lines << key + '\t' + l + '\t' + r;
        }
        plain_ = lines.join('\n');
        summary_->setText(rows == 0 ? QString("No metadata") : compared == 0 ? QString("Metadata") : QString("%1 of %2 properties differ").arg(differing).arg(compared));
        if (onTitleChanged) onTitleChanged();
    }

    QToolBar* makeToolbar() {
        using icons::Glyph;
        auto* bar = new QToolBar(this);
        bar->setObjectName("textCompareToolbar");
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setIconSize(QSize(20, 20));
        bar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        homeAction_ = bar->addAction(icons::glyph(Glyph::Home), "Home");
        sessionsAction_ = bar->addAction(icons::glyph(Glyph::Sessions), "Sessions");
        bar->addSeparator();
        const QString note = "Playback is not implemented yet (metadata compare only)";
        playLeftAction_ = bar->addAction("Play Left");
        playRightAction_ = bar->addAction("Play Right");
        playBothAction_ = bar->addAction("Play Both");
        for (QAction* a : {playLeftAction_, playRightAction_, playBothAction_}) {
            a->setEnabled(kPlaybackImplemented);
            a->setToolTip(note);
        }
        bar->addSeparator();
        reloadAction_ = bar->addAction(icons::glyph(Glyph::Refresh), "Reload");
        return bar;
    }

    void buildUi() {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        root->addWidget(makeToolbar());

        auto* paths = new QHBoxLayout;
        paths->setContentsMargins(6, 2, 6, 2);
        for (const QString& p : {leftPath_, rightPath_}) {
            auto* label = new QLabel(p.isEmpty() ? QString("(missing)") : p, this);
            label->setToolTip(p);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            paths->addWidget(label, 1);
        }
        root->addLayout(paths);

        errors_ = new QLabel(this);
        errors_->setStyleSheet("color:#e5534b;");
        errors_->setWordWrap(true);
        errors_->setContentsMargins(6, 0, 6, 0);
        errors_->hide();
        root->addWidget(errors_);

        auto* header = new QHBoxLayout;
        header->setContentsMargins(6, 2, 6, 2);
        summary_ = new QLabel("Metadata", this);
        onlyDifferences_ = new QCheckBox("Differences only", this);
        auto* copy = new QPushButton("Copy", this);
        copy->setToolTip("Copy all rows to the clipboard (tab separated)");
        header->addWidget(summary_, 1);
        header->addWidget(onlyDifferences_);
        header->addWidget(copy);
        root->addLayout(header);

        tree_ = new QTreeWidget(this);
        tree_->setColumnCount(3);
        tree_->setHeaderLabels({"Property", "Left", "Right"});
        tree_->setUniformRowHeights(true);
        tree_->setAlternatingRowColors(true);
        tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree_->setTextElideMode(Qt::ElideMiddle);
        tree_->header()->setStretchLastSection(false);
        tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        tree_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        root->addWidget(tree_, 1);

        QObject::connect(onlyDifferences_, &QCheckBox::toggled, this, [this](bool) { if (view_) rebuild(); });
        QObject::connect(copy, &QPushButton::clicked, this, [this]() { QGuiApplication::clipboard()->setText(plain_); });
        QObject::connect(homeAction_, &QAction::triggered, this, [this]() { if (onHomeRequested) onHomeRequested(); });
        QObject::connect(sessionsAction_, &QAction::triggered, this, [this]() { if (onSessionsRequested) onSessionsRequested(); });
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() { if (session_) openbc_media_reload(session_); });
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcMediaSession* session_ = nullptr;
    MediaViewPtr view_;
    QString plain_;

    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* playLeftAction_ = nullptr;
    QAction* playRightAction_ = nullptr;
    QAction* playBothAction_ = nullptr;
    QAction* reloadAction_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* errors_ = nullptr;
    QCheckBox* onlyDifferences_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app
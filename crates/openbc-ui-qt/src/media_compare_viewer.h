// media_compare_viewer.h
// ---------------------------------------------------------------------------
// MediaCompareViewer: compares the METADATA of two media files (mp3, wav, flac,
// ogg, m4a, mp4, mov, avi, mkv ...): container, codec, duration, dimensions,
// bitrate, channels, tags, embedded cover art. The audio/video payload is never
// decoded for comparison.
//
// Presentation only: header parsing and property alignment happen in
// openbc-compute (media) / openbc-core (meta_compare) behind the
// openbc_media_* C ABI. Rows whose values differ are highlighted.
//
// Playback is a preview aid only: MediaPlayerPane uses Qt Multimedia with the
// operating system's decoders, so no codecs are shipped with the app.
//
// FIX: every backendString(openbc_*(..., &length), length) call was reading
// `length` before the call wrote it (MSVC evaluates arguments right to left),
// which produced empty strings and an empty table. The pointer is now fetched
// into a local first.
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
#include <QPixmap>
#include <QPushButton>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <memory>

#include "backend_channel.h"
#include "compare_backend_api.h"
#include "media_player_pane.h"
#include "qt_style.h"

namespace openbc::app {

namespace icons = openbc::ui::icons;

using MediaViewPtr = std::shared_ptr<OpenBcMediaView>;

class MediaCompareViewer : public QWidget {
public:
    MediaCompareViewer(const QString& leftPath, const QString& rightPath, QWidget* parent = nullptr)
        : QWidget(parent), leftPath_(leftPath), rightPath_(rightPath),
          channel_(this, &MediaCompareViewer::destroyHandle, [this](const BackendChannel::Event& event) { onEvent(event); }) {
        setObjectName("mediaCompareView");
        buildUi();
        startSession();
    }

    ~MediaCompareViewer() override {
        leftPane_->stop();
        rightPane_->stop();
        stopSession();
    }

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
        previewed_ = false;
        leftPane_->reset();
        rightPane_->reset();
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

    // ---- backend string helpers (pointer is fetched BEFORE the length is read) ----

    QString text(std::size_t row, std::uint8_t field) const {
        std::size_t length = 0;
        const auto* p = openbc_media_row_text(view_.get(), row, field, &length);
        return backendString(p, length);
    }

    bool hasText(std::size_t row, std::uint8_t field) const {
        std::size_t length = 0;  // do not pass nullptr: the C side may write the length
        return openbc_media_row_text(view_.get(), row, field, &length) != nullptr;
    }

    QString sideError(std::uint8_t side) const {
        std::size_t length = 0;
        const auto* p = openbc_media_side_error(view_.get(), side, &length);
        return backendString(p, length);
    }

    QPixmap sideCover(std::uint8_t side) const {
        std::size_t length = 0;
        const auto* p = openbc_media_side_cover(view_.get(), side, &length);
        QPixmap pixmap;
        if (p && length) pixmap.loadFromData(reinterpret_cast<const uchar*>(p), static_cast<uint>(length));
        return pixmap;
    }

    void rebuild() {
        tree_->clear();
        plain_.clear();
        const OpenBcMediaView* v = view_.get();
        QStringList problems;
        for (std::uint8_t side = 0; side < 2; ++side) {
            const QString error = sideError(side);
            if (!error.isEmpty()) problems << error;
        }
        errors_->setText(problems.join("\n"));
        errors_->setVisible(!problems.isEmpty());

        leftPane_->setCover(sideCover(0));
        rightPane_->setCover(sideCover(1));

        const QString dash = QStringLiteral("-");
        const QBrush differBrush(QColor(0xe5, 0x53, 0x4b, 56));
        const bool onlyDifferences = onlyDifferences_->isChecked();
        int compared = 0, differing = 0;
        bool hasVideo[2] = {false, false};
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
            const QString group = text(i, 0), key = text(i, 1);
            const bool hasLeft = hasText(i, 2);
            const bool hasRight = hasText(i, 3);
            if (group == "Video") {
                hasVideo[0] |= hasLeft;
                hasVideo[1] |= hasRight;
            }
            if (onlyDifferences && !(differs && !informational)) continue;
            const QString l = hasLeft ? text(i, 2) : dash, r = hasRight ? text(i, 3) : dash;
            if (!top || group != currentGroup) {
                top = new QTreeWidgetItem(tree_, {group});
                QFont bold = top->font(0);
                bold.setBold(true);
                top->setFont(0, bold);
                top->setFirstColumnSpanned(true);
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
        tree_->expandAll();  // after children exist (setExpanded on an empty item does not stick)

        plain_ = lines.join('\n');
        summary_->setText(rows == 0 ? QString("No metadata") : compared == 0 ? QString("Metadata") : QString("%1 of %2 properties differ").arg(differing).arg(compared));

        // Show the first video frame as a thumbnail (once per load, not on every filter toggle).
        if (!previewed_) {
            previewed_ = true;
            if (hasVideo[0]) leftPane_->preview();
            if (hasVideo[1]) rightPane_->preview();
        }
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
        playLeftAction_ = bar->addAction("Play Left");
        playRightAction_ = bar->addAction("Play Right");
        playBothAction_ = bar->addAction("Play Both");
        playLeftAction_->setEnabled(!leftPath_.isEmpty());
        playRightAction_->setEnabled(!rightPath_.isEmpty());
        playBothAction_->setEnabled(!leftPath_.isEmpty() && !rightPath_.isEmpty());
        playLeftAction_->setToolTip("Play / pause the left file (uses the system's media decoders)");
        playRightAction_->setToolTip("Play / pause the right file (uses the system's media decoders)");
        playBothAction_->setToolTip("Restart and play both files together");
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

        // Preview players: one per side, directly under the path labels.
        auto* players = new QHBoxLayout;
        players->setContentsMargins(0, 0, 0, 0);
        leftPane_ = new MediaPlayerPane(leftPath_, this);
        rightPane_ = new MediaPlayerPane(rightPath_, this);
        leftPane_->setVisible(!leftPath_.isEmpty());
        rightPane_->setVisible(!rightPath_.isEmpty());
        players->addWidget(leftPane_, 1);
        players->addWidget(rightPane_, 1);
        root->addLayout(players);

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
        QObject::connect(reloadAction_, &QAction::triggered, this, [this]() {
            if (!session_) return;
            previewed_ = false;
            leftPane_->reset();
            rightPane_->reset();
            openbc_media_reload(session_);
        });

        // Play Left / Right: toggle that side and pause the other so two files never overlap by accident.
        QObject::connect(playLeftAction_, &QAction::triggered, this, [this]() {
            if (leftPane_->playing()) {
                leftPane_->pause();
            } else {
                rightPane_->pause();
                leftPane_->play();
            }
        });
        QObject::connect(playRightAction_, &QAction::triggered, this, [this]() {
            if (rightPane_->playing()) {
                rightPane_->pause();
            } else {
                leftPane_->pause();
                rightPane_->play();
            }
        });
        // Play Both: toggle; starting restarts both from 0 so they stay roughly in step.
        QObject::connect(playBothAction_, &QAction::triggered, this, [this]() {
            if (leftPane_->playing() || rightPane_->playing()) {
                leftPane_->pause();
                rightPane_->pause();
                return;
            }
            leftPane_->rewind();
            rightPane_->rewind();
            leftPane_->play();
            rightPane_->play();
        });
    }

    QString leftPath_;
    QString rightPath_;
    OpenBcMediaSession* session_ = nullptr;
    MediaViewPtr view_;
    QString plain_;
    bool previewed_ = false;

    QAction* homeAction_ = nullptr;
    QAction* sessionsAction_ = nullptr;
    QAction* playLeftAction_ = nullptr;
    QAction* playRightAction_ = nullptr;
    QAction* playBothAction_ = nullptr;
    QAction* reloadAction_ = nullptr;
    MediaPlayerPane* leftPane_ = nullptr;
    MediaPlayerPane* rightPane_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* errors_ = nullptr;
    QCheckBox* onlyDifferences_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    BackendChannel channel_;  // last member: destroyed after stopSession()
};

}  // namespace openbc::app
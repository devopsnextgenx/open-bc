// media_player_pane.h
// ---------------------------------------------------------------------------
// MediaPlayerPane: one side's preview player for the Media Compare tab.
// Uses Qt Multimedia with the OS-provided decoders (no codecs bundled).
// The source is loaded lazily, so opening a compare tab costs nothing until
// the user presses Play (or preview() is called for video files).
//
// Per-side transport: Play / Pause / Stop icon buttons. Volume is owned by the
// parent (MediaCompareViewer) which computes master x crossfade gains and calls
// setVolume(); the pane only applies the final linear gain.
//
// Tag editing support: the pane can release the file (release()) so the
// backend can rewrite it - on Windows an open QMediaPlayer keeps the file
// locked - and exposes a "Cover" button (replace / remove) that reports the
// user's choice through onCoverReplaced / onCoverRemoved.
// ---------------------------------------------------------------------------
#pragma once

#include <QAudioOutput>
#include <QByteArray>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>
#include <functional>

#include "qt_style.h"

namespace openbc::app {

class MediaPlayerPane : public QWidget {
public:
    explicit MediaPlayerPane(const QString& path, QWidget* parent = nullptr) : QWidget(parent), path_(path) {
        namespace icons = openbc::ui::icons;
        using icons::Glyph;

        player_ = new QMediaPlayer(this);
        audio_ = new QAudioOutput(this);
        player_->setAudioOutput(audio_);

        cover_ = new QLabel(this);
        cover_->setAlignment(Qt::AlignCenter);
        cover_->hide();

        video_ = new QVideoWidget(this);
        video_->setMinimumHeight(200);
        video_->hide();
        player_->setVideoOutput(video_);

        playButton_ = new QToolButton(this);
        playButton_->setIcon(icons::glyph(Glyph::Play));
        playButton_->setToolTip("Play");
        pauseButton_ = new QToolButton(this);
        pauseButton_->setIcon(icons::glyph(Glyph::Pause));
        pauseButton_->setToolTip("Pause");
        stopButton_ = new QToolButton(this);
        stopButton_->setIcon(icons::glyph(Glyph::Stop));
        stopButton_->setToolTip("Stop (back to start)");

        coverButton_ = new QToolButton(this);
        coverButton_->setIcon(icons::glyph(Glyph::Cover));
        coverButton_->setToolTip("Cover art: replace or remove (saved with the Save button)");
        coverButton_->setPopupMode(QToolButton::InstantPopup);
        auto* coverMenu = new QMenu(coverButton_);
        coverMenu->addAction("Replace cover...", this, [this]() { chooseCover(); });
        coverMenu->addAction("Remove cover", this, [this]() {
            if (onCoverRemoved) onCoverRemoved();
        });
        coverButton_->setMenu(coverMenu);
        coverButton_->setEnabled(false);  // enabled by the viewer once the side is editable

        slider_ = new QSlider(Qt::Horizontal, this);
        time_ = new QLabel("0:00 / 0:00", this);

        error_ = new QLabel(this);
        error_->setStyleSheet("color:#e5534b;");
        error_->setWordWrap(true);
        error_->hide();

        external_ = new QPushButton("Open in system player", this);
        external_->hide();

        auto* transport = new QHBoxLayout;
        transport->setContentsMargins(0, 0, 0, 0);
        transport->setSpacing(2);
        transport->addWidget(playButton_);
        transport->addWidget(pauseButton_);
        transport->addWidget(stopButton_);
        transport->addWidget(slider_, 1);
        transport->addWidget(time_);
        transport->addWidget(coverButton_);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 2, 6, 2);
        layout->setSpacing(4);
        layout->addWidget(cover_);
        layout->addWidget(video_, 1);
        layout->addLayout(transport);
        layout->addWidget(error_);
        layout->addWidget(external_);
        refreshEnabled();

        connect(playButton_, &QToolButton::clicked, this, [this]() { play(); });
        connect(pauseButton_, &QToolButton::clicked, this, [this]() { pause(); });
        connect(stopButton_, &QToolButton::clicked, this, [this]() {
            if (loaded_) player_->stop();
        });

        connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState) {
            refreshEnabled();
            if (onStateChanged) onStateChanged();
        });
        connect(player_, &QMediaPlayer::hasVideoChanged, this, [this](bool hasVideo) {
            video_->setVisible(hasVideo);
            if (onVideoChanged) onVideoChanged();
        });
        connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
            slider_->setRange(0, static_cast<int>(duration));
            refreshTime();
        });
        connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
            if (!slider_->isSliderDown()) slider_->setValue(static_cast<int>(position));
            refreshTime();
        });
        connect(slider_, &QSlider::sliderMoved, this, [this](int position) {
            ensureSource();
            player_->setPosition(position);
            if (onSeeked) onSeeked(position);
        });
        connect(player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& message) {
            video_->hide();
            error_->setText("Cannot play this file with the system decoders: " + message);
            error_->show();
            external_->show();
        });
        connect(external_, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(path_)); });
    }

    ~MediaPlayerPane() override { player_->stop(); }

    std::function<void()> onVideoChanged;  // fired when this side gains/loses a video track
    bool hasVideo() const { return player_->hasVideo(); }
    // Fired when playback starts/pauses/stops (viewer re-applies volume gains).
    std::function<void()> onStateChanged;
    // Fired when the user drags this side's slider (viewer may mirror it to the other side).
    std::function<void(qint64)> onSeeked;
    // Cover editing requests (viewer keeps the pending change and previews it via setCover()).
    std::function<void(const QByteArray&)> onCoverReplaced;
    std::function<void()> onCoverRemoved;

    const QString& path() const { return path_; }

    // Points the pane at another file (path selectors). Forgets the loaded source.
    void setPath(const QString& path) {
        reset();
        path_ = path;
        setCover(QPixmap());
        setVisible(!path_.isEmpty());
        refreshEnabled();
    }

    bool playing() const { return player_->playbackState() == QMediaPlayer::PlayingState; }

    void play() {
        ensureSource();
        player_->play();
    }
    void pause() { player_->pause(); }
    void rewind() {
        ensureSource();
        player_->setPosition(0);
    }
    void seek(qint64 ms) {
        if (!loaded_) return;  // never load a file just to mirror a seek
        player_->setPosition(ms);
    }
    void stop() { player_->stop(); }
    void setMuted(bool m) { audio_->setMuted(m); }
    // Final linear gain 0..1 (master x crossfade), applied immediately.
    void setVolume(double linear) { audio_->setVolume(static_cast<float>(qBound(0.0, linear, 1.0))); }

    // Loads the file and pauses on the first frame (video thumbnail without playing).
    void preview() {
        ensureSource();
        player_->pause();
    }

    // Drops the file handle so the backend can rewrite the file (Windows keeps it locked
    // while a QMediaPlayer has it open). The source is reloaded lazily on the next play.
    void release() { reset(); }

    // Forget the loaded source (used on Reload so a changed file is re-read).
    void reset() {
        player_->stop();
        player_->setSource(QUrl());
        loaded_ = false;
        error_->hide();
        external_->hide();
        video_->hide();
        slider_->setValue(0);
        refreshEnabled();
    }

    // Embedded cover art; a null pixmap clears and hides it.
    void setCover(const QPixmap& pixmap) {
        if (pixmap.isNull()) {
            cover_->clear();
            cover_->hide();
            return;
        }
        cover_->setPixmap(pixmap.scaledToHeight(160, Qt::SmoothTransformation));
        cover_->show();
    }

    void setCoverEditable(bool editable) { coverButton_->setEnabled(editable && !path_.isEmpty()); }

private:
    void ensureSource() {
        if (loaded_ || path_.isEmpty()) return;
        loaded_ = true;
        player_->setSource(QUrl::fromLocalFile(path_));
    }

    void chooseCover() {
        const QString file = QFileDialog::getOpenFileName(this, "Choose cover image", QString(),
                                                          "Images (*.jpg *.jpeg *.png *.bmp *.gif *.webp);;All files (*.*)");
        if (file.isEmpty()) return;
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, "Cover", "Cannot read " + file);
            return;
        }
        const QByteArray bytes = f.readAll();
        QPixmap probe;
        if (!probe.loadFromData(bytes)) {
            QMessageBox::warning(this, "Cover", "That file is not a readable image.");
            return;
        }
        if (onCoverReplaced) onCoverReplaced(bytes);
    }

    void refreshEnabled() {
        const bool has = !path_.isEmpty();
        const bool isPlaying = playing();
        playButton_->setEnabled(has && !isPlaying);
        pauseButton_->setEnabled(has && isPlaying);
        stopButton_->setEnabled(has && loaded_);
        slider_->setEnabled(has);
    }

    static QString format(qint64 ms) {
        const qint64 s = ms / 1000;
        return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    }

    void refreshTime() { time_->setText(format(player_->position()) + " / " + format(player_->duration())); }

    QString path_;
    bool loaded_ = false;
    QMediaPlayer* player_ = nullptr;
    QAudioOutput* audio_ = nullptr;
    QVideoWidget* video_ = nullptr;
    QLabel* cover_ = nullptr;
    QLabel* time_ = nullptr;
    QLabel* error_ = nullptr;
    QSlider* slider_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* pauseButton_ = nullptr;
    QToolButton* stopButton_ = nullptr;
    QToolButton* coverButton_ = nullptr;
    QPushButton* external_ = nullptr;
};

}  // namespace openbc::app
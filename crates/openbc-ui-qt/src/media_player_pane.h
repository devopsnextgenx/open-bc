// media_player_pane.h
// ---------------------------------------------------------------------------
// MediaPlayerPane: one side's preview player for the Media Compare tab.
// Uses Qt Multimedia with the OS-provided decoders (no codecs bundled).
// The source is loaded lazily, so opening a compare tab costs nothing until
// the user presses Play (or preview() is called for video files).
// ---------------------------------------------------------------------------
#pragma once

#include <QAudioOutput>
#include <QDesktopServices>
#include <QLabel>
#include <QMediaPlayer>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>
#include <QWidget>

namespace openbc::app {

class MediaPlayerPane : public QWidget {
public:
    explicit MediaPlayerPane(const QString& path, QWidget* parent = nullptr) : QWidget(parent), path_(path) {
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

        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setEnabled(!path_.isEmpty());
        time_ = new QLabel("0:00 / 0:00", this);

        error_ = new QLabel(this);
        error_->setStyleSheet("color:#e5534b;");
        error_->setWordWrap(true);
        error_->hide();

        external_ = new QPushButton("Open in system player", this);
        external_->hide();

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 2, 6, 2);
        layout->setSpacing(4);
        layout->addWidget(cover_);
        layout->addWidget(video_, 1);
        layout->addWidget(slider_);
        layout->addWidget(time_);
        layout->addWidget(error_);
        layout->addWidget(external_);

        connect(player_, &QMediaPlayer::hasVideoChanged, this, [this](bool hasVideo) { video_->setVisible(hasVideo); });
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
    void stop() { player_->stop(); }
    void setMuted(bool m) { audio_->setMuted(m); }

    // Loads the file and pauses on the first frame (video thumbnail without playing).
    void preview() {
        ensureSource();
        player_->pause();
    }

    // Forget the loaded source (used on Reload so a changed file is re-read).
    void reset() {
        player_->stop();
        player_->setSource(QUrl());
        loaded_ = false;
        error_->hide();
        external_->hide();
        video_->hide();
        slider_->setValue(0);
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

private:
    void ensureSource() {
        if (loaded_ || path_.isEmpty()) return;
        loaded_ = true;
        player_->setSource(QUrl::fromLocalFile(path_));
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
    QPushButton* external_ = nullptr;
};

}  // namespace openbc::app
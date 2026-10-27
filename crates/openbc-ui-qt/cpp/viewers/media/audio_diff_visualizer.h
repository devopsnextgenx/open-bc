#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QMouseEvent;
class QEvent;
class QPaintEvent;
class QPainter;
class QWheelEvent;

namespace openbc::app {

struct AudioPeak {
    float minimum = 0.0F;
    float maximum = 0.0F;
};

struct AudioPeakLevel {
    quint64 framesPerBin = 0;
    QVector<AudioPeak> bins;
};

struct AudioWaveform {
    quint64 frameCount = 0;
    quint32 sampleRate = 0;
    QVector<AudioPeakLevel> levels;
};

/// Paints two aligned audio waveforms and their absolute-difference waveform.
///
/// Peak levels should be ordered from finest to coarsest. The widget selects the
/// coarsest level no larger than the current samples-per-pixel value, keeping
/// paint work proportional to the viewport width.
///
/// A host can forward `onSeekRequested` to both players and feed either player's
/// position back through `setPlaybackPosition` (without generating another seek).
class AudioDiffVisualizer final : public QWidget {
public:
    explicit AudioDiffVisualizer(QWidget* parent = nullptr);

    /// Replaces all waveform data. The delta must cover no more than the shared
    /// prefix of A and B. Returns false and leaves the previous data untouched
    /// when the peak pyramid metadata is inconsistent.
    bool setWaveforms(AudioWaveform fileA, AudioWaveform fileB, AudioWaveform delta,
                      QString* error = nullptr);

    /// Updates the shared playhead from playback. Does not emit a seek request.
    void setPlaybackPosition(qint64 positionMs);
    void setPaused(bool paused);
    qint64 playbackPosition() const { return playheadMs_; }
    qint64 viewportStart() const { return viewportStartMs_; }
    qint64 viewportDuration() const { return viewportDurationMs_; }

    /// User-interaction callbacks. They execute on the widget's GUI thread.
    std::function<void(qint64)> onSeekRequested;
    std::function<void(qint64, qint64)> onViewportChanged;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool event(QEvent* event) override;

private:
    enum class PointerMode { None, Pending, Pan, Scrub };

    struct TrackLayout {
        QRectF plot;
        qreal baseline = 0.0;
    };

    static bool validWaveform(const AudioWaveform& waveform, QString* error);
    static qint64 durationMs(const AudioWaveform& waveform);
    static QString formatTime(qint64 milliseconds);
    static qint64 niceTickInterval(qint64 visibleDurationMs, qreal plotWidth);

    QRectF plotRect() const;
    TrackLayout trackLayout(int index) const;
    qint64 timeAtX(qreal x) const;
    qreal xAtTime(qint64 milliseconds) const;
    void setPlayhead(qint64 positionMs, bool requestSeek);
    void setViewport(qint64 startMs, qint64 durationMs, bool notify);
    void zoomAt(qreal factor, qreal anchorX);
    void drawTrack(QPainter& painter, const AudioWaveform& waveform, int trackIndex,
                   const QColor& color, bool magnitude) const;

    AudioWaveform fileA_;
    AudioWaveform fileB_;
    AudioWaveform delta_;
    qint64 totalDurationMs_ = 0;
    qint64 playheadMs_ = 0;
    qint64 viewportStartMs_ = 0;
    qint64 viewportDurationMs_ = 0;
    bool paused_ = true;

    PointerMode pointerMode_ = PointerMode::None;
    QPointF pressPosition_;
    qint64 pressViewportStartMs_ = 0;
    qint64 pressViewportDurationMs_ = 0;
    bool pointerMoved_ = false;

    QColor fileAColor_;
    QColor fileBColor_;
    QColor deltaColor_;
    QColor playheadColor_;
};

}  // namespace openbc::app

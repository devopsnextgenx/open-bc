#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <functional>

class QMouseEvent;
class QPaintEvent;
class QPainter;
class QWheelEvent;

namespace openbc::app {

/// Displays an RGB888 spectral-difference image with a synchronized time viewport and playhead.
///
/// The bitmap is copied when installed, so callers may release or reuse their Rust-side buffer
/// immediately. Connect `onViewportChanged` to the other audio views and forward their updates
/// to `setViewport`; connect `onSeekRequested` to both players.
class AudioSpectralDiffWidget final : public QWidget {
public:
    explicit AudioSpectralDiffWidget(QWidget* parent = nullptr);

    /// Copies a tightly packed or padded RGB888 image into widget-owned storage.
    bool setSpectrogramRgb888(const uchar* pixels, int width, int height, int bytesPerLine,
                              qreal nyquistHz, qint64 durationMs, QString* error = nullptr);

    /// Updates the shared viewport without emitting a viewport-change callback.
    void setViewport(qint64 startMs, qint64 durationMs);

    /// Updates the playhead from playback without requesting another seek.
    void setPlaybackPosition(qint64 positionMs);

    qint64 viewportStart() const { return viewportStartMs_; }
    qint64 viewportDuration() const { return viewportDurationMs_; }
    qint64 playbackPosition() const { return playheadMs_; }

    /// Callbacks execute on the widget's GUI thread.
    std::function<void(qint64)> onSeekRequested;
    std::function<void(qint64, qint64)> onViewportChanged;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    QRectF plotRect() const;
    qint64 timeAtX(qreal x) const;
    qreal xAtTime(qint64 milliseconds) const;
    void setPlayhead(qint64 positionMs, bool requestSeek);
    void setViewportInternal(qint64 startMs, qint64 durationMs, bool notify);
    void zoomAt(qreal factor, qreal anchorX);

    QImage spectrogram_;
    qreal nyquistHz_ = 0.0;
    qint64 totalDurationMs_ = 0;
    qint64 viewportStartMs_ = 0;
    qint64 viewportDurationMs_ = 0;
    qint64 playheadMs_ = 0;
    QPointF pressPosition_;
    qint64 pressViewportStartMs_ = 0;
    qint64 pressViewportDurationMs_ = 0;
    bool pointerDown_ = false;
    bool pointerMoved_ = false;
};

}  // namespace openbc::app

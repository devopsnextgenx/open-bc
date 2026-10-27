#include "viewers/media/audio_spectral_diff_widget.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace openbc::app {
namespace {

constexpr qreal kLeftMargin = 58.0;
constexpr qreal kRightMargin = 8.0;
constexpr qreal kTopMargin = 8.0;
constexpr qreal kBottomMargin = 31.0;
constexpr qreal kDragThreshold = 4.0;

}  // namespace

AudioSpectralDiffWidget::AudioSpectralDiffWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

bool AudioSpectralDiffWidget::setSpectrogramRgb888(const uchar* pixels, int imageWidth,
                                                   int imageHeight, int bytesPerLine,
                                                   qreal nyquistHz, qint64 durationMs,
                                                   QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (imageWidth <= 0 || imageHeight <= 0 || pixels == nullptr) {
        return fail(QStringLiteral("Spectrogram dimensions and pixel data must be non-empty."));
    }
    if (imageWidth > std::numeric_limits<int>::max() / 3 || bytesPerLine < imageWidth * 3) {
        return fail(QStringLiteral("RGB888 row stride is too small for the image width."));
    }
    if (durationMs <= 0) {
        return fail(QStringLiteral("Spectrogram duration must be greater than zero."));
    }
    if (!std::isfinite(nyquistHz) || nyquistHz <= 0.0) {
        return fail(QStringLiteral("Nyquist frequency must be finite and greater than zero."));
    }

    const QImage borrowed(pixels, imageWidth, imageHeight, bytesPerLine, QImage::Format_RGB888);
    if (borrowed.isNull()) return fail(QStringLiteral("Qt could not construct the spectrogram image."));

    QImage owned = borrowed.copy();
    if (owned.isNull()) {
        return fail(QStringLiteral("Qt could not allocate the spectrogram image."));
    }
    spectrogram_ = std::move(owned);
    totalDurationMs_ = durationMs;
    nyquistHz_ = nyquistHz;
    playheadMs_ = std::clamp(playheadMs_, qint64(0), totalDurationMs_);
    viewportStartMs_ = 0;
    viewportDurationMs_ = totalDurationMs_;
    if (error) error->clear();
    update();
    return true;
}

void AudioSpectralDiffWidget::setViewport(qint64 startMs, qint64 durationMs) {
    setViewportInternal(startMs, durationMs, false);
}

void AudioSpectralDiffWidget::setPlaybackPosition(qint64 positionMs) {
    setPlayhead(positionMs, false);
}

QRectF AudioSpectralDiffWidget::plotRect() const {
    return QRectF(kLeftMargin, kTopMargin,
                  std::max<qreal>(0.0, width() - kLeftMargin - kRightMargin),
                  std::max<qreal>(0.0, height() - kTopMargin - kBottomMargin));
}

qint64 AudioSpectralDiffWidget::timeAtX(qreal x) const {
    const QRectF plot = plotRect();
    if (plot.width() <= 0.0 || viewportDurationMs_ <= 0) return viewportStartMs_;
    const qreal fraction = std::clamp((x - plot.left()) / plot.width(), 0.0, 1.0);
    const long double time = static_cast<long double>(viewportStartMs_) +
                             fraction * static_cast<long double>(viewportDurationMs_);
    return std::clamp(static_cast<qint64>(std::llround(time)), qint64(0), totalDurationMs_);
}

qreal AudioSpectralDiffWidget::xAtTime(qint64 milliseconds) const {
    const QRectF plot = plotRect();
    if (plot.width() <= 0.0 || viewportDurationMs_ <= 0) return plot.left();
    const qreal fraction = static_cast<qreal>(milliseconds - viewportStartMs_) /
                           static_cast<qreal>(viewportDurationMs_);
    return plot.left() + fraction * plot.width();
}

void AudioSpectralDiffWidget::setPlayhead(qint64 positionMs, bool requestSeek) {
    const qint64 bounded = std::clamp(positionMs, qint64(0), totalDurationMs_);
    if (playheadMs_ == bounded) return;
    playheadMs_ = bounded;
    update();
    if (requestSeek && onSeekRequested) onSeekRequested(playheadMs_);
}

void AudioSpectralDiffWidget::setViewportInternal(qint64 startMs, qint64 durationMs,
                                                  bool notify) {
    if (totalDurationMs_ <= 0) {
        viewportStartMs_ = 0;
        viewportDurationMs_ = 0;
        update();
        return;
    }
    const qint64 boundedDuration = std::clamp(durationMs, qint64(1), totalDurationMs_);
    const qint64 boundedStart =
        std::clamp(startMs, qint64(0), totalDurationMs_ - boundedDuration);
    if (viewportStartMs_ == boundedStart && viewportDurationMs_ == boundedDuration) return;
    viewportStartMs_ = boundedStart;
    viewportDurationMs_ = boundedDuration;
    update();
    if (notify && onViewportChanged) onViewportChanged(viewportStartMs_, viewportDurationMs_);
}

void AudioSpectralDiffWidget::zoomAt(qreal factor, qreal anchorX) {
    if (totalDurationMs_ <= 0 || viewportDurationMs_ <= 0 || factor <= 0.0) return;
    const QRectF plot = plotRect();
    const qreal fraction = plot.width() > 0.0
                               ? std::clamp((anchorX - plot.left()) / plot.width(), 0.0, 1.0)
                               : 0.5;
    const qint64 anchorTime = timeAtX(anchorX);
    const long double scaled = static_cast<long double>(viewportDurationMs_) * factor;
    const qint64 nextDuration =
        scaled >= static_cast<long double>(totalDurationMs_)
            ? totalDurationMs_
            : std::clamp(static_cast<qint64>(std::llround(scaled)), qint64(1), totalDurationMs_);
    const qint64 nextStart = anchorTime -
        static_cast<qint64>(std::llround(fraction * static_cast<qreal>(nextDuration)));
    setViewportInternal(nextStart, nextDuration, true);
}

void AudioSpectralDiffWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const QRectF plot = plotRect();
    if (plot.width() <= 0.0 || plot.height() <= 0.0) return;

    const QColor text = palette().color(QPalette::Text);
    const QColor grid = palette().color(QPalette::Mid);
    if (!spectrogram_.isNull() && totalDurationMs_ > 0 && viewportDurationMs_ > 0) {
        const qreal leftFraction = static_cast<qreal>(viewportStartMs_) / totalDurationMs_;
        const qreal visibleFraction = static_cast<qreal>(viewportDurationMs_) / totalDurationMs_;
        const QRectF source(leftFraction * spectrogram_.width(), 0.0,
                            visibleFraction * spectrogram_.width(), spectrogram_.height());
        painter.drawImage(plot, spectrogram_, source);
    } else {
        painter.fillRect(plot, palette().color(QPalette::AlternateBase));
    }

    painter.setFont(QFont(font().family(), qMax(8, font().pointSize() - 1)));
    painter.setPen(grid);
    for (int division = 0; division <= 4; ++division) {
        const qreal x = plot.left() + plot.width() * division / 4.0;
        const qreal y = plot.bottom() - plot.height() * division / 4.0;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const QString frequency =
            QStringLiteral("%1").arg(nyquistHz_ * division / 4.0, 0, 'f', 0);
        painter.setPen(text);
        painter.drawText(QRectF(2.0, y - 9.0, kLeftMargin - 8.0, 18.0),
                         Qt::AlignRight | Qt::AlignVCenter, frequency);
        painter.setPen(grid);
    }
    painter.setPen(text);
    painter.drawText(QRectF(2.0, 0.0, kLeftMargin - 8.0, 16.0),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Hz"));
    painter.drawText(QRectF(plot.left(), height() - kBottomMargin + 7.0, plot.width(), 18.0),
                     Qt::AlignHCenter | Qt::AlignVCenter,
                     QStringLiteral("Time  %1 ms  -  %2 ms")
                         .arg(viewportStartMs_)
                         .arg(viewportStartMs_ + viewportDurationMs_));

    const qreal playheadX = xAtTime(playheadMs_);
    if (playheadX >= plot.left() && playheadX <= plot.right()) {
        painter.setPen(QPen(QColor(255, 224, 120), 1.5));
        painter.drawLine(QPointF(playheadX, plot.top()), QPointF(playheadX, plot.bottom()));
    }
    painter.setPen(grid);
    painter.drawRect(plot);
}

void AudioSpectralDiffWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !plotRect().contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    pointerDown_ = true;
    pointerMoved_ = false;
    pressPosition_ = event->position();
    pressViewportStartMs_ = viewportStartMs_;
    pressViewportDurationMs_ = viewportDurationMs_;
    event->accept();
}

void AudioSpectralDiffWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!pointerDown_ || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (!pointerMoved_ &&
        (event->position() - pressPosition_).manhattanLength() >= kDragThreshold) {
        pointerMoved_ = true;
    }
    if (pointerMoved_) {
        const QRectF plot = plotRect();
        const qreal deltaX = event->position().x() - pressPosition_.x();
        const qint64 shift = plot.width() > 0.0
                                 ? static_cast<qint64>(std::llround(
                                       -deltaX * pressViewportDurationMs_ / plot.width()))
                                 : 0;
        setViewportInternal(pressViewportStartMs_ + shift, pressViewportDurationMs_, true);
    }
    event->accept();
}

void AudioSpectralDiffWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !pointerDown_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (!pointerMoved_) setPlayhead(timeAtX(event->position().x()), true);
    pointerDown_ = false;
    pointerMoved_ = false;
    event->accept();
}

void AudioSpectralDiffWidget::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    zoomAt(std::pow(0.8, static_cast<qreal>(delta) / 120.0), event->position().x());
    event->accept();
}

}  // namespace openbc::app

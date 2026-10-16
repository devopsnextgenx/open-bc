#include "viewers/media/audio_diff_visualizer.h"

#include <QEvent>
#include <QFont>
#include <QGestureEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPinchGesture>
#include <QPolygonF>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace openbc::app {
namespace {

constexpr qreal kLabelWidth = 82.0;
constexpr qreal kAxisHeight = 25.0;
constexpr qreal kTrackGap = 5.0;
constexpr qreal kMinimumTrackHeight = 35.0;
constexpr qreal kScrubHitRadius = 8.0;
constexpr qreal kDragThreshold = 4.0;

quint64 binCountFor(quint64 frames, quint64 framesPerBin) {
    return frames / framesPerBin + (frames % framesPerBin != 0 ? 1 : 0);
}

qint64 saturatingDurationMs(quint64 frames, quint32 sampleRate) {
    if (sampleRate == 0) return 0;
    const long double milliseconds =
        static_cast<long double>(frames) * 1000.0L / static_cast<long double>(sampleRate);
    return milliseconds >= static_cast<long double>(std::numeric_limits<qint64>::max())
               ? std::numeric_limits<qint64>::max()
               : static_cast<qint64>(std::ceil(milliseconds));
}

}  // namespace

AudioDiffVisualizer::AudioDiffVisualizer(QWidget* parent)
    : QWidget(parent),
      fileAColor_(QColor(92, 176, 255)),
      fileBColor_(QColor(111, 205, 142)),
      deltaColor_(QColor(255, 92, 70)),
      playheadColor_(QColor(255, 224, 120)) {
    setMinimumSize(320, 3 * (kMinimumTrackHeight + kTrackGap) + kAxisHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    grabGesture(Qt::PinchGesture);
}

bool AudioDiffVisualizer::validWaveform(const AudioWaveform& waveform, QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    if (waveform.frameCount == 0) {
        if (!waveform.levels.isEmpty()) return fail("An empty waveform must not contain peak levels.");
        return true;
    }
    if (waveform.sampleRate == 0) return fail("A non-empty waveform needs a non-zero sample rate.");
    if (waveform.levels.isEmpty()) return fail("A non-empty waveform needs at least one peak level.");

    quint64 previousBinSize = 0;
    for (const AudioPeakLevel& level : waveform.levels) {
        if (level.framesPerBin == 0 || level.framesPerBin <= previousBinSize) {
            return fail("Peak levels must have strictly increasing, non-zero frames-per-bin values.");
        }
        if (static_cast<quint64>(level.bins.size()) != binCountFor(waveform.frameCount, level.framesPerBin)) {
            return fail("Peak level bin count does not match the waveform frame count.");
        }
        previousBinSize = level.framesPerBin;
    }
    return true;
}

qint64 AudioDiffVisualizer::durationMs(const AudioWaveform& waveform) {
    return saturatingDurationMs(waveform.frameCount, waveform.sampleRate);
}

bool AudioDiffVisualizer::setWaveforms(AudioWaveform fileA, AudioWaveform fileB,
                                       AudioWaveform delta, QString* error) {
    QString validationError;
    if (!validWaveform(fileA, &validationError) ||
        !validWaveform(fileB, &validationError) ||
        !validWaveform(delta, &validationError)) {
        if (error) *error = validationError;
        return false;
    }

    const quint32 sampleRate = fileA.frameCount != 0 ? fileA.sampleRate
                                : fileB.frameCount != 0 ? fileB.sampleRate
                                : delta.sampleRate;
    if ((fileA.frameCount != 0 && fileA.sampleRate != sampleRate) ||
        (fileB.frameCount != 0 && fileB.sampleRate != sampleRate) ||
        (delta.frameCount != 0 && delta.sampleRate != sampleRate)) {
        if (error) *error = "The two files and their delta must use the same sample rate.";
        return false;
    }
    if (delta.frameCount > std::min(fileA.frameCount, fileB.frameCount)) {
        if (error) *error = "The delta waveform cannot extend beyond the shared audio duration.";
        return false;
    }

    fileA_ = std::move(fileA);
    fileB_ = std::move(fileB);
    delta_ = std::move(delta);
    totalDurationMs_ = std::max(durationMs(fileA_), durationMs(fileB_));
    playheadMs_ = std::min(playheadMs_, totalDurationMs_);
    viewportStartMs_ = 0;
    viewportDurationMs_ = totalDurationMs_;
    if (error) error->clear();
    update();
    return true;
}

void AudioDiffVisualizer::setPlaybackPosition(qint64 positionMs) {
    setPlayhead(positionMs, false);
}

void AudioDiffVisualizer::setPaused(bool paused) {
    paused_ = paused;
    update();
}

QRectF AudioDiffVisualizer::plotRect() const {
    return QRectF(kLabelWidth, 3.0, std::max<qreal>(0.0, width() - kLabelWidth - 4.0),
                  std::max<qreal>(0.0, height() - kAxisHeight - 6.0));
}

AudioDiffVisualizer::TrackLayout AudioDiffVisualizer::trackLayout(int index) const {
    const QRectF all = plotRect();
    const qreal trackHeight = (all.height() - 2.0 * kTrackGap) / 3.0;
    const QRectF track(all.left(), all.top() + index * (trackHeight + kTrackGap),
                       all.width(), trackHeight);
    return {track, track.center().y()};
}

qint64 AudioDiffVisualizer::timeAtX(qreal x) const {
    const QRectF plot = plotRect();
    if (plot.width() <= 0.0 || viewportDurationMs_ <= 0) return viewportStartMs_;
    const qreal fraction = std::clamp((x - plot.left()) / plot.width(), 0.0, 1.0);
    const long double time = static_cast<long double>(viewportStartMs_) +
                             static_cast<long double>(fraction) * viewportDurationMs_;
    return std::clamp(static_cast<qint64>(std::llround(time)), qint64(0), totalDurationMs_);
}

qreal AudioDiffVisualizer::xAtTime(qint64 milliseconds) const {
    const QRectF plot = plotRect();
    if (plot.width() <= 0.0 || viewportDurationMs_ <= 0) return plot.left();
    const qreal fraction = static_cast<qreal>(milliseconds - viewportStartMs_) /
                           static_cast<qreal>(viewportDurationMs_);
    return plot.left() + fraction * plot.width();
}

void AudioDiffVisualizer::setPlayhead(qint64 positionMs, bool requestSeek) {
    const qint64 bounded = std::clamp(positionMs, qint64(0), totalDurationMs_);
    if (playheadMs_ == bounded) return;
    playheadMs_ = bounded;
    update();
    if (requestSeek && onSeekRequested) onSeekRequested(playheadMs_);
}

void AudioDiffVisualizer::setViewport(qint64 startMs, qint64 duration, bool notify) {
    if (totalDurationMs_ <= 0) {
        viewportStartMs_ = 0;
        viewportDurationMs_ = 0;
        update();
        return;
    }

    const qint64 boundedDuration = std::clamp(duration, qint64(1), totalDurationMs_);
    const qint64 boundedStart = std::clamp(startMs, qint64(0), totalDurationMs_ - boundedDuration);
    if (viewportStartMs_ == boundedStart && viewportDurationMs_ == boundedDuration) return;
    viewportStartMs_ = boundedStart;
    viewportDurationMs_ = boundedDuration;
    update();
    if (notify && onViewportChanged) onViewportChanged(viewportStartMs_, viewportDurationMs_);
}

void AudioDiffVisualizer::zoomAt(qreal factor, qreal anchorX) {
    if (totalDurationMs_ <= 0 || viewportDurationMs_ <= 0 || factor <= 0.0) return;
    const QRectF plot = plotRect();
    const qreal fraction = plot.width() > 0.0
                               ? std::clamp((anchorX - plot.left()) / plot.width(), 0.0, 1.0)
                               : 0.5;
    const qint64 anchorTime = timeAtX(anchorX);
    const long double scaledDuration =
        static_cast<long double>(viewportDurationMs_) * static_cast<long double>(factor);
    const qint64 nextDuration = scaledDuration >= static_cast<long double>(totalDurationMs_)
                                    ? totalDurationMs_
                                    : std::clamp(static_cast<qint64>(std::llround(scaledDuration)),
                                                 qint64(1), totalDurationMs_);
    const qint64 nextStart = anchorTime -
        static_cast<qint64>(std::llround(fraction * static_cast<qreal>(nextDuration)));
    setViewport(nextStart, nextDuration, true);
}

void AudioDiffVisualizer::drawTrack(QPainter& painter, const AudioWaveform& waveform,
                                    int trackIndex, const QColor& color, bool magnitude) const {
    const TrackLayout layout = trackLayout(trackIndex);
    const QRectF plot = layout.plot;
    if (waveform.frameCount == 0 || waveform.sampleRate == 0 || waveform.levels.isEmpty() ||
        plot.width() <= 0.0 || viewportDurationMs_ <= 0) {
        return;
    }

    const long double framesPerPixel =
        static_cast<long double>(viewportDurationMs_) * waveform.sampleRate /
        (1000.0L * std::max<qreal>(1.0, plot.width()));
    const AudioPeakLevel* selected = &waveform.levels.front();
    for (const AudioPeakLevel& level : waveform.levels) {
        if (static_cast<long double>(level.framesPerBin) > framesPerPixel) break;
        selected = &level;
    }

    QVector<QLineF> strokes;
    strokes.reserve(static_cast<qsizetype>(std::ceil(plot.width())));
    QPolygonF fill;
    if (magnitude) fill.reserve(static_cast<qsizetype>(2.0 * std::ceil(plot.width())));

    const qreal amplitudeHeight = plot.height() * 0.42;
    const long double visibleStartFrame =
        static_cast<long double>(viewportStartMs_) * waveform.sampleRate / 1000.0L;
    const long double framesInViewport =
        static_cast<long double>(viewportDurationMs_) * waveform.sampleRate / 1000.0L;
    const int pixelCount = std::max(1, static_cast<int>(std::ceil(plot.width())));

    for (int pixel = 0; pixel < pixelCount; ++pixel) {
        const long double firstFrame = visibleStartFrame +
            framesInViewport * pixel / pixelCount;
        const long double lastFrame = visibleStartFrame +
            framesInViewport * (pixel + 1) / pixelCount;
        quint64 begin = static_cast<quint64>(std::max(0.0L, std::floor(firstFrame)));
        quint64 end = static_cast<quint64>(std::max(0.0L, std::ceil(lastFrame)));
        begin = std::min(begin, waveform.frameCount);
        end = std::min(end, waveform.frameCount);
        if (begin == end && begin < waveform.frameCount) ++end;
        if (begin >= end) continue;

        const quint64 firstBin = begin / selected->framesPerBin;
        const quint64 lastBin = (end - 1) / selected->framesPerBin;
        float minimum = std::numeric_limits<float>::infinity();
        float maximum = -std::numeric_limits<float>::infinity();
        for (quint64 bin = firstBin; bin <= lastBin; ++bin) {
            const AudioPeak peak = selected->bins[static_cast<qsizetype>(bin)];
            minimum = std::min(minimum, peak.minimum);
            maximum = std::max(maximum, peak.maximum);
        }

        const qreal x = plot.left() + (pixel + 0.5) * plot.width() / pixelCount;
        if (magnitude) {
            const qreal amplitude = std::clamp<qreal>(maximum, 0.0, 1.0);
            const qreal top = layout.baseline - amplitude * amplitudeHeight;
            strokes.append(QLineF(x, layout.baseline, x, top));
            fill.append(QPointF(x, top));
        } else {
            const qreal low = std::clamp<qreal>(minimum, -1.0, 1.0);
            const qreal high = std::clamp<qreal>(maximum, -1.0, 1.0);
            strokes.append(QLineF(x, layout.baseline - high * amplitudeHeight,
                                  x, layout.baseline - low * amplitudeHeight));
        }
    }

    painter.setPen(QPen(color, 1.0));
    painter.drawLines(strokes);
    if (magnitude && !fill.isEmpty()) {
        QPolygonF area = fill;
        area.append(QPointF(fill.last().x(), layout.baseline));
        area.append(QPointF(fill.first().x(), layout.baseline));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(color.red(), color.green(), color.blue(), 42));
        painter.drawPolygon(area);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, 1.0));
        painter.drawLines(strokes);
    }
}

QString AudioDiffVisualizer::formatTime(qint64 milliseconds) {
    const qint64 minutes = milliseconds / 60000;
    const qint64 seconds = (milliseconds / 1000) % 60;
    const qint64 millis = milliseconds % 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(millis, 3, 10, QLatin1Char('0'));
}

qint64 AudioDiffVisualizer::niceTickInterval(qint64 visibleDurationMs, qreal plotWidth) {
    const qreal targetCount = std::max<qreal>(1.0, plotWidth / 105.0);
    const qreal raw = static_cast<qreal>(visibleDurationMs) / targetCount;
    const qreal magnitude = std::pow(10.0, std::floor(std::log10(std::max<qreal>(1.0, raw))));
    const qreal normalized = raw / magnitude;
    const qreal multiplier = normalized <= 1.0 ? 1.0 : normalized <= 2.0 ? 2.0 : normalized <= 5.0 ? 5.0 : 10.0;
    const qreal interval = multiplier * magnitude;
    return interval >= static_cast<qreal>(std::numeric_limits<qint64>::max())
               ? std::numeric_limits<qint64>::max()
               : std::max<qint64>(1, static_cast<qint64>(interval));
}

void AudioDiffVisualizer::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const QRectF plot = plotRect();
    if (plot.width() <= 0.0 || plot.height() <= 0.0) return;

    const QColor grid = palette().color(QPalette::Mid);
    const QColor text = palette().color(QPalette::Text);
    const QStringList names{QStringLiteral("Audio File A"), QStringLiteral("Audio File B"),
                            QStringLiteral("Delta |A-B|")};

    const qint64 tickInterval = niceTickInterval(viewportDurationMs_, plot.width());
    qint64 firstTick = 0;
    if (tickInterval > 0) {
        const qint64 quotient = viewportStartMs_ / tickInterval;
        const qint64 remainder = viewportStartMs_ % tickInterval;
        const qint64 nextQuotient = quotient + (remainder != 0 ? 1 : 0);
        firstTick = nextQuotient > std::numeric_limits<qint64>::max() / tickInterval
                        ? std::numeric_limits<qint64>::max()
                        : nextQuotient * tickInterval;
    }
    painter.setFont(QFont(font().family(), qMax(8, font().pointSize() - 1)));

    for (int index = 0; index < 3; ++index) {
        const TrackLayout layout = trackLayout(index);
        painter.fillRect(layout.plot, index == 2 ? QColor(deltaColor_.red(), deltaColor_.green(), deltaColor_.blue(), 10)
                                                : QColor(text.red(), text.green(), text.blue(), 9));
        painter.setPen(grid);
        painter.drawLine(layout.plot.topLeft(), layout.plot.topRight());
        painter.drawLine(layout.plot.bottomLeft(), layout.plot.bottomRight());
        painter.drawLine(QPointF(layout.plot.left(), layout.baseline),
                         QPointF(layout.plot.right(), layout.baseline));
        painter.setPen(text);
        painter.drawText(QRectF(4.0, layout.plot.top(), kLabelWidth - 10.0, layout.plot.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, names[index]);
    }

    painter.setPen(grid);
    for (qint64 tick = firstTick; tick <= viewportStartMs_ + viewportDurationMs_;) {
        const qreal x = xAtTime(tick);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        if (tick > std::numeric_limits<qint64>::max() - tickInterval) break;
        tick += tickInterval;
    }

    drawTrack(painter, fileA_, 0, fileAColor_, false);
    drawTrack(painter, fileB_, 1, fileBColor_, false);
    drawTrack(painter, delta_, 2, deltaColor_, true);

    painter.setPen(QPen(playheadColor_, 1.5));
    const qreal playheadX = xAtTime(playheadMs_);
    if (playheadX >= plot.left() && playheadX <= plot.right()) {
        painter.drawLine(QPointF(playheadX, plot.top()), QPointF(playheadX, plot.bottom()));
        painter.setBrush(playheadColor_);
        painter.drawEllipse(QPointF(playheadX, plot.top()), 3.0, 3.0);
    }

    painter.setPen(text);
    const qreal axisTop = height() - kAxisHeight;
    painter.drawLine(QPointF(plot.left(), axisTop), QPointF(plot.right(), axisTop));
    for (qint64 tick = firstTick; tick <= viewportStartMs_ + viewportDurationMs_;) {
        const qreal x = xAtTime(tick);
        painter.drawLine(QPointF(x, axisTop), QPointF(x, axisTop + 4.0));
        const QString label = formatTime(tick);
        const qreal labelWidth = painter.fontMetrics().horizontalAdvance(label) + 4.0;
        painter.drawText(QRectF(x - labelWidth / 2.0, axisTop + 4.0, labelWidth, kAxisHeight - 4.0),
                         Qt::AlignHCenter | Qt::AlignTop, label);
        if (tick > std::numeric_limits<qint64>::max() - tickInterval) break;
        tick += tickInterval;
    }
}

void AudioDiffVisualizer::mousePressEvent(QMouseEvent* event) {
    const QRectF plot = plotRect();
    const QRectF timeline(plot.left(), plot.top(), plot.width(), height() - plot.top());
    if (event->button() != Qt::LeftButton || !timeline.contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }
    pressPosition_ = event->position();
    pressViewportStartMs_ = viewportStartMs_;
    pressViewportDurationMs_ = viewportDurationMs_;
    pointerMoved_ = false;
    pointerMode_ = std::abs(xAtTime(playheadMs_) - pressPosition_.x()) <= kScrubHitRadius
                       ? PointerMode::Scrub
                       : PointerMode::Pending;
    event->accept();
}

void AudioDiffVisualizer::mouseMoveEvent(QMouseEvent* event) {
    if (pointerMode_ == PointerMode::None || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (!pointerMoved_ &&
        (event->position() - pressPosition_).manhattanLength() >= kDragThreshold) {
        pointerMoved_ = true;
        if (pointerMode_ == PointerMode::Pending) pointerMode_ = PointerMode::Pan;
    }

    if (!pointerMoved_) return;
    if (pointerMode_ == PointerMode::Scrub) {
        setPlayhead(timeAtX(event->position().x()), true);
    } else if (pointerMode_ == PointerMode::Pan) {
        const QRectF plot = plotRect();
        const qreal deltaX = event->position().x() - pressPosition_.x();
        const qint64 shift = plot.width() > 0.0
                                 ? static_cast<qint64>(std::llround(
                                       -deltaX * static_cast<qreal>(pressViewportDurationMs_) / plot.width()))
                                 : 0;
        setViewport(pressViewportStartMs_ + shift, pressViewportDurationMs_, true);
    }
    event->accept();
}

void AudioDiffVisualizer::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || pointerMode_ == PointerMode::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (pointerMode_ == PointerMode::Scrub) {
        setPlayhead(timeAtX(event->position().x()), true);
    } else if (!pointerMoved_ && paused_) {
        setPlayhead(timeAtX(event->position().x()), true);
    }
    pointerMode_ = PointerMode::None;
    pointerMoved_ = false;
    event->accept();
}

void AudioDiffVisualizer::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    zoomAt(std::pow(0.8, static_cast<qreal>(delta) / 120.0), event->position().x());
    event->accept();
}

bool AudioDiffVisualizer::event(QEvent* event) {
    if (event->type() == QEvent::Gesture) {
        auto* gestures = static_cast<QGestureEvent*>(event);
        if (auto* pinch = static_cast<QPinchGesture*>(gestures->gesture(Qt::PinchGesture))) {
            if (pinch->changeFlags() & QPinchGesture::ScaleFactorChanged) {
                zoomAt(1.0 / pinch->lastScaleFactor() * pinch->scaleFactor(),
                       pinch->centerPoint().x());
            }
            gestures->accept(pinch);
            return true;
        }
    }
    return QWidget::event(event);
}

}  // namespace openbc::app

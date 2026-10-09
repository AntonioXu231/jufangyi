#include "waveform_widget.h"

#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>

WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(105);
    setFocusPolicy(Qt::StrongFocus);
    auto *refresh = new QTimer(this);
    refresh->setInterval(50);
    connect(refresh, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        constexpr qint64 kPulseHoldMs = 15000;
        bool visualsChanged = expirePulses(now);
        if (m_trigger.active && now - m_trigger.createdMs > kPulseHoldMs) {
            m_trigger.active = false;
            visualsChanged = true;
        }
        if (visualsChanged || !m_pulses.isEmpty() || m_trigger.active) update();
    });
    refresh->start();
}

bool WaveformWidget::expirePulses(qint64 nowMs)
{
    constexpr qint64 kPulseHoldMs = 15000;
    int expired = 0;
    while (expired < m_pulses.size() &&
           nowMs - m_pulses[expired].createdMs > kPulseHoldMs)
        ++expired;
    if (expired == 0) return false;
    m_pulses.remove(0, expired);
    return true;
}

void WaveformWidget::setTraceColor(const QColor &color)
{
    m_traceColor = color;
    update();
}

WidgetPaintMetrics WaveformWidget::takePaintMetrics()
{
    const WidgetPaintMetrics metrics = m_paintMetrics;
    m_paintMetrics = {};
    return metrics;
}

void WaveformWidget::setFrame(const QVector<qint16> &samples, const QVector<PdPulse> &pulses)
{
    constexpr int maximumHistorySamples = 131072;
    constexpr int kTriggerPreSamples = 256;
    constexpr int kTriggerPostSamples = 768;
    constexpr qint64 kPulseHoldMs = 15000;
    if (m_singleFrameMode) {
        m_samples.clear();
        m_pulses.clear();
        m_trigger.active = false;
    }
    m_fullCycleEnvelope = false;
    m_envelopeMinimum.clear();
    m_envelopeMaximum.clear();
    m_sourceSampleCount = 0U;
    const int oldCount = m_samples.size();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_samples += samples;
    for (const auto &pulse : pulses) {
        PdPulse positioned = pulse;
        positioned.sampleIndex += oldCount;
        m_pulses.append(PulseVisual{positioned, now, false});
    }
    const int trim = m_singleFrameMode
        ? 0 : qMax(0, m_samples.size() - maximumHistorySamples);
    if (trim > 0) {
        m_samples.remove(0, trim);
        QVector<PulseVisual> retained;
        for (auto pulse : m_pulses) {
            pulse.pulse.sampleIndex -= trim;
            if (pulse.pulse.sampleIndex >= 0 && now - pulse.createdMs <= kPulseHoldMs)
                retained.append(pulse);
        }
        m_pulses = retained;
        if (m_trigger.active) {
            m_trigger.startIndex -= trim;
            m_trigger.endIndex -= trim;
            if (m_trigger.endIndex < 0) m_trigger.active = false;
        }
    }
    // Wait until the post-trigger samples have arrived, then latch the
    // complete 256-pre/768-post window for the highlighted waveform band.
    for (int i = m_pulses.size() - 1; i >= 0; --i) {
        auto &pulse = m_pulses[i];
        const int center = pulse.pulse.sampleIndex;
        if (pulse.triggerCaptured || center < kTriggerPreSamples ||
            m_samples.size() - center <= kTriggerPostSamples) continue;
        m_trigger.active = true;
        m_trigger.startIndex = center - kTriggerPreSamples;
        m_trigger.endIndex = center + kTriggerPostSamples;
        m_trigger.createdMs = now;
        pulse.triggerCaptured = true;
        break;
    }
    if (m_trigger.active && now - m_trigger.createdMs > kPulseHoldMs)
        m_trigger.active = false;
    if (m_followLatestWindow)
        m_visibleSamples = m_singleFrameMode
            ? m_samples.size() : qMax(128, qMin(16384, m_samples.size()));
    else
        m_visibleSamples = qMin(m_visibleSamples, qMax(128, m_samples.size()));
    update();
}

void WaveformWidget::setEnvelope(const QVector<qint16> &minimum,
                                 const QVector<qint16> &maximum,
                                 quint32 sourceSampleCount)
{
    if (minimum.size() != maximum.size() || minimum.isEmpty()) return;
    m_samples.clear();
    m_fullCycleEnvelope = true;
    m_envelopeMinimum = minimum;
    m_envelopeMaximum = maximum;
    m_sourceSampleCount = sourceSampleCount;
    m_visibleSamples = m_followLatestWindow ? minimum.size()
                                             : qMin(m_visibleSamples, minimum.size());
    expirePulses(QDateTime::currentMSecsSinceEpoch());
    update();
}

void WaveformWidget::appendPhaseEvents(const QVector<PdPulse> &events)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &event : events) {
        if (!event.phaseValid) continue;
        PdPulse positioned = event;
        positioned.sampleIndex = static_cast<int>(event.phaseDeg * 1024.0 / 360.0);
        m_pulses.append(PulseVisual{positioned, now, false});
    }
    update();
}

void WaveformWidget::setEventThreshold(double rawQ88)
{
    m_eventThreshold = qMax(0.0, rawQ88);
    update();
}

void WaveformWidget::setSingleFrameMode(bool enabled)
{
    m_singleFrameMode = enabled;
    m_samples.clear();
    m_pulses.clear();
    m_trigger.active = false;
    m_followLatestWindow = true;
    m_visibleSamples = 16384;
    update();
}

void WaveformWidget::resetZoom()
{
    m_followLatestWindow = true;
    const int available = m_fullCycleEnvelope ? m_envelopeMinimum.size() : m_samples.size();
    m_visibleSamples = m_fullCycleEnvelope ? available : qMax(128, qMin(16384, available));
    update();
}

void WaveformWidget::wheelEvent(QWheelEvent *event)
{
    const int available = m_fullCycleEnvelope ? m_envelopeMinimum.size() : m_samples.size();
    if (available == 0) return;
    const bool magnify = event->angleDelta().y() > 0;
    const int next = magnify ? static_cast<int>(m_visibleSamples * 0.72)
                             : static_cast<int>(m_visibleSamples / 0.72);
    m_visibleSamples = qBound(qMin(128, available), next, available);
    m_followLatestWindow = false;
    update();
    event->accept();
}

void WaveformWidget::paintEvent(QPaintEvent *)
{
    struct EventColumn {
        int count = 0;
        int positiveCount = 0;
        int negativeCount = 0;
        double topY = 0.0;
        double bottomY = 0.0;
        double fadeSum = 0.0;
    };
    QElapsedTimer paintTimer;
    paintTimer.start();
    const int retainedEvents = m_pulses.size();
    const auto recordPaintMetrics = [this, &paintTimer, retainedEvents](
        quint64 sampleVisits, quint64 eventVisits) {
        const qint64 elapsedNs = paintTimer.nsecsElapsed();
        ++m_paintMetrics.paintCount;
        m_paintMetrics.sampleVisits += sampleVisits;
        m_paintMetrics.eventVisits += eventVisits;
        m_paintMetrics.totalPaintNs += elapsedNs;
        m_paintMetrics.maxPaintNs = qMax(m_paintMetrics.maxPaintNs, elapsedNs);
        m_paintMetrics.maxRetainedEvents =
            qMax(m_paintMetrics.maxRetainedEvents, retainedEvents);
    };
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(8, 15, 25));
    const QRectF area = rect().adjusted(48, 18, -12, -24);
    p.setPen(QPen(QColor(58, 105, 130), 1));
    p.drawRect(area);
    p.setPen(QPen(QColor(52, 81, 100), 1, Qt::DashLine));
    p.drawLine(QPointF(area.left(), area.center().y()), QPointF(area.right(), area.center().y()));
    for (int i = 1; i < 4; ++i)
        p.drawLine(QPointF(area.left() + area.width() * i / 4.0, area.top()),
                   QPointF(area.left() + area.width() * i / 4.0, area.bottom()));
    p.setPen(QColor(170, 210, 230));
    p.drawText(6, 18, QStringLiteral("ADC"));
    p.drawText(3, static_cast<int>(area.top() + 12), QStringLiteral("+2048"));
    p.drawText(4, static_cast<int>(area.center().y()), QStringLiteral("0"));
    p.drawText(3, static_cast<int>(area.bottom()), QStringLiteral("-2048"));
    p.drawText(area.left(), height() - 5,
               m_fullCycleEnvelope
                   ? QStringLiteral("完整周期：%1 点 · 当前显示 %2 相位列")
                         .arg(m_sourceSampleCount).arg(m_visibleSamples)
                   : QStringLiteral("滚轮缩放：%1 点").arg(m_visibleSamples));
    if (m_samples.isEmpty() && m_envelopeMinimum.isEmpty()) {
        p.drawText(area, Qt::AlignCenter, QStringLiteral("等待实时原始波形帧"));
        recordPaintMetrics(0U, 0U);
        return;
    }
    const int available = m_fullCycleEnvelope ? m_envelopeMinimum.size() : m_samples.size();
    const int visible = qMin(m_visibleSamples, available);
    const int first = available - visible;
    const auto map = [&](int index, double value) {
        const double x = area.left() + (index - first) * area.width() / qMax(1, visible - 1);
        /* Fixed ADC scale makes amplitudes comparable across frames/channels. */
        const double y = area.center().y() - value * area.height() * 0.44 / 2048.0;
        return QPointF(x, y);
    };
    if (m_fullCycleEnvelope) {
        QPainterPath high(map(first, m_envelopeMaximum[first]));
        QPainterPath low(map(first, m_envelopeMinimum[first]));
        p.setPen(QPen(QColor(m_traceColor.red(), m_traceColor.green(),
                              m_traceColor.blue(), 95), 1.0));
        for (int i = first; i < first + visible; ++i) {
            const QPointF top = map(i, m_envelopeMaximum[i]);
            const QPointF bottom = map(i, m_envelopeMinimum[i]);
            p.drawLine(top, bottom);
            if (i != first) {
                high.lineTo(top);
                low.lineTo(bottom);
            }
        }
        p.setPen(QPen(m_traceColor, 1.0));
        p.drawPath(high);
        p.drawPath(low);
    } else {
        const int pixelColumns = qMax(1, qRound(area.width()));
        if (visible <= pixelColumns * 2) {
            QPainterPath path(map(first, m_samples[first]));
            for (int i = first + 1; i < first + visible; ++i)
                path.lineTo(map(i, m_samples[i]));
            p.setPen(QPen(m_traceColor, 1.1));
            p.drawPath(path);
        } else {
            /* Visit every retained sample, but render each horizontal pixel as
             * its min/max pair. This preserves narrow transients without
             * constructing a million-segment path or slowing live capture. */
            p.setPen(QPen(m_traceColor, 1.0));
            QPainterPath centers;
            for (int column = 0; column < pixelColumns; ++column) {
                const int begin = first + static_cast<int>(
                    (static_cast<qint64>(column) * visible) / pixelColumns);
                const int end = first + static_cast<int>(
                    (static_cast<qint64>(column + 1) * visible) / pixelColumns);
                if (begin >= end) continue;
                qint16 low = m_samples[begin];
                qint16 high = low;
                for (int sample = begin + 1; sample < end; ++sample) {
                    low = qMin(low, m_samples[sample]);
                    high = qMax(high, m_samples[sample]);
                }
                const double x = area.left() + column;
                p.drawLine(QPointF(x, map(begin, high).y()),
                           QPointF(x, map(begin, low).y()));
                const double mean = (static_cast<double>(low) + high) * 0.5;
                if (column == 0) centers.moveTo(x, map(begin, mean).y());
                else centers.lineTo(x, map(begin, mean).y());
            }
            p.setPen(QPen(m_traceColor.lighter(125), 1.0));
            p.drawPath(centers);
        }
    }
    if (!m_fullCycleEnvelope && m_trigger.active && m_trigger.endIndex >= first &&
        m_trigger.startIndex < m_samples.size()) {
        const int triggerStart = qMax(first, m_trigger.startIndex);
        const int triggerEnd = qMin(m_samples.size() - 1, m_trigger.endIndex);
        const double x0 = map(triggerStart, 0.0).x();
        const double x1 = map(triggerEnd, 0.0).x();
        p.fillRect(QRectF(x0, area.top(), qMax(2.0, x1 - x0), area.height()),
                   QColor(m_traceColor.red(), m_traceColor.green(), m_traceColor.blue(), 28));
        p.setPen(QColor(m_traceColor).lighter(145));
        p.drawText(QPointF(x0 + 5.0, area.top() + 15.0),
                   QStringLiteral("TRIGGER 256+768"));
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kPulseHoldMs = 15000;
    const int eventPixelColumns = qMax(1, qRound(area.width()));
    QVector<EventColumn> eventColumns(eventPixelColumns);
    for (const auto &visual : m_pulses) {
        const auto &pulse = visual.pulse;
        if (std::abs(pulse.amplitude) < m_eventThreshold) continue;
        const int pulseIndex = m_fullCycleEnvelope && pulse.phaseValid
            ? static_cast<int>(pulse.phaseDeg * available / 360.0) : pulse.sampleIndex;
        if (pulseIndex < first || pulseIndex >= first + visible) continue;
        const double fade = qBound(0.0, 1.0 -
                                   static_cast<double>(now - visual.createdMs) / kPulseHoldMs,
                                   1.0);
        const QPointF point = m_fullCycleEnvelope
            ? QPointF(map(pulseIndex, 0.0).x(), area.center().y())
            : map(pulseIndex, m_samples[pulseIndex]);
        const int column = qBound(0,
            static_cast<int>(point.x() - area.left()), eventPixelColumns - 1);
        EventColumn &bucket = eventColumns[column];
        if (bucket.count == 0) {
            bucket.topY = point.y();
            bucket.bottomY = point.y();
        } else {
            bucket.topY = qMin(bucket.topY, point.y());
            bucket.bottomY = qMax(bucket.bottomY, point.y());
        }
        ++bucket.count;
        if (pulse.amplitude < 0.0) ++bucket.negativeCount;
        else ++bucket.positiveCount;
        bucket.fadeSum += fade;
    }

    /* Keep every pulse in the 15-second model, but render at screen resolution:
     * one density marker per pixel column avoids tens of thousands of text and
     * painter operations while retaining pile-up count and amplitude spread. */
    int annotationBudget = 8;
    int lastAnnotationColumn = -72;
    for (int column = 0; column < eventColumns.size(); ++column) {
        const EventColumn &bucket = eventColumns[column];
        if (bucket.count == 0) continue;
        QColor pulseColor = m_traceColor;
        if (bucket.positiveCount > bucket.negativeCount)
            pulseColor = pulseColor.lighter(140);
        else if (bucket.negativeCount > bucket.positiveCount)
            pulseColor = pulseColor.darker(145);
        const double logCount = std::log2(static_cast<double>(bucket.count) + 1.0);
        const double averageFade = bucket.fadeSum / bucket.count;
        const int alpha = qBound(28, static_cast<int>(
            (72.0 + 34.0 * logCount) * (0.35 + 0.65 * averageFade)), 235);
        pulseColor.setAlpha(alpha);
        const double x = area.left() + column + 0.5;
        const double y = (bucket.topY + bucket.bottomY) * 0.5;
        p.setPen(QPen(pulseColor, qMin(4.0, 1.1 + 0.45 * logCount),
                      Qt::SolidLine, Qt::RoundCap));
        if (bucket.bottomY - bucket.topY > 1.0)
            p.drawLine(QPointF(x, bucket.topY), QPointF(x, bucket.bottomY));
        else
            p.drawLine(QPointF(x, y - 3.0), QPointF(x, y + 3.0));
        p.setBrush(pulseColor);
        p.drawEllipse(QPointF(x, y), qMin(5.0, 1.6 + 0.45 * logCount),
                      qMin(5.0, 1.6 + 0.45 * logCount));
        if (bucket.count > 1 && annotationBudget > 0 &&
            column - lastAnnotationColumn >= 72) {
            p.setPen(QColor(245, 250, 255, 220));
            p.drawText(QPointF(x + 5.0, y - 5.0),
                       QStringLiteral("×%1").arg(bucket.count));
            lastAnnotationColumn = column;
            --annotationBudget;
        }
    }
    recordPaintMetrics(static_cast<quint64>(visible),
                       static_cast<quint64>(retainedEvents));
}

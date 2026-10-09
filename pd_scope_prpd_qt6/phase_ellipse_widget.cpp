#include "phase_ellipse_widget.h"

#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <cmath>
#include <climits>

namespace {
constexpr qint64 kPulseHoldNs = 15000000000LL;
constexpr int kPhaseBuckets = 3600; // 0.1 degree per display bin.
constexpr double kPi = 3.14159265358979323846;
}

PhaseEllipseWidget::PhaseVisual PhaseEllipseWidget::makePhaseVisual(
    const PdPulse &event, qint64 receivedAtNs) const
{
    double phase = std::fmod(event.phaseDeg, 360.0);
    if (phase < 0.0) phase += 360.0;
    const int bucket = qBound(0, static_cast<int>(phase * 10.0), kPhaseBuckets - 1);
    return PhaseVisual{event, receivedAtNs, receivedAtNs + kPulseHoldNs,
                       bucket, std::abs(event.amplitude) >= m_eventThreshold};
}

PhaseEllipseWidget::PhaseEllipseWidget(QWidget *parent) : QWidget(parent),
    m_phaseBucketCounts(kPhaseBuckets, 0U)
{
    setMinimumHeight(125);
    setFocusPolicy(Qt::StrongFocus);
    m_clock.start();
    m_expiryTimer = new QTimer(this);
    m_expiryTimer->setSingleShot(true);
    m_expiryTimer->setTimerType(Qt::PreciseTimer);
    connect(m_expiryTimer, &QTimer::timeout, this, &PhaseEllipseWidget::expirePhaseEvents);
}

void PhaseEllipseWidget::addToPhaseBucket(const PhaseVisual &visual)
{
    if (visual.passesThreshold) ++m_phaseBucketCounts[visual.phaseBucket];
}

void PhaseEllipseWidget::removeFromPhaseBucket(const PhaseVisual &visual)
{
    if (visual.passesThreshold && m_phaseBucketCounts[visual.phaseBucket] > 0U)
        --m_phaseBucketCounts[visual.phaseBucket];
}

void PhaseEllipseWidget::rebuildPhaseBuckets()
{
    m_phaseBucketCounts.fill(0U);
    for (auto &visual : m_phaseEvents) {
        visual.passesThreshold = std::abs(visual.event.amplitude) >= m_eventThreshold;
        addToPhaseBucket(visual);
    }
}

void PhaseEllipseWidget::expirePhaseEvents()
{
    const qint64 now = m_clock.nsecsElapsed();
    bool changed = false;
    while (!m_phaseEvents.empty() && now >= m_phaseEvents.front().expiresAtNs) {
        removeFromPhaseBucket(m_phaseEvents.front());
        m_phaseEvents.pop_front();
        changed = true;
    }
    if (changed) update();
    scheduleNextPhaseUpdate();
}

void PhaseEllipseWidget::scheduleNextPhaseUpdate()
{
    if (m_phaseEvents.empty()) {
        m_expiryTimer->stop();
        return;
    }
    const qint64 remainingNs = qMax<qint64>(1,
        m_phaseEvents.front().expiresAtNs - m_clock.nsecsElapsed());
    const qint64 waitMs = qMax<qint64>(1, (remainingNs + 999999) / 1000000);
    m_expiryTimer->start(static_cast<int>(qMin<qint64>(waitMs, INT_MAX)));
}

void PhaseEllipseWidget::setFrame(const QVector<qint16> &samples,
                                  const QVector<PdPulse> &pulses,
                                  bool phaseSynchronized)
{
    m_samples = samples;
    m_envelopeMinimum.clear();
    m_envelopeMaximum.clear();
    m_fullCycleEnvelope = false;
    m_phaseSynchronized = phaseSynchronized;
    if (phaseSynchronized) {
        expirePhaseEvents();
        const qint64 now = m_clock.nsecsElapsed();
        for (const auto &pulse : pulses) {
            if (!pulse.phaseValid) continue;
            const PhaseVisual visual = makePhaseVisual(pulse, now);
            m_phaseEvents.push_back(visual);
            addToPhaseBucket(visual);
        }
        scheduleNextPhaseUpdate();
    }
    update();
}

void PhaseEllipseWidget::setEnvelopeFrame(const QVector<qint16> &minimum,
                                         const QVector<qint16> &maximum,
                                         bool phaseSynchronized)
{
    if (minimum.size() != maximum.size() || minimum.isEmpty()) return;
    m_samples.clear();
    m_envelopeMinimum = minimum;
    m_envelopeMaximum = maximum;
    m_fullCycleEnvelope = true;
    m_phaseSynchronized = phaseSynchronized;
    if (phaseSynchronized) expirePhaseEvents();
    update();
}

void PhaseEllipseWidget::setChannelColor(const QColor &color)
{
    m_channelColor = color;
    update();
}

WidgetPaintMetrics PhaseEllipseWidget::takePaintMetrics()
{
    const WidgetPaintMetrics metrics = m_paintMetrics;
    m_paintMetrics = {};
    return metrics;
}

void PhaseEllipseWidget::appendPhaseEvents(const QVector<PdPulse> &events)
{
    expirePhaseEvents();
    const qint64 now = m_clock.nsecsElapsed();
    for (const auto &event : events) {
        if (!event.phaseValid) continue;
        const PhaseVisual visual = makePhaseVisual(event, now);
        m_phaseEvents.push_back(visual);
        addToPhaseBucket(visual);
    }
    scheduleNextPhaseUpdate();
    update();
}

void PhaseEllipseWidget::clearPhaseEvents()
{
    m_phaseEvents.clear();
    m_phaseBucketCounts.fill(0U);
    m_expiryTimer->stop();
    update();
}

void PhaseEllipseWidget::setEventThreshold(double rawQ88)
{
    const double next = qMax(0.0, rawQ88);
    if (qFuzzyCompare(m_eventThreshold + 1.0, next + 1.0)) return;
    m_eventThreshold = next;
    rebuildPhaseBuckets();
    update();
}

void PhaseEllipseWidget::setWaveformGain(double gain)
{
    const double next = qBound(1.0, gain, 12.0);
    if (qFuzzyCompare(m_waveformGain, next)) return;
    m_waveformGain = next;
    update();
}

void PhaseEllipseWidget::resetZoom()
{
    setZoomFactor(1.0);
}

void PhaseEllipseWidget::setZoomFactor(double factor)
{
    m_zoom = qBound(0.55, factor, 3.8);
    update();
}

void PhaseEllipseWidget::zoomIn()
{
    setZoomFactor(m_zoom * 1.18);
}

void PhaseEllipseWidget::zoomOut()
{
    setZoomFactor(m_zoom / 1.18);
}

void PhaseEllipseWidget::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() > 0) zoomIn();
    else zoomOut();
    event->accept();
}

void PhaseEllipseWidget::paintEvent(QPaintEvent *)
{
    QElapsedTimer paintTimer;
    paintTimer.start();
    const int retainedEvents = static_cast<int>(m_phaseEvents.size());
    const auto recordPaintMetrics = [this, &paintTimer, retainedEvents](
        quint64 sampleVisits, quint64 bucketVisits) {
        const qint64 elapsedNs = paintTimer.nsecsElapsed();
        ++m_paintMetrics.paintCount;
        m_paintMetrics.sampleVisits += sampleVisits;
        m_paintMetrics.bucketVisits += bucketVisits;
        m_paintMetrics.totalPaintNs += elapsedNs;
        m_paintMetrics.maxPaintNs = qMax(m_paintMetrics.maxPaintNs, elapsedNs);
        m_paintMetrics.maxRetainedEvents =
            qMax(m_paintMetrics.maxRetainedEvents, retainedEvents);
    };
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(9, 14, 23));
    const QRectF area = rect().adjusted(26, 12, -16, -24);
    p.setClipRect(area);
    p.setPen(QPen(QColor(104, 152, 172), 1));
    p.drawLine(QPointF(area.left(), area.center().y()),
               QPointF(area.right(), area.center().y()));
    p.drawLine(QPointF(area.center().x(), area.top()),
               QPointF(area.center().x(), area.bottom()));

    const qreal ellipseHeight = qMin(area.height() * 0.84, area.width() / 3.4);
    const qreal ellipseWidth = ellipseHeight * 3.4;
    const QRectF ellipse(area.center().x() - ellipseWidth * m_zoom * 0.5,
                         area.center().y() - ellipseHeight * m_zoom * 0.5,
                         ellipseWidth * m_zoom, ellipseHeight * m_zoom);
    p.setPen(QPen(QColor(60, 160, 190), 1, Qt::DashLine));
    p.drawEllipse(ellipse);

    const auto phasePoint = [&](double phaseDeg) {
        const double angle = phaseDeg * kPi / 180.0;
        return QPointF(ellipse.center().x() - std::cos(angle) * ellipse.width() * 0.5,
                       ellipse.center().y() - std::sin(angle) * ellipse.height() * 0.5);
    };
    if (m_samples.isEmpty() && m_envelopeMinimum.isEmpty() && m_phaseEvents.empty()) {
        p.setPen(QColor(185, 215, 230));
        p.drawText(area, Qt::AlignCenter, QStringLiteral("等待相位关联的实时波形帧"));
        recordPaintMetrics(0U, 0U);
        return;
    }
    if (m_phaseSynchronized && m_fullCycleEnvelope) {
        p.setPen(QPen(QColor(m_channelColor.red(), m_channelColor.green(),
                              m_channelColor.blue(), 125), 1.0));
        for (int i = 0; i < m_envelopeMinimum.size(); ++i) {
            const double phase = 360.0 * i / m_envelopeMinimum.size();
            const double angle = phase * kPi / 180.0;
            const double x = ellipse.center().x() - std::cos(angle) *
                             ellipse.width() * 0.5;
            const double ellipseY = ellipse.center().y() - std::sin(angle) *
                                    ellipse.height() * 0.5;
            const double verticalScale =
                ellipse.height() * 0.75 * m_waveformGain / 2048.0;
            const double lowY = ellipseY -
                static_cast<double>(m_envelopeMaximum[i]) * verticalScale;
            const double highY = ellipseY -
                static_cast<double>(m_envelopeMinimum[i]) * verticalScale;
            const QPointF low(x, lowY);
            const QPointF high(x, highY);
            p.drawLine(low, high);
            p.drawPoint(low);
            p.drawPoint(high);
        }
    } else if (m_phaseSynchronized) {
        p.setPen(QPen(m_channelColor.lighter(125), 1.0));
        for (int i = 0; i < m_samples.size(); ++i) {
            const double phase = 2.0 * kPi * i / qMax(1, m_samples.size() - 1);
            const double x = ellipse.center().x() - std::cos(phase) *
                             ellipse.width() * 0.5;
            const double ellipseY = ellipse.center().y() - std::sin(phase) *
                                    ellipse.height() * 0.5;
            const double y = ellipseY - static_cast<double>(m_samples[i]) *
                             (ellipse.height() * 0.75 * m_waveformGain / 2048.0);
            const QPointF point(x, y);
            p.drawPoint(point);
        }
    }

    quint64 visibleEvents = 0;
    for (int bucket = 0; bucket < m_phaseBucketCounts.size(); ++bucket) {
        const quint64 count = m_phaseBucketCounts[bucket];
        if (count == 0U) continue;
        visibleEvents += count;
        const double phase = (static_cast<double>(bucket) + 0.5) / 10.0;
        const QPointF point = phasePoint(phase);
        const double weight = std::log2(static_cast<double>(count) + 1.0);
        const double tickLength = 5.0 + qMin(28.0, weight * 5.0);
        const double direction = std::sin(phase * kPi / 180.0) >= 0.0 ? -1.0 : 1.0;
        QColor color = m_channelColor;
        color.setAlpha(qMin(255, 155 + static_cast<int>(20.0 * weight)));
        p.setPen(QPen(color, qMin(5.0, 1.2 + weight * 0.65), Qt::SolidLine,
                      Qt::RoundCap));
        p.drawLine(point, point + QPointF(0.0, direction * tickLength));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(point, qMin(4.5, 1.4 + weight * 0.55),
                      qMin(4.5, 1.4 + weight * 0.55));
    }
    if (visibleEvents == 0 && !m_phaseSynchronized) {
        p.setPen(QColor(255, 205, 110));
        p.drawText(area, Qt::AlignCenter,
                   QStringLiteral("原始波形已到达；等待超过上位机阈值的 PL 事件"));
    }
    p.setClipping(false);
    p.setPen(QColor(180, 210, 225));
    p.drawText(area.left(), height() - 6, QStringLiteral("0°/360°"));
    p.drawText(ellipse.center().x() - 12, height() - 6,
               QStringLiteral("90°↑  270°↓"));
    p.drawText(area.right() - 115, height() - 6,
               QStringLiteral("180°  %1x").arg(m_zoom, 0, 'f', 2));
    const quint64 sampleVisits = !m_phaseSynchronized ? 0U
        : static_cast<quint64>(m_fullCycleEnvelope
              ? m_envelopeMinimum.size() : m_samples.size());
    recordPaintMetrics(sampleVisits,
                       static_cast<quint64>(m_phaseBucketCounts.size()));
}

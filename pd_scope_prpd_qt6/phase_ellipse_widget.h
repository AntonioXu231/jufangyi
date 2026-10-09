#pragma once

#include "scope_types.h"

#include <QColor>
#include <QElapsedTimer>
#include <QWidget>

#include <deque>

class QWheelEvent;
class QTimer;

class PhaseEllipseWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit PhaseEllipseWidget(QWidget *parent = nullptr);
    void setFrame(const QVector<qint16> &samples, const QVector<PdPulse> &pulses,
                  bool phaseSynchronized);
    void setEnvelopeFrame(const QVector<qint16> &minimum,
                          const QVector<qint16> &maximum,
                          bool phaseSynchronized);
    void setChannelColor(const QColor &color);
    void appendPhaseEvents(const QVector<PdPulse> &events);
    void clearPhaseEvents();
    /* 上位机动态筛选阈值；不写入 PL。历史事件保留到 15 s，到期后删除。 */
    void setEventThreshold(double rawQ88);
    void setWaveformGain(double gain);
    void resetZoom();
    void setZoomFactor(double factor);
    void zoomIn();
    void zoomOut();
    WidgetPaintMetrics takePaintMetrics();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void expirePhaseEvents();
    void scheduleNextPhaseUpdate();

    struct PhaseVisual {
        PdPulse event;
        qint64 visibleFromNs = 0;
        qint64 expiresAtNs = 0;
        int phaseBucket = 0;
        bool passesThreshold = false;
    };
    PhaseVisual makePhaseVisual(const PdPulse &event, qint64 receivedAtNs) const;
    void rebuildPhaseBuckets();
    void addToPhaseBucket(const PhaseVisual &visual);
    void removeFromPhaseBucket(const PhaseVisual &visual);

    QVector<qint16> m_samples;
    QVector<qint16> m_envelopeMinimum;
    QVector<qint16> m_envelopeMaximum;
    std::deque<PhaseVisual> m_phaseEvents;
    QVector<quint64> m_phaseBucketCounts;
    QElapsedTimer m_clock;
    QTimer *m_expiryTimer = nullptr;
    QColor m_channelColor = QColor(105, 245, 215);
    bool m_phaseSynchronized = false;
    bool m_fullCycleEnvelope = false;
    double m_zoom = 1.0;
    double m_waveformGain = 1.0;
    WidgetPaintMetrics m_paintMetrics;
    /* 0 保持原有行为：所有已同步的 PL 事件都显示。 */
    double m_eventThreshold = 0.0;
};

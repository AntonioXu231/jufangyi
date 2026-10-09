#pragma once

#include "scope_types.h"

#include <QColor>
#include <QWidget>

class QWheelEvent;

class WaveformWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit WaveformWidget(QWidget *parent = nullptr);
    void setFrame(const QVector<qint16> &samples, const QVector<PdPulse> &pulses);
    void setEnvelope(const QVector<qint16> &minimum, const QVector<qint16> &maximum,
                     quint32 sourceSampleCount);
    void appendPhaseEvents(const QVector<PdPulse> &events);
    void setEventThreshold(double rawQ88);
    void setSingleFrameMode(bool enabled);
    void setTraceColor(const QColor &color);
    void resetZoom();
    WidgetPaintMetrics takePaintMetrics();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    bool expirePulses(qint64 nowMs);

    struct PulseVisual {
        PdPulse pulse;
        qint64 createdMs = 0;
        bool triggerCaptured = false;
    };

    struct TriggerWindow {
        bool active = false;
        int startIndex = 0;
        int endIndex = 0;
        qint64 createdMs = 0;
    };

    QVector<qint16> m_samples;
    QVector<qint16> m_envelopeMinimum;
    QVector<qint16> m_envelopeMaximum;
    QVector<PulseVisual> m_pulses;
    QColor m_traceColor = QColor(100, 225, 255);
    TriggerWindow m_trigger;
    int m_visibleSamples = 16384;
    bool m_followLatestWindow = true;
    bool m_singleFrameMode = false;
    bool m_fullCycleEnvelope = false;
    quint32 m_sourceSampleCount = 0;
    double m_eventThreshold = 0.0;
    WidgetPaintMetrics m_paintMetrics;
};

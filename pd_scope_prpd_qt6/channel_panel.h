#pragma once

#include "scope_types.h"
#include "waveform_widget.h"

#include <QWidget>

class QLabel;
class PhaseEllipseWidget;

class ChannelPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit ChannelPanel(int channel, QWidget *parent = nullptr);
    void present(const QVector<qint16> &samples, const QVector<PdPulse> &pulses,
                 bool phaseSynchronized);
    void presentEnvelope(const QVector<qint16> &minimum,
                         const QVector<qint16> &maximum,
                         quint32 sourceSampleCount,
                         bool phaseSynchronized);
    void appendPhaseEvents(const QVector<PdPulse> &events);
    void clearPhaseEvents();
    void setPhaseEventThreshold(double rawAdc);
    void resetZoom();
    WidgetPaintMetrics takeWaveformPaintMetrics();
    WidgetPaintMetrics takePhasePaintMetrics();

private:
    int m_channel;
    QLabel *m_peakLabel = nullptr;
    WaveformWidget *m_waveform = nullptr;
    PhaseEllipseWidget *m_ellipse = nullptr;
};

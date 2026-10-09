#pragma once

#include "scope_types.h"

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QStackedWidget;
class PhaseEllipseWidget;

class SinglePhasePage final : public QWidget
{
    Q_OBJECT
public:
    explicit SinglePhasePage(QWidget *parent = nullptr);

    void setEnvelopeFrame(const ScopeFrame &frame);
    void presentFrame(int channel, const QVector<qint16> &samples,
                      const QVector<PdPulse> &pulses, bool phaseSynchronized);
    void appendPhaseEvents(int channel, const QVector<PdPulse> &events);
    void clearPhaseEvents();
    void setEventThreshold(double rawQ88);
    void setWaveformGain(double gain);
    void resetZoom();
    WidgetPaintMetrics takePaintMetrics();

private:
    PhaseEllipseWidget *selectedGraph() const;

    QComboBox *m_channelSelect = nullptr;
    QDoubleSpinBox *m_waveformGain = nullptr;
    QStackedWidget *m_graphStack = nullptr;
    QVector<PhaseEllipseWidget *> m_graphs;
};

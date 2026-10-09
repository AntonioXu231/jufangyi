#include "channel_panel.h"
#include "phase_ellipse_widget.h"
#include "waveform_widget.h"

#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>

namespace {
const QColor kHeader[] = { QColor(255, 216, 0), QColor(105, 245, 215), QColor(255, 93, 82), QColor(230, 80, 255) };
}

ChannelPanel::ChannelPanel(int channel, QWidget *parent) : QWidget(parent), m_channel(channel)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    auto *header = new QWidget(this);
    header->setStyleSheet(QStringLiteral("background:#048fd5;"));
    auto *headerLayout = new QHBoxLayout(header);
    auto *number = new QLabel(QString::number(channel + 1), header);
    number->setStyleSheet(QStringLiteral("font-weight:bold;font-size:18px;color:%1;").arg(kHeader[channel].name()));
    m_peakLabel = new QLabel(QStringLiteral("0 raw ADC"), header);
    m_peakLabel->setAlignment(Qt::AlignCenter);
    auto *reset = new QPushButton(QStringLiteral("复位缩放"), header);
    reset->setMaximumWidth(82);
    connect(reset, &QPushButton::clicked, this, &ChannelPanel::resetZoom);
    headerLayout->addWidget(number);
    headerLayout->addWidget(m_peakLabel, 1);
    headerLayout->addWidget(new QLabel(QStringLiteral("实时波形 / 相位图"), header));
    headerLayout->addWidget(reset);
    m_waveform = new WaveformWidget(this);
    m_ellipse = new PhaseEllipseWidget(this);
    m_waveform->setTraceColor(kHeader[channel]);
    m_ellipse->setChannelColor(kHeader[channel]);
    layout->addWidget(header);
    layout->addWidget(m_waveform, 3);
    layout->addWidget(m_ellipse, 4);
}

void ChannelPanel::present(const QVector<qint16> &samples, const QVector<PdPulse> &pulses,
                           bool phaseSynchronized)
{
    double peak = 0.0;
    for (const qint16 sample : samples) peak = qMax(peak, std::abs(static_cast<double>(sample)));
    m_peakLabel->setText(QStringLiteral("%1 raw ADC").arg(peak, 0, 'f', 0));
    m_waveform->setFrame(samples, pulses);
    m_ellipse->setFrame(samples, pulses, phaseSynchronized);
}

void ChannelPanel::presentEnvelope(const QVector<qint16> &minimum,
                                   const QVector<qint16> &maximum,
                                   quint32 sourceSampleCount,
                                   bool phaseSynchronized)
{
    if (minimum.size() != maximum.size() || minimum.isEmpty()) return;
    qint16 peak = 0;
    for (int i = 0; i < minimum.size(); ++i) {
        peak = qMax<qint16>(peak, qAbs(minimum[i]));
        peak = qMax<qint16>(peak, qAbs(maximum[i]));
    }
    m_peakLabel->setText(QStringLiteral("峰值 %1 raw ADC · %2 点/周期")
                             .arg(peak).arg(sourceSampleCount));
    m_waveform->setEnvelope(minimum, maximum, sourceSampleCount);
    m_ellipse->setEnvelopeFrame(minimum, maximum, phaseSynchronized);
}

void ChannelPanel::resetZoom()
{
    m_waveform->resetZoom();
    m_ellipse->resetZoom();
}

WidgetPaintMetrics ChannelPanel::takeWaveformPaintMetrics()
{
    return m_waveform->takePaintMetrics();
}

WidgetPaintMetrics ChannelPanel::takePhasePaintMetrics()
{
    return m_ellipse->takePaintMetrics();
}

void ChannelPanel::appendPhaseEvents(const QVector<PdPulse> &events)
{
    m_waveform->appendPhaseEvents(events);
    m_ellipse->appendPhaseEvents(events);
}

void ChannelPanel::clearPhaseEvents()
{
    m_ellipse->clearPhaseEvents();
}

void ChannelPanel::setPhaseEventThreshold(double rawAdc)
{
    m_waveform->setEventThreshold(rawAdc);
    m_ellipse->setEventThreshold(rawAdc);
}

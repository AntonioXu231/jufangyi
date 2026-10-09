#include "single_phase_page.h"

#include "phase_ellipse_widget.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
constexpr double kInitialZoom = 0.74;
const QColor kChannelColors[] = {
    QColor(255, 216, 0), QColor(105, 245, 215),
    QColor(255, 93, 82), QColor(230, 80, 255)
};
}

SinglePhasePage::SinglePhasePage(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(6);

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(new QLabel(QStringLiteral("查看通道："), this));
    m_channelSelect = new QComboBox(this);
    for (int channel = 0; channel < 4; ++channel)
        m_channelSelect->addItem(QStringLiteral("通道 %1").arg(channel + 1), channel);
    toolbar->addWidget(m_channelSelect);
    toolbar->addWidget(new QLabel(QStringLiteral("波形偏差增益："), this));
    m_waveformGain = new QDoubleSpinBox(this);
    m_waveformGain->setRange(1.0, 12.0);
    m_waveformGain->setDecimals(1);
    m_waveformGain->setSingleStep(0.5);
    m_waveformGain->setValue(4.0);
    m_waveformGain->setSuffix(QStringLiteral("×"));
    m_waveformGain->setToolTip(QStringLiteral(
        "只放大波形相对椭圆的显示偏差，不改ADC样点/事件幅值；1×为真实比例。"));
    toolbar->addWidget(m_waveformGain);
    toolbar->addWidget(new QLabel(
        QStringLiteral("滚轮或按钮缩放 · 每通道独立保留15秒事件 · 默认留有坐标边距"), this));
    auto *zoomOut = new QPushButton(QStringLiteral("缩小"), this);
    auto *zoomIn = new QPushButton(QStringLiteral("放大"), this);
    auto *reset = new QPushButton(QStringLiteral("复位缩放"), this);
    toolbar->addWidget(zoomOut);
    toolbar->addWidget(zoomIn);
    toolbar->addWidget(reset);
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    m_graphStack = new QStackedWidget(this);
    for (int channel = 0; channel < 4; ++channel) {
        auto *graph = new PhaseEllipseWidget(m_graphStack);
        graph->setChannelColor(kChannelColors[channel]);
        graph->setZoomFactor(kInitialZoom);
        graph->setWaveformGain(m_waveformGain->value());
        m_graphs.append(graph);
        m_graphStack->addWidget(graph);
    }
    layout->addWidget(m_graphStack, 1);

    connect(m_channelSelect, qOverload<int>(&QComboBox::currentIndexChanged),
            m_graphStack, &QStackedWidget::setCurrentIndex);
    connect(m_waveformGain, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &SinglePhasePage::setWaveformGain);
    connect(zoomIn, &QPushButton::clicked, this, [this] {
        if (auto *graph = selectedGraph()) graph->zoomIn();
    });
    connect(zoomOut, &QPushButton::clicked, this, [this] {
        if (auto *graph = selectedGraph()) graph->zoomOut();
    });
    connect(reset, &QPushButton::clicked, this, &SinglePhasePage::resetZoom);
}

void SinglePhasePage::setEnvelopeFrame(const ScopeFrame &frame)
{
    if (!frame.fullCycleEnvelope || frame.minimum.size() != 4 ||
        frame.maximum.size() != 4) return;
    for (int channel = 0; channel < 4; ++channel) {
        const bool phaseLocked = (frame.phaseLockMask & (1U << channel)) != 0U;
        m_graphs[channel]->setEnvelopeFrame(frame.minimum[channel],
                                             frame.maximum[channel], phaseLocked);
    }
}

void SinglePhasePage::presentFrame(int channel, const QVector<qint16> &samples,
                                   const QVector<PdPulse> &pulses,
                                   bool phaseSynchronized)
{
    if (channel < 0 || channel >= m_graphs.size()) return;
    m_graphs[channel]->setFrame(samples, pulses, phaseSynchronized);
}

void SinglePhasePage::appendPhaseEvents(int channel, const QVector<PdPulse> &events)
{
    if (channel < 0 || channel >= m_graphs.size() || events.isEmpty()) return;
    m_graphs[channel]->appendPhaseEvents(events);
}

void SinglePhasePage::clearPhaseEvents()
{
    for (auto *graph : m_graphs) graph->clearPhaseEvents();
}

void SinglePhasePage::setEventThreshold(double rawQ88)
{
    for (auto *graph : m_graphs) graph->setEventThreshold(rawQ88);
}

void SinglePhasePage::setWaveformGain(double gain)
{
    for (auto *graph : m_graphs) graph->setWaveformGain(gain);
}

void SinglePhasePage::resetZoom()
{
    if (auto *graph = selectedGraph()) graph->setZoomFactor(kInitialZoom);
}

WidgetPaintMetrics SinglePhasePage::takePaintMetrics()
{
    WidgetPaintMetrics aggregate;
    for (auto *graph : m_graphs) {
        const WidgetPaintMetrics metrics = graph->takePaintMetrics();
        aggregate.paintCount += metrics.paintCount;
        aggregate.sampleVisits += metrics.sampleVisits;
        aggregate.eventVisits += metrics.eventVisits;
        aggregate.bucketVisits += metrics.bucketVisits;
        aggregate.totalPaintNs += metrics.totalPaintNs;
        aggregate.maxPaintNs = qMax(aggregate.maxPaintNs, metrics.maxPaintNs);
        aggregate.maxRetainedEvents =
            qMax(aggregate.maxRetainedEvents, metrics.maxRetainedEvents);
    }
    return aggregate;
}

PhaseEllipseWidget *SinglePhasePage::selectedGraph() const
{
    const int channel = m_channelSelect ? m_channelSelect->currentIndex() : 0;
    return channel >= 0 && channel < m_graphs.size() ? m_graphs[channel] : nullptr;
}

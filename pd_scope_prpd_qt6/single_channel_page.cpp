#include "single_channel_page.h"

#include "fft_widget.h"
#include "waveform_widget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
const QColor kChannelColors[] = {
    QColor(255, 216, 0), QColor(105, 245, 215),
    QColor(255, 93, 82), QColor(230, 80, 255)
};
}

SingleChannelPage::SingleChannelPage(QWidget *parent) : QWidget(parent)
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
    m_info = new QLabel(QStringLiteral("等待实时采样帧"), this);
    toolbar->addWidget(m_info, 1);
    layout->addLayout(toolbar);

    auto *archiveToolbar = new QHBoxLayout;
    archiveToolbar->addWidget(new QLabel(QStringLiteral("SNAP序号："), this));
    m_snapshotSequence = new QSpinBox(this);
    m_snapshotSequence->setRange(0, 2147483647);
    m_snapshotSequence->setToolTip(QStringLiteral("从 CATALOG 返回的保留序号区间中选择完整快照。"));
    archiveToolbar->addWidget(m_snapshotSequence);
    auto *latestSnapshot = new QPushButton(QStringLiteral("读取最新 SNAP"), this);
    archiveToolbar->addWidget(latestSnapshot);
    archiveToolbar->addWidget(new QLabel(QStringLiteral("起点："), this));
    m_snapshotStart = new QSpinBox(this);
    m_snapshotStart->setRange(0, static_cast<int>(kScopeMaxFftStart));
    m_snapshotStart->setSingleStep(1024);
    m_snapshotStart->setSuffix(QStringLiteral(" 点"));
    archiveToolbar->addWidget(m_snapshotStart);
    auto *archiveFft = new QPushButton(QStringLiteral("PS FFT 归档快照"), this);
    archiveToolbar->addWidget(archiveFft);
    auto *fullSnapshot = new QPushButton(QStringLiteral("读取完整 SNAP + PS FFT"), this);
    fullSnapshot->setToolTip(QStringLiteral(
        "暂停采集，读取所选 SNAP 的四通道全部原始点，并由 PS 计算所选 1024 点窗 FFT。"));
    archiveToolbar->addWidget(fullSnapshot);
    auto *liveView = new QPushButton(QStringLiteral("返回实时"), this);
    archiveToolbar->addWidget(liveView);
    archiveToolbar->addStretch(1);
    layout->addLayout(archiveToolbar);
    connect(latestSnapshot, &QPushButton::clicked,
            this, &SingleChannelPage::snapshotCatalogRequested);
    connect(archiveFft, &QPushButton::clicked, this, [this] {
        emit snapshotFftRequested(static_cast<quint32>(m_snapshotSequence->value()),
                                  m_channelSelect->currentData().toInt(),
                                  static_cast<quint32>(m_snapshotStart->value()));
        m_info->setText(QStringLiteral("正在暂停采集，读取所选 SNAP 的同一 1024 点波形与 PS FFT…"));
    });
    connect(fullSnapshot, &QPushButton::clicked, this, [this] {
        emit fullSnapshotRequested(static_cast<quint32>(m_snapshotSequence->value()),
                                   m_channelSelect->currentData().toInt(),
                                   static_cast<quint32>(m_snapshotStart->value()));
        m_info->setText(QStringLiteral(
            "正在暂停采集并分块读取完整 1,300,000 点快照；完成后显示全波形和所选 PS FFT…"));
    });
    connect(liveView, &QPushButton::clicked, this, &SingleChannelPage::returnToLiveView);

    m_waveTitle = new QLabel(QStringLiteral("当前完整采样帧 · 时域波形"), this);
    m_waveTitle->setStyleSheet(QStringLiteral("font-weight:bold;color:#bfefff;padding:3px;"));
    layout->addWidget(m_waveTitle);
    m_waveform = new WaveformWidget(this);
    m_waveform->setSingleFrameMode(true);
    layout->addWidget(m_waveform, 3);

    m_fftTitle = new QLabel(QStringLiteral("PS 端同帧 Hann 窗 FFT（去直流；Qt 仅绘图）"), this);
    m_fftTitle->setStyleSheet(QStringLiteral("font-weight:bold;color:#bfefff;padding:3px;"));
    layout->addWidget(m_fftTitle);
    m_fft = new FftWidget(this);
    layout->addWidget(m_fft, 2);

    connect(m_channelSelect, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &SingleChannelPage::refreshSelectedChannel);
}

void SingleChannelPage::setFrame(const ScopeFrame &frame,
                                 const QVector<QVector<PdPulse>> &pulses)
{
    m_frame = frame;
    m_pulses = pulses;
    m_hasFrame = frame.samples.size() == 4;
    m_hasSpectrum = false;
    if (m_archiveView) return;
    refreshSelectedChannel();
}

void SingleChannelPage::setSpectrum(const ScopeSpectrum &spectrum)
{
    const int channel = m_channelSelect->currentData().toInt();
    if (!m_hasFrame || spectrum.sequence != m_frame.sequence || spectrum.channel != channel)
        return;
    m_spectrum = spectrum;
    m_hasSpectrum = true;
    if (!m_archiveView) m_fft->setSpectrum(m_spectrum, kChannelColors[channel]);
}

void SingleChannelPage::setArchiveSpectrum(const ScopeArchiveSpectrum &spectrum)
{
    const int channel = m_channelSelect->currentData().toInt();
    if (spectrum.channel < 0 || spectrum.channel > 3 || spectrum.samples.size() != 1024 ||
        spectrum.magnitudes.size() != 513) {
        m_info->setText(QStringLiteral("归档 FFT 返回数据与所选通道/点数不一致。"));
        return;
    }
    m_archiveSpectrum = spectrum;
    m_archiveView = true;
    const bool hasCompleteSnapshot = spectrum.fullChannels.size() == 4 &&
        spectrum.fullChannels[channel].size() == static_cast<int>(spectrum.snapshotSamples);
    if (!hasCompleteSnapshot && spectrum.channel != channel) {
        m_info->setText(QStringLiteral("PS FFT返回通道与当前选择不一致；请重新读取所选通道。"));
        m_archiveView = false;
        return;
    }
    m_waveform->setFrame(hasCompleteSnapshot ? spectrum.fullChannels[channel]
                                               : spectrum.samples, {});
    if (hasCompleteSnapshot) m_waveform->resetZoom();
    ScopeSpectrum displaySpectrum;
    displaySpectrum.sequence = spectrum.snapshotSequence;
    displaySpectrum.channel = spectrum.channel;
    displaySpectrum.fromArchive = true;
    displaySpectrum.startSample = spectrum.startSample;
    displaySpectrum.sampleRateHz = spectrum.sampleRateHz;
    displaySpectrum.binHz = spectrum.binHz;
    displaySpectrum.peakBin = spectrum.peakBin;
    displaySpectrum.peakHz = spectrum.peakHz;
    displaySpectrum.amplitudeCode = spectrum.amplitudeCode;
    displaySpectrum.dcCode = spectrum.dcCode;
    displaySpectrum.magnitudes = spectrum.magnitudes;
    if (spectrum.channel == channel)
        m_fft->setSpectrum(displaySpectrum, kChannelColors[channel]);
    else
        m_fft->setWaiting(kChannelColors[channel]);
    if (hasCompleteSnapshot) {
        m_waveTitle->setText(QStringLiteral(
            "完整归档 SNAP #%1 · 通道 %2 · 全部 %3 点（滚轮按像素缩放）")
            .arg(spectrum.snapshotSequence).arg(channel + 1)
            .arg(spectrum.snapshotSamples));
        m_info->setText(QStringLiteral(
            "SNAP #%1 · 通道 %2 · 已加载完整 %3 点 · FFT窗起点 %4 · FFT通道 %5 · Fs %6 MSPS · 主峰 %7 Hz")
            .arg(spectrum.snapshotSequence).arg(channel + 1)
            .arg(spectrum.snapshotSamples).arg(spectrum.startSample)
            .arg(spectrum.channel + 1).arg(spectrum.sampleRateHz / 1.0e6, 0, 'f', 3)
            .arg(spectrum.peakHz));
    } else {
        m_waveTitle->setText(QStringLiteral(
            "归档 SNAP #%1 · 通道 %2 · 起点 %3 · 同一 1024 点时域窗口")
            .arg(spectrum.snapshotSequence).arg(channel + 1).arg(spectrum.startSample));
        m_info->setText(QStringLiteral(
            "SNAP #%1 · 通道 %2 · 起点 %3 / %4 · Fs %5 MSPS · Δf %6 Hz · 主峰 %7 Hz")
            .arg(spectrum.snapshotSequence).arg(channel + 1).arg(spectrum.startSample)
            .arg(spectrum.snapshotSamples).arg(spectrum.sampleRateHz / 1.0e6, 0, 'f', 3)
            .arg(spectrum.binHz).arg(spectrum.peakHz));
    }
    m_fftTitle->setText(spectrum.channel == channel
        ? QStringLiteral("PS 端归档 Hann 窗 FFT（同一 SNAP/通道/起点；Qt 仅绘图）")
        : QStringLiteral("FFT来自通道 %1；当前波形是通道 %2，选择通道后重新请求 PS FFT。")
              .arg(spectrum.channel + 1).arg(channel + 1));
}

void SingleChannelPage::setSnapshotCatalog(quint64 first, quint64 next, quint32 state)
{
    if (next <= first) {
        m_info->setText(QStringLiteral("PS 当前没有可选完整 SNAP。"));
        return;
    }
    const int maxSequence = static_cast<int>(qMin<quint64>(next - 1U, 2147483647U));
    const int minSequence = static_cast<int>(qMin<quint64>(first, 2147483647U));
    m_snapshotSequence->setRange(minSequence, maxSequence);
    m_snapshotSequence->setValue(maxSequence);
    m_info->setText(QStringLiteral("SNAP保留区间 [%1,%2)，板端 state=%3；最新序号已选中。")
                        .arg(first).arg(next).arg(state));
}

void SingleChannelPage::returnToLiveView()
{
    m_archiveView = false;
    m_waveTitle->setText(QStringLiteral("当前完整采样帧 · 时域波形"));
    m_fftTitle->setText(QStringLiteral("PS 端同帧 Hann 窗 FFT（去直流；Qt 仅绘图）"));
    refreshSelectedChannel();
}

void SingleChannelPage::resetZoom()
{
    m_waveform->resetZoom();
}

WidgetPaintMetrics SingleChannelPage::takeWaveformPaintMetrics()
{
    return m_waveform->takePaintMetrics();
}

void SingleChannelPage::refreshSelectedChannel()
{
    const int channel = m_channelSelect->currentData().toInt();
    emit channelSelected(channel);
    if (m_archiveView && channel != m_archiveSpectrum.channel) {
        if (m_archiveSpectrum.fullChannels.size() == 4 &&
            m_archiveSpectrum.fullChannels[channel].size() ==
                static_cast<int>(m_archiveSpectrum.snapshotSamples)) {
            m_waveform->setFrame(m_archiveSpectrum.fullChannels[channel], {});
            m_waveform->resetZoom();
            m_fft->setWaiting(kChannelColors[channel]);
            m_waveTitle->setText(QStringLiteral(
                "完整归档 SNAP #%1 · 通道 %2 · 全部 %3 点（滚轮按像素缩放）")
                .arg(m_archiveSpectrum.snapshotSequence).arg(channel + 1)
                .arg(m_archiveSpectrum.snapshotSamples));
            m_fftTitle->setText(QStringLiteral(
                "选择“PS FFT 归档快照”计算当前通道；FFT 由 PS 处理，Qt 仅绘图"));
            m_info->setText(QStringLiteral(
                "完整 SNAP #%1 · 通道 %2 · %3 点已加载；点击 PS FFT 可计算该通道所选窗口。")
                .arg(m_archiveSpectrum.snapshotSequence).arg(channel + 1)
                .arg(m_archiveSpectrum.snapshotSamples));
            return;
        }
        m_archiveView = false;
        m_waveTitle->setText(QStringLiteral("当前完整采样帧 · 时域波形"));
        m_fftTitle->setText(QStringLiteral("PS 端同帧 Hann 窗 FFT（去直流；Qt 仅绘图）"));
    }
    if (m_archiveView) {
        m_info->setText(QStringLiteral("归档 SNAP #%1 固定显示通道 %2；选择其他通道后请重新获取 FFT。")
                            .arg(m_archiveSpectrum.snapshotSequence).arg(channel + 1));
        return;
    }
    if (!m_hasFrame || channel < 0 || channel >= m_frame.samples.size()) {
        m_info->setText(QStringLiteral("等待通道 %1 的实时采样帧").arg(channel + 1));
        return;
    }
    const auto &samples = m_frame.samples[channel];
    const QVector<PdPulse> pulses = channel < m_pulses.size()
                                        ? m_pulses[channel] : QVector<PdPulse>{};
    m_waveform->setFrame(samples, pulses);
    m_fft->setWaiting(kChannelColors[channel]);
    if (m_hasSpectrum && m_spectrum.sequence == m_frame.sequence &&
        m_spectrum.channel == channel)
        m_fft->setSpectrum(m_spectrum, kChannelColors[channel]);
    m_info->setText(QStringLiteral("通道 %1 · 帧 #%2 · %3 点 · Fs %4 MSPS · 时长 %5 μs")
        .arg(channel + 1).arg(m_frame.sequence).arg(samples.size())
        .arg(m_frame.sampleRateHz / 1.0e6, 0, 'f', 3)
        .arg(samples.size() * 1.0e6 / m_frame.sampleRateHz, 0, 'f', 2));
}

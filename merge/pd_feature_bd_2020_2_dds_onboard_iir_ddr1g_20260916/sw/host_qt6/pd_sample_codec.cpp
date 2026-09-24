#include "pd_sample_codec.h"

#include <QtGlobal>

namespace pdsample {

quint16 decodeChannelSample(const uchar *p, int channel)
{
    switch (channel) {
    case 0:
        return static_cast<quint16>(p[0] | ((p[1] & 0x0FU) << 8));
    case 1:
        return static_cast<quint16>((p[1] >> 4) | (p[2] << 4));
    case 2:
        return static_cast<quint16>(p[3] | ((p[4] & 0x0FU) << 8));
    default:
        return static_cast<quint16>((p[4] >> 4) | (p[5] << 4));
    }
}

bool decodeFrame(const QByteArray &raw, double sampleRateHz, qint64 sequence,
                 WaveformFrame &out)
{
    out = WaveformFrame();
    if (raw.isEmpty() || (raw.size() % kBlockBytes) != 0) return false;
    if (!(sampleRateHz > 0.0)) return false;

    const int samples = raw.size() / kBytesPerSample;
    out.sequence = sequence;
    out.sampleCount = samples;
    out.sampleRateHz = sampleRateHz;
    out.sampleIntervalSec = 1.0 / sampleRateHz;
    for (int c = 0; c < kChannelCount; ++c) out.channel[c].resize(samples);

    const auto *bytes = reinterpret_cast<const uchar *>(raw.constData());
    for (int i = 0; i < samples; ++i) {
        const uchar *p = bytes + i * kBytesPerSample;
        for (int c = 0; c < kChannelCount; ++c)
            out.channel[c][i] = static_cast<double>(decodeChannelSample(p, c));
    }
    return true;
}

bool decodePeakEvent(quint64 word, PeakEvent &out, bool &valid)
{
    valid = true;
    const quint64 type = (word >> eventpacket::kTypeShift);
    if (type == eventpacket::kTypeCycle) return false;   /* 周期包，正常跳过 */
    if (type != eventpacket::kTypePeak) { valid = false; return false; }

    const qint16 raw = static_cast<qint16>((word >> eventpacket::kQ88Shift) & 0xFFFFU);
    out.qRaw = raw;
    out.q88 = raw / 256.0; /* Q8.8：低 8 位是小数 */
    /* 折算到 AD 码域，供与波形上的脉冲幅值互相印证（默认标定下等于 raw）。 */
    out.adcCodes = eventpacket::adcCodesFromField(raw);

    const quint64 phase = (word >> eventpacket::kPhaseShift) & eventpacket::kPhaseMask;
    out.phaseWindow = static_cast<uint>(phase);
    out.phaseDeg = phase * 360.0 / static_cast<double>(1ULL << eventpacket::kPhaseBits);

    out.channel = static_cast<int>((word >> eventpacket::kChannelShift) & 0x3U);
    /* [27] polarity：0 = 负，1 = 正（RTL 拼的是 ~q0_pol，而 q0_pol 是 0=正）。 */
    out.positive = ((word >> eventpacket::kPolarityShift) & 0x1U) != 0U;
    /* [24:0] evt_seq：每通道自增，可做丢帧检测。 */
    out.evtSeq = static_cast<quint32>((word & eventpacket::kEvtSeqMask));
    return true;
}

bool measure(const WaveformFrame &frame, ChannelStats &stats)
{
    stats = ChannelStats();
    if (frame.sampleCount <= 0) return false;

    for (int c = 0; c < kChannelCount; ++c) {
        if (frame.channel[c].size() != frame.sampleCount) return false;
        stats.minimum[c] = frame.channel[c].at(0);
        stats.maximum[c] = frame.channel[c].at(0);
    }

    double sum[kChannelCount] = {0.0, 0.0, 0.0, 0.0};
    for (int i = 0; i < frame.sampleCount; ++i) {
        for (int c = 0; c < kChannelCount; ++c) {
            const double value = frame.channel[c].at(i);
            if (value < stats.minimum[c]) stats.minimum[c] = value;
            if (value > stats.maximum[c]) stats.maximum[c] = value;
            sum[c] += value;
        }
    }
    stats.sampleCount = frame.sampleCount;
    for (int c = 0; c < kChannelCount; ++c)
        stats.mean[c] = sum[c] / static_cast<double>(frame.sampleCount);
    return true;
}

int findTriggerIndex(const QVector<double> &samples, double level, double hysteresis,
                     bool risingEdge)
{
    if (samples.size() < 2) return -1;
    const double armedLevel = risingEdge ? level - hysteresis : level + hysteresis;

    /* 第一遍：先找到"离开触发带"的位置，保证不会把抖动当成连续多次触发。 */
    int armed = -1;
    for (int i = 0; i < samples.size(); ++i) {
        const bool left = risingEdge ? samples.at(i) <= armedLevel : samples.at(i) >= armedLevel;
        if (left) {
            armed = i;
            break;
        }
    }
    if (armed < 0) return -1;

    /* 第二遍：从离开触发带之后，找第一次穿过电平的边沿。 */
    for (int i = armed; i + 1 < samples.size(); ++i) {
        const double previous = samples.at(i);
        const double current = samples.at(i + 1);
        if (risingEdge && previous < level && current >= level) return i + 1;
        if (!risingEdge && previous > level && current <= level) return i + 1;
    }
    return -1;
}

} // namespace pdsample

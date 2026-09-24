#include "pd_pulse_detect.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>

namespace pddetect {
namespace {

constexpr int kHistogramBuckets = 4096; /* 12-bit ADC 的全码域 */
/* 1.4826 = 正态分布下 MAD 与 sigma 的换算常数。 */
constexpr double kMadToSigma = 1.4826;

/*
 * 直方图统计。
 * 用它而不是排序求中位数：520,000 点排序是 O(N log N) 且要额外分配，
 * 而直方图是 O(N) + O(4096)，实测快两个数量级，且不需要额外内存峰值。
 */
struct Histogram {
    QVector<int> counts;
    int total = 0;

    Histogram() : counts(kHistogramBuckets, 0) {}

    void add(int bucket)
    {
        if (bucket < 0) bucket = 0;
        if (bucket >= kHistogramBuckets) bucket = kHistogramBuckets - 1;
        counts[bucket] += 1;
        total += 1;
    }

    int quantile(double fraction) const
    {
        if (total <= 0) return 0;
        int target = static_cast<int>(fraction * static_cast<double>(total));
        if (target < 0) target = 0;
        int running = 0;
        for (int i = 0; i < counts.size(); ++i) {
            running += counts.at(i);
            if (running > target) return i;
        }
        return counts.size() - 1;
    }
};

int toBucket(double value)
{
    if (!std::isfinite(value)) return 0;
    const double rounded = std::floor(value + 0.5);
    if (rounded <= 0.0) return 0;
    if (rounded >= kHistogramBuckets) return kHistogramBuckets - 1;
    return static_cast<int>(rounded);
}

double resolveThreshold(const ChannelResult &estimate, const Settings &settings)
{
    double threshold = 0.0;
    if (settings.adaptiveMultiplier > 0.0)
        threshold = settings.adaptiveMultiplier * estimate.sigma;
    if (settings.absoluteThresholdCodes > 0.0)
        threshold = std::max(threshold, settings.absoluteThresholdCodes);
    return threshold;
}

} // namespace

QVector<Pulse> flatten(const Result &result)
{
    int total = 0;
    for (int c = 0; c < pdsample::kChannelCount; ++c) total += result.pulses[c].size();
    QVector<Pulse> flat;
    flat.reserve(total);
    for (int c = 0; c < pdsample::kChannelCount; ++c)
        for (const Pulse &pulse : result.pulses[c]) flat.append(pulse);
    /* 按时间排序，波形控件按顺序绘制时视觉更自然。 */
    std::sort(flat.begin(), flat.end(),
              [](const Pulse &a, const Pulse &b) { return a.refinedIndex < b.refinedIndex; });
    return flat;
}

ChannelResult estimateChannel(const QVector<double> &data, const Settings &settings)
{
    ChannelResult result;
    if (data.isEmpty()) return result;

    const double *samples = data.constData();
    const int count = data.size();

    Histogram level;
    level.counts.reserve(kHistogramBuckets);
    for (int i = 0; i < count; ++i) level.add(toBucket(samples[i]));
    result.baseline = static_cast<double>(level.quantile(0.5));

    Histogram deviation;
    for (int i = 0; i < count; ++i) deviation.add(toBucket(std::fabs(samples[i] - result.baseline)));
    result.sigma = kMadToSigma * static_cast<double>(deviation.quantile(0.5));

    result.effectiveThreshold = resolveThreshold(result, settings);
    return result;
}

Result detect(const pdsample::WaveformFrame &frame, const Settings &settings)
{
    QElapsedTimer timer;
    timer.start();

    Result result;
    if (frame.sampleCount <= 0) {
        result.note = QStringLiteral("帧长度为 0，未做检测。");
        return result;
    }

    int truncatedChannels = 0;
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        const QVector<double> &data = frame.channel[c];
        if (data.size() != frame.sampleCount) {
            result.note = QStringLiteral("CH%1 长度与帧长不一致，未做检测。").arg(c);
            return result;
        }

        ChannelResult &channel = result.channel[c];
        channel = estimateChannel(data, settings);

        /*
         * 两个阈值都没启用时不做检测。
         * 这种情况必须显式说明，而不是"阈值 0 于是每个样点都算脉冲"——
         * 后者会在界面上画出满屏高亮，看起来像"到处都是局放"。
         */
        if (channel.effectiveThreshold <= 0.0) continue;

        const double baseline = channel.baseline;
        const double threshold = channel.effectiveThreshold;
        const double *samples = data.constData();
        const int count = data.size();

        /* ---- 单趟扫描：按越限区间提取正/负极值 ---- */
        bool inRun = false;
        int runStart = 0;
        double positivePeak = 0.0;
        int positiveIndex = 0;
        double negativePeak = 0.0;
        int negativeIndex = 0;

        auto closeRun = [&](int endIndex) {
            if (!inRun) return;
            if (positivePeak > threshold) {
                Pulse pulse;
                pulse.channel = c;
                pulse.index = positiveIndex;
                pulse.baseline = baseline;
                pulse.code = positivePeak;
                pulse.absCode = positivePeak;
                pulse.positive = true;
                pulse.confirmed = settings.absoluteThresholdCodes > 0.0 &&
                                  positivePeak >= settings.absoluteThresholdCodes;
                /* 抛物线插值：用极值点及其左右邻居估更准的位置。 */
                double refined = static_cast<double>(positiveIndex);
                if (positiveIndex > 0 && positiveIndex + 1 < count) {
                    const double left = samples[positiveIndex - 1] - baseline;
                    const double mid = positivePeak;
                    const double right = samples[positiveIndex + 1] - baseline;
                    const double denominator = left - 2.0 * mid + right;
                    if (std::fabs(denominator) > 1e-9) {
                        double delta = 0.5 * (left - right) / denominator;
                        refined += std::max(-1.0, std::min(1.0, delta));
                    }
                }
                pulse.refinedIndex = refined;
                if (frame.sampleRateHz > 0.0) pulse.timeSec = refined / frame.sampleRateHz;
                result.pulses[c].append(pulse);
            }
            if (-negativePeak > threshold) {
                Pulse pulse;
                pulse.channel = c;
                pulse.index = negativeIndex;
                pulse.baseline = baseline;
                pulse.code = negativePeak;
                pulse.absCode = -negativePeak;
                pulse.positive = false;
                pulse.confirmed = settings.absoluteThresholdCodes > 0.0 &&
                                  -negativePeak >= settings.absoluteThresholdCodes;
                double refined = static_cast<double>(negativeIndex);
                if (negativeIndex > 0 && negativeIndex + 1 < count) {
                    const double left = samples[negativeIndex - 1] - baseline;
                    const double mid = negativePeak;
                    const double right = samples[negativeIndex + 1] - baseline;
                    const double denominator = left - 2.0 * mid + right;
                    if (std::fabs(denominator) > 1e-9) {
                        double delta = 0.5 * (left - right) / denominator;
                        refined += std::max(-1.0, std::min(1.0, delta));
                    }
                }
                pulse.refinedIndex = refined;
                if (frame.sampleRateHz > 0.0) pulse.timeSec = refined / frame.sampleRateHz;
                result.pulses[c].append(pulse);
            }
            Q_UNUSED(endIndex);
            inRun = false;
            positivePeak = 0.0;
            negativePeak = 0.0;
        };

        for (int i = 0; i < count; ++i) {
            const double deviation = samples[i] - baseline;
            if (std::fabs(deviation) > threshold) {
                if (!inRun) {
                    inRun = true;
                    runStart = i;
                    positivePeak = 0.0;
                    negativePeak = 0.0;
                }
                if (deviation > positivePeak) {
                    positivePeak = deviation;
                    positiveIndex = i;
                }
                if (deviation < negativePeak) {
                    negativePeak = deviation;
                    negativeIndex = i;
                }
            } else if (inRun) {
                closeRun(i);
            }
        }
        if (inRun) closeRun(count);

        /* ---- 去抖：同极性且间隔小于 mergeGapSamples 的相邻脉冲合并 ---- */
        QVector<Pulse> merged;
        merged.reserve(result.pulses[c].size());
        for (const Pulse &pulse : result.pulses[c]) {
            if (!merged.isEmpty()) {
                Pulse &previous = merged.last();
                if (previous.positive == pulse.positive &&
                    pulse.index - previous.index < settings.mergeGapSamples) {
                    if (pulse.absCode > previous.absCode) {
                        /* 保留幅值更大的那个，但把 confirmed 一起带过来。 */
                        previous = pulse;
                    }
                    continue;
                }
            }
            merged.append(pulse);
        }
        result.pulses[c] = merged;

        /* ---- 上限：超出时保留幅值最大的若干个，再按时间顺序排回 ---- */
        if (settings.maxPulsesPerChannel > 0 &&
            result.pulses[c].size() > settings.maxPulsesPerChannel) {
            std::partial_sort(result.pulses[c].begin(),
                              result.pulses[c].begin() + settings.maxPulsesPerChannel,
                              result.pulses[c].end(),
                              [](const Pulse &a, const Pulse &b) { return a.absCode > b.absCode; });
            result.pulses[c].resize(settings.maxPulsesPerChannel);
            std::sort(result.pulses[c].begin(), result.pulses[c].end(),
                      [](const Pulse &a, const Pulse &b) { return a.index < b.index; });
            ++truncatedChannels;
        }

        for (const Pulse &pulse : result.pulses[c]) {
            ++result.totalPulses;
            if (pulse.confirmed) ++result.confirmedPulses;
        }
        channel.pulseCount = result.pulses[c].size();
        channel.confirmedCount = static_cast<int>(std::count_if(
            result.pulses[c].cbegin(), result.pulses[c].cend(),
            [](const Pulse &pulse) { return pulse.confirmed; }));
    }

    result.ok = true;
    QStringList parts;
    parts << QStringLiteral("基线用中位数、噪声用 MAD(×1.4826)；判据 = max(用户阈值 %1 码, %2×sigma)")
                 .arg(settings.absoluteThresholdCodes, 0, 'f', 0)
                 .arg(settings.adaptiveMultiplier, 0, 'f', 1);
    if (truncatedChannels > 0)
        parts << QStringLiteral("%1 个通道的脉冲数超过上限 %2，已按幅值截断")
                     .arg(truncatedChannels).arg(settings.maxPulsesPerChannel);
    parts << QStringLiteral("%1 ms").arg(timer.elapsed());
    result.note = parts.join(QStringLiteral("；"));
    return result;
}

} // namespace pddetect

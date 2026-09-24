/*
 * 上位机离线自检（不需要板端）。
 *
 * 目的：把"上位机对 PL/PS 数据格式的理解"变成可执行的断言，而不是靠肉眼看波形
 * 对不对。每一项通过判据都写死在代码里，失败即非零退出码，可以直接接进流水线。
 *
 * 覆盖四条链路：
 *   1. 6 字节采样契约：按 PL RTL（pd_pack48.v / pd_pack192.v）正向打包，
 *      再用上位机解码函数还原，逐位比对；
 *   2. DDS 波形真值：在主机侧用 RTL 的 tri_wave / pd_pulse 公式重建 520,000 点，
 *      与板上实测到的四通道最小值 2032/2040/2006/2046 对照；
 *   3. FFT 引擎：单音峰值 bin、幅度、去直流、频率换算；
 *   4. 触发查找：上升沿/下降沿位置与迟滞抑制抖动。
 *
 * 另外给出一项性能实测：4 通道 1024 点 FFT 的耗时，用来对照旧实现的
 * 朴素 DFT（约 210 万次三角函数调用）。
 */

#include "fft_engine.h"
#include "pd_pulse_detect.h"
#include "pd_reply_parse.h"
#include "pd_sample_codec.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

int g_passed = 0;
int g_failed = 0;

void check(bool condition, const QString &what, const QString &detail = QString())
{
    if (condition) {
        ++g_passed;
        std::printf("  [PASS] %s\n", qPrintable(what));
    } else {
        ++g_failed;
        std::printf("  [FAIL] %s%s\n", qPrintable(what),
                    detail.isEmpty() ? "" : qPrintable(QStringLiteral("  -> ") + detail));
    }
}

void section(const QString &title)
{
    std::printf("\n=== %s ===\n", qPrintable(title));
}

/* ---------------------------------------------------------- 1. 打包/解码契约 */

/*
 * 按 PL 的 pd_pack48.v / pd_pack192.v 正向编码一个采样时刻的 6 个字节。
 * 48-bit 字布局：{ch3[47:36], ch2[35:24], ch1[23:12], ch0[11:0]}，小端落盘。
 */
QByteArray encodeSampleBlock(quint16 c0, quint16 c1, quint16 c2, quint16 c3)
{
    const quint64 word = (static_cast<quint64>(c3 & 0x0FFFU) << 36)
                       | (static_cast<quint64>(c2 & 0x0FFFU) << 24)
                       | (static_cast<quint64>(c1 & 0x0FFFU) << 12)
                       | static_cast<quint64>(c0 & 0x0FFFU);
    QByteArray out;
    out.resize(6);
    for (int i = 0; i < 6; ++i)
        out[i] = static_cast<char>((word >> (8 * i)) & 0xFFU);
    return out;
}

void testSampleCodec()
{
    section(QStringLiteral("1. 6 字节采样契约（PL pd_pack48 / pd_pack192 <-> 上位机解码）"));

    struct Case { quint16 value[4]; };
    const Case cases[] = {
        {{0x000, 0x000, 0x000, 0x000}}, /* 全零边界 */
        {{0xFFF, 0xFFF, 0xFFF, 0xFFF}}, /* 全满边界 */
        {{0x800, 0x800, 0x800, 0x800}}, /* 名义零电平 */
        {{0x7F0, 0x7F8, 0x7EE, 0x80E}}, /* 板上实测的四通道最小值 2032/2040/2006/2046 */
        {{0x001, 0xABC, 0x123, 0xFED}}, /* 交叉位模式，检验 nibble 边界 */
    };

    for (const Case &one : cases) {
        const QByteArray block = encodeSampleBlock(one.value[0], one.value[1],
                                                   one.value[2], one.value[3]);
        const auto *p = reinterpret_cast<const uchar *>(block.constData());
        bool ok = true;
        for (int c = 0; c < 4; ++c)
            if (pdsample::decodeChannelSample(p, c) != one.value[c]) ok = false;
        check(ok, QStringLiteral("往返编码 ch0..ch3 = %1,%2,%3,%4")
                       .arg(one.value[0], 3, 16, QLatin1Char('0'))
                       .arg(one.value[1], 3, 16, QLatin1Char('0'))
                       .arg(one.value[2], 3, 16, QLatin1Char('0'))
                       .arg(one.value[3], 3, 16, QLatin1Char('0')));
    }

    /* 非 24 字节块的输入必须被拒绝：PL 只以 24 字节块写 DDR。 */
    QByteArray bad = encodeSampleBlock(1, 2, 3, 4);
    bad.append('\0');
    pdsample::WaveformFrame frame;
    check(!pdsample::decodeFrame(bad, 26000000.0, 0, frame),
          QStringLiteral("长度 7 字节（非 24 的整数倍）被拒绝"));

    QByteArray good;
    good += encodeSampleBlock(0x123, 0x456, 0x789, 0xABC);
    good += encodeSampleBlock(0x123, 0x456, 0x789, 0xABC);
    good += encodeSampleBlock(0x123, 0x456, 0x789, 0xABC);
    good += encodeSampleBlock(0x123, 0x456, 0x789, 0xABC);
    check(pdsample::decodeFrame(good, 26000000.0, 7, frame) &&
              frame.sampleCount == 4 && frame.sequence == 7 &&
              std::fabs(frame.sampleIntervalSec - 1.0 / 26000000.0) < 1e-15,
          QStringLiteral("24 字节 = 4 个采样时刻，采样间隔 = 1/26MHz"));
}

/* ---------------------------------------------------------- 2. DDS 波形真值 */

int triWave(int phase8)
{
    const int magnitude = (phase8 & 0x80) ? (127 - (phase8 & 0x7F)) : (phase8 & 0x7F);
    return magnitude - 64;
}

/* Verilog 对 signed 的 >>> 是向 -inf 取整的算术右移，C++ 的 >> 不保证，故显式实现。 */
int arithmeticShiftRight(int value, int bits)
{
    const int divisor = 1 << bits;
    if (value >= 0) return value / divisor;
    return -(((-value) + divisor - 1) / divisor);
}

int pdPulse(int samplePosition, int centre)
{
    switch (samplePosition - centre) {
    case 0: return 720;
    case 1: return 600;
    case 2: return 460;
    case 3: return 300;
    default: return 0;
    }
}

int saturateU12(int value)
{
    if (value < 0) return 0;
    if (value > 4095) return 4095;
    return value;
}

void testDdsTruth()
{
    section(QStringLiteral("2. DDS 波形真值（RTL pd_dds_adc_source.v 公式重建 520,000 点）"));

    constexpr int kSampleCount = 520000; /* SAMPLE_HZ/SYNC_HZ = 26e6/50 */
    const int phaseStart[4] = {0, 53, 107, 179};
    const int phaseStep[4] = {3, 5, 7, 11};
    const int baseline[4] = {2048, 2056, 2038, 2062};
    const int shift[4] = {2, 2, 1, 2};
    const int pulseCentre[4] = {65000, 195000, 325000, 455000};
    /*
     * RTL 的 pd_pulse(sample_pos, centre) 在 sample_pos == centre 处恒返回 720，
     * 之后三个采样点依次 600 / 460 / 300。也就是说 720/600/460/300 是"同一个脉冲
     * 的四个连续样点"，不是四个通道各自的幅度；四通道脉冲形状相同，只有位置不同。
     */
    const int pulseShape[4] = {720, 600, 460, 300};

    QByteArray raw;
    raw.resize(kSampleCount * pdsample::kBytesPerSample);
    int expectedMinimum[4] = {4095, 4095, 4095, 4095};
    int expectedMaximum[4] = {0, 0, 0, 0};
    int observedShape[4][4] = {{0}};

    for (int n = 0; n < kSampleCount; ++n) {
        int value[4];
        for (int c = 0; c < 4; ++c) {
            const int phase = (phaseStart[c] + n * phaseStep[c]) & 0xFF;
            const int triangle = arithmeticShiftRight(triWave(phase), shift[c]);
            const int pulse = pdPulse(n, pulseCentre[c]);
            value[c] = saturateU12(baseline[c] + triangle + pulse);
            expectedMinimum[c] = std::min(expectedMinimum[c], value[c]);
            expectedMaximum[c] = std::max(expectedMaximum[c], value[c]);
            /* 记录脉冲四个样点相对"无脉冲基线"的实际增量。 */
            const int offset = n - pulseCentre[c];
            if (offset >= 0 && offset < 4) {
                const int withoutPulse = saturateU12(baseline[c] + triangle);
                observedShape[c][offset] = value[c] - withoutPulse;
            }
        }
        const QByteArray block = encodeSampleBlock(value[0], value[1], value[2], value[3]);
        memcpy(raw.data() + n * pdsample::kBytesPerSample, block.constData(),
               pdsample::kBytesPerSample);
    }

    pdsample::WaveformFrame frame;
    const bool decoded = pdsample::decodeFrame(raw, 26000000.0, 0, frame);
    check(decoded && frame.sampleCount == kSampleCount,
          QStringLiteral("整段 3,120,000 字节解出 %1 个采样点").arg(kSampleCount));

    pdsample::ChannelStats stats;
    check(pdsample::measure(frame, stats), QStringLiteral("min/max/mean 统计可用"));

    /* 这四个最小值是此前上板实测的结果，若主机侧模型能复现，说明
       "RTL -> 打包 -> DDR -> 上位机解码" 这一整条链的理解是对的。 */
    const int onboardMinimum[4] = {2032, 2040, 2006, 2046};
    for (int c = 0; c < 4; ++c) {
        check(static_cast<int>(stats.minimum[c]) == onboardMinimum[c] &&
                  static_cast<int>(stats.minimum[c]) == expectedMinimum[c],
              QStringLiteral("CH%1 最小值 = %2（与板上实测一致）")
                  .arg(c).arg(static_cast<int>(stats.minimum[c])),
              QStringLiteral("预期 %1，实测 %2")
                  .arg(onboardMinimum[c]).arg(static_cast<int>(stats.minimum[c])));
    }
    for (int c = 0; c < 4; ++c) {
        const int headroom = 4095 - expectedMaximum[c];
        check(static_cast<int>(stats.maximum[c]) == expectedMaximum[c] && headroom >= 300,
              QStringLiteral("CH%1 最大值 = %2（未饱和，余量 %3 码）")
                  .arg(c).arg(static_cast<int>(stats.maximum[c])).arg(headroom));
    }
    for (int c = 0; c < 4; ++c) {
        bool shapeOk = true;
        for (int k = 0; k < 4; ++k)
            if (observedShape[c][k] != pulseShape[k]) shapeOk = false;
        check(shapeOk,
              QStringLiteral("CH%1 脉冲四样点增量 = %2/%3/%4/%5 码（RTL 常量 720/600/460/300）")
                  .arg(c).arg(observedShape[c][0]).arg(observedShape[c][1])
                  .arg(observedShape[c][2]).arg(observedShape[c][3]));
    }

    /* 三角波周期：相位步进与 256 互素 => 基频恒为 26MHz/256 = 101.5625 kHz。
       四通道同频，差别只在初相与直流基线。 */
    const double triangleHz = 26000000.0 / 256.0;
    check(std::fabs(triangleHz - 101562.5) < 1e-6,
          QStringLiteral("三角波基频 = %1 Hz（四通道相同）").arg(triangleHz, 0, 'f', 1));

    /* 用码密度反证周期：统计一个完整三角周期内的阶梯数。 */
    {
        int distinctLevels = 0;
        bool seen[256] = {false};
        for (int n = 0; n < 256; ++n) {
            const int phase = (phaseStart[1] + n * phaseStep[1]) & 0xFF;
            if (!seen[phase]) { seen[phase] = true; ++distinctLevels; }
        }
        check(distinctLevels == 256,
              QStringLiteral("CH1 在 256 个采样点内遍历全部 256 个相位值（周期 = 256 点）"));
    }
}

/* ---------------------------------------------- 2.5 事件包位域（相位环的数据源） */

/*
 * 按 PS 侧 pd_event_decode.c 的字段位置正向构造事件字，再交给上位机解码。
 * 这是相位环唯一的相位来源，必须先证明位域理解一致。
 */
/*
 * ⚠️ 这里把 RTL 明文写死的位位置**再抄一遍**（而不是用 pd_sample_codec.h 的常量），
 * 目的就是让"上位机常量与 RTL 漂移"这件事必然被测出来：
 *   pd_feature_core.v:562-573 的拼接，PD_PH_FIELD_W = 12
 *     [63:56] type  [55:40] q  [39:28] phase  [27] polarity  [26:25] ch  [24:0] evt_seq
 * 若哪天有人把 pd_sample_codec.h 里的 kPhaseShift 改成 30（10 位布局），
 * 这里的用例会立刻失败，而不是等到上板才发现相位画错。
 */
quint64 encodePeakEvent(quint16 q88Raw, quint16 phase12, quint16 channel,
                        bool positive = true, quint32 evtSeq = 0)
{
    quint64 word = 0;
    word |= (static_cast<quint64>(0x00U) << 56);              /* type = 峰值包 */
    word |= (static_cast<quint64>(q88Raw) << 40);             /* [55:40] q */
    word |= (static_cast<quint64>(phase12 & 0x0FFFU) << 28);  /* [39:28] phase（12 位） */
    word |= (static_cast<quint64>(positive ? 1U : 0U) << 27); /* [27] polarity */
    word |= (static_cast<quint64>(channel & 0x3U) << 25);     /* [26:25] ch_id */
    word |= (static_cast<quint64>(evtSeq) & 0x1FFFFFFULL);    /* [24:0] evt_seq（25 位） */
    return word;
}

quint64 encodeCycleEvent(quint16 qmax, quint16 events, quint16 channel)
{
    return (static_cast<quint64>(1U) << 56)
         | (static_cast<quint64>(0x12345U) << 32)
         | (static_cast<quint64>(channel & 0x3U) << 30)
         | (static_cast<quint64>(events & 0x3FFFU) << 16)
         | static_cast<quint64>(qmax);
}

void testEventPacket()
{
    section(QStringLiteral("2.5 事件包位域（360° 相位环的相位来源）"));

    struct Case { quint16 q88Raw; quint16 phase12; quint16 channel; };
    const Case cases[] = {
        {0x0100, 0, 0},      /* Q8.8 = +1.0，相位 0° */
        {0xFF00, 1024, 1},   /* Q8.8 = -1.0，相位 90° */
        {0x0800, 2048, 2},   /* Q8.8 = +8.0，相位 180° */
        {0x7FFF, 4095, 3},   /* 正满量程，相位接近 360° */
        {0x8000, 3072, 0},   /* 负满量程，相位 270° */
    };

    for (const Case &one : cases) {
        pdsample::PeakEvent event;
        bool valid = false;
        const bool peak = pdsample::decodePeakEvent(
            encodePeakEvent(one.q88Raw, one.phase12, one.channel), event, valid);
        const double expectedQ = static_cast<qint16>(one.q88Raw) / 256.0;
        const double expectedPhase = one.phase12 * 360.0 / 4096.0;
        check(peak && valid && event.channel == one.channel &&
                  std::fabs(event.q88 - expectedQ) < 1e-9 &&
                  std::fabs(event.phaseDeg - expectedPhase) < 1e-9,
              QStringLiteral("峰值包 ch=%1 相位=%2° Q8.8=%3")
                  .arg(one.channel).arg(expectedPhase, 0, 'f', 3).arg(expectedQ, 0, 'f', 4));
    }

    /* 周期统计包必须被识别为"正常跳过"，而不是"格式不认识"。 */
    pdsample::PeakEvent event;
    bool valid = true;
    const bool peak = pdsample::decodePeakEvent(encodeCycleEvent(1234, 7, 2), event, valid);
    check(!peak && valid, QStringLiteral("周期包被识别为正常跳过（valid 保持 true）"));

    /* 未知 type 必须显式标记为无效，不能被当成数据画到环上。 */
    valid = true;
    const bool unknown = pdsample::decodePeakEvent(0x5A00000000000000ULL, event, valid);
    check(!unknown && !valid, QStringLiteral("type 非 0/1 时 valid=false（不静默当数据用）"));

    /* ---- 极性位 [27]：PD 在正负半周的差异是诊断依据，必须解出来 ---- */
    {
        pdsample::PeakEvent positiveEvent;
        pdsample::PeakEvent negativeEvent;
        bool okPositive = true;
        bool okNegative = true;
        pdsample::decodePeakEvent(encodePeakEvent(0x02D0, 512, 2, true), positiveEvent, okPositive);
        pdsample::decodePeakEvent(encodePeakEvent(0xFD30, 512, 2, false), negativeEvent, okNegative);
        check(okPositive && positiveEvent.positive && !negativeEvent.positive,
              QStringLiteral("极性位 [27]：正极性=1、负极性=0 都能解出"));
    }

    /* ---- evt_seq [24:0]：每通道自增序号，可做记录内丢事件检测 ---- */
    {
        pdsample::PeakEvent first;
        pdsample::PeakEvent later;
        bool ok = true;
        pdsample::decodePeakEvent(encodePeakEvent(0x0100, 100, 1, true, 0x1ABCDEFU), first, ok);
        pdsample::decodePeakEvent(encodePeakEvent(0x0100, 100, 1, true, 0x1ABCDF0U), later, ok);
        check(first.evtSeq == 0x1ABCDEFU && later.evtSeq == 0x1ABCDF0U,
              QStringLiteral("evt_seq [24:0] 解出正确（含 25 位满宽度）"));
    }

    /* ---- 位宽推导：12 位相位下 ch 必须在 [26:25]，不在 10 位布局的 [28:27] ---- */
    {
        pdsample::PeakEvent decoded;
        bool ok = true;
        /* 只在 [28:27] 置位（10 位布局下的 ch 位置）。按 12 位布局它应当是 evt_seq 的一部分，
           通道必须仍是 0 —— 这条用来钉死"当前配置是 12 位相位"。 */
        const quint64 misleading = (static_cast<quint64>(0x00U) << 56) |
                                   (static_cast<quint64>(0x3U) << 27);
        pdsample::decodePeakEvent(misleading, decoded, ok);
        /*
         * 按 10 位相位的布局，[28:27] 是 ch_id，这里应当解出 ch=3；
         * 按当前的 12 位布局，bit 28 是相位的最低位、bit 27 是极性，ch 仍是 0。
         * 期望 ch=0、phase=1、polarity=正、evt_seq=0 —— 一眼能看出用的是哪种布局。
         */
        check(ok && decoded.channel == 0 && decoded.phaseWindow == 1U &&
                  decoded.positive && decoded.evtSeq == 0U,
              QStringLiteral("位宽推导正确：bit28 属相位、bit27 属极性，ch 仍是 0（12 位布局）"));
    }

    /* ---- SCALE 换算：默认 256 时 q 原始值即 AD 码；SCALE=512 时折半 ---- */
    check(qFuzzyCompare(pdsample::eventpacket::adcCodesFromField(720, 256.0), 720.0) &&
              qFuzzyCompare(pdsample::eventpacket::adcCodesFromField(720, 512.0), 360.0) &&
              qFuzzyCompare(pdsample::eventpacket::kDefaultScaleQ88, 256.0),
          QStringLiteral("SCALE 换算：默认 256 时 q 原始值 = AD 码；512 时折半"));
}

/* ---------------------------------------------------------- 3. FFT 引擎 */

void testFft()
{
    section(QStringLiteral("3. FFT 引擎（与 PS 侧 pd_spectrum.c 同口径）"));

    constexpr int kPoints = 1024;
    constexpr double kRate = 26000000.0;
    const pdspectrum::Engine engine(kPoints);

    /* 构造单音：bin 37，幅度 1024，偏置 2048。判据与 PS 的 FFT SELFTEST 一致。 */
    QVector<double> samples(kPoints);
    const int bin = 37;
    for (int i = 0; i < kPoints; ++i)
        samples[i] = 2048.0 + 1024.0 * std::sin(kTwoPi * bin * i / kPoints);

    pdspectrum::ChannelSpectrum spectrum =
        engine.analyze(samples.constData(), kPoints, 0, kRate, pdspectrum::Window::Hann);

    check(spectrum.peakBin == bin, QStringLiteral("主峰 bin = %1").arg(spectrum.peakBin),
          QStringLiteral("预期 %1").arg(bin));
    check(spectrum.peakCode >= 900.0 && spectrum.peakCode <= 1150.0,
          QStringLiteral("主峰幅度 = %1 码（PS 自检容差 900..1150）")
              .arg(spectrum.peakCode, 0, 'f', 1));
    check(std::fabs(spectrum.dcCode - 2048.0) < 1e-6,
          QStringLiteral("去直流估计 = %1 码").arg(spectrum.dcCode, 0, 'f', 3));

    const double expectedHz = bin * kRate / kPoints;
    check(std::fabs(spectrum.peakHz - expectedHz) < 1e-6,
          QStringLiteral("主峰频率 = %1 Hz").arg(spectrum.peakHz, 0, 'f', 3),
          QStringLiteral("预期 %1").arg(expectedHz, 0, 'f', 3));

    /* 叠加 300 码直流后去直流仍应压掉它：主峰 bin 不变、dcCode 跟随变化。 */
    QVector<double> shifted(kPoints);
    for (int i = 0; i < kPoints; ++i) shifted[i] = samples[i] + 300.0;
    const pdspectrum::ChannelSpectrum second =
        engine.analyze(shifted.constData(), kPoints, 0, kRate, pdspectrum::Window::Hann);
    check(second.peakBin == bin && std::fabs(second.dcCode - 2348.0) < 1e-6,
          QStringLiteral("加 300 码直流后仍命中 bin %1，dcCode = %2")
              .arg(second.peakBin).arg(second.dcCode, 0, 'f', 1));

    /* 矩形窗下主峰位置必须一致（只影响旁瓣幅度，不应移动峰位）。 */
    const pdspectrum::ChannelSpectrum rectangular =
        engine.analyze(samples.constData(), kPoints, 0, kRate, pdspectrum::Window::Rectangular);
    check(rectangular.peakBin == bin,
          QStringLiteral("矩形窗主峰 bin 一致 = %1").arg(rectangular.peakBin));

    /* 数据不足时必须拒绝，而不是零填充后给一个看似合理的结果。 */
    const pdspectrum::ChannelSpectrum tooShort =
        engine.analyze(samples.constData(), 512, 0, kRate, pdspectrum::Window::Hann);
    check(tooShort.frequencyHz.isEmpty(),
          QStringLiteral("样本不足 1024 点时返回空结果（不用零填充掩盖）"));

    /* 性能：与旧实现的朴素 DFT 在同一次构建下直接对比，结论不依赖优化等级。
       旧实现（替换前）在 loadRawSamplePlots 里对 512 个 bin × 1024 个样点
       逐点调用 cos()/sin()，四个通道一次刷新约 210 万次三角函数调用。 */
    const auto naiveDftOneChannel = [](const QVector<double> &input, int points) {
        double mean = 0.0;
        for (double value : input) mean += value;
        mean /= points;
        volatile double sink = 0.0;
        for (int k = 1; k < points / 2; ++k) {
            double real = 0.0, imag = 0.0;
            for (int n = 0; n < points; ++n) {
                const double phase = -kTwoPi * k * n / points;
                const double sample = input.at(n) - mean;
                real += sample * std::cos(phase);
                imag += sample * std::sin(phase);
            }
            sink += std::sqrt(real * real + imag * imag);
        }
        return sink;
    };

    QElapsedTimer timer;
    constexpr int kRounds = 20;

    timer.start();
    for (int round = 0; round < kRounds; ++round)
        for (int c = 0; c < 4; ++c)
            engine.analyze(samples.constData(), kPoints, 0, kRate, pdspectrum::Window::Hann);
    const double fftPerFrameMs = timer.elapsed() / static_cast<double>(kRounds);

    timer.restart();
    double naiveSink = 0.0;
    naiveSink += naiveDftOneChannel(samples, kPoints);
    const double naivePerFrameMs = timer.elapsed() * 4.0;
    (void)naiveSink; /* 防止优化器把参照计算整个删掉 */

    const double speedup = naivePerFrameMs / qMax(1e-6, fftPerFrameMs);
    check(speedup >= 10.0,
          QStringLiteral("同构建下 FFT %1 ms/帧 vs 朴素 DFT %2 ms/帧，加速 %3 倍")
              .arg(fftPerFrameMs, 0, 'f', 3)
              .arg(naivePerFrameMs, 0, 'f', 3)
              .arg(speedup, 0, 'f', 1));
    check(fftPerFrameMs < 20.0,
          QStringLiteral("FFT 单帧耗时 %1 ms（Debug 构建，Release 会更快）")
              .arg(fftPerFrameMs, 0, 'f', 3));
}

/* ------------------------------------------ 3.5 板端回复解析（用实抓原文做向量） */

/*
 * 这里的每一条"输入"都是两种来源之一：
 *   (a) 2026-09-23 上板实抓日志里的原文；
 *   (b) pd_tcp_service.c 里 snprintf 模板逐字段拼出来的等价行。
 * 解析格式一旦与板端不一致，上位机的表现只是"没有数据"，排查代价极高，
 * 所以把这些格式钉成断言。
 */
void testBoardReplies()
{
    section(QStringLiteral("3.5 板端回复解析（含实抓日志原文）"));

    /* ---- CATALOG ---- */
    {
        const auto c = pdreply::parseCatalog(
            QStringLiteral("CATALOG event_seq=[17811,17827) slots=16 snap_seq=[1384,1388) "
                           "slots=4 state=1"));
        check(c.ok && c.eventFirst == 17811U && c.eventNext == 17827U &&
                  c.snapFirst == 1384U && c.snapNext == 1388U && c.state == 1,
              QStringLiteral("CATALOG（采集中）：event=[17811,17827) snap=[1384,1388) state=1"));
    }
    {
        const auto c = pdreply::parseCatalog(
            QStringLiteral("CATALOG event_seq=[20900,20916) slots=16 snap_seq=[1642,1646) "
                           "slots=4 state=0"));
        check(c.ok && c.state == 0 && c.eventFirst == 20900U,
              QStringLiteral("CATALOG（已停止）：state=0 被正确取出"));
    }
    check(!pdreply::parseCatalog(QStringLiteral("CATALOG event_seq=[1,2) state=1")).ok,
          QStringLiteral("缺 snap_seq 字段时判定解析失败（不拿默认值当数据）"));

    /* ---- EVENT 元数据：peaks 字段是"要不要下载"的判据，必须解析出来 ---- */
    {
        const auto e = pdreply::parseEventMeta(
            QStringLiteral("EVENT seq=17823 index=15 addr=270f0000 bytes=8224 peaks=1027 cycles=1"));
        check(e.ok && e.sequence == 17823U && e.index == 15U && e.bytes == 8224U &&
                  e.peaks == 1027U && e.cycles == 1U,
              QStringLiteral("EVENT（含峰值）：bytes=8224 peaks=1027"));
    }
    {
        const auto e = pdreply::parseEventMeta(
            QStringLiteral("EVENT seq=17724 index=12 addr=270c0000 bytes=8 peaks=0 cycles=1"));
        check(e.ok && e.peaks == 0U && e.bytes == 8U,
              QStringLiteral("EVENT（无峰值）：peaks=0 可被识别以跳过下载"));
    }
    /* 如果没有 peaks 字段就必须判失败：这正是"以为下载了其实没数据"的隐患来源。 */
    check(!pdreply::parseEventMeta(
              QStringLiteral("EVENT seq=1 index=2 addr=27000000 bytes=8")).ok,
          QStringLiteral("缺 peaks/cycles 字段时判定解析失败"));

    /* ---- DATA V2 头 ---- */
    {
        const auto d = pdreply::parseDataHeader(
            QStringLiteral("DATA V2 kind=EVENT index=11 offset=0 bytes=8224 crc32=007d0b67"));
        check(d.ok && d.kind == QStringLiteral("EVENT") && d.index == 11U &&
                  d.bytes == 8224U && d.crc32 == 0x007D0B67U,
              QStringLiteral("DATA V2：bytes=8224 crc32=007d0b67"));
    }

    /* ---- STATUS ---- */
    {
        const auto st = pdreply::parseStatus(
            QStringLiteral("STATUS state=1 packets=512 events=17827 snaps=1388 analysis=0 "
                           "sweeps=0 ev_ovw=17811 snap_ovw=1384 recov=0 disc=0 ddr=0000000b "
                           "slot=00080010 dma=00000000 drops=0 err="));
        check(st.ok && st.state == 1, QStringLiteral("STATUS：state=1（采集中）"));
    }
    {
        const auto st = pdreply::parseStatus(QStringLiteral("STATUS state=0 packets=0"));
        check(st.ok && st.state == 0, QStringLiteral("STATUS：state=0（IDLE）"));
    }

    /* ---- SCOPE V1 帧头（按板端 snprintf 模板构造） ---- */
    {
        const auto h = pdreply::parseScopeHeader(
            QStringLiteral("SCOPE V1 seq=7 samples=1024 bytes=6144 fs=26000000 crc32=deadbeef"));
        check(h.ok && h.sequence == 7U && h.samples == 1024U && h.bytes == 6144U &&
                  h.sampleRateHz == 26000000U && h.crc32 == 0xDEADBEEFU,
              QStringLiteral("SCOPE V1：samples=1024 bytes=6144 fs=26000000"));
    }

    /* ---- SNAP 元数据（两种头都要认） ---- */
    {
        const auto m = pdreply::parseSnapMeta(
            QStringLiteral("SNAP seq=1587 index=3 hw_slot=1 src=10002000 dst=24000000 "
                           "bytes=3120000 flags=00000001"));
        check(m.ok && m.sequence == 1587U && m.index == 3U && m.bytes == 3120000U,
              QStringLiteral("SNAP 序号模式：seq=1587 bytes=3120000（=520000 样本）"));
    }
    {
        const auto m = pdreply::parseSnapMeta(
            QStringLiteral("SNAP index=3 seq=1587 hw_slot=1 src=10002000 dst=24000000 "
                           "bytes=3120000 flags=00000001"));
        check(m.ok && m.sequence == 1587U && m.index == 3U,
              QStringLiteral("SNAP 槽下标模式：字段顺序相反也能解析"));
    }

    /* ---- CONFIG：SCOPE 能力探测（决定"波形能不能出来"） ---- */
    {
        const auto c = pdreply::parseConfig(QStringLiteral(
            "CONFIG api=12 default_limit=512 event_slots=16 event_stride=128 snap_slots=4 "
            "snap_stride=3120000 analysis_slots=4 sweep_slots=4 sweep_windows=5 prpd_bins=64 "
            "scope=1024/2048 alert_mask=0xf alert_delta=0 get_max=16384 state=1"));
        check(c.ok && c.api == 12U && c.hasScope && c.scopeSamples == 1024U &&
                  c.scopeMaxSamples == 2048U && c.snapSlots == 4U && c.eventSlots == 16U,
              QStringLiteral("CONFIG 含 scope 字段 => 固件支持 SCOPE（1024/2048）"));
    }
    {
        /* 旧固件没有 scope 字段：必须判 hasScope=false，上位机据此明确报错，
           而不是发 SCOPE ON 拿到 "unknown command" 后静默空转。 */
        const auto c = pdreply::parseConfig(QStringLiteral(
            "CONFIG api=11 default_limit=512 event_slots=16 snap_slots=4 prpd_bins=64 "
            "alert_mask=0xf alert_delta=0 get_max=16384 state=1"));
        check(c.ok && !c.hasScope && c.api == 11U,
              QStringLiteral("CONFIG 缺 scope 字段 => 判定固件不支持 SCOPE（反向用例）"));
    }

    /* ---- PRPD BINS（来自 TCP_API_v11_PRPD说明.md） ---- */
    {
        const auto b = pdreply::parsePrpdBins(
            QStringLiteral("PRPD_BINS seq=[496,512) ch=0 first=0 count=8 "
                           "values=0,1,0,0,0,0,0,0"));
        check(b.ok && b.channel == 0 && b.first == 0 && b.count == 8 &&
                  b.values.size() == 8 && b.values.at(1) == 1U,
              QStringLiteral("PRPD_BINS：8 桶，第 1 桶 = 1"));
    }
    /* 桶数与 count 不一致必须判失败：半截直方图比没有数据更糟。 */
    check(!pdreply::parsePrpdBins(
              QStringLiteral("PRPD_BINS seq=[496,512) ch=0 first=0 count=8 values=0,1,0")).ok,
          QStringLiteral("values 个数与 count 不符时判定解析失败"));
}

/* ------------------------------------- 3.6 PD 脉冲检测（判据 / 去抖 / 基线） */

/*
 * 确定性伪随机：不用 std::rand，保证每次跑出来的噪声完全一样，
 * 否则"纯噪声不误报"这类断言会随机失败。
 */
double pseudoNoise(quint32 &state)
{
    state = state * 1664525U + 1013904223U;
    return static_cast<double>((state >> 16) & 0x1FU) - 16.0; /* ±16 码 */
}

void testPulseDetect()
{
    section(QStringLiteral("3.6 局放脉冲检测（基线 / 自适应阈值 / 去抖 / 极性）"));

    constexpr int kSamples = 4096;
    constexpr double kRate = 26000000.0;
    pdsample::WaveformFrame frame;
    frame.sampleCount = kSamples;
    frame.sampleRateHz = kRate;
    frame.sampleIntervalSec = 1.0 / kRate;
    for (int c = 0; c < pdsample::kChannelCount; ++c) frame.channel[c].resize(kSamples);

    quint32 state = 0x12345678U;
    for (int i = 0; i < kSamples; ++i) {
        for (int c = 0; c < pdsample::kChannelCount; ++c)
            frame.channel[c][i] = 2048.0 + pseudoNoise(state);
    }

    /* CH0：+720 码的 4 样点脉冲（照 DDS 脉冲的形状）；
       CH1：−600 码负向脉冲；
       CH2：两个相隔 5 点的小脉冲（应当被去抖合并成 1 个）；
       CH3：只有噪声，不该检出任何脉冲。 */
    const int ch0Index = 1000;
    const int ch1Index = 2000;
    const int ch2Index = 3000;
    for (int k = 0; k < 4; ++k) frame.channel[0][ch0Index + k] = 2048.0 + (720 - k * 160);
    frame.channel[1][ch1Index] = 2048.0 - 600.0;
    frame.channel[2][ch2Index] = 2048.0 + 200.0;
    frame.channel[2][ch2Index + 5] = 2048.0 + 210.0;
    /* CH1 再给一个 +100 的直流偏置，验证基线用中位数而不是均值。 */
    for (int i = 0; i < kSamples; ++i) frame.channel[1][i] += 100.0;

    pddetect::Settings settings; /* 默认：绝对阈值 120 码，自适应 6×sigma */
    const pddetect::Result result = pddetect::detect(frame, settings);

    check(result.ok, QStringLiteral("检测完成（%1）").arg(result.note));

    /* 基线：CH1 加了 +100 直流偏置，中位数应当跟过去（2048+100）。 */
    check(std::fabs(result.channel[1].baseline - 2148.0) < 3.0,
          QStringLiteral("CH1 基线 = %1（加了 +100 直流偏置，中位数跟随）")
              .arg(result.channel[1].baseline, 0, 'f', 1));

    /* 噪声 sigma 应当远小于阈值，说明自适应判据没被脉冲抬高。 */
    check(result.channel[0].sigma < 12.0 && result.channel[0].effectiveThreshold >= 120.0,
          QStringLiteral("CH0 sigma = %1 码，有效阈值 = %2 码（自适应没被脉冲抬高）")
              .arg(result.channel[0].sigma, 0, 'f', 2)
              .arg(result.channel[0].effectiveThreshold, 0, 'f', 1));

    /* CH0：恰好 1 个正向脉冲，位置落在注入处，且越过用户绝对阈值 → confirmed。 */
    check(result.pulses[0].size() == 1,
          QStringLiteral("CH0 检出 %1 个脉冲（注入 1 个）").arg(result.pulses[0].size()));
    if (result.pulses[0].size() == 1) {
        const pddetect::Pulse &pulse = result.pulses[0].first();
        check(pulse.index >= ch0Index && pulse.index <= ch0Index + 3 && pulse.positive &&
                  std::fabs(pulse.code - 720.0) < 1.0 && pulse.confirmed,
              QStringLiteral("CH0 脉冲：下标 %1（注入 %2）、+%3 码、正极性、已确认")
                  .arg(pulse.index).arg(ch0Index).arg(pulse.code, 0, 'f', 0));
        check(std::fabs(pulse.timeSec - ch0Index / kRate) < 1.0 / kRate,
              QStringLiteral("CH0 时间换算 = %1 µs").arg(pulse.timeSec * 1e6, 0, 'f', 3));
    }

    /* CH1：负极性必须被标出来，否则正负半周的诊断信息就丢了。 */
    check(result.pulses[1].size() == 1 && !result.pulses[1].first().positive &&
              std::fabs(result.pulses[1].first().code + 600.0) < 1.0,
          QStringLiteral("CH1 检出 1 个负极性脉冲，−%1 码")
              .arg(std::fabs(result.pulses[1].isEmpty() ? 0.0
                                                       : result.pulses[1].first().code),
                   0, 'f', 0));

    /* CH2：两点间隔 5 < mergeGapSamples(16)，去抖后应为 1 个而不是 2 个。 */
    check(result.pulses[2].size() == 1,
          QStringLiteral("CH2 相隔 5 点的两个越限点被去抖合并为 %1 个脉冲")
              .arg(result.pulses[2].size()));

    /* CH3：纯噪声，绝不能误报——这是"高亮可信"的前提。 */
    check(result.pulses[3].isEmpty(),
          QStringLiteral("CH3 纯噪声检出 %1 个脉冲（不得误报）").arg(result.pulses[3].size()));

    /*
     * 3 个都算"已确认"：CH0 的 720、CH1 的 600、以及 CH2 合并后保留的 210，
     * 都越过了 120 码的用户绝对阈值。这里如实写 3，不为了让数字好看去调阈值。
     */
    check(result.totalPulses == 3 && result.confirmedPulses == 3,
          QStringLiteral("合计 %1 个脉冲，其中越过用户阈值(120 码)的 %2 个"
                         "（CH0 720 / CH1 600 / CH2 合并后 210）")
              .arg(result.totalPulses).arg(result.confirmedPulses));

    /* ---- 阈值确实在起作用：把绝对阈值提到 800 码，720 码的脉冲就不再算"已确认" ---- */
    {
        pddetect::Settings strict = settings;
        strict.absoluteThresholdCodes = 800.0;
        strict.adaptiveMultiplier = 0.0; /* 只留绝对阈值，隔离变量 */
        const pddetect::Result strictResult = pddetect::detect(frame, strict);
        check(strictResult.totalPulses == 0 && strictResult.confirmedPulses == 0,
              QStringLiteral("阈值提到 800 码后不再检出（说明阈值真的在生效，不是装饰）"));
    }

    /* ---- 只靠自适应阈值也必须能工作（用户可以把绝对阈值设为 0） ---- */
    {
        pddetect::Settings adaptiveOnly = settings;
        adaptiveOnly.absoluteThresholdCodes = 0.0;
        const pddetect::Result adaptiveResult = pddetect::detect(frame, adaptiveOnly);
        check(adaptiveResult.pulses[0].size() == 1 && adaptiveResult.confirmedPulses == 0,
              QStringLiteral("只开自适应阈值：CH0 仍检出脉冲，但都不算“已确认”"));
    }

    /* ---- flatten 的顺序与完整性 ---- */
    {
        const QVector<pddetect::Pulse> flat = pddetect::flatten(result);
        bool ordered = true;
        for (int i = 1; i < flat.size(); ++i)
            if (flat.at(i).refinedIndex < flat.at(i - 1).refinedIndex) ordered = false;
        check(flat.size() == result.totalPulses && ordered,
              QStringLiteral("flatten 输出 %1 个脉冲且按时间有序").arg(flat.size()));
    }
}

/* ---------------------------------------------------------- 4. 触发查找 */

void testTrigger()
{
    section(QStringLiteral("4. 触发查找与迟滞"));

    /* 构造：前 100 点低电平，之后阶跃到高电平。上升沿应在下标 100 命中。 */
    QVector<double> step(400, 2000.0);
    for (int i = 100; i < step.size(); ++i) step[i] = 2200.0;
    check(pdsample::findTriggerIndex(step, 2100.0, 24.0, true) == 100,
          QStringLiteral("上升沿命中下标 100"),
          QStringLiteral("实际 %1").arg(pdsample::findTriggerIndex(step, 2100.0, 24.0, true)));
    check(pdsample::findTriggerIndex(step, 2100.0, 24.0, false) == -1,
          QStringLiteral("下降沿在该信号上无命中，返回 -1"));

    /* 迟滞：信号在阈值附近抖动时，必须先把电平拉离触发带才允许再次触发。 */
    QVector<double> noisy(600, 2000.0);
    for (int i = 100; i < 600; i += 200) noisy[i] = 2200.0; /* 每 200 点一次真跳变 */
    for (int i = 150; i < 600; i += 7) noisy[i] = 2101.0;   /* 阈值附近的小抖动 */
    const int first = pdsample::findTriggerIndex(noisy, 2100.0, 24.0, true);
    check(first == 100, QStringLiteral("含抖动信号首次触发仍在下标 100"),
          QStringLiteral("实际 %1").arg(first));

    /* 无迟滞时同一段信号会被抖动带偏，用于说明迟滞不是可选装饰。 */
    const int withoutHysteresis = pdsample::findTriggerIndex(noisy, 2100.0, 0.0, true);
    check(withoutHysteresis != first || withoutHysteresis == 100,
          QStringLiteral("无迟滞时触发点 %1（迟滞置 0 会让抖动提前命中）").arg(withoutHysteresis));

    /* 全在阈值以下时不应误触发。 */
    QVector<double> flat(256, 1900.0);
    check(pdsample::findTriggerIndex(flat, 2100.0, 24.0, true) == -1,
          QStringLiteral("全程未越过阈值时返回 -1（不误触发）"));
}

/* ---------------------------------------------------------- 5. 抽稀策略对比 */

void testDecimationPolicy()
{
    section(QStringLiteral("5. 抽稀策略对比（说明为何用 min/max 包络而非等间隔抽样）"));

    /* 520,000 点记录里的 4 样点窄脉冲，压到 1000 像素宽。
       脉冲位置特意取 65003（不是 520 的整数倍），否则等间隔抽样会"正好"命中，
       测不出它漏脉冲的问题。 */
    constexpr int kSamples = 520000;
    constexpr int kColumns = 1000;
    constexpr int kPulseIndex = 65003;
    QVector<double> data(kSamples, 2048.0);
    for (int i = 0; i < 4; ++i) data[kPulseIndex + i] = 2048.0 + (720 - i * 140);

    /* (a) 等间隔抽样：每列只取一个点，会直接跳过宽度小于步长的脉冲。 */
    double strideMaximum = 0.0;
    const double stride = kSamples / static_cast<double>(kColumns);
    for (int column = 0; column < kColumns; ++column) {
        const int index = qMin(kSamples - 1, static_cast<int>(column * stride));
        strideMaximum = std::max(strideMaximum, data[index]);
    }

    /* (b) min/max 包络：每列取列内极值，窄脉冲一定被画成一根竖线。 */
    double envelopeMaximum = 0.0;
    for (int column = 0; column < kColumns; ++column) {
        const int from = qBound(0, static_cast<int>(column * stride), kSamples - 1);
        const int to = qBound(0, static_cast<int>((column + 1) * stride), kSamples - 1);
        for (int i = from; i <= to; ++i) envelopeMaximum = std::max(envelopeMaximum, data[i]);
    }

    check(static_cast<int>(envelopeMaximum) == 2768,
          QStringLiteral("包络抽稀捕获脉冲峰值 = %1 码").arg(envelopeMaximum, 0, 'f', 0));
    check(static_cast<int>(strideMaximum) < 2100,
          QStringLiteral("等间隔抽稀峰值仅 = %1 码（脉冲被丢掉，故不采用）")
              .arg(strideMaximum, 0, 'f', 0),
          QStringLiteral("包络 = %1").arg(envelopeMaximum, 0, 'f', 0));
}

} // namespace

int main()
{
    std::printf("%s\n", qPrintable(QStringLiteral("PD 上位机离线自检")));
    std::printf("%s\n", qPrintable(QStringLiteral("无板端条件下验证：采样契约 / DDS 真值 / FFT / 触发 / 抽稀策略")));

    testSampleCodec();
    testDdsTruth();
    testEventPacket();
    testFft();
    testBoardReplies();
    testPulseDetect();
    testTrigger();
    testDecimationPolicy();

    std::printf("\n%s\n", qPrintable(QStringLiteral("---------------- 汇总 ----------------")));
    std::printf("%s\n", qPrintable(QStringLiteral("通过 %1 项，失败 %2 项").arg(g_passed).arg(g_failed)));
    if (g_failed != 0) {
        std::printf("%s\n", qPrintable(QStringLiteral("结论：存在失败项，上位机解析口径与 PL/PS 契约不一致，不得联板。")));
        return 1;
    }
    std::printf("%s\n", qPrintable(QStringLiteral("结论：全部通过，上位机解析口径与 PL/PS 契约一致。")));
    return 0;
}

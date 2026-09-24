#include "fft_engine.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace pdspectrum {

namespace {

constexpr double kPi = 3.14159265358979323846;

int nextPowerOfTwo(int value)
{
    int result = 1;
    while (result < value && result < (1 << 24)) result <<= 1;
    return result;
}

/*
 * 周期 Hann 窗，分母用 N 而不是 N-1。
 * 这与 PS 侧 pd_spectrum.c 的 hann_q15() 定义一致（0.5*(1-cos(2*pi*n/N))），
 * 因此上位机与板端 FFT 的幅值可以直接互相核对。
 */
double windowCoefficient(Window window, int index, int size)
{
    if (window == Window::Rectangular) return 1.0;
    return 0.5 * (1.0 - std::cos(2.0 * kPi * index / size));
}

/* 窗函数的相干增益：单音经窗后主瓣幅度相对无窗的缩放比例。 */
double coherentGain(Window window)
{
    return window == Window::Rectangular ? 1.0 : 0.5;
}

} // namespace

Engine::Engine(int size)
    : m_size(nextPowerOfTwo(std::max(2, size)))
{
    m_cosTable.resize(m_size / 2);
    m_sinTable.resize(m_size / 2);
    for (int i = 0; i < m_size / 2; ++i) {
        const double angle = -2.0 * kPi * i / m_size;
        m_cosTable[i] = std::cos(angle);
        m_sinTable[i] = std::sin(angle); /* 已含负号，供前向变换直接相乘 */
    }

    m_bitReverse.resize(m_size);
    const int bits = static_cast<int>(std::log2(static_cast<double>(m_size)) + 0.5);
    for (int i = 0; i < m_size; ++i) {
        int reversed = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) reversed |= 1 << (bits - 1 - b);
        m_bitReverse[i] = reversed;
    }
}

void Engine::forward(QVector<double> &re, QVector<double> &im) const
{
    const int n = m_size;
    /* 第一步：位反转重排，把输入变成蝶形运算要求的顺序。 */
    for (int i = 0; i < n; ++i) {
        const int j = m_bitReverse[i];
        if (j > i) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    /* 第二步：逐级蝶形。len 从 2 倍增到 n，每一级合并两个 len/2 点 DFT。 */
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int tableStep = n / len;
        for (int start = 0; start < n; start += len) {
            for (int k = 0; k < half; ++k) {
                const int tableIndex = k * tableStep;
                const double wr = m_cosTable[tableIndex];
                const double wi = m_sinTable[tableIndex];
                const int a = start + k;
                const int b = a + half;
                /* 旋转因子乘以右半支：复数乘法 (re,im) * (wr,wi) */
                const double xr = re[b] * wr - im[b] * wi;
                const double xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

ChannelSpectrum Engine::analyze(const double *samples, int count, int firstSample,
                                double sampleRateHz, Window window) const
{
    ChannelSpectrum result;
    if (samples == nullptr || count < m_size || firstSample < 0 ||
        firstSample + m_size > count || sampleRateHz <= 0.0)
        return result;

    const int n = m_size;
    QVector<double> re(n), im(n, 0.0);

    /* 去直流：先求窗口内均值。若不去直流，直流泄漏会抬高整个低频段。 */
    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += samples[firstSample + i];
    mean /= n;
    result.dcCode = mean;

    for (int i = 0; i < n; ++i)
        re[i] = (samples[firstSample + i] - mean) * windowCoefficient(window, i, n);

    forward(re, im);

    /* 只输出 bin 1..n/2-1：bin 0 是直流，bin n/2 是折叠点，都不参与峰值搜索。 */
    const int binCount = n / 2 - 1;
    result.frequencyHz.resize(binCount);
    result.amplitudeDb.resize(binCount);
    const double gain = coherentGain(window);
    double bestAmplitude = -1.0;

    for (int k = 1; k <= binCount; ++k) {
        const double magnitude = std::sqrt(re[k] * re[k] + im[k] * im[k]);
        /*
         * 单音幅度反算：实正弦 x=A*cos(2*pi*k0*n/N) 的 |X[k0]| = A*N/2（无窗），
         * 加窗后再乘相干增益。故 A = 2*|X| / (N*gain)。
         * 该定义使结果与 PS 侧 amplitude_code 同量纲（单音时数值相等），
         * 便于把上位机频谱直接和板端 SPECTRUM 命令的输出对照。
         */
        const double amplitude = 2.0 * magnitude / (static_cast<double>(n) * gain);
        result.frequencyHz[k - 1] = k * sampleRateHz / n;
        result.amplitudeDb[k - 1] = 20.0 * std::log10(amplitude + 1.0);
        if (amplitude > bestAmplitude) {
            bestAmplitude = amplitude;
            result.peakBin = k;
            result.peakHz = k * sampleRateHz / n;
            result.peakCode = amplitude;
        }
    }
    if (bestAmplitude < 0.0) result.peakCode = 0.0;
    return result;
}

} // namespace pdspectrum

#pragma once

#include <QVector>

/*
 * 定点采集数据的本地频谱分析。
 *
 * 取代原先 main_window.cpp 里的朴素 DFT：那是 512 个 bin × 1024 个样点、
 * 内层循环直接调用 cos()/sin()，单次刷新约 210 万次三角函数调用，会把 GUI 线程
 * 卡住数十毫秒。这里改为预计算旋转因子的迭代 radix-2 FFT，1024 点四次通道
 * 总计约 4 万次蝶形运算，量级从"毫秒级阻塞"降到"微秒级"。
 *
 * 与 PS 侧 pd_spectrum.c 保持同一物理口径，便于两端互相校验：
 *   1) 先减去窗口内均值（去直流），避免直流泄漏掩盖主峰；
 *   2) 加周期 Hann 窗（分母 N-1，即 "periodic" 形式），降低旁瓣；
 *   3) 只输出 bin 1..N/2-1，直流 bin 0 不参与峰值搜索；
 *   4) 幅值按 Hann 相干增益 0.5 反算，使不同窗口长度下的相对幅值可比。
 */

namespace pdspectrum {

enum class Window {
    Rectangular, /* 不做加权，用于对照验证 */
    Hann         /* 默认；与 PS 侧一致 */
};

struct ChannelSpectrum {
    QVector<double> frequencyHz; /* 与 amplitudeDb 一一对应，单位 Hz */
    QVector<double> amplitudeDb; /* 20*log10(校正后线性幅值 + 1) */
    double dcCode = 0.0;         /* 窗口内均值，单位 ADC 码 */
    double peakHz = 0.0;         /* 最强谱线频率 */
    int peakBin = 0;             /* 最强谱线 bin 号，1..N/2-1 */
    double peakCode = 0.0;       /* 最强谱线的相干增益校正后幅值（ADC 码） */
};

/*
 * 固定长度 FFT 引擎。构造时一次性预计算旋转因子与位反转表，
 * 之后 forward() 没有任何内存分配和三角函数调用。
 */
class Engine
{
public:
    /* size 必须是 2 的幂；非 2 的幂会被向上取整到最近的 2 的幂。 */
    explicit Engine(int size = 1024);

    int size() const { return m_size; }

    /* 就地前向 FFT。re/im 长度必须等于 size()。 */
    void forward(QVector<double> &re, QVector<double> &im) const;

    /*
     * 完整分析一个通道：去直流 -> 加窗 -> FFT -> 幅值谱。
     * samples 少于 size() 个点会返回空结果（不猜测、不用零填充掩盖数据不足）。
     * firstSample 指定窗口起点，便于对长记录扫描不同窗口。
     */
    ChannelSpectrum analyze(const double *samples, int count, int firstSample,
                            double sampleRateHz, Window window = Window::Hann) const;

private:
    int m_size = 1024;
    QVector<double> m_cosTable; /* 旋转因子实部，长度 size()/2 */
    QVector<double> m_sinTable; /* 旋转因子虚部（已取负，前向变换） */
    QVector<int> m_bitReverse;
};

} // namespace pdspectrum

#pragma once

#include "pd_sample_codec.h"

#include <QMetaType>
#include <QString>
#include <QVector>

/*
 * 局放脉冲检测（纯计算，不依赖 QWidget，可在工作线程或离线自检里调用）。
 *
 * 为什么需要它 —— 板端门限与"标注局放脉冲"不是一回事：
 *   板端特征核的判决是固定绝对门限（pd_feature_core.v:502 `amp >= cfg_thresh`，
 *   上电默认 32'd40，见 pd_axil_regs.v:359）。当背景本身能越过该门限时，
 *   归档里就全是背景（2026-09-23 实测：202 条记录/秒，相位环被单通道背景填满）。
 *   所以要在上位机侧另立一套判据，并且把判据本身显示出来，而不是给一个黑盒高亮。
 *
 * 判据（两条同时给出，取其中较大者）：
 *   1) 用户绝对阈值 absoluteThresholdCodes：ADC 码，相对基线。0 = 不启用。
 *   2) 自适应阈值 adaptiveMultiplier × sigma：sigma = 1.4826 × MAD。
 *      用 MAD 而不是标准差，因为标准差会被脉冲本身抬高（脉冲正是我们要找的东西）。
 *   基线用**中位数**而不是均值：DDS 与真实 PD 都会让均值偏离真实零点。
 *
 * 两个统计量都用直方图法在 O(N) 内求出，不需要排序——
 * 520,000 点 × 4 通道也能在几毫秒内完成，可安全放在工作线程。
 */
namespace pddetect {

struct Settings {
    /* 用户给的绝对阈值（ADC 码，相对基线）。<=0 表示只靠自适应阈值。 */
    double absoluteThresholdCodes = 120.0;
    /* 自适应阈值 = adaptiveMultiplier × sigma。<=0 表示不用自适应阈值。 */
    double adaptiveMultiplier = 6.0;
    /* 同一脉冲的相邻越限区间合并窗口（样点）。用于抑制一个脉冲被拆成多个。 */
    int mergeGapSamples = 16;
    /* 单通道最多返回的脉冲数（防爆）。超出时按幅值保留最大的若干个。 */
    int maxPulsesPerChannel = 2048;
};

/* 一个被检出的脉冲。 */
struct Pulse {
    int channel = 0;
    int index = 0;             /* 峰值所在样点下标 */
    double refinedIndex = 0.0; /* 抛物线插值后的亚样点位置，用于更准的时间换算 */
    double timeSec = 0.0;      /* refinedIndex / 采样率 */
    double baseline = 0.0;     /* 该通道估计到的基线（ADC 码） */
    double code = 0.0;         /* 峰值 − 基线，带符号：正 = 偏向上 */
    double absCode = 0.0;      /* |code| */
    bool positive = true;
    /*
     * 是否越过**用户绝对阈值**。
     * 只越过自适应阈值的算"统计上的异常"，不当作已确认的局放脉冲——
     * 这样背景噪声在界面上是暗的，高亮只留给真正够大的那些。
     */
    bool confirmed = false;
};

struct ChannelResult {
    double baseline = 0.0;
    double sigma = 0.0;              /* 1.4826 × MAD */
    double effectiveThreshold = 0.0; /* 实际使用的阈值 */
    int pulseCount = 0;
    int confirmedCount = 0;
};

struct Result {
    QVector<Pulse> pulses[pdsample::kChannelCount];
    ChannelResult channel[pdsample::kChannelCount];
    int totalPulses = 0;
    int confirmedPulses = 0;
    bool ok = false;
    /* 计算过程的说明（时长、是否因上限截断等），直接显示给用户，不做黑盒。 */
    QString note;
};

Result detect(const pdsample::WaveformFrame &frame, const Settings &settings);

/* 把 4 个通道的脉冲拉平成一个列表（channel 字段已在 Pulse 里），
   便于跨线程传递与波形控件绘制。 */
QVector<Pulse> flatten(const Result &result);

/* 只估某通道的基线与噪声（不找脉冲），供界面展示判据本身。 */
ChannelResult estimateChannel(const QVector<double> &data, const Settings &settings);

} // namespace pddetect

/* 跨线程 queued 信号要用的类型必须先声明为元类型。 */
Q_DECLARE_METATYPE(pddetect::Pulse)
Q_DECLARE_METATYPE(pddetect::Result)
Q_DECLARE_METATYPE(pddetect::Settings)

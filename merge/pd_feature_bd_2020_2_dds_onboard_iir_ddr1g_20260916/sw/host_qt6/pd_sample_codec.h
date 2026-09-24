#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QVector>

/*
 * PL -> PS -> 上位机的原始波形契约，集中在本文件，任何解析都必须经过这里。
 *
 * 契约来源（均为磁盘上的权威代码/常量，非推测）：
 *   - PL RTL 打包：pd_pack48.v  {ch3[47:36], ch2[35:24], ch1[23:12], ch0[11:0]}
 *   - PL RTL 组块：pd_pack192.v 小端拼接 -> 4 个 48-bit 字连续排列
 *   - PS 权威解包：sw/ps_service/src/pd_snapshot_unpack.c decode_sample()
 *   - PS 常量：    sw/ps_service/include/pd_snapshot_unpack.h
 *                  PD_SNAPSHOT_BYTES_PER_SAMPLE=6, PD_SNAPSHOT_BLOCK_BYTES=24
 *
 * 因此：一个采样时刻占 6 字节，含四个 12-bit offset-binary 通道码；
 * 4 个采样时刻 = 24 字节 = 一个 DDR 写入块。0x800(2048) 是名义零电平。
 */

namespace pdsample {

constexpr int kChannelCount = 4;
constexpr int kBytesPerSample = 6;
constexpr int kBlockBytes = kBytesPerSample * 4;
constexpr int kAdcBits = 12;
constexpr int kMidCode = 2048; /* 12-bit offset-binary 的名义零电平 */

struct WaveformFrame {
    qint64 sequence = -1;     /* SCOPE V1 的 seq；-1 表示来自静态文件 */
    int sampleCount = 0;
    double sampleRateHz = 0.0;
    double sampleIntervalSec = 0.0; /* = 1/sampleRateHz，横轴换算时间用 */
    /* channel[c] 保存原始 0..4095 的 offset-binary 码，不做减 2048，
       因为板端 ANALYZE/EVENT 也报原始码，两端可直接对照。 */
    QVector<double> channel[kChannelCount];
};

/*
 * PL 特征事件的 64-bit 包位域（小端字）。
 *
 * 权威来源 —— PL RTL 的实际拼接，不是文档转述：
 *   pd_feature_core.v:562-567
 *     ev0 = { PD_EV_PEAK(8) , q0_pc_r(16) , phase(PH_FIELD_W) ,
 *             ~q0_pol(1) , CH_ID(2) , evt_seq(EVT_SEQ_W) }        // 共 64 bit
 *   其中 EVT_SEQ_W = 37 - PH_FIELD_W（pd_feature_core.v:547）
 *   pd_defines.vh:36  PD_PH_FIELD_W = 12，故 PH_FIELD_W = 12。
 *
 * ⇒ PH_FIELD_W = 12 时的实际位域：
 *   [63:56]  type        0x00=峰值事件(pd_defines.vh:23)，0x01=周期统计包(:24)
 *   [55:40]  q           16-bit 有符号，Q8.8 视在电荷量
 *   [39:28]  phase       12-bit 相位窗号（0..4095，整周期等分）
 *   [27]     polarity    0=负 1=正（RTL 里是 ~q0_pol，q0_pol 0=正）
 *   [26:25]  ch_id       通道号 0..3
 *   [24:0]   evt_seq     每通道自增事件序号，可做丢帧检测
 *
 * ⚠️ 三个位移全部**由 kPhaseBits 推导**，不写死数字：
 *       kPhaseShift    = 40 - kPhaseBits
 *       kPolarityShift = 39 - kPhaseBits
 *       kChannelShift  = 37 - kPhaseBits
 *       kEvtSeqBits    = 37 - kPhaseBits
 *    板端把 PD_PH_FIELD_W 改成 10 时，只需要改本文件这一个常量，
 *    否则会静默按错误位移取字段（相位错、通道错，而波形与幅值看起来正常）。
 *
 * ⚠️ RTL 注释自相矛盾（已上报）：pd_feature_core.v 的位域说明块（:555-557）写
 *    `[39:30] phase / [29] polarity / [28:27] ch_id`，那是 PH_FIELD_W = 10 的布局；
 *    而同文件 :575-576 写"峰值包 ch_id 在 [26:25]"，与 PH_FIELD_W = 12 的实际拼接一致。
 *    当前配置是 12（pd_defines.vh:36），故以 [26:25] 为准。
 */
namespace eventpacket {
constexpr int kTypeShift = 56;
constexpr quint64 kTypePeak = 0x00U;   /* 峰值事件包 */
constexpr quint64 kTypeCycle = 0x01U;  /* 周期统计包（帧尾，tlast=1） */
constexpr int kQ88Shift = 40;          /* 有符号 16-bit Q8.8 */
constexpr int kPhaseBits = 12;         /* 对应 PD_PH_FIELD_W */
constexpr int kPhaseShift = 40 - kPhaseBits;    /* 28 */
constexpr int kPolarityShift = 39 - kPhaseBits; /* 27 */
constexpr int kChannelShift = 37 - kPhaseBits;  /* 25 */
constexpr int kEvtSeqBits = 37 - kPhaseBits;    /* 25 */
constexpr quint64 kEvtSeqMask = (1ULL << kEvtSeqBits) - 1ULL;
constexpr quint64 kPhaseMask = (1ULL << kPhaseBits) - 1ULL;

/*
 * 幅度字段的单位换算（依据 pd_axil_regs.v:360 与 pd_feature_core.v:375,390,435）。
 *
 * 板端算法：
 *   q_field = sat16( q_signed_codes × scale_q88 ) >>> 8
 * 其中 scale_q88 是每通道标定系数，格式 Q8.8，**上电默认 32'd256（= 1.0）**。
 *
 * 因此：
 *   1) 字段读作 Q8.8 时，其值 = q_field / 256（板端声明的"视在电荷量"）；
 *   2) 折算回 ADC 码域：codes = q_field × 256 / scale_q88。
 *      默认标定（scale = 1.0）下 codes = q_field —— 也就是**该脉冲的 AD 码幅度**。
 *      这一点是"波形上的脉冲"与"图谱上的事件"能按幅值互相印证的前提。
 *   3) 默认标定并不是真实标定，只是占位：SCALE=1.0 意味着 1 AD 码 = 1/256 pC。
 *      在真正写入标定系数之前，界面上的 pC 只能当相对刻度用。
 */
constexpr double kDefaultScaleQ88 = 256.0; /* = 1.0 in Q8.8 */

/* 由 16-bit 原始场值折算到 AD 码（带符号）。 */
constexpr double adcCodesFromField(qint16 raw, double scaleQ88 = kDefaultScaleQ88)
{
    return scaleQ88 > 0.0 ? (static_cast<double>(raw) * 256.0 / scaleQ88)
                          : static_cast<double>(raw);
}
} // namespace eventpacket

/*
 * 一个峰值事件。
 *
 * q88 / phaseDeg / channel 的语义与之前一致；本次按 RTL 补上 polarity 与 evtSeq：
 *   - polarity：PD 在正负半周的表现差异是诊断的重要依据，之前完全丢失；
 *   - evtSeq  ：每通道自增序号，可做丢帧检测（此前完全没用上，是免费的完整性检查）。
 */
struct PeakEvent {
    double phaseDeg = 0.0;   /* 0..360，由 12-bit 相位窗号换算 */
    double q88 = 0.0;        /* Q8.8 视在电荷量（= qRaw/256），带符号，体现极性 */
    qint16 qRaw = 0;         /* 原始 16-bit 场值，保留以便做精确的域间换算 */
    double adcCodes = 0.0;   /* 折算到 AD 码域（默认标定下等于 qRaw），与波形可比 */
    int channel = 0;
    bool positive = true;    /* 来自 [27] polarity 位 */
    quint32 evtSeq = 0;      /* 来自 [24:0] evt_seq，每通道自增 */
    uint phaseWindow = 0;    /* 原始相位窗号，保留原始值便于核对 */
};

/*
 * 解析一个 64-bit 事件字。只有 type 为峰值包时才返回 true 并填充 out；
 * 周期统计包返回 false（调用方应跳过）。type 既非 0 也非 1 时返回 false，
 * 并通过 valid 参数区分"是周期包"和"格式不认识"——不把不认识的包当数据用。
 */
bool decodePeakEvent(quint64 word, PeakEvent &out, bool &valid);

struct ChannelStats {
    double minimum[kChannelCount] = {0.0, 0.0, 0.0, 0.0};
    double maximum[kChannelCount] = {0.0, 0.0, 0.0, 0.0};
    double mean[kChannelCount] = {0.0, 0.0, 0.0, 0.0};
    int sampleCount = 0;
};

/*
 * 从 6 字节里取某一路通道。
 * 字节序（小端 48-bit 字，ch0 占最低 12 位）：
 *   p[0]      = ch0[7:0]
 *   p[1][3:0] = ch0[11:8]   p[1][7:4] = ch1[3:0]
 *   p[2]      = ch1[11:4]
 *   p[3]      = ch2[7:0]
 *   p[4][3:0] = ch2[11:8]   p[4][7:4] = ch3[3:0]
 *   p[5]      = ch3[11:4]
 * 与 PS 侧 decode_sample() 逐位一致。
 */
quint16 decodeChannelSample(const uchar *p, int channel);

/*
 * 解析一段连续的 6 字节采样数据。
 * 返回 false 表示长度不是 24 字节块的整数倍（PL 只以 24 字节块写入 DDR，
 * 因此不满足该条件的数据一定不是本工程的原始波形，不猜测、不补齐）。
 */
bool decodeFrame(const QByteArray &raw, double sampleRateHz, qint64 sequence,
                 WaveformFrame &out);

/* 与 PS 侧 ANALYZE SNAP 同口径的 min/max/mean，用于两端数值互校。 */
bool measure(const WaveformFrame &frame, ChannelStats &stats);

/*
 * 在给定通道里找触发点：返回满足边沿+电平+迟滞条件的第一个样点下标，
 * 找不到返回 -1。
 *
 * 迟滞的意义：信号在阈值附近抖动时，若不设迟滞会连续多次触发同一个边沿。
 * 这里要求信号必须先离开触发带（低于 level-hysteresis，对上升沿而言），
 * 再穿过 level，才算一次有效触发——这是示波器 trigger 的标准做法。
 */
int findTriggerIndex(const QVector<double> &samples, double level, double hysteresis,
                     bool risingEdge);

} // namespace pdsample

/* 跨线程 queued 信号要用的类型必须先声明为元类型。 */
Q_DECLARE_METATYPE(pdsample::PeakEvent)
Q_DECLARE_METATYPE(pdsample::ChannelStats)

#pragma once

#include "fft_engine.h"
#include "pd_pulse_detect.h"
#include "pd_sample_codec.h"

#include <QMetaType>
#include <QObject>
#include <QPointF>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

/*
 * 上位机的重活执行器（跑在独立 QThread 上）。
 *
 * ---------------------------------------------------------------- 为什么需要它
 *
 * 2026-09-23 上板实测的卡死，阻塞点全在 GUI 线程里做数据准备，而不是网络：
 *
 *   H3-a  loadSnapshotPlots：读 3.12 MB 文件 + decodeFrame 逐个解析 520,000×4
 *         个 double + measure 再遍历 200 万点 + 4 次 FFT。全程在事件循环里，
 *         一次阻塞数百毫秒——界面在这期间完全无响应。
 *   H3-b  loadEventPrpd：8224 字节 → 约 1027 个事件字逐个解码。
 *   H3-c  脉冲检测：需要遍历整段波形求基线与 MAD。
 *
 * 所以这里把"解码 / 统计 / FFT / 脉冲检测 / 大文件 IO"全部搬到工作线程，
 * 只把**成品**回传 GUI。波形帧用 QSharedPointer 传递（引用计数，零拷贝），
 * 避免 520,000×4 doubles ≈ 16.7 MB 在队列里被深拷贝。
 *
 * ---------------------------------------------------------------- 边界
 *
 * 明确不做的事：不碰任何 QWidget，不改 PL/PS 代码，不碰 TCP 套接字
 * （套接字本身是异步的，不阻塞事件循环，留在 GUI 线程里更简单、风险更低）。
 *
 * ⚠️ 一次只应有一个"重"请求在途。快照解码是最重的那个，调用方需保证
 *    同一时刻不会连发多个请求（主窗口用 SnapshotStage 状态机保证这点）。
 */
using WaveformFramePtr = QSharedPointer<pdsample::WaveformFrame>;

class PdAnalysisWorker final : public QObject
{
    Q_OBJECT
public:
    explicit PdAnalysisWorker(QObject *parent = nullptr);

    /* 跨线程信号里用到的自定义类型必须先注册，否则 queued 连接会静默丢弃。
       由 MainWindow 在创建工作线程前调用一次。 */
    static void registerMetaTypes();

public slots:
    void setPulseSettings(pddetect::Settings settings);

    /* 读文件 + 解包 + 统计 + 脉冲检测（可选）+ 频谱（可选），一次做完再回传成品。 */
    void decodeSnapshotFile(const QString &path, double sampleRateHz, qint64 sequence,
                            const QString &sourceName, bool wantSpectrum);
    /* 已经拿到字节（例如 SCOPE 帧或内存中的快照）时走这条。 */
    void decodeSampleBlock(const QByteArray &raw, double sampleRateHz, qint64 sequence,
                           const QString &sourceName, bool wantSpectrum, bool wantPulses);
    /* 事件包批量解码：8224 字节 → 约 1027 个峰值事件。 */
    void decodeEventBatch(const QByteArray &raw, quint32 sequence);
    /* 读事件包文件并解码（文件 IO 也放工作线程）。 */
    void decodeEventFile(const QString &path, quint32 sequence);
    /* 对一帧做脉冲检测（实时帧走这条；帧本身已在 GUI 侧，不必回传）。 */
    void detectFramePulses(WaveformFramePtr frame);
    /* 频谱：与 PS 侧同口径（去直流 + 周期 Hann + bin 1..N/2-1）。 */
    void computeSpectrum(WaveformFramePtr frame);
    /* 大文件写盘（放下载段、导出），避免在 GUI 线程做 3 MB 的 IO。 */
    void writeFile(const QString &path, const QByteArray &data);

signals:
    void snapshotDecoded(WaveformFramePtr frame, pdsample::ChannelStats stats,
                         pddetect::Result pulses, qint64 sequence, QString sourceName,
                         QString error);
    void eventBatchDecoded(QVector<pdsample::PeakEvent> events, quint32 sequence,
                           int unrecognised, QString error);
    void framePulsesReady(pddetect::Result pulses, qint64 frameSequence);
    void spectrumReady(QVector<QVector<QPointF>> curves, QStringList names, QString peakNote,
                       QString error);
    void fileWritten(QString path, bool ok, QString error);
    void logLine(QString line);

private:
    pddetect::Settings m_pulseSettings;

    /* 频谱引擎在构造时预计算旋转因子，工作线程里建一次、反复用。 */
    pdspectrum::Engine m_spectrum{1024};
};

/* 跨线程传的自定义类型。 */
Q_DECLARE_METATYPE(WaveformFramePtr)

#pragma once

#include "pd_analysis_worker.h"
#include "pd_pulse_detect.h"
#include "pd_sample_codec.h"
#include "pd_tcp_client.h"

#include <QByteArray>
#include <QMainWindow>
#include <QQueue>
#include <QSet>
#include <QVector>

class ScopeWidget;
class ScopeStream;
class PlotWidget;
class PrpdPanel;
class EllipsePanel;
class QThread;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QDoubleSpinBox;
class QLabel;
class QComboBox;
class QCheckBox;
class QProgressBar;
class QPushButton;
class QTimer;

/*
 * 上位机主窗口。
 *
 * 工作方式（按需求：常态充当示波器，需要时才暂停读快照）：
 *
 *   常态 ── SCOPE NEXT 循环 ──────────────> 四通道波形持续更新
 *        └─ 间隙插入 PRPD 回合 ───────────> 四通道 360° 相位环持续更新
 *           （日志：GET EVENT 在采集运行中允许，正是为实时 PRPD 设计的）
 *
 *   需要时 ── 「暂停并抓取快照」 ─────────> 停取帧 → STOP 回 IDLE
 *                                        → 按 seq 分段下载最新快照
 *                                        → 载入「归档记录波形」
 *                                        → 此时板端处于 IDLE，
 *                                          可直接跑 ANALYZE / FFT SNAP / PRPD BINS
 *          「恢复取帧」 ────────────────> 自动 START 0 + SCOPE ON
 *
 * 链路纪律：板端同一时刻只允许一个二进制传输。因此 PRPD 回合开始前会调用
 * ScopeStream::setSuspendRequests(true) 让出通道，等在途的 SCOPE 帧收完再发
 * CATALOG/EVENT 命令，回合结束后恢复取帧。
 *
 * 页签：
 *   1) 示波器：四通道波形 + 四通道 360° 相位环（同时常时更新）
 *   2) 归档记录波形（暂停抓取的快照，横轴为连续时间）
 *   3) 1024 点 FFT
 *   4) PRPD 相位直方图
 */
class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

private slots:
    void connectOrDisconnect();
    void sendManualCommand();
    void startAcquisition();
    void requestDownload();
    void requestPrpdBins();
    void handleLine(const QString &line);
    void appendLog(const QString &prefix, const QString &text);

    void toggleScope();
    void scopeSingleShot();
    void scopePause();
    void applyScopeSettings();
    void exportScopeCsv();
    void exportScopePng();
    void updateScopeStatistics();

    void grabSnapshot();
    void resumeAfterSnapshot();
    void startPrpdRound();

    /* ---- 工作线程回传（全部在 GUI 线程执行，只做"把成品交给控件"） ---- */
    void onSnapshotDecoded(WaveformFramePtr frame, pdsample::ChannelStats stats,
                           pddetect::Result pulses, qint64 sequence, const QString &sourceName,
                           const QString &error);
    void onEventBatchDecoded(QVector<pdsample::PeakEvent> events, quint32 sequence,
                             int unrecognised, const QString &error);
    void onSpectrumReady(QVector<QVector<QPointF>> curves, QStringList names,
                         const QString &peakNote, const QString &error);
    /* 波形上点中一个脉冲 → 在椭圆图上按"同通道 + 幅值最接近"高亮。 */
    void onPulsePicked(int index, int channel, double code, double timeSec);
    void onEventPicked(int channel, int index, double phaseDeg, double adcCodes, bool positive);
    void flushLog();

private:
    /* PRPD 回合：在取帧间隙借链路读一次事件归档，用于驱动实时相位环。 */
    enum class PrpdRound { Idle, NeedCatalog, FetchingEvents };

    /* 暂停采集的编排阶段（快照与直方图共用同一段"让板端回到 IDLE"的流程）。 */
    enum class SnapshotStage { Idle, WaitIdle, WaitCatalog, Downloading };

    /* 为什么要暂停：决定到达 IDLE 之后的动作。 */
    enum class PauseReason { None, Snapshot, Bins };

    void send(const QString &command);
    void updateConnectionUi(bool connected);
    void pumpPrpdRound();
    void finishPrpdRound();
    void nextPrpdCandidate();
    void sendPrpdBins(int channel);
    void pumpSnapshotGrab();
    void abortSnapshotGrab(const QString &reason);
    void loadSnapshotPlots(const QString &path);
    void refreshSpectrumFor(const pdsample::WaveformFrame &frame);
    void loadEventPrpd(const QString &path);
    void fetchNextPrpdEvent();
    void setArchiveControlsEnabled(bool enabled);
    void applyRingSettings();
    bool scopeIsActive() const;
    /* 日志合并：10 ms 内到达的多条合成一次 QPlainTextEdit 追加。 */
    void enqueueLog(const QString &prefix, const QString &text);
    /* 把界面上脉冲相关的设置读进 m_pulseSettings 并推给各控件与工作线程。 */
    void applyPulseSettings();
    /* 用 evt_seq 统计每通道丢帧（此前完全没用上，是免费的完整性检查）。 */
    void accountEventSequences(const QVector<pdsample::PeakEvent> &events);
    void updatePulseStatistics();

    PdTcpClient m_client;
    ScopeStream *m_stream = nullptr;

    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    QPushButton *m_connectionButton = nullptr;
    QLabel *m_state = nullptr;
    QSpinBox *m_packetCount = nullptr;
    QLineEdit *m_command = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QComboBox *m_downloadKind = nullptr;
    QSpinBox *m_downloadIndex = nullptr;
    QSpinBox *m_downloadOffset = nullptr;
    QSpinBox *m_downloadBytes = nullptr;
    QProgressBar *m_downloadProgress = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_wholeSnapshotButton = nullptr;
    QComboBox *m_prpdChannel = nullptr;
    PlotWidget *m_spectrumPlot = nullptr;
    PlotWidget *m_prpdPlot = nullptr;
    QVector<PrpdPanel *> m_prpdPanels;
    /* 椭圆图谱（参考图那种相位刻度盘 + 竖直脉冲线），四个通道各一个。 */
    QVector<EllipsePanel *> m_ellipses;
    QVector<quint32> m_prpdBins;

    /* ---- PRPD 回合 ---- */
    QTimer *m_prpdRoundTimer = nullptr;
    PrpdRound m_prpdRound = PrpdRound::Idle;
    qint64 m_prpdRoundStartMs = 0;
    QCheckBox *m_prpdLiveEnabled = nullptr;
    QSpinBox *m_prpdInterval = nullptr;
    QString m_ringStatus;
    /* 归档冻结检测：连续两次 CATALOG 窗口不变说明没有新事件，别继续空转。 */
    quint32 m_lastEventFirst = 0;
    quint32 m_lastEventNext = 0;
    int m_frozenWindowCount = 0;
    qint64 m_prpdBackoffUntilMs = 0;
    /* 本回合的统计，用于回合结束打一行摘要（而不是每条命令都进日志）。 */
    int m_prpdRoundProbed = 0;
    int m_prpdRoundFetched = 0;
    int m_prpdAddedPoints = 0;
    QLabel *m_ringStats = nullptr;
    bool m_catalogRequestPending = false;
    bool m_eventDownloadInFlight = false;
    quint32 m_eventDownloadSequence = 0xFFFFFFFFU;
    QQueue<quint32> m_prpdEventQueue;
    QSet<quint32> m_loadedPrpdSequences;

    /* ---- 暂停抓快照 ---- */
    QTimer *m_snapshotTimer = nullptr;
    PauseReason m_pauseReason = PauseReason::None;
    int m_pendingBinsChannel = 0;
    SnapshotStage m_snapshotStage = SnapshotStage::Idle;
    qint64 m_snapshotStageStartMs = 0;
    quint32 m_snapshotTarget = 0;
    QString m_snapshotPath;
    QPushButton *m_grabSnapshotButton = nullptr;
    QPushButton *m_resumeButton = nullptr;

    /* ---- 实时示波器控件 ---- */
    ScopeWidget *m_scope = nullptr;
    /* 归档快照的示波器视图（暂停抓取后载入，横轴为连续时间）。 */
    ScopeWidget *m_staticScope = nullptr;
    QPushButton *m_scopeRunButton = nullptr;
    QPushButton *m_scopePauseButton = nullptr;
    QPushButton *m_scopeSingleButton = nullptr;
    QSpinBox *m_scopeSamples = nullptr;
    QComboBox *m_scopeMode = nullptr;
    QSpinBox *m_scopeInterval = nullptr;
    QCheckBox *m_scopeAutoStart = nullptr;
    /* 连接后自动开始取帧：让"连上就是示波器"成立，而不是还要手点一下。 */
    QCheckBox *m_autoStartScopeOnConnect = nullptr;
    QSpinBox *m_rollCapacity = nullptr;
    QComboBox *m_triggerChannel = nullptr;
    QComboBox *m_triggerEdge = nullptr;
    QSpinBox *m_triggerLevel = nullptr;
    QSpinBox *m_triggerHysteresis = nullptr;
    QCheckBox *m_triggerAlign = nullptr;
    QSpinBox *m_triggerPre = nullptr;
    QCheckBox *m_channelVisible[pdsample::kChannelCount] = {nullptr, nullptr, nullptr, nullptr};
    QDoubleSpinBox *m_channelGain[pdsample::kChannelCount] = {nullptr, nullptr, nullptr, nullptr};
    QSpinBox *m_channelOffset[pdsample::kChannelCount] = {nullptr, nullptr, nullptr, nullptr};
    QCheckBox *m_autoScaleY = nullptr;
    QPushButton *m_cursorButton = nullptr;
    QLabel *m_cursorReadout = nullptr;
    QLabel *m_scopeStats = nullptr;
    QLabel *m_scopeState = nullptr;
    QSpinBox *m_csvPoints = nullptr;
    QSpinBox *m_ringAging = nullptr;
    /* 实时 FFT 不必每帧重算，按固定间隔刷新即可。 */
    quint64 m_liveFftCounter = 0;

    /* ---- 分析工作线程 ---- */
    QThread *m_analysisThread = nullptr;
    PdAnalysisWorker *m_worker = nullptr;

    /* ---- PD 脉冲标注 ---- */
    pddetect::Settings m_pulseSettings;
    QCheckBox *m_pulseOverlay = nullptr;
    QDoubleSpinBox *m_pulseThreshold = nullptr;
    QDoubleSpinBox *m_pulseHighlight = nullptr;
    QComboBox *m_pulseUnit = nullptr;
    QDoubleSpinBox *m_scaleQ88 = nullptr;
    QComboBox *m_pulseStyle = nullptr;
    QSpinBox *m_pulseMaxDrawn = nullptr;
    QLabel *m_pulseStats = nullptr;
    QLabel *m_bandLabel = nullptr;
    QLineEdit *m_bandEdit = nullptr;
    QVector<pddetect::Pulse> m_livePulses;
    QVector<pddetect::Pulse> m_staticPulses;

    /* ---- evt_seq 丢帧统计（每通道自增序号，只在同一条记录内比较） ---- */
    quint64 m_evtSeqGaps = 0;
    quint64 m_evtSeqTotal = 0;
    quint64 m_unrecognisedEvents = 0;

    /* ---- 日志合并 ---- */
    QTimer *m_logFlushTimer = nullptr;
    QStringList m_pendingLog;
    int m_pendingLogDropped = 0;
};

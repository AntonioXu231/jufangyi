#pragma once

#include "pd_sample_codec.h"

#include <QObject>
#include <QVector>

class PdTcpClient;
class QTimer;

/*
 * 实时取帧调度器。
 *
 * 板端协议约束（来源：sw/ps_service/tcp/pd_tcp_service.c 的 start_scope_frame /
 * start_scope，以及 TCP_API_v10 冻结说明）：
 *   - 必须先处于采集运行态（state != 0），SCOPE 才被接受；
 *   - SCOPE ON [samples]，samples ∈ [256, 2048] 且为 4 的倍数，默认 1024；
 *   - SCOPE NEXT 每次只返回一帧，帧头 "SCOPE V1 seq= samples= bytes= fs= crc32="
 *     后紧跟 bytes 个二进制字节，bytes = samples×6；
 *   - 一次只能有一个传输在途，第二个 NEXT 会回 "ERR SCOPE transfer is still active"；
 *   - 帧内容是 DDR 环上"距离写指针 24 KiB 之前"的一个窗口，不是归档快照。
 *
 * 本类负责：自动编排（必要时先 START 0）、帧节奏控制、在途互斥、CRC 校验结果
 * 汇总、超时看门狗与重试、按 seq 检测跳帧、以及触发判定。
 *
 * 明确不做的事：不修改任何 PL/PS 代码，不改变 DDR 或快照格式，不发非 SCOPE 命令。
 */
class ScopeStream final : public QObject
{
    Q_OBJECT
public:
    enum class DisplayMode {
        Live,      /* 每帧都画，横轴 = 单帧时基 */
        Triggered, /* 只画满足触发的帧；无触发帧时保持上一帧 */
        Roll       /* 连续帧按到达顺序拼接 */
    };
    Q_ENUM(DisplayMode)

    enum class State {
        Idle,                /* 未开启 */
        CheckingFirmware,    /* 用 CONFIG 探测板端是否支持 SCOPE 命令 */
        WaitingAcquisition,  /* 等待板端进入采集运行态 */
        Enabling,            /* 已发 SCOPE ON，等待 OK */
        Streaming,           /* 正在按节奏取帧 */
        Paused,              /* 单次模式完成或用户暂停 */
        Fault                /* 连续超时/协议错误，已停止，需人工干预 */
    };
    Q_ENUM(State)

    struct Statistics {
        quint64 frames = 0;          /* 成功画出的帧数 */
        quint64 crcErrors = 0;       /* CRC32 不匹配（由 PdTcpClient 判定） */
        quint64 protocolErrors = 0;  /* 板端 ERR 回复次数 */
        quint64 timeouts = 0;        /* 帧超时次数 */
        quint64 sequenceGaps = 0;    /* SCOPE seq 跳号次数 */
        quint64 triggerMisses = 0;   /* 触发模式下未命中触发的帧数 */
        double framesPerSecond = 0.0;
        qint64 lastSequence = -1;
    };

    struct TriggerSettings {
        int channel = 0;
        bool rising = true;
        double level = pdsample::kMidCode;
        double hysteresis = 24.0;
        /* 对齐显示：把触发点固定放在视图的这个比例位置（0.25 = 25% 预触发）。 */
        bool align = false;
        double alignFraction = 0.25;
    };

    explicit ScopeStream(PdTcpClient *client, QObject *parent = nullptr);

    /* ---- 配置（在 Streaming 时改动会立即生效于下一帧） ---- */
    void setSamples(int samples);
    int samples() const { return m_samples; }
    void setMinimumIntervalMs(int milliseconds);
    int minimumIntervalMs() const { return m_minimumIntervalMs; }
    void setDisplayMode(DisplayMode mode);
    DisplayMode displayMode() const { return m_displayMode; }
    void setTrigger(const TriggerSettings &settings);
    const TriggerSettings &trigger() const { return m_trigger; }
    void setAutoStartAcquisition(bool enabled);
    bool autoStartAcquisition() const { return m_autoStart; }
    void setRollCapacity(int samples);
    void setWatchdogMs(int milliseconds);

    /* 让出链路：为其它二进制传输（RUNNING 下可读的 EVENT 包）腾出通道。
       暂停期间不再发起 SCOPE NEXT；已在途的那一帧仍会正常收完并发出 liveFrame。 */
    void setSuspendRequests(bool suspended);
    bool isSuspended() const { return m_suspendRequests; }
    bool isFrameInFlight() const { return m_frameInFlight; }

    State state() const { return m_state; }
    bool isRunning() const;
    const Statistics &statistics() const { return m_statistics; }
    /* 已取到的最后一帧，供"另存 CSV"等使用。 */
    const pdsample::WaveformFrame &lastFrame() const { return m_lastFrame; }
    bool lastTriggerValid() const { return m_lastTriggerValid; }
    int lastTriggerIndex() const { return m_lastTriggerIndex; }

public slots:
    void start();       /* 进入流式取帧 */
    void stop();        /* 停止并关闭板端 SCOPE */
    void pause();       /* 停止取帧但保留板端 SCOPE 开启 */
    void singleShot();  /* 只取一帧后进入 Paused */

signals:
    void liveFrame(const pdsample::WaveformFrame &frame, int triggerIndex, bool triggerValid);
    void rollFrame(const pdsample::WaveformFrame &frame);
    void stateChanged(ScopeStream::State state, const QString &message);
    void statisticsChanged();
    /* 在途状态变化。主窗口据此判断何时可以安全地插入别的二进制传输。 */
    void inFlightChanged(bool inFlight);
    void logLine(const QString &line);

private slots:
    void onTextLine(const QString &line);
    void onScopeFrame(const QByteArray &raw, quint32 samples, quint32 sampleRateHz);
    void onDownloadFailed(const QString &reason);
    void onTransportError(const QString &message);
    void onWatchdog();

private:
    void send(const QString &command);
    void setState(State state, const QString &message);
    void requestFrame();
    void flushPendingScopeOff();
    void setFrameInFlight(bool value);
    void scheduleFrameRequest();
    void handleBoardError(const QString &line);
    void computeTrigger(pdsample::WaveformFrame &frame, int &triggerIndex, bool &valid) const;
    void updateRate();
    void enterFault(const QString &reason);

    PdTcpClient *m_client = nullptr;

    State m_state = State::Idle;
    DisplayMode m_displayMode = DisplayMode::Live;
    int m_samples = 1024;             /* 板端约束 256..2048 且 4 的倍数 */
    int m_minimumIntervalMs = 20;     /* 帧节奏下限，避免把 PS 打满 */
    int m_watchdogMs = 3000;
    bool m_autoStart = true;
    bool m_singleShotPending = false;

    bool m_frameInFlight = false;
    bool m_suspendRequests = false;
    bool m_scopeEnabledOnBoard = false;
    /* 停止时若正有一帧在途，SCOPE OFF 必须等帧收完再发；
       板端同一时刻只接受一个传输，二进制期间发文本命令会被客户端挡掉。 */
    bool m_scopeOffPending = false;
    bool m_stopRequested = false;
    int m_consecutiveTimeouts = 0;
    int m_transportRetries = 0;

    TriggerSettings m_trigger;
    pdsample::WaveformFrame m_lastFrame;
    int m_lastTriggerIndex = -1;
    bool m_lastTriggerValid = false;

    Statistics m_statistics;

    QTimer *m_paceTimer = nullptr;     /* 帧节奏 */
    QTimer *m_watchdog = nullptr;      /* 帧超时 */
    QTimer *m_rateTimer = nullptr;     /* 1 秒窗口算帧率 */
    QTimer *m_orchestrationTimer = nullptr; /* 等待采集状态时的轮询 */
    quint64 m_framesInWindow = 0;
};

#include "scope_stream.h"

#include "pd_reply_parse.h"
#include "pd_tcp_client.h"

#include <QRegularExpression>
#include <QTimer>

namespace {

/* 板端 SCOPE V1 帧头：SCOPE V1 seq=N samples=N bytes=N fs=N crc32=HEX */
const QRegularExpression &scopeHeader()
{
    static const QRegularExpression expression(
        QStringLiteral("^SCOPE V1 seq=(\\d+) samples=(\\d+) bytes=(\\d+) fs=(\\d+) crc32=([0-9a-fA-F]{8})"));
    return expression;
}

const QRegularExpression &statusState()
{
    static const QRegularExpression expression(QStringLiteral("^STATUS state=(\\d+)"));
    return expression;
}

} // namespace

ScopeStream::ScopeStream(PdTcpClient *client, QObject *parent)
    : QObject(parent), m_client(client)
{
    m_paceTimer = new QTimer(this);
    m_paceTimer->setSingleShot(true);
    connect(m_paceTimer, &QTimer::timeout, this, &ScopeStream::requestFrame);

    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    connect(m_watchdog, &QTimer::timeout, this, &ScopeStream::onWatchdog);

    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(1000);
    connect(m_rateTimer, &QTimer::timeout, this, &ScopeStream::updateRate);

    m_orchestrationTimer = new QTimer(this);
    m_orchestrationTimer->setInterval(250);
    connect(m_orchestrationTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::CheckingFirmware) send(QStringLiteral("CONFIG"));
        else if (m_state == State::WaitingAcquisition) send(QStringLiteral("STATUS"));
    });

    if (m_client != nullptr) {
        connect(m_client, &PdTcpClient::textLine, this, &ScopeStream::onTextLine);
        connect(m_client, &PdTcpClient::scopeFrame, this, &ScopeStream::onScopeFrame);
        connect(m_client, &PdTcpClient::downloadFailed, this, &ScopeStream::onDownloadFailed);
        connect(m_client, &PdTcpClient::transportError, this, &ScopeStream::onTransportError);
        connect(m_client, &PdTcpClient::disconnected, this, [this] {
            m_orchestrationTimer->stop();
            m_paceTimer->stop();
            m_watchdog->stop();
            m_rateTimer->stop();
            setFrameInFlight(false);
            m_scopeEnabledOnBoard = false;
            setState(State::Idle, QStringLiteral("TCP 已断开，实时取帧已停止。"));
        });
    }
}

/* ------------------------------------------------------------------ 配置 */

void ScopeStream::setSamples(int samples)
{
    /* 板端硬约束：256..2048 且 4 的倍数。不合法的值在这里就挡掉，
       而不是发出去等板端回 ERR，避免把协议错误混进"正常丢帧"统计里。 */
    const int bounded = qBound(256, samples, 2048);
    m_samples = (bounded / 4) * 4;
}

void ScopeStream::setMinimumIntervalMs(int milliseconds)
{
    m_minimumIntervalMs = qBound(0, milliseconds, 1000);
}

void ScopeStream::setDisplayMode(DisplayMode mode)
{
    m_displayMode = mode;
}

void ScopeStream::setTrigger(const TriggerSettings &settings)
{
    m_trigger = settings;
    m_trigger.channel = qBound(0, m_trigger.channel, pdsample::kChannelCount - 1);
    m_trigger.hysteresis = qBound(0.0, m_trigger.hysteresis, 512.0);
    m_trigger.alignFraction = qBound(0.0, m_trigger.alignFraction, 0.9);
}

void ScopeStream::setAutoStartAcquisition(bool enabled)
{
    m_autoStart = enabled;
}

void ScopeStream::setRollCapacity(int samples)
{
    Q_UNUSED(samples);
    /* 滚动容量由 ScopeWidget::setRollCapacity 控制，这里保留接口以便将来
       在 PS 侧限制帧长时同步调整。 */
}

void ScopeStream::setWatchdogMs(int milliseconds)
{
    m_watchdogMs = qBound(500, milliseconds, 30000);
}

bool ScopeStream::isRunning() const
{
    return m_state == State::Streaming || m_state == State::Enabling ||
           m_state == State::WaitingAcquisition || m_state == State::CheckingFirmware;
}

/* ------------------------------------------------------------------ 状态机 */

void ScopeStream::setState(State state, const QString &message)
{
    if (m_state == state && message.isEmpty()) return;
    m_state = state;
    emit stateChanged(m_state, message);
    emit logLine(QStringLiteral("[scope] %1").arg(message));
}

void ScopeStream::start()
{
    if (m_client == nullptr || !m_client->isConnected()) {
        setState(State::Idle, QStringLiteral("未连接板端，无法开启实时示波器。"));
        return;
    }
    m_stopRequested = false;
    m_singleShotPending = false;
    m_consecutiveTimeouts = 0;
    m_statistics.triggerMisses = 0;
    /* 重启后本机不知道板端 seq 的起点，重置基线，避免把重启当成跳号。 */
    m_statistics.lastSequence = -1;

    emit logLine(QStringLiteral("[scope] 开启实时取帧：%1 样本/帧，显示模式 %2，最小帧间隔 %3 ms")
                     .arg(m_samples)
                     .arg(m_displayMode == DisplayMode::Live ? QStringLiteral("实时")
                          : m_displayMode == DisplayMode::Triggered ? QStringLiteral("触发")
                                                                    : QStringLiteral("滚动"))
                     .arg(m_minimumIntervalMs));
    /*
     * 先探测固件能力：CONFIG 回复里有没有 scope= 字段，决定板端有没有 SCOPE 命令。
     * 2026-09-23 上板实测：旧 ELF 对 SCOPE ON / SCOPE OFF 都回
     * "ERR unknown command"，波形永远出不来；当时这一条被静默吞掉，
     * 表现为"界面没数据但也没有任何错误"。所以先探测、再明确报错。
     */
    setState(State::CheckingFirmware, QStringLiteral("检查板端固件能力（CONFIG）…"));
    m_orchestrationTimer->start();
    send(QStringLiteral("CONFIG"));
}

void ScopeStream::pause()
{
    m_paceTimer->stop();
    m_watchdog->stop();
    if (m_state != State::Idle && m_state != State::Fault)
        setState(State::Paused, QStringLiteral("已暂停取帧（板端 SCOPE 仍开启）。"));
}

void ScopeStream::singleShot()
{
    if (m_client == nullptr || !m_client->isConnected()) {
        setState(State::Idle, QStringLiteral("未连接板端，无法取单帧。"));
        return;
    }
    m_singleShotPending = true;
    if (m_state == State::Streaming) {
        m_paceTimer->stop();
        /* 正在流式时只需等下一帧到达即可满足单次请求。 */
        return;
    }
    start();
}

void ScopeStream::stop()
{
    m_stopRequested = true;
    m_singleShotPending = false;
    m_paceTimer->stop();
    m_watchdog->stop();
    m_orchestrationTimer->stop();
    m_rateTimer->stop();
    setFrameInFlight(false);

    if (m_scopeEnabledOnBoard && m_client != nullptr && m_client->isConnected()) {
        if (m_frameInFlight) m_scopeOffPending = true;   /* 等帧收完再关 */
        else send(QStringLiteral("SCOPE OFF"));
    }
    m_scopeEnabledOnBoard = false;

    if (m_state != State::Idle)
        setState(State::Idle, QStringLiteral("实时取帧已停止。"));
}

void ScopeStream::send(const QString &command)
{
    if (m_client != nullptr) m_client->sendCommand(command);
}

void ScopeStream::setFrameInFlight(bool value)
{
    if (m_frameInFlight == value) return;
    m_frameInFlight = value;
    emit inFlightChanged(m_frameInFlight);
}

void ScopeStream::setSuspendRequests(bool suspended)
{
    if (m_suspendRequests == suspended) return;
    m_suspendRequests = suspended;
    if (m_suspendRequests) {
        /* 停掉已排队的下一帧；在途帧不动，让它自然收完。 */
        m_paceTimer->stop();
        emit logLine(QStringLiteral("[scope] 暂停取帧请求，让出链路给归档读取。"));
    } else {
        emit logLine(QStringLiteral("[scope] 恢复取帧。"));
        if (m_state == State::Streaming) scheduleFrameRequest();
    }
}

void ScopeStream::flushPendingScopeOff()
{
    if (!m_scopeOffPending) return;
    m_scopeOffPending = false;
    if (m_client != nullptr && m_client->isConnected()) send(QStringLiteral("SCOPE OFF"));
}

void ScopeStream::requestFrame()
{
    if (m_state != State::Streaming || m_client == nullptr || !m_client->isConnected()) return;
    if (m_frameInFlight || m_suspendRequests) return;
    setFrameInFlight(true);

    send(QStringLiteral("SCOPE NEXT"));
    m_watchdog->start(m_watchdogMs);
    if (m_rateTimer->isActive() == false) m_rateTimer->start();
}

void ScopeStream::scheduleFrameRequest()
{
    if (m_state != State::Streaming || m_stopRequested || m_suspendRequests) return;
    /* 用最小帧间隔给板端留出处理 DMA 与归档的时间；0 表示不设下限。 */
    if (m_minimumIntervalMs <= 0) {
        requestFrame();
        return;
    }
    m_paceTimer->start(m_minimumIntervalMs);
}

/* ------------------------------------------------------------------ 文本回复 */

void ScopeStream::onTextLine(const QString &line)
{
    if (m_state == State::CheckingFirmware) {
        const pdreply::Config config = pdreply::parseConfig(line);
        if (config.ok) {
            if (!config.hasScope) {
                enterFault(QStringLiteral(
                    "板端固件不支持 SCOPE 实时取帧命令（CONFIG 回复 api=%1，但缺少 scope= 字段）。"
                    "请用当前 sw/ps_service 重新编译并下载 ELF；在此之前实时示波器无法工作，"
                    "相位环（事件归档路径）仍然可用。").arg(config.api));
                return;
            }
            emit logLine(QStringLiteral("[scope] 固件支持 SCOPE：api=%1，samples=%2/%3")
                             .arg(config.api).arg(config.scopeSamples)
                             .arg(config.scopeMaxSamples));
            setState(State::WaitingAcquisition, QStringLiteral("检查板端采集状态…"));
            m_orchestrationTimer->start();
            send(QStringLiteral("STATUS"));
            return;
        }
    }

    if (m_state == State::WaitingAcquisition) {
        const auto match = statusState().match(line);
        if (match.hasMatch()) {
            const int boardState = match.captured(1).toInt();
            if (boardState != 0) {
                /* 已在采集运行态，可以开 SCOPE。 */
                m_orchestrationTimer->stop();
                m_scopeEnabledOnBoard = true;
                setState(State::Enabling, QStringLiteral("采集运行中，发送 SCOPE ON %1。").arg(m_samples));
                send(QStringLiteral("SCOPE ON %1").arg(m_samples));
            } else if (m_autoStart) {
                m_orchestrationTimer->stop();
                emit logLine(QStringLiteral("[scope] 板端处于 IDLE，自动发送 START 0（连续采集）。"));
                send(QStringLiteral("START 0"));
                m_orchestrationTimer->start();
            } else {
                m_orchestrationTimer->stop();
                enterFault(QStringLiteral("板端 IDLE 且未启用自动启动。请先执行 START 0，再开启实时示波器。"));
            }
            return;
        }
    }

    if (line.startsWith(QStringLiteral("OK SCOPE ON"))) {
        if (m_state == State::Enabling) {
            setState(State::Streaming, QStringLiteral("实时取帧已开始。"));
            m_consecutiveTimeouts = 0;
            requestFrame();
        }
        return;
    }
    if (line.startsWith(QStringLiteral("OK SCOPE OFF"))) {
        m_scopeEnabledOnBoard = false;
        return;
    }
    if (line.startsWith(QStringLiteral("ERR "))) handleBoardError(line);
}

void ScopeStream::handleBoardError(const QString &line)
{
    /*
     * 在"等 SCOPE ON 的 OK"或"探测固件能力"期间，任何 ERR 都说明 SCOPE 没被接受。
     * 以前这里只匹配已知字符串，导致 "ERR unknown command" 被静默忽略，
     * 表现成"波形一直空着却没有任何报错"——2026-09-23 上板正是被这一点坑了。
     */
    if (m_state == State::Enabling || m_state == State::CheckingFirmware) {
        enterFault(QStringLiteral("板端未接受 SCOPE 命令：%1").arg(line));
        return;
    }

    /* 只处理与本类发出的命令相关的错误。其它 ERR 可能来自主窗口的
       常规命令（CATALOG/PRPD/ANALYZE 等），不能据此判故障，但必须计数并显示。 */
    const bool scopeRelated = line.contains(QStringLiteral("SCOPE"));
    const bool catchAll = m_frameInFlight && m_state == State::Streaming;
    if (!scopeRelated && !catchAll) return;

    ++m_statistics.protocolErrors;
    emit logLine(QStringLiteral("[scope] 板端拒绝：%1").arg(line));
    emit statisticsChanged();

    if (line.contains(QStringLiteral("still active"))) {
        /* 上一次传输尚未收尾；等在途状态自然结束，稍后重试。 */
        setFrameInFlight(false);
        m_watchdog->stop();
        if (m_state == State::Streaming) m_paceTimer->start(50);
        return;
    }
    if (line.contains(QStringLiteral("SCOPE is off"))) {
        m_scopeEnabledOnBoard = true;
        setState(State::Enabling, QStringLiteral("板端 SCOPE 已关闭，重新开启。"));
        send(QStringLiteral("SCOPE ON %1").arg(m_samples));
        return;
    }
    if (line.contains(QStringLiteral("requires a running acquisition"))) {
        /* 采集停了（可能是用户按了 STOP，或有界采集跑完）。按配置决定是否自动重启。 */
        setFrameInFlight(false);
        m_watchdog->stop();
        if (m_autoStart && !m_stopRequested) {
            setState(State::WaitingAcquisition, QStringLiteral("采集已停止，自动重启连续采集…"));
            send(QStringLiteral("START 0"));
            m_orchestrationTimer->start();
        } else {
            enterFault(QStringLiteral("采集已停止，实时取帧无法继续。"));
        }
        return;
    }
    if (line.contains(QStringLiteral("raw DDR ring is not ready"))) {
        setFrameInFlight(false);
        m_watchdog->stop();
        /* 环尚未就绪通常出现在采集刚开始时，稍后重试。 */
        m_paceTimer->start(200);
        return;
    }
    /* 其它与 SCOPE 相关的错误：停在故障态，把处置权交回用户，不静默续跑。 */
    enterFault(QStringLiteral("板端返回无法自动处置的错误：%1").arg(line));
}

/* ------------------------------------------------------------------ 帧到达 */

void ScopeStream::onScopeFrame(const QByteArray &raw, quint32 samples, quint32 sampleRateHz)
{
    m_watchdog->stop();
    setFrameInFlight(false);
    m_consecutiveTimeouts = 0;

    pdsample::WaveformFrame frame;
    if (!pdsample::decodeFrame(raw, static_cast<double>(sampleRateHz), -1, frame) ||
        frame.sampleCount != static_cast<int>(samples)) {
        /* 长度/块对齐不满足 PL 的 24 字节块契约，属于契约不匹配，不能当数据用。 */
        ++m_statistics.protocolErrors;
        emit statisticsChanged();
        emit logLine(QStringLiteral("[scope] 帧长度不符合 24 字节块契约：收到 %1 字节，期望 %2。")
                         .arg(raw.size()).arg(samples * pdsample::kBytesPerSample));
        if (m_state == State::Streaming) scheduleFrameRequest();
        flushPendingScopeOff();
        return;
    }

    if (m_statistics.lastSequence >= 0 && frame.sequence != m_statistics.lastSequence + 1)
        ++m_statistics.sequenceGaps;
    m_statistics.lastSequence = frame.sequence;

    int triggerIndex = -1;
    bool triggerValid = false;
    if (m_displayMode == DisplayMode::Triggered) {
        computeTrigger(frame, triggerIndex, triggerValid);
        if (!triggerValid) {
            ++m_statistics.triggerMisses;
            /* 无触发帧时不覆盖画面（示波器的"保持"行为），但仍继续取帧。 */
            emit statisticsChanged();
            if (m_state == State::Streaming) scheduleFrameRequest();
            return;
        }
    }

    m_lastFrame = frame;
    m_lastTriggerIndex = triggerIndex;
    m_lastTriggerValid = triggerValid;
    ++m_statistics.frames;
    ++m_framesInWindow;

    if (m_displayMode == DisplayMode::Roll) emit rollFrame(frame);
    else emit liveFrame(frame, triggerIndex, triggerValid);
    emit statisticsChanged();

    if (m_singleShotPending) {
        m_singleShotPending = false;
        setState(State::Paused, QStringLiteral("单帧已取到，已暂停。"));
        flushPendingScopeOff();
        return;
    }
    scheduleFrameRequest();
    flushPendingScopeOff();
}

void ScopeStream::computeTrigger(pdsample::WaveformFrame &frame, int &triggerIndex,
                                 bool &valid) const
{
    triggerIndex = -1;
    valid = false;
    if (m_trigger.channel < 0 || m_trigger.channel >= pdsample::kChannelCount) return;

    const QVector<double> &samples = frame.channel[m_trigger.channel];
    triggerIndex = pdsample::findTriggerIndex(samples, m_trigger.level,
                                              m_trigger.hysteresis, m_trigger.rising);
    valid = triggerIndex >= 0;
}

/* ------------------------------------------------------------------ 异常路径 */

void ScopeStream::onWatchdog()
{
    if (m_state != State::Streaming && m_state != State::Enabling) return;
    if (!m_frameInFlight) return;

    ++m_statistics.timeouts;
    ++m_consecutiveTimeouts;
    setFrameInFlight(false);
    emit statisticsChanged();
    emit logLine(QStringLiteral("[scope] 帧超时（第 %1 次，累计 %2 次）。")
                     .arg(m_consecutiveTimeouts).arg(m_statistics.timeouts));

    if (m_consecutiveTimeouts >= 3) {
        enterFault(QStringLiteral("连续 %1 次帧超时，已停止取帧以免掩盖链路故障。")
                       .arg(m_consecutiveTimeouts));
        return;
    }
    /* 退避重试：先确认 TCP 连接还在，再重新要一帧。 */
    if (m_client == nullptr || !m_client->isConnected()) {
        enterFault(QStringLiteral("帧超时且 TCP 已断开。"));
        return;
    }
    requestFrame();
}

void ScopeStream::onDownloadFailed(const QString &reason)
{
    /* PdTcpClient 在 CRC32 不匹配或帧头非法时会走这里。这些帧不能画。 */
    if (!m_frameInFlight) return;
    setFrameInFlight(false);
    m_watchdog->stop();
    ++m_statistics.crcErrors;
    emit statisticsChanged();
    emit logLine(QStringLiteral("[scope] 帧校验失败：%1").arg(reason));
    if (m_state == State::Streaming) scheduleFrameRequest();
}

void ScopeStream::onTransportError(const QString &message)
{
    if (m_state == State::Idle) return;
    emit logLine(QStringLiteral("[scope] 传输错误：%1").arg(message));
}

void ScopeStream::enterFault(const QString &reason)
{
    setFrameInFlight(false);
    m_watchdog->stop();
    m_paceTimer->stop();
    m_orchestrationTimer->stop();
    m_rateTimer->stop();
    /* 尽量把板端 SCOPE 关掉，别留下一个开着却没人取帧的会话。
       若此刻仍在二进制传输中，客户端会拒绝发送，错误会出现在日志里——不隐藏。 */
    if (m_scopeEnabledOnBoard && m_client != nullptr && m_client->isConnected())
        send(QStringLiteral("SCOPE OFF"));
    m_scopeEnabledOnBoard = false;
    m_scopeOffPending = false;
    setState(State::Fault, reason);
}

void ScopeStream::updateRate()
{
    m_statistics.framesPerSecond = static_cast<double>(m_framesInWindow);
    m_framesInWindow = 0;
    emit statisticsChanged();
}

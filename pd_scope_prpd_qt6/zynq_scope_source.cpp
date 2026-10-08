#include "zynq_scope_source.h"

#include <QDateTime>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>

#include <cmath>

ZynqScopeSource::ZynqScopeSource(QObject *parent) : QObject(parent)
{
    m_socket = new QTcpSocket(this);
    m_socket->setProxy(QNetworkProxy::NoProxy);
    connect(m_socket, &QTcpSocket::connected, this, &ZynqScopeSource::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        m_streaming = false;
        m_requestKind = RequestKind::None;
        m_frameTimer->stop();
        m_envelopeTimer->stop();
        m_pumpTimer->stop();
        m_archiveFftTimer->stop();
        m_archiveFftPhase = ArchiveFftPhase::None;
        m_resumeAfterSnapshotFft = false;
        resetFrame();
        emit disconnected();
    });
    connect(m_socket, &QTcpSocket::readyRead, this, &ZynqScopeSource::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &ZynqScopeSource::onError);

    m_pumpTimer = new QTimer(this);
    m_pumpTimer->setSingleShot(true);
    connect(m_pumpTimer, &QTimer::timeout, this, &ZynqScopeSource::pumpRequests);

    m_frameTimer = new QTimer(this);
    m_frameTimer->setInterval(100); // Event-priority mode keeps a 10 Hz live waveform.
    connect(m_frameTimer, &QTimer::timeout, this, &ZynqScopeSource::markFrameDue);

    m_envelopeTimer = new QTimer(this);
    m_envelopeTimer->setInterval(20); // 50 Hz phase-aligned complete-cycle envelope.
    connect(m_envelopeTimer, &QTimer::timeout, this, [this] {
        if (!m_streaming) return;
        m_envelopeDue = true;
        schedulePump();
    });

    m_eventBatchTimer = new QTimer(this);
    m_eventBatchTimer->setSingleShot(true);
    m_eventBatchTimer->setInterval(20);
    connect(m_eventBatchTimer, &QTimer::timeout, this, &ZynqScopeSource::flushEventBatch);

    m_archiveFftTimer = new QTimer(this);
    m_archiveFftTimer->setSingleShot(true);
    connect(m_archiveFftTimer, &QTimer::timeout,
            this, &ZynqScopeSource::pollArchiveFftStop);

    m_diagnosticTimer = new QTimer(this);
    m_diagnosticTimer->setInterval(1000);
    connect(m_diagnosticTimer, &QTimer::timeout,
            this, &ZynqScopeSource::publishStreamDiagnostics);
    m_diagnosticTimer->start();
}

void ZynqScopeSource::connectToBoard(const QString &host, quint16 port)
{
    m_streaming = false;
    m_startPending = false;
    m_configPending = false;
    m_scopeOnPending = false;
    m_ownsAcquisition = false;
    m_scopeEnabled = false;
    m_disconnectRequested = false;
    m_requestKind = RequestKind::None;
    m_requestStartedAtMs = 0;
    m_frameDue = false;
    m_envelopeDue = false;
    m_forcePeaksAfterEnvelope = false;
    m_peaksSinceEnvelope = 0U;
    m_nextNormalIsFrame = true;
    m_manualPeaksPending = false;
    m_spectrumPending = false;
    m_haveFrame = false;
    m_haveCompletedEventSequence = false;
    m_eventBacklogPackets = 0U;
    m_psSkipped = 0;
    m_sequenceGaps = 0;
    m_crcErrors = 0;
    m_protocolErrors = 0;
    m_scopeFrameCount = 0;
    m_envelopeFrameCount = 0;
    m_liveSpectrumCount = 0;
    m_archiveSpectrumCount = 0;
    m_envelopeNoneCount = 0;
    m_envelopeSkipCount = 0;
    m_lastEnvelopeSequence = 0;
    m_lastEnvelopeLockMask = 0U;
    m_lastStreamError.clear();
    m_pendingEvents.clear();
    m_frameTimer->stop();
    m_envelopeTimer->stop();
    m_pumpTimer->stop();
    m_archiveFftTimer->stop();
    if (m_archiveFftPhase != ArchiveFftPhase::None)
        m_resumeAfterSnapshotFft = false;
    m_archiveFftPhase = ArchiveFftPhase::None;
    m_snapshotFftPending = false;
    m_catalogPending = false;
    m_resumeAfterSnapshotFft = false;
    resetFrame();
    m_rx.clear();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) m_socket->abort();
    publishStreamDiagnostics();
    m_socket->connectToHost(host, port);
}

void ZynqScopeSource::disconnectFromBoard()
{
    m_streaming = false;
    m_frameTimer->stop();
    m_envelopeTimer->stop();
    m_pumpTimer->stop();
    m_archiveFftTimer->stop();
    m_manualPeaksPending = false;
    if (m_archiveFftPhase != ArchiveFftPhase::None)
        m_resumeAfterSnapshotFft = false;
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    if (m_requestKind != RequestKind::None || m_waitingBinary) {
        m_disconnectRequested = true;
        return;
    }
    finishDisconnect();
}

void ZynqScopeSource::setEventPriority(bool enabled)
{
    m_eventPriority = enabled;
    m_peaksSinceEnvelope = 0U;
    if (m_streaming) {
        if (enabled) m_frameTimer->start();
        else {
            m_frameTimer->stop();
            m_frameDue = false;
            m_nextNormalIsFrame = true;
        }
        schedulePump();
    }
    emit statusChanged(enabled
        ? QStringLiteral("事件优先模式：持续轮询 PL 事件，并刷新全周期波形/相位包络。")
        : QStringLiteral("均衡模式：波形与 PL 事件轮询交替。"));
}

void ZynqScopeSource::requestEventNow()
{
    if (!m_streaming) {
        emit statusChanged(QStringLiteral("手动读取事件需要先连接并启动实时采集。"));
        return;
    }
    m_manualPeaksPending = true;
    schedulePump();
}

void ZynqScopeSource::shutdown()
{
    disconnectFromBoard();
}

void ZynqScopeSource::onConnected()
{
    emit connected();
    emit statusChanged(QStringLiteral("已连接 Zynq；正在检查实时事件协议。"));
    m_configPending = true;
    sendLine(QStringLiteral("CONFIG"));
}

void ZynqScopeSource::sendLine(const QString &line)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) return;
    m_socket->write(line.toUtf8() + '\n');
}

void ZynqScopeSource::onReadyRead()
{
    m_rx += m_socket->readAll();
    if (m_waitingBinary) processBinary();
    if (!m_waitingBinary) processLines();
}

void ZynqScopeSource::processLines()
{
    while (!m_waitingBinary) {
        const int newline = m_rx.indexOf('\n');
        if (newline < 0) return;
        const QString line = QString::fromUtf8(m_rx.left(newline)).trimmed();
        m_rx.remove(0, newline + 1);

        if (line.startsWith(QStringLiteral("PD_ACQ TCP"))) {
            emit statusChanged(line);
            continue;
        }
        if (m_configPending && line.startsWith(QStringLiteral("CONFIG "))) {
            m_configPending = false;
            static const QRegularExpression apiExpression(
                QStringLiteral("(?:^|\\s)api=(\\d+)(?:\\s|$)"));
            const auto match = apiExpression.match(line);
            const int api = match.hasMatch() ? match.captured(1).toInt() : 0;
            if (api < 17 || !line.contains(
                    QStringLiteral("scope_env=cycle520000_bins1024_ch4_minmax16")) ||
                !line.contains(QStringLiteral("scope_peaks_batch=32")) ||
                !line.contains(QStringLiteral("snap_fft_wave=ps_q15_1024x1_samples_plus_bins"))) {
                emit sourceError(QStringLiteral(
                    "板端协议 API %1 缺少批量事件/全周期包络/归档 FFT 能力。请导入 PS API 17 源码并由你在 Vitis 中构建下载；本程序不会启动采集。")
                    .arg(api));
                continue;
            }
            emit statusChanged(QStringLiteral("板端 API %1 已确认；启动连续采集。").arg(api));
            m_startPending = true;
            sendLine(QStringLiteral("START 0"));
            continue;
        }
        if (m_startPending && line == QStringLiteral("ERR already running")) {
            m_startPending = false;
            m_ownsAcquisition = false;
            m_scopeOnPending = true;
            sendLine(QStringLiteral("SCOPE ON 1024"));
            continue;
        }
        if (line.startsWith(QStringLiteral("ERR "))) {
            if (m_archiveFftPhase != ArchiveFftPhase::None) {
                emit sourceError(line);
                finishArchiveFft(true);
                continue;
            }
            m_streaming = false;
            m_requestKind = RequestKind::None;
            m_startPending = false;
            m_scopeOnPending = false;
            m_frameTimer->stop();
            m_envelopeTimer->stop();
            emit sourceError(line);
            continue;
        }
        if (m_startPending && line.startsWith(QStringLiteral("OK start accepted"))) {
            m_startPending = false;
            m_ownsAcquisition = true;
            m_scopeOnPending = true;
            sendLine(QStringLiteral("SCOPE ON 1024"));
            continue;
        }
        if (m_scopeOnPending && line.startsWith(QStringLiteral("OK SCOPE ON"))) {
            m_scopeOnPending = false;
            m_scopeEnabled = true;
            m_streaming = true;
            m_nextNormalIsFrame = true;
            m_peaksSinceEnvelope = 0U;
            m_eventBacklogPackets = 0U;
            emit statusChanged(m_eventPriority
                ? QStringLiteral("实时事件流已启动；四通道全周期包络、PL 事件与 PS FFT 开始轮询。")
                : QStringLiteral("实时事件流已启动；均衡模式轮询波形与事件。"));
            if (m_eventPriority) m_frameTimer->start();
            m_envelopeTimer->start();
            publishStreamDiagnostics();
            schedulePump();
            continue;
        }
        if (m_archiveFftPhase == ArchiveFftPhase::DisableScope &&
            line.startsWith(QStringLiteral("OK SCOPE OFF"))) {
            m_scopeEnabled = false;
            m_archiveFftPhase = ArchiveFftPhase::Stop;
            sendLine(QStringLiteral("STOP"));
            continue;
        }
        if (m_archiveFftPhase == ArchiveFftPhase::Stop &&
            line.startsWith(QStringLiteral("OK stop requested"))) {
            m_ownsAcquisition = false;
            m_archiveFftPhase = ArchiveFftPhase::WaitIdle;
            m_archiveFftPollCount = 0U;
            m_archiveFftTimer->start(50);
            emit statusChanged(QStringLiteral("正在停止采集以安全读取归档快照；完成后会自动恢复实时采集。"));
            continue;
        }
        if (m_archiveFftPhase == ArchiveFftPhase::Status &&
            line.startsWith(QStringLiteral("STATUS "))) {
            static const QRegularExpression stateExpression(
                QStringLiteral("(?:^|\\s)state=(\\d+)(?:\\s|$)"));
            const auto stateMatch = stateExpression.match(line);
            if (!stateMatch.hasMatch()) {
                emit sourceError(QStringLiteral("停止采集后的 STATUS 格式无法解析：%1").arg(line));
                finishArchiveFft(true);
                continue;
            }
            const quint32 state = stateMatch.captured(1).toUInt();
            if (state == 0U) {
                m_archiveFftPhase = ArchiveFftPhase::Catalog;
                m_requestKind = RequestKind::Catalog;
                sendLine(QStringLiteral("CATALOG"));
                emit statusChanged(QStringLiteral("采集已停止；正在重新确认 SNAP 序号仍在四槽保留窗口内。"));
            } else {
                m_archiveFftPhase = ArchiveFftPhase::WaitIdle;
                m_archiveFftTimer->start(50);
            }
            continue;
        }
        if (m_requestKind == RequestKind::Catalog && line.startsWith(QStringLiteral("CATALOG "))) {
            static const QRegularExpression catalogExpression(QStringLiteral(
                "^CATALOG event_seq=\\[(\\d+),(\\d+)\\) slots=(\\d+) snap_seq=\\[(\\d+),(\\d+)\\) slots=(\\d+) state=(\\d+)$"));
            const auto match = catalogExpression.match(line);
            if (!match.hasMatch()) {
                emit sourceError(QStringLiteral("SNAP CATALOG 格式错误：%1").arg(line));
                if (m_archiveFftPhase == ArchiveFftPhase::Catalog) {
                    finishArchiveFft(true);
                    continue;
                }
                finishRequest(false);
            } else {
                const quint64 first = match.captured(4).toULongLong();
                const quint64 next = match.captured(5).toULongLong();
                const quint32 state = match.captured(7).toUInt();
                if (m_archiveFftPhase == ArchiveFftPhase::Catalog) {
                    m_requestKind = RequestKind::None;
                    m_requestStartedAtMs = 0;
                    if (state != 0U || next <= first) {
                        emit sourceError(QStringLiteral("采集已停，但当前没有可用的归档 SNAP。"));
                        finishArchiveFft(true);
                        continue;
                    }
                    const quint64 requestedSequence = m_snapshotFftSequence;
                    if (requestedSequence < first || requestedSequence >= next) {
                        m_snapshotFftSequence = static_cast<quint32>(next - 1U);
                        emit snapshotCatalogReady(first, next, state);
                        emit statusChanged(QStringLiteral(
                            "所选 SNAP #%1 已被四槽环覆盖，自动改用最新可用 SNAP #%2。")
                            .arg(requestedSequence).arg(m_snapshotFftSequence));
                    }
                    m_archiveFftPhase = ArchiveFftPhase::Transfer;
                    m_requestKind = RequestKind::SnapshotSpectrum;
                    m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
                    sendLine(QStringLiteral("FFT WAVE SNAP SEQ %1 CHANNEL %2 START %3")
                        .arg(m_snapshotFftSequence).arg(m_snapshotFftChannel)
                        .arg(m_snapshotFftStart));
                    publishStreamDiagnostics();
                } else {
                    emit snapshotCatalogReady(first, next, state);
                    finishRequest(false);
                }
            }
            continue;
        }
        if (line.startsWith(QStringLiteral("FFT_SNAP_WAVE_V1 "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^FFT_SNAP_WAVE_V1 seq=(\\d+) index=(\\d+) channel=(\\d+) start=(\\d+) snap_samples=(\\d+) samples=(\\d+) points=(\\d+) fs=(\\d+) bin_hz=(\\d+) peak_bin=(\\d+) peak_hz=(\\d+) amplitude=(\\d+) dc=(\\d+) sample_bytes=(\\d+) bins_bytes=(\\d+) bytes=(\\d+) crc32=([0-9a-fA-F]{8})$"));
            const auto match = expression.match(line);
            if (!match.hasMatch() || m_requestKind != RequestKind::SnapshotSpectrum ||
                m_archiveFftPhase != ArchiveFftPhase::Transfer) {
                emit sourceError(QStringLiteral("PS 归档 FFT 帧头格式错误：%1").arg(line));
                finishArchiveFft(true);
                continue;
            }
            m_archiveSpectrum.snapshotSequence = match.captured(1).toULongLong();
            m_archiveSpectrum.channel = match.captured(3).toInt();
            m_archiveSpectrum.startSample = match.captured(4).toUInt();
            m_archiveSpectrum.snapshotSamples = match.captured(5).toUInt();
            const quint32 samples = match.captured(6).toUInt();
            const quint32 points = match.captured(7).toUInt();
            m_archiveSpectrum.sampleRateHz = match.captured(8).toUInt();
            m_archiveSpectrum.binHz = match.captured(9).toUInt();
            m_archiveSpectrum.peakBin = match.captured(10).toUInt();
            m_archiveSpectrum.peakHz = match.captured(11).toUInt();
            m_archiveSpectrum.amplitudeCode = match.captured(12).toUInt();
            m_archiveSpectrum.dcCode = match.captured(13).toUInt();
            m_expectedSnapshotSampleBytes = match.captured(14).toUInt();
            m_expectedSnapshotBinsBytes = match.captured(15).toUInt();
            m_expectedBytes = match.captured(16).toUInt();
            m_expectedCrc = match.captured(17).toUInt(nullptr, 16);
            m_expectedSnapshotSamples = samples;
            if (m_archiveSpectrum.channel < 0 || m_archiveSpectrum.channel > 3 ||
                m_archiveSpectrum.channel != static_cast<int>(m_snapshotFftChannel) ||
                m_archiveSpectrum.snapshotSequence != m_snapshotFftSequence ||
                m_archiveSpectrum.startSample != m_snapshotFftStart ||
                m_archiveSpectrum.snapshotSamples != 520000U || samples != 1024U ||
                points != 1024U || m_archiveSpectrum.sampleRateHz != 26000000U ||
                m_archiveSpectrum.binHz == 0U || m_archiveSpectrum.peakBin > 512U ||
                m_expectedSnapshotSampleBytes != 2048U ||
                m_expectedSnapshotBinsBytes != 1026U || m_expectedBytes != 3074U) {
                emit sourceError(QStringLiteral("PS 归档 FFT 帧头参数不一致。"));
                finishArchiveFft(true);
                continue;
            }
            m_waitingBinary = true;
            m_binaryKind = BinaryKind::SnapshotSpectrum;
            processBinary();
            return;
        }
        if (line.startsWith(QStringLiteral("SCOPE V1 "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^SCOPE V1 seq=(\\d+) samples=(\\d+) bytes=(\\d+) fs=(\\d+) crc32=([0-9a-fA-F]{8})$"));
            const auto match = expression.match(line);
            if (!match.hasMatch()) {
                emit sourceError(QStringLiteral("SCOPE 帧头格式错误：%1").arg(line));
                m_streaming = false;
                continue;
            }
            m_sequence = match.captured(1).toULongLong();
            m_expectedSamples = match.captured(2).toUInt();
            m_expectedBytes = match.captured(3).toUInt();
            m_expectedSampleRate = match.captured(4).toUInt();
            m_expectedCrc = match.captured(5).toUInt(nullptr, 16);
            if (m_expectedSamples == 0U || m_expectedBytes != m_expectedSamples * 6U ||
                m_expectedBytes > 16384U || m_expectedSampleRate == 0U ||
                m_requestKind != RequestKind::Frame) {
                emit sourceError(QStringLiteral("SCOPE 帧参数无效。"));
                m_streaming = false;
                continue;
            }
            m_waitingBinary = true;
            m_binaryKind = BinaryKind::Scope;
            processBinary();
            return;
        }
        if (line.startsWith(QStringLiteral("SCOPE_ENV V1 "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^SCOPE_ENV V1 seq=(\\d+) snap_seq=(\\d+) samples=(\\d+) bins=(\\d+) bytes=(\\d+) fs=(\\d+) lock=([0-9a-fA-F]+) phase=cycle_start crc32=([0-9a-fA-F]{8})$"));
            const auto match = expression.match(line);
            if (!match.hasMatch()) {
                emit sourceError(QStringLiteral("全周期包络帧头格式错误：%1").arg(line));
                finishRequest(true);
                continue;
            }
            m_sequence = match.captured(1).toULongLong();
            m_snapshotSequence = match.captured(2).toULongLong();
            m_expectedSourceSamples = match.captured(3).toUInt();
            m_expectedEnvelopeBins = match.captured(4).toUInt();
            m_expectedBytes = match.captured(5).toUInt();
            m_expectedSampleRate = match.captured(6).toUInt();
            m_expectedEnvelopeLockMask = match.captured(7).toUInt(nullptr, 16);
            m_expectedCrc = match.captured(8).toUInt(nullptr, 16);
            if (m_requestKind != RequestKind::Envelope || m_expectedSourceSamples != 520000U ||
                m_expectedEnvelopeBins != 1024U || m_expectedBytes != 16384U ||
                m_expectedBytes != m_expectedEnvelopeBins * 4U * 4U ||
                m_expectedSampleRate != 26000000U) {
                emit sourceError(QStringLiteral("全周期包络帧参数无效。"));
                finishRequest(true);
                continue;
            }
            m_waitingBinary = true;
            m_binaryKind = BinaryKind::Envelope;
            processBinary();
            return;
        }
        if (line.startsWith(QStringLiteral("SCOPE_ENV SKIP "))) {
            ++m_envelopeSkipCount;
            emit statusChanged(QStringLiteral(
                "全周期帧未发送（%1）；保持上一帧并等待下一周期。")
                .arg(line.mid(QStringLiteral("SCOPE_ENV SKIP ").size())));
            publishStreamDiagnostics();
            finishRequest(true);
            continue;
        }
        if (line.startsWith(QStringLiteral("SCOPE_ENV NONE "))) {
            ++m_envelopeNoneCount;
            emit statusChanged(QStringLiteral("尚无完整 SNAP 包络（%1 次等待）。")
                               .arg(m_envelopeNoneCount));
            publishStreamDiagnostics();
            finishRequest(true);
            continue;
        }
        if (line.startsWith(QStringLiteral("SCOPE_FFT V1 "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^SCOPE_FFT V1 seq=(\\d+) channel=(\\d+) samples=(\\d+) points=(\\d+) fs=(\\d+) bin_hz=(\\d+) peak_bin=(\\d+) peak_hz=(\\d+) amplitude=(\\d+) dc=(\\d+) bytes=(\\d+) crc32=([0-9a-fA-F]{8})$"));
            const auto match = expression.match(line);
            if (!match.hasMatch()) {
                emit sourceError(QStringLiteral("PS 实时 FFT 帧头格式错误：%1").arg(line));
                finishRequest(true);
                continue;
            }
            m_spectrumHeader.sequence = match.captured(1).toULongLong();
            m_spectrumHeader.channel = match.captured(2).toInt();
            m_expectedSamples = match.captured(3).toUInt();
            const quint32 points = match.captured(4).toUInt();
            m_spectrumHeader.sampleRateHz = match.captured(5).toUInt();
            m_spectrumHeader.binHz = match.captured(6).toUInt();
            m_spectrumHeader.peakBin = match.captured(7).toUInt();
            m_spectrumHeader.peakHz = match.captured(8).toUInt();
            m_spectrumHeader.amplitudeCode = match.captured(9).toUInt();
            m_spectrumHeader.dcCode = match.captured(10).toUInt();
            m_expectedBytes = match.captured(11).toUInt();
            m_expectedCrc = match.captured(12).toUInt(nullptr, 16);
            if (m_requestKind != RequestKind::Spectrum || m_expectedSamples != 1024U ||
                points != 1024U || m_expectedBytes != 1026U ||
                m_spectrumHeader.channel < 0 || m_spectrumHeader.channel > 3 ||
                m_spectrumHeader.sampleRateHz == 0U || m_spectrumHeader.binHz == 0U ||
                m_spectrumHeader.peakBin > 512U) {
                emit sourceError(QStringLiteral("PS 实时 FFT 帧头参数无效。"));
                finishRequest(true);
                continue;
            }
            m_waitingBinary = true;
            m_binaryKind = BinaryKind::Spectrum;
            processBinary();
            return;
        }
        if (line.startsWith(QStringLiteral("PEAKS V3 "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^PEAKS V3 packets=(\\d+) peaks=(\\d+) bytes=(\\d+) crc32=([0-9a-fA-F]{8}) wins=(\\d+),(\\d+),(\\d+),(\\d+) lock=([0-9a-fA-F]+) first=(\\d+) next=(\\d+) start=(\\d+) gap=(\\d+) skipped=(\\d+)$"));
            const auto match = expression.match(line);
            if (!match.hasMatch() || m_requestKind != RequestKind::Peaks) {
                emit sourceError(QStringLiteral("PL 批量事件帧头格式错误：%1").arg(line));
                m_streaming = false;
                continue;
            }
            m_expectedPeakPackets = match.captured(1).toUInt();
            m_expectedPeakCount = match.captured(2).toUInt();
            m_expectedBytes = match.captured(3).toUInt();
            m_expectedCrc = match.captured(4).toUInt(nullptr, 16);
            for (int channel = 0; channel < 4; ++channel)
                m_phaseWindows[channel] = match.captured(5 + channel).toUInt();
            m_phaseLockMask = match.captured(9).toUInt(nullptr, 16);
            m_archiveFirst = match.captured(10).toULongLong();
            m_archiveNext = match.captured(11).toULongLong();
            const quint64 batchStart = match.captured(12).toULongLong();
            m_expectedPeakGap = match.captured(13).toUInt();
            m_psSkipped = qMax(m_psSkipped, match.captured(14).toULongLong());
            if (m_expectedPeakPackets == 0U || m_expectedPeakPackets > 32U ||
                m_expectedBytes == 0U || m_expectedBytes > 131072U ||
                m_expectedPeakCount > m_expectedBytes / 8U ||
                batchStart > m_archiveNext) {
                emit sourceError(QStringLiteral("PL 批量事件帧参数无效。"));
                m_streaming = false;
                continue;
            }
            if (m_expectedPeakGap > 0U)
                emit statusChanged(QStringLiteral("PS 事件环已覆盖 %1 个旧包；正在批量追赶新事件。")
                                   .arg(m_expectedPeakGap));
            emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                      m_sequenceGaps, m_crcErrors);
            m_waitingBinary = true;
            m_binaryKind = BinaryKind::PeakBatch;
            processBinary();
            return;
        }
        if (line.startsWith(QStringLiteral("PEAKS V2 "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^PEAKS V2 seq=(\\d+) total=(\\d+) words=(\\d+) bytes=(\\d+) crc32=([0-9a-fA-F]{8}) wins=(\\d+),(\\d+),(\\d+),(\\d+) lock=([0-9a-fA-F]+) first=(\\d+) next=(\\d+) gap=(\\d+) skipped=(\\d+)$"));
            const auto match = expression.match(line);
            if (!match.hasMatch()) {
                emit sourceError(QStringLiteral("PL 完整事件帧头格式错误：%1").arg(line));
                m_streaming = false;
                continue;
            }
            m_peakPacketSequence = match.captured(1).toULongLong();
            m_expectedPeakCount = match.captured(2).toUInt();
            m_expectedPeakWords = match.captured(3).toUInt();
            m_expectedBytes = match.captured(4).toUInt();
            m_expectedCrc = match.captured(5).toUInt(nullptr, 16);
            for (int channel = 0; channel < 4; ++channel)
                m_phaseWindows[channel] = match.captured(6 + channel).toUInt();
            m_phaseLockMask = match.captured(10).toUInt(nullptr, 16);
            m_archiveFirst = match.captured(11).toULongLong();
            m_archiveNext = match.captured(12).toULongLong();
            const quint64 gap = match.captured(13).toULongLong();
            m_psSkipped = qMax(m_psSkipped, match.captured(14).toULongLong());
            if (gap > 0U)
                emit statusChanged(QStringLiteral("事件归档赶不上采集：本次跳过 %1 包。")
                                   .arg(gap));
            if (m_expectedBytes != m_expectedPeakWords * 8U ||
                m_expectedBytes > 65528U || (m_expectedBytes & 7U) != 0U ||
                m_expectedPeakCount > m_expectedPeakWords ||
                m_requestKind != RequestKind::Peaks) {
                emit sourceError(QStringLiteral("PL 完整事件帧参数无效。"));
                m_streaming = false;
                continue;
            }
            emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                     m_sequenceGaps, m_crcErrors);
            if (m_expectedBytes == 0U) {
                finishRequest(true);
                continue;
            }
            m_waitingBinary = true;
            m_binaryKind = BinaryKind::Peaks;
            processBinary();
            return;
        }
        if (line.startsWith(QStringLiteral("PEAKS NONE "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^PEAKS NONE first=(\\d+) next=(\\d+) skipped=(\\d+)$"));
            const auto match = expression.match(line);
            if (match.hasMatch()) {
                m_archiveFirst = match.captured(1).toULongLong();
                m_archiveNext = match.captured(2).toULongLong();
                m_eventBacklogPackets = 0U;
                m_psSkipped = qMax(m_psSkipped, match.captured(3).toULongLong());
                emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                         m_sequenceGaps, m_crcErrors);
            }
            finishRequest(true);
            continue;
        }
        if (line.startsWith(QStringLiteral("PEAKS SKIP "))) {
            static const QRegularExpression expression(QStringLiteral(
                "^PEAKS SKIP seq=(\\d+) skipped=(\\d+)$"));
            const auto match = expression.match(line);
            if (match.hasMatch()) {
                m_psSkipped = qMax(m_psSkipped, match.captured(2).toULongLong());
                emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                         m_sequenceGaps, m_crcErrors);
            }
            emit statusChanged(QStringLiteral("PS 跳过不可解码事件：%1").arg(line));
            finishRequest(true);
            continue;
        }
        if (line.startsWith(QStringLiteral("PEAKS V1 "))) {
            m_streaming = false;
            emit sourceError(QStringLiteral("板端仍在发送 PEAKS V1 抽取数据；需要下载 API 17 ELF。"));
            continue;
        }
        emit statusChanged(line);
    }
}

void ZynqScopeSource::processBinary()
{
    const quint32 remaining = m_expectedBytes - static_cast<quint32>(m_frame.size());
    const int take = qMin<int>(m_rx.size(), static_cast<int>(remaining));
    m_frame += m_rx.left(take);
    m_rx.remove(0, take);
    if (static_cast<quint32>(m_frame.size()) != m_expectedBytes) return;

    const BinaryKind completedKind = m_binaryKind;
    if (crc32(m_frame) != m_expectedCrc) {
        ++m_crcErrors;
        emit sourceError(QStringLiteral("PL/PS TCP 二进制数据 CRC32 校验失败；该数据未加入显示。"));
        emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                 m_sequenceGaps, m_crcErrors);
        resetFrame();
        if (completedKind == BinaryKind::SnapshotSpectrum)
            finishArchiveFft(true);
        else
            finishRequest(true);
        publishStreamDiagnostics();
        return;
    }

    if (completedKind == BinaryKind::Envelope) {
        ScopeFrame frame;
        frame.sequence = m_snapshotSequence;
        frame.sampleRateHz = m_expectedSampleRate;
        frame.phaseLockMask = m_expectedEnvelopeLockMask;
        frame.phaseSynchronized = frame.phaseLockMask == 0x0FU;
        frame.fullCycleEnvelope = true;
        frame.sourceSampleCount = m_expectedSourceSamples;
        ++m_envelopeFrameCount;
        m_lastEnvelopeSequence = m_snapshotSequence;
        m_lastEnvelopeLockMask = m_expectedEnvelopeLockMask;
        frame.minimum.resize(4);
        frame.maximum.resize(4);
        for (int channel = 0; channel < 4; ++channel) {
            frame.minimum[channel].resize(static_cast<int>(m_expectedEnvelopeBins));
            frame.maximum[channel].resize(static_cast<int>(m_expectedEnvelopeBins));
        }
        const auto *payload = reinterpret_cast<const uchar *>(m_frame.constData());
        for (quint32 bin = 0; bin < m_expectedEnvelopeBins; ++bin) {
            for (int channel = 0; channel < 4; ++channel) {
                const int offset = static_cast<int>((bin * 4U +
                                                      static_cast<quint32>(channel)) * 4U);
                frame.minimum[channel][static_cast<int>(bin)] =
                    qFromLittleEndian<qint16>(payload + offset);
                frame.maximum[channel][static_cast<int>(bin)] =
                    qFromLittleEndian<qint16>(payload + offset + 2);
            }
        }
        emit frameReady(frame);
        publishStreamDiagnostics();
        resetFrame();
        finishRequest(false);
        return;
    }

    if (completedKind == BinaryKind::Spectrum) {
        m_spectrumHeader.magnitudes.resize(static_cast<int>(m_expectedBytes / 2U));
        for (int bin = 0; bin < m_spectrumHeader.magnitudes.size(); ++bin) {
            const auto *p = reinterpret_cast<const uchar *>(m_frame.constData()) + bin * 2;
            m_spectrumHeader.magnitudes[bin] = qFromLittleEndian<quint16>(p);
        }
        emit spectrumReady(m_spectrumHeader);
        ++m_liveSpectrumCount;
        publishStreamDiagnostics();
        resetFrame();
        finishRequest(false);
        return;
    }

    if (completedKind == BinaryKind::SnapshotSpectrum) {
        m_archiveSpectrum.samples.resize(static_cast<int>(m_expectedSnapshotSamples));
        m_archiveSpectrum.magnitudes.resize(static_cast<int>(m_expectedSnapshotBinsBytes / 2U));
        const auto *payload = reinterpret_cast<const uchar *>(m_frame.constData());
        for (int i = 0; i < m_archiveSpectrum.samples.size(); ++i)
            m_archiveSpectrum.samples[i] = qFromLittleEndian<qint16>(payload + i * 2);
        for (int bin = 0; bin < m_archiveSpectrum.magnitudes.size(); ++bin) {
            const int offset = static_cast<int>(m_expectedSnapshotSampleBytes) + bin * 2;
            m_archiveSpectrum.magnitudes[bin] = qFromLittleEndian<quint16>(payload + offset);
        }
        emit archiveSpectrumReady(m_archiveSpectrum);
        ++m_archiveSpectrumCount;
        resetFrame();
        finishArchiveFft(true);
        publishStreamDiagnostics();
        return;
    }

    if (completedKind == BinaryKind::PeakBatch) {
        const auto *payload = reinterpret_cast<const uchar *>(m_frame.constData());
        quint32 offset = 0U;
        quint32 records = 0U;
        quint32 seenPeaks = 0U;
        bool batchMalformed = false;
        const bool hadPreviousSequence = m_haveCompletedEventSequence;
        QVector<PdPulse> events;
        events.reserve(static_cast<int>(m_expectedPeakCount));

        while (records < m_expectedPeakPackets) {
            if (m_expectedBytes - offset < 8U) {
                emit sourceError(QStringLiteral("PL 批量事件记录头被截断。"));
                batchMalformed = true;
                break;
            }
            const quint32 sequence = qFromLittleEndian<quint32>(payload + offset);
            const quint32 recordBytes = qFromLittleEndian<quint32>(payload + offset + 4U);
            offset += 8U;
            if (recordBytes == 0U || (recordBytes & 7U) != 0U ||
                recordBytes > 65536U || recordBytes > m_expectedBytes - offset) {
                emit sourceError(QStringLiteral("PL 批量事件记录长度无效。"));
                batchMalformed = true;
                break;
            }
            if (m_haveCompletedEventSequence &&
                sequence > m_lastCompletedEventSequence + 1U)
                m_sequenceGaps += sequence - m_lastCompletedEventSequence - 1U;
            else if (!m_haveCompletedEventSequence && records == 0U &&
                     !hadPreviousSequence)
                m_sequenceGaps += m_expectedPeakGap;

            const quint32 words = recordBytes / 8U;
            for (quint32 i = 0U; i < words; ++i) {
                const quint64 word = qFromLittleEndian<quint64>(payload + offset + i * 8U);
                const quint32 type = static_cast<quint32>(word >> 56);
                if (type == 1U) continue;
                if (type != 0U) {
                    emit sourceError(QStringLiteral("PL 批量事件含未知字类型。"));
                    batchMalformed = true;
                    continue;
                }
                ++seenPeaks;
                PdPulse pulse;
                pulse.channel = static_cast<int>((word >> 25) & 0x3U);
                pulse.sampleIndex = -1;
                const quint32 phaseIndex = static_cast<quint32>((word >> 28) & 0xFFFU);
                const quint32 phaseWindow = m_phaseWindows[pulse.channel];
                pulse.phaseValid = phaseWindow != 0U && phaseWindow <= 2048U &&
                    phaseIndex < phaseWindow &&
                    ((m_phaseLockMask & (1U << pulse.channel)) != 0U);
                if (phaseWindow != 0U && phaseIndex < phaseWindow)
                    pulse.phaseDeg = static_cast<double>(phaseIndex) * 360.0 / phaseWindow;
                pulse.amplitude = static_cast<qint16>((word >> 40) & 0xFFFFU);
                pulse.frameSequence = sequence;
                pulse.packetWordIndex = i;
                pulse.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
                events.append(pulse);
            }
            offset += recordBytes;
            m_peakPacketSequence = sequence;
            m_lastCompletedEventSequence = sequence;
            m_haveCompletedEventSequence = true;
            ++records;
        }

        if (records != m_expectedPeakPackets || offset != m_expectedBytes ||
            seenPeaks != m_expectedPeakCount) {
            emit sourceError(QStringLiteral(
                "PL 批量事件完整性不符：records=%1/%2 bytes=%3/%4 peaks=%5/%6。")
                .arg(records).arg(m_expectedPeakPackets).arg(offset).arg(m_expectedBytes)
                .arg(seenPeaks).arg(m_expectedPeakCount));
            batchMalformed = true;
        }
        if (batchMalformed) {
            ++m_protocolErrors;
            events.clear();
        } else if (m_haveCompletedEventSequence) {
            const quint64 cursor = qMax(m_archiveFirst,
                                        m_lastCompletedEventSequence + 1U);
            m_eventBacklogPackets = m_archiveNext > cursor ?
                m_archiveNext - cursor : 0U;
        }
        if (!events.isEmpty()) {
            m_pendingEvents += events;
            if (!m_eventBatchTimer->isActive()) m_eventBatchTimer->start();
        }
        emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                  m_sequenceGaps, m_crcErrors);
        publishStreamDiagnostics();
        resetFrame();
        finishRequest(false);
        return;
    }

    if (completedKind == BinaryKind::Peaks) {
        QVector<PdPulse> events;
        events.reserve(static_cast<int>(m_expectedPeakCount));
        quint32 seenPeaks = 0U;
        for (quint32 i = 0; i < m_expectedPeakWords; ++i) {
            const auto *p = reinterpret_cast<const uchar *>(m_frame.constData()) + i * 8U;
            const quint64 word = qFromLittleEndian<quint64>(p);
            const quint32 type = static_cast<quint32>(word >> 56);
            if (type == 1U) continue; // Cycle summary metadata, not an individual pulse.
            if (type != 0U) {
                emit sourceError(QStringLiteral("PL 事件包含未知字类型；保留包序号以便诊断。"));
                continue;
            }
            ++seenPeaks;
            PdPulse pulse;
            pulse.channel = static_cast<int>((word >> 25) & 0x3U);
            pulse.sampleIndex = -1; // PL pulse timing is phase-indexed, not raw-window-indexed.
            const quint32 phaseIndex = static_cast<quint32>((word >> 28) & 0xFFFU);
            const quint32 phaseWindow = m_phaseWindows[pulse.channel];
            pulse.phaseValid = phaseWindow != 0U && phaseWindow <= 2048U &&
                               phaseIndex < phaseWindow &&
                               ((m_phaseLockMask & (1U << pulse.channel)) != 0U);
            if (phaseWindow != 0U && phaseIndex < phaseWindow)
                pulse.phaseDeg = static_cast<double>(phaseIndex) * 360.0 / phaseWindow;
            pulse.amplitude = static_cast<qint16>((word >> 40) & 0xFFFFU);
            pulse.frameSequence = m_peakPacketSequence;
            pulse.packetWordIndex = i;
            pulse.receivedAtMs = QDateTime::currentMSecsSinceEpoch();
            events.append(pulse); // Keep unsynchronized pulses in the table too.
        }
        if (seenPeaks != m_expectedPeakCount)
            emit sourceError(QStringLiteral("事件帧头声明 %1 个峰值，实际解码 %2 个。")
                             .arg(m_expectedPeakCount).arg(seenPeaks));
        if (m_haveCompletedEventSequence && m_peakPacketSequence > m_lastCompletedEventSequence + 1U)
            m_sequenceGaps += m_peakPacketSequence - m_lastCompletedEventSequence - 1U;
        m_lastCompletedEventSequence = m_peakPacketSequence;
        m_haveCompletedEventSequence = true;
        if (!events.isEmpty()) {
            m_pendingEvents += events;
            if (!m_eventBatchTimer->isActive()) m_eventBatchTimer->start();
        }
        emit eventTransportStatus(m_archiveFirst, m_archiveNext, m_psSkipped,
                                 m_sequenceGaps, m_crcErrors);
        publishStreamDiagnostics();
        resetFrame();
        finishRequest(false);
        return;
    }

    ScopeFrame frame;
    frame.sequence = m_sequence;
    frame.sampleRateHz = m_expectedSampleRate;
    frame.samples.resize(4);
    for (int i = 0; i < static_cast<int>(m_expectedSamples); ++i) {
        const auto *p = reinterpret_cast<const uchar *>(m_frame.constData()) + i * 6;
        const quint16 code[4] = {
            static_cast<quint16>(p[0] | ((p[1] & 0x0F) << 8)),
            static_cast<quint16>((p[1] >> 4) | (p[2] << 4)),
            static_cast<quint16>(p[3] | ((p[4] & 0x0F) << 8)),
            static_cast<quint16>((p[4] >> 4) | (p[5] << 4))
        };
        for (int channel = 0; channel < 4; ++channel)
            frame.samples[channel].append(static_cast<qint16>(code[channel]) - 2048);
    }
    resetFrame();
    m_haveFrame = true;
    m_spectrumPending = true;
    ++m_scopeFrameCount;
    emit frameReady(frame);
    publishStreamDiagnostics();
    finishRequest(false);
}

void ZynqScopeSource::requestNextFrame()
{
    if (!m_streaming || m_socket->state() != QAbstractSocket::ConnectedState ||
        m_requestKind != RequestKind::None) return;
    m_requestKind = RequestKind::Frame;
    m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    sendLine(QStringLiteral("SCOPE NEXT"));
    publishStreamDiagnostics();
}

void ZynqScopeSource::requestNextEnvelope()
{
    if (!m_streaming || m_socket->state() != QAbstractSocket::ConnectedState ||
        m_requestKind != RequestKind::None) return;
    m_envelopeDue = false;
    m_forcePeaksAfterEnvelope = true;
    m_peaksSinceEnvelope = 0U;
    m_requestKind = RequestKind::Envelope;
    m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    sendLine(QStringLiteral("SCOPE ENVELOPE"));
    publishStreamDiagnostics();
}

void ZynqScopeSource::requestNextPeaks()
{
    if (!m_streaming || m_socket->state() != QAbstractSocket::ConnectedState ||
        m_requestKind != RequestKind::None) return;
    m_requestKind = RequestKind::Peaks;
    if (!m_currentRequestWasManual) ++m_peaksSinceEnvelope;
    m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    sendLine(QStringLiteral("SCOPE PEAKS BATCH 32"));
    publishStreamDiagnostics();
}

void ZynqScopeSource::requestScopeSpectrum()
{
    if (!m_streaming || m_socket->state() != QAbstractSocket::ConnectedState ||
        m_requestKind != RequestKind::None || !m_haveFrame) return;
    m_spectrumPending = false;
    m_requestKind = RequestKind::Spectrum;
    m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    sendLine(QStringLiteral("SCOPE FFT CHANNEL %1").arg(m_fftChannel));
    publishStreamDiagnostics();
}

void ZynqScopeSource::setFftChannel(int channel)
{
    if (channel < 0 || channel > 3 || m_fftChannel == channel) return;
    m_fftChannel = channel;
    if (m_streaming && m_haveFrame) {
        m_spectrumPending = true;
        schedulePump();
    }
}

void ZynqScopeSource::requestSnapshotCatalog()
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        emit sourceError(QStringLiteral("读取 SNAP 目录需要先连接 Zynq。"));
        return;
    }
    m_catalogPending = true;
    schedulePump();
}

void ZynqScopeSource::requestSnapshotFft(quint32 sequence, int channel, quint32 startSample)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        emit sourceError(QStringLiteral("归档 FFT 需要先连接 Zynq。"));
        return;
    }
    if (channel < 0 || channel > 3 || startSample > 518976U) {
        emit sourceError(QStringLiteral("归档 FFT 参数越界；通道为 0..3，起点为 0..518976。"));
        return;
    }
    if (m_archiveFftPhase != ArchiveFftPhase::None || m_snapshotFftPending) {
        emit sourceError(QStringLiteral("已有归档 FFT 操作正在进行。"));
        return;
    }
    m_snapshotFftSequence = sequence;
    m_snapshotFftChannel = static_cast<quint32>(channel);
    m_snapshotFftStart = startSample;
    m_snapshotFftPending = true;
    schedulePump();
}

void ZynqScopeSource::finishRequest(bool emptyPeaks)
{
    const RequestKind completed = m_requestKind;
    const bool manual = m_currentRequestWasManual;
    m_requestKind = RequestKind::None;
    m_requestStartedAtMs = 0;
    m_currentRequestWasManual = false;
    if (m_disconnectRequested) {
        finishDisconnect();
        return;
    }
    if (!m_streaming) return;
    if (!m_eventPriority && !manual) {
        if (completed == RequestKind::Frame) m_nextNormalIsFrame = false;
        else if (completed == RequestKind::Peaks) m_nextNormalIsFrame = true;
    }
    schedulePump(emptyPeaks && m_eventPriority && completed == RequestKind::Peaks ? 8 : 0);
    publishStreamDiagnostics();
}

void ZynqScopeSource::schedulePump(int delayMs)
{
    if (m_streaming && !m_pumpTimer->isActive()) m_pumpTimer->start(qMax(0, delayMs));
}

void ZynqScopeSource::pumpRequests()
{
    if (!m_streaming || m_requestKind != RequestKind::None || m_waitingBinary ||
        m_socket->state() != QAbstractSocket::ConnectedState) return;
    if (m_catalogPending) {
        m_catalogPending = false;
        m_requestKind = RequestKind::Catalog;
        m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        sendLine(QStringLiteral("CATALOG"));
        publishStreamDiagnostics();
        return;
    }
    if (m_snapshotFftPending) {
        m_snapshotFftPending = false;
        m_resumeAfterSnapshotFft = true;
        m_streaming = false;
        m_frameTimer->stop();
        m_envelopeTimer->stop();
        m_pumpTimer->stop();
        m_archiveFftPollCount = 0U;
        m_archiveFftPhase = ArchiveFftPhase::DisableScope;
        m_requestStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        sendLine(QStringLiteral("SCOPE OFF"));
        publishStreamDiagnostics();
        return;
    }
    if (m_manualPeaksPending) {
        m_manualPeaksPending = false;
        m_currentRequestWasManual = true;
        requestNextPeaks();
        return;
    }
    // Keep the PS FFT paired with the most recently received frame, then
    // service the 10 Hz frame deadline before background envelope/event work.
    // In event-priority mode m_envelopeDue can remain asserted continuously;
    // checking it first starves SCOPE NEXT and leaves the waveform/FFT panes
    // permanently empty even though event batches are still arriving.
    if (m_spectrumPending && m_haveFrame) {
        requestScopeSpectrum();
        return;
    }
    if (m_eventPriority && m_frameDue) {
        m_frameDue = false;
        requestNextFrame();
        return;
    }
    if (m_forcePeaksAfterEnvelope) {
        m_forcePeaksAfterEnvelope = false;
        requestNextPeaks();
        return;
    }
    /* A 2048-slot archive can be overwritten while a full-cycle envelope
     * is processed. Drain several complete event batches first only when the
     * retained backlog grows; waveform and FFT deadlines above still run. */
    if (m_eventPriority && m_envelopeDue) {
        const quint32 burstLimit = m_eventBacklogPackets >= 1024U ? 6U :
                                   m_eventBacklogPackets >= 256U ? 3U : 0U;
        if (m_peaksSinceEnvelope < burstLimit) {
            requestNextPeaks();
            return;
        }
    }
    if (m_envelopeDue) {
        requestNextEnvelope();
        return;
    }
    if (m_eventPriority) {
        requestNextPeaks();
    } else if (m_nextNormalIsFrame) {
        requestNextFrame();
    } else {
        requestNextPeaks();
    }
}

void ZynqScopeSource::markFrameDue()
{
    if (!m_streaming || !m_eventPriority) return;
    m_frameDue = true;
    schedulePump();
}

void ZynqScopeSource::flushEventBatch()
{
    if (m_pendingEvents.isEmpty()) return;
    QVector<PdPulse> batch;
    batch.swap(m_pendingEvents);
    emit phaseEventsReady(batch);
}

void ZynqScopeSource::finishDisconnect()
{
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        if (m_scopeEnabled) m_socket->write("SCOPE OFF\n");
        if (m_ownsAcquisition) m_socket->write("STOP\n");
        m_socket->disconnectFromHost();
    }
    m_scopeEnabled = false;
    m_ownsAcquisition = false;
    m_disconnectRequested = false;
}

void ZynqScopeSource::resetFrame()
{
    m_waitingBinary = false;
    m_binaryKind = BinaryKind::None;
    m_frame.clear();
    m_expectedBytes = 0U;
    m_expectedCrc = 0U;
    m_expectedSamples = 0U;
    m_expectedSampleRate = 0U;
    m_expectedEnvelopeBins = 0U;
    m_expectedSourceSamples = 0U;
    m_expectedEnvelopeLockMask = 0U;
    m_expectedPeakCount = 0U;
    m_expectedPeakWords = 0U;
    m_expectedPeakPackets = 0U;
    m_expectedPeakGap = 0U;
    m_expectedSnapshotSamples = 0U;
    m_expectedSnapshotSampleBytes = 0U;
    m_expectedSnapshotBinsBytes = 0U;
}

quint32 ZynqScopeSource::crc32(const QByteArray &bytes)
{
    quint32 crc = 0xFFFFFFFFU;
    for (const char value : bytes) {
        crc ^= static_cast<quint8>(value);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFU;
}

void ZynqScopeSource::onError()
{
    m_lastStreamError = m_socket->errorString();
    emit sourceError(m_lastStreamError);
    publishStreamDiagnostics();
}

void ZynqScopeSource::pollArchiveFftStop()
{
    if (m_archiveFftPhase != ArchiveFftPhase::WaitIdle ||
        m_socket->state() != QAbstractSocket::ConnectedState) return;
    if (++m_archiveFftPollCount > 100U) {
        emit sourceError(QStringLiteral("等待板端停止超时；归档 FFT 未执行。"));
        finishArchiveFft(true);
        return;
    }
    m_archiveFftPhase = ArchiveFftPhase::Status;
    sendLine(QStringLiteral("STATUS"));
}

void ZynqScopeSource::finishArchiveFft(bool resumeAcquisition)
{
    m_archiveFftTimer->stop();
    m_archiveFftPhase = ArchiveFftPhase::None;
    m_requestKind = RequestKind::None;
    m_requestStartedAtMs = 0;
    m_waitingBinary = false;
    m_binaryKind = BinaryKind::None;
    m_archiveFftPollCount = 0U;
    const bool disconnectAfter = m_disconnectRequested;
    const bool shouldResume = !disconnectAfter && resumeAcquisition && m_resumeAfterSnapshotFft &&
        m_socket->state() == QAbstractSocket::ConnectedState;
    m_resumeAfterSnapshotFft = false;
    if (disconnectAfter) {
        m_disconnectRequested = false;
        finishDisconnect();
        return;
    }
    if (shouldResume) {
        emit statusChanged(QStringLiteral("归档 FFT 完成；恢复连续采集和实时图谱。"));
        m_startPending = true;
        sendLine(QStringLiteral("START 0"));
    } else {
        m_streaming = false;
    }
}

void ZynqScopeSource::publishStreamDiagnostics()
{
    QString pending = QStringLiteral("none");
    switch (m_requestKind) {
    case RequestKind::Frame: pending = QStringLiteral("SCOPE NEXT"); break;
    case RequestKind::Envelope: pending = QStringLiteral("SCOPE ENVELOPE"); break;
    case RequestKind::Peaks: pending = QStringLiteral("SCOPE PEAKS BATCH"); break;
    case RequestKind::Spectrum: pending = QStringLiteral("SCOPE FFT"); break;
    case RequestKind::Catalog: pending = QStringLiteral("CATALOG"); break;
    case RequestKind::SnapshotSpectrum: pending = QStringLiteral("FFT SNAP WAVE"); break;
    case RequestKind::None: break;
    }
    if (m_requestKind == RequestKind::None) {
        switch (m_archiveFftPhase) {
        case ArchiveFftPhase::DisableScope: pending = QStringLiteral("暂停 SCOPE"); break;
        case ArchiveFftPhase::Stop: pending = QStringLiteral("STOP"); break;
        case ArchiveFftPhase::WaitIdle: pending = QStringLiteral("等待 IDLE"); break;
        case ArchiveFftPhase::Status: pending = QStringLiteral("STATUS"); break;
        case ArchiveFftPhase::Catalog: pending = QStringLiteral("确认 SNAP 目录"); break;
        case ArchiveFftPhase::Transfer: pending = QStringLiteral("归档 FFT 传输"); break;
        case ArchiveFftPhase::None: break;
        }
    }
    const qint64 elapsedMs = m_requestStartedAtMs == 0
        ? 0 : qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_requestStartedAtMs);
    QString message = QStringLiteral(
        "Qt API17：帧%1；包络%2 SNAP#%3 lock=0x%4 NONE=%5 SKIP=%6；FFT%7/%8；事件缺口%9 跳过%10 待取%11；CRC%12 协议%13；待完成=%14 %15ms")
        .arg(m_scopeFrameCount).arg(m_envelopeFrameCount).arg(m_lastEnvelopeSequence)
        .arg(m_lastEnvelopeLockMask, 0, 16).arg(m_envelopeNoneCount).arg(m_envelopeSkipCount)
        .arg(m_liveSpectrumCount).arg(m_archiveSpectrumCount).arg(m_sequenceGaps)
        .arg(m_psSkipped).arg(m_eventBacklogPackets).arg(m_crcErrors).arg(m_protocolErrors)
        .arg(pending).arg(elapsedMs);
    if (!m_lastStreamError.isEmpty())
        message += QStringLiteral("；最近错误：%1").arg(m_lastStreamError);
    emit streamDiagnosticsChanged(message);
}

#pragma once

#include "scope_types.h"

#include <QObject>

class QTcpSocket;
class QTimer;

class ZynqScopeSource final : public QObject
{
    Q_OBJECT
public:
    explicit ZynqScopeSource(QObject *parent = nullptr);

    /* These methods run on this object's worker thread after it is moved. */
    void connectToBoard(const QString &host, quint16 port);
    void disconnectFromBoard();
    void setEventPriority(bool enabled);
    void requestEventNow();
    void setFftChannel(int channel);
    void requestSnapshotCatalog();
    void requestSnapshotFft(quint32 sequence, int channel, quint32 startSample);
    void shutdown();

signals:
    void frameReady(const ScopeFrame &frame);
    void spectrumReady(const ScopeSpectrum &spectrum);
    void phaseEventsReady(const QVector<PdPulse> &events);
    void eventTransportStatus(quint64 first, quint64 next, quint64 psSkipped,
                              quint64 sequenceGaps, quint64 crcErrors);
    void connected();
    void disconnected();
    void statusChanged(const QString &message);
    void sourceError(const QString &message);
    void streamDiagnosticsChanged(const QString &message);
    void snapshotCatalogReady(quint64 first, quint64 next, quint32 state);
    void archiveSpectrumReady(const ScopeArchiveSpectrum &spectrum);

private slots:
    void onConnected();
    void onReadyRead();
    void onError();
    void pollArchiveFftStop();
    void finishArchiveFft(bool resumeAcquisition);
    void pumpRequests();
    void markFrameDue();
    void flushEventBatch();

private:
    enum class BinaryKind { None, Scope, Envelope, Peaks, PeakBatch, Spectrum, SnapshotSpectrum };
    enum class RequestKind { None, Frame, Envelope, Peaks, Spectrum, Catalog, SnapshotSpectrum };
    enum class ArchiveFftPhase { None, DisableScope, Stop, WaitIdle, Status, Catalog, Transfer };

    void sendLine(const QString &line);
    void processLines();
    void processBinary();
    void requestNextFrame();
    void requestNextEnvelope();
    void requestNextPeaks();
    void requestScopeSpectrum();
    void finishRequest(bool emptyPeaks = false);
    void schedulePump(int delayMs = 0);
    void finishDisconnect();
    void resetFrame();
    void publishStreamDiagnostics();
    static quint32 crc32(const QByteArray &bytes);

    QTcpSocket *m_socket = nullptr;
    QTimer *m_pumpTimer = nullptr;
    QTimer *m_frameTimer = nullptr;
    QTimer *m_envelopeTimer = nullptr;
    QTimer *m_eventBatchTimer = nullptr;
    QTimer *m_archiveFftTimer = nullptr;
    QTimer *m_diagnosticTimer = nullptr;
    QByteArray m_rx;
    QByteArray m_frame;
    quint32 m_expectedBytes = 0;
    quint32 m_expectedCrc = 0;
    quint32 m_expectedSamples = 0;
    quint32 m_expectedSampleRate = 0;
    quint32 m_expectedEnvelopeBins = 0;
    quint32 m_expectedSourceSamples = 0;
    quint32 m_expectedEnvelopeLockMask = 0;
    quint64 m_sequence = 0;
    quint64 m_snapshotSequence = 0;
    quint64 m_peakPacketSequence = 0;
    ScopeSpectrum m_spectrumHeader;
    quint32 m_expectedPeakCount = 0;
    quint32 m_expectedPeakWords = 0;
    quint32 m_expectedPeakPackets = 0;
    quint32 m_expectedPeakGap = 0;
    quint32 m_expectedSnapshotSamples = 0;
    quint32 m_expectedSnapshotSampleBytes = 0;
    quint32 m_expectedSnapshotBinsBytes = 0;
    quint32 m_phaseWindows[4] = {0U, 0U, 0U, 0U};
    quint32 m_phaseLockMask = 0U;
    quint64 m_archiveFirst = 0;
    quint64 m_archiveNext = 0;
    quint64 m_eventBacklogPackets = 0;
    quint64 m_psSkipped = 0;
    quint64 m_sequenceGaps = 0;
    quint64 m_crcErrors = 0;
    quint64 m_protocolErrors = 0;
    quint64 m_scopeFrameCount = 0;
    quint64 m_envelopeFrameCount = 0;
    quint64 m_liveSpectrumCount = 0;
    quint64 m_archiveSpectrumCount = 0;
    quint64 m_envelopeNoneCount = 0;
    quint64 m_envelopeSkipCount = 0;
    quint64 m_lastEnvelopeSequence = 0;
    quint32 m_lastEnvelopeLockMask = 0;
    QString m_lastStreamError;
    quint64 m_lastCompletedEventSequence = 0;
    bool m_haveCompletedEventSequence = false;
    QVector<PdPulse> m_pendingEvents;
    BinaryKind m_binaryKind = BinaryKind::None;
    RequestKind m_requestKind = RequestKind::None;
    bool m_waitingBinary = false;
    bool m_streaming = false;
    bool m_startPending = false;
    bool m_configPending = false;
    bool m_scopeOnPending = false;
    bool m_ownsAcquisition = false;
    bool m_scopeEnabled = false;
    bool m_eventPriority = true;
    bool m_frameDue = false;
    bool m_envelopeDue = false;
    bool m_forcePeaksAfterEnvelope = false;
    quint32 m_peaksSinceEnvelope = 0;
    bool m_nextNormalIsFrame = true;
    bool m_manualPeaksPending = false;
    bool m_spectrumPending = false;
    bool m_haveFrame = false;
    int m_fftChannel = 0;
    bool m_currentRequestWasManual = false;
    bool m_disconnectRequested = false;
    bool m_catalogPending = false;
    bool m_snapshotFftPending = false;
    bool m_resumeAfterSnapshotFft = false;
    ArchiveFftPhase m_archiveFftPhase = ArchiveFftPhase::None;
    quint32 m_snapshotFftSequence = 0;
    quint32 m_snapshotFftChannel = 0;
    quint32 m_snapshotFftStart = 0;
    quint32 m_archiveFftPollCount = 0;
    qint64 m_requestStartedAtMs = 0;
    ScopeArchiveSpectrum m_archiveSpectrum;
};

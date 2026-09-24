#pragma once

#include <QObject>
#include <QByteArray>
#include <QTcpSocket>

class PdTcpClient final : public QObject
{
    Q_OBJECT
public:
    explicit PdTcpClient(QObject *parent = nullptr);

    void connectToBoard(const QString &host, quint16 port);
    void disconnectFromBoard();
    bool isConnected() const;
    /* 整份下载进行中：这段期间的 DATA 头每 16 KiB 一条，日志里没必要逐条打印。 */
    bool isWholeSnapshotDownload() const { return m_wholeSnapshot; }
    void sendCommand(const QString &command);
    void downloadRecord(const QString &kind, quint32 index, quint32 offset,
                        quint32 bytes, const QString &destination);
    void downloadRecordBySequence(const QString &kind, quint32 sequence, quint32 offset,
                                  quint32 bytes, const QString &destination);
    void downloadWholeSnapshot(quint32 index, const QString &destination);
    /* 按归档 sequence 下载整份快照。板端对 SNAP 用半开区间 [first,next)
       校验 sequence，比槽下标更安全——槽会被新快照轮转覆盖。 */
    void downloadWholeSnapshotBySequence(quint32 sequence, const QString &destination);

signals:
    void connected();
    void disconnected();
    void textLine(const QString &line);
    void transportError(const QString &message);
    void downloadProgress(qint64 received, qint64 total);
    void downloadComplete(const QString &path, quint32 bytes, quint32 crc32, const QString &kind);
    void downloadFailed(const QString &reason);
    void scopeFrame(const QByteArray &packedSamples, quint32 samples, quint32 sampleRateHz);

private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);

private:
    enum class ReceiveMode { Text, Binary };

    void processTextLines();
    void processBinary();
    void beginNextWholeChunk();
    void resetDownload();
    static quint32 crc32(const QByteArray &bytes);

    QTcpSocket m_socket;
    QByteArray m_rx;
    ReceiveMode m_mode = ReceiveMode::Text;
    QString m_destination;
    QString m_expectedKind;
    quint32 m_expectedBytes = 0;
    quint32 m_expectedCrc = 0;
    quint32 m_scopeSamples = 0;
    quint32 m_scopeSampleRateHz = 0;
    QByteArray m_download;
    bool m_wholeSnapshot = false;
    bool m_wholeSnapshotBySequence = false;
    quint32 m_wholeSnapshotSequence = 0;
    quint32 m_wholeSnapshotIndex = 0;
    quint32 m_wholeBytes = 0;
    quint32 m_wholeOffset = 0;
};

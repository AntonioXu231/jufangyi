#include "pd_tcp_client.h"

#include <QFile>
#include <QNetworkProxy>
#include <QRegularExpression>

PdTcpClient::PdTcpClient(QObject *parent)
    : QObject(parent)
{
    /* Acquisition traffic is a private LAN TCP connection, never an HTTP/SOCKS proxy request. */
    m_socket.setProxy(QNetworkProxy::NoProxy);
    connect(&m_socket, &QTcpSocket::connected, this, &PdTcpClient::connected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &PdTcpClient::disconnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &PdTcpClient::onReadyRead);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &PdTcpClient::onSocketError);
}

void PdTcpClient::connectToBoard(const QString &host, quint16 port)
{
    resetDownload();
    m_rx.clear();
    m_socket.abort();
    m_socket.connectToHost(host, port);
}

void PdTcpClient::disconnectFromBoard()
{
    resetDownload();
    m_socket.disconnectFromHost();
}

bool PdTcpClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

void PdTcpClient::sendCommand(const QString &command)
{
    if (!isConnected()) {
        emit transportError(QStringLiteral("未连接到板端 TCP 服务。"));
        return;
    }
    if (m_mode != ReceiveMode::Text) {
        emit transportError(QStringLiteral("二进制下载尚未完成，不能发送新命令。"));
        return;
    }
    const QByteArray wire = command.trimmed().toUtf8() + '\n';
    if (wire.size() == 1) return;
    m_socket.write(wire);
}

void PdTcpClient::downloadRecord(const QString &kind, quint32 index, quint32 offset,
                                 quint32 bytes, const QString &destination)
{
    if (kind != QStringLiteral("SNAP") && kind != QStringLiteral("EVENT")) {
        emit downloadFailed(QStringLiteral("下载类型只能是 SNAP 或 EVENT。"));
        return;
    }
    if (bytes == 0U || bytes > 16384U) {
        emit downloadFailed(QStringLiteral("单次下载长度必须为 1..16384 字节。"));
        return;
    }
    if (destination.isEmpty()) {
        emit downloadFailed(QStringLiteral("请先选择本地保存文件。"));
        return;
    }
    m_destination = destination;
    m_expectedKind = kind;
    m_expectedBytes = 0;
    m_expectedCrc = 0;
    m_download.clear();
    sendCommand(QStringLiteral("GET %1 %2 %3 %4").arg(kind).arg(index).arg(offset).arg(bytes));
}

void PdTcpClient::downloadRecordBySequence(const QString &kind, quint32 sequence, quint32 offset,
                                           quint32 bytes, const QString &destination)
{
    if (kind != QStringLiteral("SNAP") && kind != QStringLiteral("EVENT")) {
        emit downloadFailed(QStringLiteral("下载类型只能是 SNAP 或 EVENT。"));
        return;
    }
    if (bytes == 0U || bytes > 16384U || destination.isEmpty()) {
        emit downloadFailed(QStringLiteral("序号下载要求有效的保存路径和 1..16384 字节长度。"));
        return;
    }
    m_destination = destination;
    m_expectedKind = kind;
    m_expectedBytes = 0;
    m_expectedCrc = 0;
    m_download.clear();
    /* Sequence addressing prevents a recycled ring slot being misidentified. */
    sendCommand(QStringLiteral("GET %1 SEQ %2 %3 %4")
                .arg(kind).arg(sequence).arg(offset).arg(bytes));
}

void PdTcpClient::downloadWholeSnapshot(quint32 index, const QString &destination)
{
    if (!isConnected()) {
        emit downloadFailed(QStringLiteral("未连接到板端 TCP 服务。"));
        return;
    }
    if (m_mode != ReceiveMode::Text || destination.isEmpty()) {
        emit downloadFailed(QStringLiteral("请等待当前下载完成，并选择本地保存文件。"));
        return;
    }
    m_wholeSnapshot = true;
    m_wholeSnapshotBySequence = false;
    m_wholeSnapshotSequence = 0;
    m_wholeSnapshotIndex = index;
    m_wholeBytes = 0;
    m_wholeOffset = 0;
    m_destination = destination;
    sendCommand(QStringLiteral("SNAP %1").arg(index));
}

void PdTcpClient::downloadWholeSnapshotBySequence(quint32 sequence, const QString &destination)
{
    if (!isConnected()) {
        emit downloadFailed(QStringLiteral("未连接到板端 TCP 服务。"));
        return;
    }
    if (m_mode != ReceiveMode::Text || destination.isEmpty()) {
        emit downloadFailed(QStringLiteral("请等待当前下载完成，并选择本地保存文件。"));
        return;
    }
    m_wholeSnapshot = true;
    m_wholeSnapshotBySequence = true;
    m_wholeSnapshotSequence = sequence;
    m_wholeSnapshotIndex = sequence % 4U; /* 仅用于显示，实际寻址用 sequence */
    m_wholeBytes = 0;
    m_wholeOffset = 0;
    m_destination = destination;
    sendCommand(QStringLiteral("SNAP SEQ %1").arg(sequence));
}

void PdTcpClient::onReadyRead()
{
    m_rx += m_socket.readAll();
    if (m_mode == ReceiveMode::Text) processTextLines();
    if (m_mode == ReceiveMode::Binary) processBinary();
}

void PdTcpClient::processTextLines()
{
    while (m_mode == ReceiveMode::Text) {
        const int newline = m_rx.indexOf('\n');
        if (newline < 0) return;

        const QString line = QString::fromUtf8(m_rx.left(newline)).trimmed();
        m_rx.remove(0, newline + 1);
        /* 元数据有两种头：槽下标模式 "SNAP index=" 与序号模式 "SNAP seq="，
           两者都带 bytes=，这里统一处理。 */
        if (m_wholeSnapshot && m_wholeBytes == 0U &&
            (line.startsWith(QStringLiteral("SNAP index=")) ||
             line.startsWith(QStringLiteral("SNAP seq=")))) {
            const QRegularExpression bytesExpr(QStringLiteral("\\bbytes=(\\d+)"));
            const auto bytesMatch = bytesExpr.match(line);
            if (!bytesMatch.hasMatch() || bytesMatch.captured(1).toUInt() == 0U) {
                emit downloadFailed(QStringLiteral("无法从快照目录取得有效长度：%1").arg(line));
                resetDownload();
                return;
            }
            m_wholeBytes = bytesMatch.captured(1).toUInt();
            QFile output(m_destination);
            if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                emit downloadFailed(QStringLiteral("无法创建文件：%1").arg(m_destination));
                resetDownload();
                return;
            }
            output.close();
            emit textLine(line);
            beginNextWholeChunk();
            return;
        }
        if (m_wholeSnapshot && m_wholeBytes == 0U && line.startsWith(QStringLiteral("ERR "))) {
            emit textLine(line);
            emit downloadFailed(QStringLiteral("完整快照下载被板端拒绝。"));
            resetDownload();
            return;
        }
        if (line.startsWith(QStringLiteral("DATA V2 "))) {
            const QRegularExpression bytesExpr(QStringLiteral("\\bbytes=(\\d+)"));
            const QRegularExpression crcExpr(QStringLiteral("\\bcrc32=([0-9a-fA-F]{8})"));
            const auto bytesMatch = bytesExpr.match(line);
            const auto crcMatch = crcExpr.match(line);
            if (!bytesMatch.hasMatch() || !crcMatch.hasMatch()) {
                emit downloadFailed(QStringLiteral("DATA 头格式错误：%1").arg(line));
                resetDownload();
                return;
            }
            m_expectedBytes = bytesMatch.captured(1).toUInt();
            m_expectedCrc = crcMatch.captured(1).toUInt(nullptr, 16);
            if (m_expectedBytes == 0U || m_expectedBytes > 16384U) {
                emit downloadFailed(QStringLiteral("DATA 头中的字节数无效。"));
                resetDownload();
                return;
            }
            m_mode = ReceiveMode::Binary;
            emit textLine(line);
            return;
        }
        if (line.startsWith(QStringLiteral("SCOPE V1 "))) {
            const QRegularExpression samplesExpr(QStringLiteral("\\bsamples=(\\d+)"));
            const QRegularExpression bytesExpr(QStringLiteral("\\bbytes=(\\d+)"));
            const QRegularExpression rateExpr(QStringLiteral("\\bfs=(\\d+)"));
            const QRegularExpression crcExpr(QStringLiteral("\\bcrc32=([0-9a-fA-F]{8})"));
            const auto samplesMatch = samplesExpr.match(line);
            const auto bytesMatch = bytesExpr.match(line);
            const auto rateMatch = rateExpr.match(line);
            const auto crcMatch = crcExpr.match(line);
            if (!samplesMatch.hasMatch() || !bytesMatch.hasMatch() || !rateMatch.hasMatch() ||
                !crcMatch.hasMatch()) {
                emit downloadFailed(QStringLiteral("SCOPE 帧头格式错误：%1").arg(line));
                resetDownload();
                return;
            }
            m_scopeSamples = samplesMatch.captured(1).toUInt();
            m_expectedBytes = bytesMatch.captured(1).toUInt();
            m_scopeSampleRateHz = rateMatch.captured(1).toUInt();
            m_expectedCrc = crcMatch.captured(1).toUInt(nullptr, 16);
            if (m_scopeSamples == 0U || m_expectedBytes != m_scopeSamples * 6U ||
                m_expectedBytes > 16384U || m_scopeSampleRateHz == 0U) {
                emit downloadFailed(QStringLiteral("SCOPE 帧参数无效。"));
                resetDownload();
                return;
            }
            m_expectedKind = QStringLiteral("SCOPE");
            m_mode = ReceiveMode::Binary;
            emit textLine(line);
            return;
        }
        emit textLine(line);
    }
}

void PdTcpClient::processBinary()
{
    const quint32 remaining = m_expectedBytes - static_cast<quint32>(m_download.size());
    const int take = qMin<int>(m_rx.size(), static_cast<int>(remaining));
    m_download += m_rx.left(take);
    m_rx.remove(0, take);
    /* 实时示波器帧不进下载进度条：它不是"文件下载"，混进去会让进度条每帧乱跳。 */
    if (m_wholeSnapshot)
        emit downloadProgress(m_wholeOffset + m_download.size(), m_wholeBytes);
    else if (m_expectedKind != QStringLiteral("SCOPE"))
        emit downloadProgress(m_download.size(), m_expectedBytes);
    if (static_cast<quint32>(m_download.size()) != m_expectedBytes) return;

    const quint32 actualCrc = crc32(m_download);
    if (actualCrc != m_expectedCrc) {
        emit downloadFailed(QStringLiteral("CRC32 不匹配：板端=%1，本机=%2")
                            .arg(m_expectedCrc, 8, 16, QLatin1Char('0'))
                            .arg(actualCrc, 8, 16, QLatin1Char('0')));
        resetDownload();
        if (!m_rx.isEmpty()) processTextLines();
        return;
    }
    if (m_expectedKind == QStringLiteral("SCOPE")) {
        const QByteArray frame = m_download;
        const quint32 samples = m_scopeSamples;
        const quint32 sampleRateHz = m_scopeSampleRateHz;
        resetDownload();
        emit scopeFrame(frame, samples, sampleRateHz);
        if (!m_rx.isEmpty()) processTextLines();
        return;
    }
    QFile output(m_destination);
    const QIODevice::OpenMode mode = m_wholeSnapshot ? (QIODevice::WriteOnly | QIODevice::Append)
                                                     : QIODevice::WriteOnly;
    if (!output.open(mode)) {
        emit downloadFailed(QStringLiteral("无法写入文件：%1").arg(m_destination));
        resetDownload();
        return;
    }
    output.write(m_download);
    output.close();
    if (m_wholeSnapshot) {
        m_wholeOffset += m_expectedBytes;
        if (m_wholeOffset < m_wholeBytes) {
            m_mode = ReceiveMode::Text;
            m_expectedBytes = 0U;
            m_expectedCrc = 0U;
            m_download.clear();
            beginNextWholeChunk();
            return;
        }
        const QString path = m_destination;
        const quint32 bytes = m_wholeBytes;
        const QString kind = QStringLiteral("SNAP");
        resetDownload();
        emit downloadComplete(path, bytes, 0U, kind);
    } else {
        const QString path = m_destination;
        const quint32 bytes = m_expectedBytes;
        const QString kind = m_expectedKind;
        resetDownload();
        emit downloadComplete(path, bytes, actualCrc, kind);
    }
    if (!m_rx.isEmpty()) processTextLines();
}

void PdTcpClient::beginNextWholeChunk()
{
    const quint32 remaining = m_wholeBytes - m_wholeOffset;
    const quint32 bytes = qMin<quint32>(remaining, 16384U);
    if (m_wholeSnapshotBySequence)
        sendCommand(QStringLiteral("GET SNAP SEQ %1 %2 %3")
                    .arg(m_wholeSnapshotSequence).arg(m_wholeOffset).arg(bytes));
    else
        sendCommand(QStringLiteral("GET SNAP %1 %2 %3")
                    .arg(m_wholeSnapshotIndex).arg(m_wholeOffset).arg(bytes));
}

void PdTcpClient::resetDownload()
{
    m_mode = ReceiveMode::Text;
    m_destination.clear();
    m_expectedKind.clear();
    m_expectedBytes = 0;
    m_expectedCrc = 0;
    m_scopeSamples = 0;
    m_scopeSampleRateHz = 0;
    m_download.clear();
    m_wholeSnapshot = false;
    m_wholeSnapshotBySequence = false;
    m_wholeSnapshotSequence = 0;
    m_wholeSnapshotIndex = 0;
    m_wholeBytes = 0;
    m_wholeOffset = 0;
}

quint32 PdTcpClient::crc32(const QByteArray &bytes)
{
    quint32 crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= static_cast<quint8>(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFU;
}

void PdTcpClient::onSocketError(QAbstractSocket::SocketError)
{
    emit transportError(m_socket.errorString());
}

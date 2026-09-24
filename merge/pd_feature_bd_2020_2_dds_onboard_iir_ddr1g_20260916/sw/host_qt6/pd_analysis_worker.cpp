#include "pd_analysis_worker.h"

#include <QElapsedTimer>
#include <QFile>
#include <QMetaType>
#include <QtEndian>

PdAnalysisWorker::PdAnalysisWorker(QObject *parent) : QObject(parent) {}

void PdAnalysisWorker::registerMetaTypes()
{
    /*
     * queued 连接对未注册的类型只会打印一句运行期警告然后丢弃，
     * 表现为"工作线程明明发了结果，界面却什么都不动"——很难查。
     * 所以这里集中注册，并在自检里断言注册成功。
     */
    qRegisterMetaType<WaveformFramePtr>("WaveformFramePtr");
    qRegisterMetaType<pdsample::ChannelStats>("pdsample::ChannelStats");
    qRegisterMetaType<pdsample::PeakEvent>("pdsample::PeakEvent");
    qRegisterMetaType<QVector<pdsample::PeakEvent>>("QVector<pdsample::PeakEvent>");
    qRegisterMetaType<pddetect::Pulse>("pddetect::Pulse");
    qRegisterMetaType<pddetect::Result>("pddetect::Result");
    qRegisterMetaType<pddetect::Settings>("pddetect::Settings");
    qRegisterMetaType<QVector<QVector<QPointF>>>("QVector<QVector<QPointF>>");
}

void PdAnalysisWorker::setPulseSettings(pddetect::Settings settings)
{
    m_pulseSettings = settings;
}

void PdAnalysisWorker::decodeSnapshotFile(const QString &path, double sampleRateHz,
                                          qint64 sequence, const QString &sourceName,
                                          bool wantSpectrum)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit snapshotDecoded(WaveformFramePtr(), pdsample::ChannelStats(), pddetect::Result(),
                             sequence, sourceName,
                             QStringLiteral("无法读取图谱文件：%1").arg(path));
        return;
    }
    const QByteArray raw = file.readAll();
    decodeSampleBlock(raw, sampleRateHz, sequence, sourceName, wantSpectrum, true);
}

void PdAnalysisWorker::decodeSampleBlock(const QByteArray &raw, double sampleRateHz,
                                         qint64 sequence, const QString &sourceName,
                                         bool wantSpectrum, bool wantPulses)
{
    QElapsedTimer timer;
    timer.start();

    if (raw.size() < 1024 * pdsample::kBytesPerSample ||
        (raw.size() % pdsample::kBlockBytes) != 0) {
        emit snapshotDecoded(WaveformFramePtr(), pdsample::ChannelStats(), pddetect::Result(),
                             sequence, sourceName,
                             QStringLiteral("数据长度不是 24 字节块的整数倍（%1 字节），"
                                            "不猜测、不补齐，未绘图。")
                                 .arg(raw.size()));
        return;
    }

    WaveformFramePtr frame(new pdsample::WaveformFrame);
    if (!pdsample::decodeFrame(raw, sampleRateHz, sequence, *frame)) {
        emit snapshotDecoded(WaveformFramePtr(), pdsample::ChannelStats(), pddetect::Result(),
                             sequence, sourceName,
                             QStringLiteral("6 字节解包失败，未绘图。"));
        return;
    }

    pdsample::ChannelStats stats;
    pdsample::measure(*frame, stats);

    pddetect::Result pulses;
    if (wantPulses) pulses = pddetect::detect(*frame, m_pulseSettings);

    emit logLine(QStringLiteral("[worker] %1 解码完成：%2 点，%3 个脉冲，耗时 %4 ms")
                     .arg(sourceName)
                     .arg(frame->sampleCount)
                     .arg(pulses.totalPulses)
                     .arg(timer.elapsed()));

    emit snapshotDecoded(frame, stats, pulses, sequence, sourceName, QString());

    if (wantSpectrum) computeSpectrum(frame);
}

void PdAnalysisWorker::decodeEventFile(const QString &path, quint32 sequence)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit eventBatchDecoded(QVector<pdsample::PeakEvent>(), sequence, 0,
                               QStringLiteral("无法读取 PRPD 事件文件：%1").arg(path));
        return;
    }
    decodeEventBatch(file.readAll(), sequence);
}

void PdAnalysisWorker::decodeEventBatch(const QByteArray &raw, quint32 sequence)
{
    if (raw.isEmpty() || (raw.size() % 8) != 0) {
        emit eventBatchDecoded(QVector<pdsample::PeakEvent>(), sequence, 0,
                               QStringLiteral("事件文件长度不是 64-bit 事件包的整数倍"
                                              "（%1 字节），未解析。")
                                   .arg(raw.size()));
        return;
    }

    QVector<pdsample::PeakEvent> events;
    events.reserve(raw.size() / 8);
    int unrecognised = 0;
    for (int offset = 0; offset < raw.size(); offset += 8) {
        const quint64 word =
            qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(raw.constData() + offset));
        pdsample::PeakEvent event;
        bool valid = true;
        if (!pdsample::decodePeakEvent(word, event, valid)) {
            /*
             * 周期统计包是正常的，跳过即可；但 type 既非 0 也非 1 说明包格式
             * 与上位机理解不一致，必须计数并报出来，不能当常规情况吞掉。
             */
            if (!valid) ++unrecognised;
            continue;
        }
        if (event.channel < 0 || event.channel >= pdsample::kChannelCount) continue;
        events.append(event);
    }

    emit eventBatchDecoded(events, sequence, unrecognised, QString());
}

void PdAnalysisWorker::detectFramePulses(WaveformFramePtr frame)
{
    if (frame.isNull() || frame->sampleCount <= 0) {
        emit framePulsesReady(pddetect::Result(), frame.isNull() ? -1 : frame->sequence);
        return;
    }
    emit framePulsesReady(pddetect::detect(*frame, m_pulseSettings), frame->sequence);
}

void PdAnalysisWorker::computeSpectrum(WaveformFramePtr frame)
{
    if (frame.isNull() || frame->sampleCount < m_spectrum.size()) {
        emit spectrumReady(QVector<QVector<QPointF>>(), QStringList(), QString(),
                           QStringLiteral("样本不足 %1 点，不做 FFT（不零填充掩盖）。")
                               .arg(m_spectrum.size()));
        return;
    }

    QVector<QVector<QPointF>> spectrum(pdsample::kChannelCount);
    QStringList names;
    QString peakNote;
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        names << QStringLiteral("CH%1").arg(c);
        const pdspectrum::ChannelSpectrum result =
            m_spectrum.analyze(frame->channel[c].constData(), frame->sampleCount, 0,
                               frame->sampleRateHz, pdspectrum::Window::Hann);
        const int bins = result.frequencyHz.size();
        spectrum[c].reserve(bins);
        for (int k = 0; k < bins; ++k)
            spectrum[c].append(QPointF(result.frequencyHz.at(k), result.amplitudeDb.at(k)));
        peakNote += QStringLiteral("CH%1 峰 %2 kHz(%3 码)  ")
                        .arg(c)
                        .arg(result.peakHz / 1e3, 0, 'f', 1)
                        .arg(result.peakCode, 0, 'f', 0);
    }
    emit spectrumReady(spectrum, names, peakNote, QString());
}

void PdAnalysisWorker::writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        emit fileWritten(path, false, QStringLiteral("无法写入：%1").arg(path));
        return;
    }
    const qint64 written = file.write(data);
    file.close();
    emit fileWritten(path, written == data.size(),
                     written == data.size()
                         ? QString()
                         : QStringLiteral("只写入 %1 / %2 字节").arg(written).arg(data.size()));
}

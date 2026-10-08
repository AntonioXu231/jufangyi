#pragma once

#include <QVector>
#include <QMetaType>
#include <QtGlobal>

/* A frame contains one simultaneous sample window for all four channels. */
struct ScopeFrame {
    quint64 sequence = 0;
    double sampleRateHz = 26000000.0;
    bool phaseSynchronized = false;
    bool fullCycleEnvelope = false;
    quint32 phaseLockMask = 0;
    quint32 sourceSampleCount = 0;
    QVector<QVector<qint16>> samples;
    QVector<QVector<qint16>> minimum;
    QVector<QVector<qint16>> maximum;
};

/* Magnitude spectrum calculated by the Zynq PS from the matching live frame. */
struct ScopeSpectrum {
    quint64 sequence = 0;
    int channel = 0;
    bool fromArchive = false;
    quint32 startSample = 0;
    quint32 sampleRateHz = 0;
    quint32 binHz = 0;
    quint32 peakBin = 0;
    quint32 peakHz = 0;
    quint32 amplitudeCode = 0;
    quint32 dcCode = 0;
    QVector<quint16> magnitudes;
};

/* A matched 1024-sample waveform/PS-spectrum window taken from a retained SNAP. */
struct ScopeArchiveSpectrum {
    quint64 snapshotSequence = 0;
    int channel = 0;
    quint32 startSample = 0;
    quint32 snapshotSamples = 0;
    quint32 sampleRateHz = 0;
    quint32 binHz = 0;
    quint32 peakBin = 0;
    quint32 peakHz = 0;
    quint32 amplitudeCode = 0;
    quint32 dcCode = 0;
    QVector<qint16> samples;
    QVector<quint16> magnitudes;
};

/* Raw-window index and PL phase are independent unless the source proves a timestamp mapping. */
struct PdPulse {
    int channel = 0;
    int sampleIndex = 0;
    double phaseDeg = 0.0;
    bool phaseValid = false;
    double amplitude = 0.0;
    quint64 frameSequence = 0;
    quint32 packetWordIndex = 0;
    qint64 receivedAtMs = 0;
};

Q_DECLARE_METATYPE(ScopeFrame)
Q_DECLARE_METATYPE(ScopeSpectrum)
Q_DECLARE_METATYPE(ScopeArchiveSpectrum)
Q_DECLARE_METATYPE(QVector<PdPulse>)

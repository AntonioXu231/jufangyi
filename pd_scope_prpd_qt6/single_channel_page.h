#pragma once

#include "scope_types.h"

#include <QWidget>

class QComboBox;
class FftWidget;
class QLabel;
class WaveformWidget;
class QSpinBox;

class SingleChannelPage final : public QWidget
{
    Q_OBJECT
public:
    explicit SingleChannelPage(QWidget *parent = nullptr);
    void setFrame(const ScopeFrame &frame, const QVector<QVector<PdPulse>> &pulses);
    void setSpectrum(const ScopeSpectrum &spectrum);
    void setArchiveSpectrum(const ScopeArchiveSpectrum &spectrum);
    void setSnapshotCatalog(quint64 first, quint64 next, quint32 state);
    void returnToLiveView();
    void resetZoom();

signals:
    void channelSelected(int channel);
    void snapshotCatalogRequested();
    void snapshotFftRequested(quint32 sequence, int channel, quint32 startSample);

private slots:
    void refreshSelectedChannel();

private:
    QComboBox *m_channelSelect = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_waveTitle = nullptr;
    QLabel *m_fftTitle = nullptr;
    QSpinBox *m_snapshotSequence = nullptr;
    QSpinBox *m_snapshotStart = nullptr;
    WaveformWidget *m_waveform = nullptr;
    FftWidget *m_fft = nullptr;
    ScopeFrame m_frame;
    QVector<QVector<PdPulse>> m_pulses;
    bool m_hasFrame = false;
    ScopeSpectrum m_spectrum;
    ScopeArchiveSpectrum m_archiveSpectrum;
    bool m_hasSpectrum = false;
    bool m_archiveView = false;
};

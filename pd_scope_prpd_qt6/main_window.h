#pragma once

#include "scope_types.h"

#include <QMainWindow>

class DemoSource;
class ZynqScopeSource;
class ChannelPanel;
class EventTableModel;
class SingleChannelPage;
class QDoubleSpinBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QThread;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

private slots:
    void consumeFrame(const ScopeFrame &frame);
    void setRunning(bool running);

private:
    QVector<PdPulse> detectPulses(int channel, const QVector<qint16> &samples,
                                  quint64 frameSequence, bool phaseSynchronized) const;
    bool passesHostThreshold(const PdPulse &event) const;

    DemoSource *m_source = nullptr;
    ZynqScopeSource *m_zynqSource = nullptr;
    QVector<ChannelPanel *> m_panels;
    QDoubleSpinBox *m_threshold = nullptr;
    QDoubleSpinBox *m_phaseThreshold = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_diagnostics = nullptr;
    QLineEdit *m_host = nullptr;
    QSpinBox *m_port = nullptr;
    EventTableModel *m_eventModel = nullptr;
    SingleChannelPage *m_singleChannelPage = nullptr;
    QVector<QVector<PdPulse>> m_latestFramePulses;
    QThread *m_zynqThread = nullptr;
    QPushButton *m_zynqConnectButton = nullptr;
    bool m_zynqConnected = false;
    QString m_transportSummary;
    QString m_lastSourceError;
    /* PL 事件全部是候选；最终显示阈值在 Qt 上位机动态执行。 */
    quint64 m_phaseEventCount = 0;
    quint64 m_phaseEventAccepted = 0;
    quint64 m_phaseEventRejected = 0;
};

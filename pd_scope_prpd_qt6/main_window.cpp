#include "main_window.h"
#include "channel_panel.h"
#include "demo_source.h"
#include "event_table_model.h"
#include "single_channel_page.h"
#include "single_phase_page.h"
#include "zynq_scope_source.h"

#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTableView>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

MainWindow::MainWindow()
{
    qRegisterMetaType<ScopeFrame>("ScopeFrame");
    qRegisterMetaType<ScopeSpectrum>("ScopeSpectrum");
    qRegisterMetaType<ScopeArchiveSpectrum>("ScopeArchiveSpectrum");
    qRegisterMetaType<QVector<PdPulse>>("QVector<PdPulse>");

    setWindowTitle(QStringLiteral("局部放电实时波形与相位图谱 - Qt 6"));
    const QRect available = QApplication::primaryScreen()
                                ? QApplication::primaryScreen()->availableGeometry()
                                : QRect(0, 0, 1600, 1000);
    resize(qMax(640, qMin(1700, available.width() - 24)),
           qMax(560, qMin(1000, available.height() - 48)));
    setStyleSheet(QStringLiteral(
        "QMainWindow{background:#07101b;color:#e9f7ff;}"
        "QLabel{color:#e9f7ff;}"
        "QGroupBox{border:1px solid #057fc5;margin-top:10px;color:#dff5ff;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;}"
        "QPushButton{background:#007dbd;color:white;border:1px solid #21bbff;padding:6px 10px;}"
        "QPushButton:hover{background:#009eea;}"
        "QPushButton:checked{background:#07527a;border:2px solid #68e6ff;}"
        "QDoubleSpinBox,QSpinBox,QLineEdit{background:#152a3b;color:white;border:1px solid #3984ad;padding:3px;}"
        "QComboBox{background:#152a3b;color:white;border:1px solid #3984ad;padding:4px;}"
        "QTabWidget::pane{border:1px solid #057fc5;}"
        "QTabBar::tab{background:#102437;color:#cdefff;padding:8px 16px;border:1px solid #155a7c;}"
        "QTabBar::tab:selected{background:#087fb9;color:white;}"
        "QTableView{background:#08121e;color:#e9f7ff;gridline-color:#24465c;selection-background-color:#07527a;}"
        "QHeaderView::section{background:#087fb9;color:white;padding:4px;border:1px solid #155a7c;}"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(5, 4, 5, 4);
    auto *controlBox = new QGroupBox(QStringLiteral("实时显示控制"), central);
    auto *control = new QGridLayout(controlBox);
    control->setContentsMargins(7, 8, 7, 7);
    control->setHorizontalSpacing(6);
    control->setVerticalSpacing(4);
    m_host = new QLineEdit(QStringLiteral("192.168.1.10"), controlBox);
    m_port = new QSpinBox(controlBox);
    m_port->setRange(1, 65535);
    m_port->setValue(6001);
    m_zynqConnectButton = new QPushButton(QStringLiteral("连接真实 Zynq"), controlBox);
    auto *start = new QPushButton(QStringLiteral("开始模拟实时数据"), controlBox);
    auto *stop = new QPushButton(QStringLiteral("暂停显示"), controlBox);
    auto *manualEvents = new QPushButton(QStringLiteral("手动读事件"), controlBox);
    auto *eventPriority = new QPushButton(QStringLiteral("事件优先"), controlBox);
    eventPriority->setCheckable(true);
    eventPriority->setChecked(true);
    eventPriority->setToolTip(QStringLiteral(
        "优先连续拉取完整 PL 事件包；波形以约 10 帧/秒保持可见。取消勾选恢复波形/事件交替轮询。"));
    auto *eventTableToggle = new QPushButton(QStringLiteral("事件表"), controlBox);
    auto *reset = new QPushButton(QStringLiteral("复位全部缩放"), controlBox);
    m_threshold = new QDoubleSpinBox(controlBox);
    /* Preserve the original waveform threshold behavior and default. */
    m_threshold->setRange(100.0, 2000.0);
    m_threshold->setSingleStep(50.0);
    m_threshold->setValue(850.0);
    m_threshold->setSuffix(QStringLiteral(" raw ADC"));
    m_threshold->setToolTip(QStringLiteral(
        "原有实时波形候选阈值（绝对 ADC 码）。保持原有波形检测行为。"));
    m_phaseThreshold = new QDoubleSpinBox(controlBox);
    m_phaseThreshold->setRange(0.0, 32767.0);
    m_phaseThreshold->setSingleStep(50.0);
    m_phaseThreshold->setValue(0.0);
    m_phaseThreshold->setSuffix(QStringLiteral(" Q8.8 码"));
    m_phaseThreshold->setToolTip(QStringLiteral(
        "上位机 PL 事件显示阈值，按峰值记录中的有符号 Q8.8 原始码比较（256 码对应 1.0 Q 单位），不写入 PL。0 表示显示所有已同步事件；大于 0 时仅筛选图谱显示，原始事件仍留存并显示在事件表。"));
    m_status = new QLabel(QStringLiteral("事件优先模式已选；连接 Zynq 后将连续收取完整 PL 事件包。"),
                          controlBox);
    m_status->setMinimumWidth(220);
    m_status->setWordWrap(true);
    m_diagnostics = new QLabel(QStringLiteral(
        "流诊断：等待连接；实时验收需看到原始帧/完整周期包络/PS FFT 计数递增，事件缺口与 CRC 错保持 0。"),
        controlBox);
    m_diagnostics->setWordWrap(true);
    m_diagnostics->setStyleSheet(QStringLiteral("color:#8fdcff;font-size:11px;"));
    m_latestStreamDiagnostics = m_diagnostics->text();

    control->addWidget(new QLabel(QStringLiteral("Zynq IP："), controlBox), 0, 0);
    control->addWidget(m_host, 0, 1);
    control->addWidget(new QLabel(QStringLiteral("端口："), controlBox), 0, 2);
    control->addWidget(m_port, 0, 3);
    control->addWidget(m_zynqConnectButton, 0, 4);
    control->addWidget(eventPriority, 0, 5);
    control->addWidget(manualEvents, 0, 6);
    control->addWidget(start, 1, 0);
    control->addWidget(stop, 1, 1);
    control->addWidget(new QLabel(QStringLiteral("波形阈值："), controlBox), 1, 2);
    control->addWidget(m_threshold, 1, 3);
    control->addWidget(new QLabel(QStringLiteral("图谱阈值："), controlBox), 1, 4);
    control->addWidget(m_phaseThreshold, 1, 5);
    control->addWidget(eventTableToggle, 1, 6);
    control->addWidget(reset, 1, 7);
    control->addWidget(m_status, 2, 0, 1, 8);
    control->addWidget(m_diagnostics, 3, 0, 1, 8);
    control->setColumnStretch(1, 1);
    layout->addWidget(controlBox);

    auto *views = new QTabWidget(central);
    views->setDocumentMode(true);
    auto *gridHost = new QWidget(views);
    auto *grid = new QGridLayout(gridHost);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(4);
    for (int channel = 0; channel < 4; ++channel) {
        auto *panel = new ChannelPanel(channel, gridHost);
        panel->setPhaseEventThreshold(m_phaseThreshold->value());
        m_panels.append(panel);
        grid->addWidget(panel, channel / 2, channel % 2);
    }
    views->addTab(gridHost, QStringLiteral("四通道实时图谱"));
    m_singleChannelPage = new SingleChannelPage(views);
    views->addTab(m_singleChannelPage, QStringLiteral("单通道波形 + FFT"));
    m_singlePhasePage = new SinglePhasePage(views);
    views->addTab(m_singlePhasePage, QStringLiteral("单通道相位图"));
    layout->addWidget(views, 1);
    setCentralWidget(central);

    m_eventModel = new EventTableModel(this);
    auto *eventTable = new QTableView(this);
    eventTable->setModel(m_eventModel);
    eventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    eventTable->setSelectionMode(QAbstractItemView::SingleSelection);
    eventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    eventTable->setAlternatingRowColors(true);
    eventTable->verticalHeader()->hide();
    eventTable->horizontalHeader()->setStretchLastSection(true);
    eventTable->setColumnWidth(0, 125);
    eventTable->setColumnWidth(1, 105);
    eventTable->setColumnWidth(2, 105);
    eventTable->setColumnWidth(3, 70);
    eventTable->setColumnWidth(4, 105);
    eventTable->setColumnWidth(5, 140);
    auto *eventDock = new QDockWidget(QStringLiteral("PL 峰值事件（滚动保留 15 秒）"), this);
    eventDock->setObjectName(QStringLiteral("plEventTableDock"));
    eventDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    eventDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    eventDock->setWidget(eventTable);
    eventDock->setMinimumHeight(150);
    addDockWidget(Qt::BottomDockWidgetArea, eventDock);
    eventDock->hide();

    m_source = new DemoSource(this);
    m_zynqThread = new QThread(this);
    m_zynqSource = new ZynqScopeSource();
    m_zynqSource->moveToThread(m_zynqThread);
    connect(m_zynqThread, &QThread::finished, m_zynqSource, &QObject::deleteLater);
    m_zynqThread->start();

    connect(m_source, &DemoSource::frameReady, this, &MainWindow::consumeFrame);
    connect(m_zynqSource, &ZynqScopeSource::frameReady, this, &MainWindow::consumeFrame);
    connect(m_zynqSource, &ZynqScopeSource::spectrumReady,
            m_singleChannelPage, &SingleChannelPage::setSpectrum);
    connect(m_zynqSource, &ZynqScopeSource::archiveSpectrumReady,
            m_singleChannelPage, &SingleChannelPage::setArchiveSpectrum);
    connect(m_zynqSource, &ZynqScopeSource::snapshotCatalogReady,
            m_singleChannelPage, &SingleChannelPage::setSnapshotCatalog);
    connect(m_singleChannelPage, &SingleChannelPage::channelSelected, this, [this](int channel) {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource, channel] { source->setFftChannel(channel); },
            Qt::QueuedConnection);
    });
    connect(m_singleChannelPage, &SingleChannelPage::snapshotCatalogRequested, this, [this] {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource] { source->requestSnapshotCatalog(); },
            Qt::QueuedConnection);
    });
    connect(m_singleChannelPage, &SingleChannelPage::snapshotFftRequested, this,
            [this](quint32 sequence, int channel, quint32 startSample) {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource, sequence, channel, startSample] {
                source->requestSnapshotFft(sequence, channel, startSample);
            }, Qt::QueuedConnection);
    });
    connect(m_singleChannelPage, &SingleChannelPage::fullSnapshotRequested, this,
            [this](quint32 sequence, int channel, quint32 startSample) {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource, sequence, channel, startSample] {
                source->requestFullSnapshotFft(sequence, channel, startSample);
            }, Qt::QueuedConnection);
    });
    connect(m_zynqSource, &ZynqScopeSource::phaseEventsReady, this,
            [this](const QVector<PdPulse> &events) {
        QElapsedTimer batchTimer;
        batchTimer.start();
        m_phaseEventCount += static_cast<quint64>(events.size());
        QVector<PdPulse> received = events;
        for (auto &event : received) {
            if (passesHostThreshold(event)) ++m_phaseEventAccepted;
            else ++m_phaseEventRejected;
        }
        m_eventModel->appendEvents(received);
        for (int channel = 0; channel < 4; ++channel) {
            QVector<PdPulse> selected;
            for (const auto &event : received)
                if (event.channel == channel) selected.append(event);
            if (!selected.isEmpty()) {
                m_panels[channel]->appendPhaseEvents(selected);
                m_singlePhasePage->appendPhaseEvents(channel, selected);
            }
        }
        m_status->setText(QStringLiteral(
            "PL峰值 %1；接收时阈值累计通过 %2 / 未通过 %3；15秒事件表 %4 行；%5")
            .arg(m_phaseEventCount).arg(m_phaseEventAccepted).arg(m_phaseEventRejected)
            .arg(m_eventModel->rowCount()).arg(m_transportSummary));
        const qint64 elapsedNs = batchTimer.nsecsElapsed();
        ++m_guiEventBatchCount;
        m_guiEventBatchEvents += static_cast<quint64>(events.size());
        m_guiEventBatchTotalNs += elapsedNs;
        m_guiEventBatchMaxNs = qMax(m_guiEventBatchMaxNs, elapsedNs);
    });
    connect(m_zynqSource, &ZynqScopeSource::eventTransportStatus, this,
            [this](quint64 first, quint64 next, quint64 psSkipped,
                   quint64 sequenceGaps, quint64 crcErrors) {
        m_transportSummary = QStringLiteral("PS保留 [%1,%2)，PS跳过 %3 包，序号缺口 %4，CRC错 %5")
            .arg(first).arg(next).arg(psSkipped).arg(sequenceGaps).arg(crcErrors);
    });
    connect(m_zynqSource, &ZynqScopeSource::connected, this, [this] {
        m_zynqConnected = true;
        m_zynqConnectButton->setText(QStringLiteral("断开 Zynq"));
        m_status->setText(QStringLiteral("TCP 已连接，等待板端 API 18（完整 SNAP + 批量事件 + 全周期包络 + PS FFT）。"));
    });
    connect(m_zynqSource, &ZynqScopeSource::disconnected, this, [this] {
        m_zynqConnected = false;
        m_zynqConnectButton->setText(QStringLiteral("连接真实 Zynq"));
        m_status->setText(QStringLiteral("Zynq TCP 已断开。"));
    });
    connect(m_zynqSource, &ZynqScopeSource::statusChanged, this,
            [this](const QString &message) { m_status->setText(message); });
    connect(m_zynqSource, &ZynqScopeSource::sourceError, this,
            [this](const QString &message) {
        m_lastSourceError = message;
        m_status->setText(QStringLiteral("Zynq 错误：%1").arg(message));
        refreshDiagnostics();
    });
    connect(m_zynqSource, &ZynqScopeSource::streamDiagnosticsChanged, this,
            [this](const QString &message) {
        m_latestStreamDiagnostics = message;
        refreshDiagnostics();
    });

    m_guiPerformanceClock.start();
    auto *guiPerformanceTimer = new QTimer(this);
    guiPerformanceTimer->setInterval(1000);
    connect(guiPerformanceTimer, &QTimer::timeout,
            this, &MainWindow::collectGuiPerformanceMetrics);
    guiPerformanceTimer->start();

    connect(m_zynqConnectButton, &QPushButton::clicked, this, [this] {
        if (m_zynqConnected) {
            QMetaObject::invokeMethod(m_zynqSource,
                [source = m_zynqSource] { source->disconnectFromBoard(); },
                Qt::QueuedConnection);
            return;
        }
        m_source->stop();
        m_phaseEventCount = 0;
        m_phaseEventAccepted = 0;
        m_phaseEventRejected = 0;
        m_transportSummary.clear();
        m_lastSourceError.clear();
        m_eventModel->clear();
        for (auto *panel : m_panels) panel->clearPhaseEvents();
        m_singlePhasePage->clearPhaseEvents();
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource, host = m_host->text(), port = m_port->value()] {
                source->connectToBoard(host, static_cast<quint16>(port));
            }, Qt::QueuedConnection);
    });
    connect(eventPriority, &QPushButton::toggled, this, [this](bool enabled) {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource, enabled] { source->setEventPriority(enabled); },
            Qt::QueuedConnection);
    });
    connect(manualEvents, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource] { source->requestEventNow(); }, Qt::QueuedConnection);
    });
    connect(start, &QPushButton::clicked, this, [this] { setRunning(true); });
    connect(stop, &QPushButton::clicked, this, [this] { setRunning(false); });
    connect(reset, &QPushButton::clicked, this, [this] {
        for (auto *panel : m_panels) panel->resetZoom();
        m_singleChannelPage->resetZoom();
        m_singlePhasePage->resetZoom();
    });
    connect(eventTableToggle, &QPushButton::clicked, this, [eventDock, eventTableToggle] {
        eventDock->setVisible(eventTableToggle->isChecked());
    });
    connect(eventDock, &QDockWidget::visibilityChanged, eventTableToggle,
            &QPushButton::setChecked);
    eventTableToggle->setCheckable(true);
    eventTableToggle->setChecked(false);

    connect(m_phaseThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
        for (auto *panel : m_panels) panel->setPhaseEventThreshold(value);
        m_singlePhasePage->setEventThreshold(value);
        m_eventModel->setThreshold(value);
        m_status->setText(QStringLiteral("上位机图谱阈值为 %1 Q8.8 码；不修改 PL 门限，原始事件仍按 15 秒保留。")
                          .arg(value, 0, 'f', 0));
    });
}

MainWindow::~MainWindow()
{
    if (m_zynqThread && m_zynqThread->isRunning() && m_zynqSource) {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource] { source->shutdown(); }, Qt::BlockingQueuedConnection);
        m_zynqThread->quit();
        m_zynqThread->wait();
    }
}

void MainWindow::setRunning(bool running)
{
    if (running) {
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource] { source->disconnectFromBoard(); }, Qt::QueuedConnection);
        m_source->start();
        m_status->setText(QStringLiteral("演示刷新：50 帧/秒；相位刻度与事件按 15 秒滚动保留。"));
    } else {
        m_source->stop();
        QMetaObject::invokeMethod(m_zynqSource,
            [source = m_zynqSource] { source->disconnectFromBoard(); }, Qt::QueuedConnection);
        m_status->setText(QStringLiteral("显示已暂停；已接收的椭圆刻度保留至各自 15 秒到期。"));
    }
}

QVector<PdPulse> MainWindow::detectPulses(int channel, const QVector<qint16> &samples,
                                          quint64 frameSequence,
                                          bool phaseSynchronized) const
{
    QVector<PdPulse> pulses;
    const double threshold = m_threshold->value();
    int holdoff = 0;
    for (int i = 1; i < samples.size(); ++i) {
        if (holdoff > 0) { --holdoff; continue; }
        const double current = std::abs(static_cast<double>(samples[i]));
        const double previous = std::abs(static_cast<double>(samples[i - 1]));
        if (current >= threshold && previous < threshold) {
            PdPulse pulse;
            pulse.channel = channel;
            pulse.sampleIndex = i;
            pulse.phaseValid = phaseSynchronized;
            if (phaseSynchronized)
                pulse.phaseDeg = 360.0 * i / qMax(1, samples.size() - 1);
            pulse.amplitude = samples[i];
            pulse.frameSequence = frameSequence;
            pulses.append(pulse);
            holdoff = 18;
        }
    }
    return pulses;
}

bool MainWindow::passesHostThreshold(const PdPulse &event) const
{
    return m_phaseThreshold == nullptr || m_phaseThreshold->value() <= 0.0 ||
           std::abs(event.amplitude) >= m_phaseThreshold->value();
}

void MainWindow::refreshDiagnostics()
{
    QString message = m_latestStreamDiagnostics;
    if (!m_lastSourceError.isEmpty())
        message += QStringLiteral("；最近错误：%1").arg(m_lastSourceError);
    if (!m_guiPerformanceSummary.isEmpty())
        message += QStringLiteral("；%1").arg(m_guiPerformanceSummary);
    m_diagnostics->setText(message);
}

void MainWindow::collectGuiPerformanceMetrics()
{
    const qint64 windowNs = qMax<qint64>(1, m_guiPerformanceClock.nsecsElapsed());
    m_guiPerformanceClock.restart();

    WidgetPaintMetrics waveform;
    WidgetPaintMetrics phase;
    for (auto *panel : m_panels) {
        const WidgetPaintMetrics waveformMetrics = panel->takeWaveformPaintMetrics();
        waveform.paintCount += waveformMetrics.paintCount;
        waveform.sampleVisits += waveformMetrics.sampleVisits;
        waveform.eventVisits += waveformMetrics.eventVisits;
        waveform.totalPaintNs += waveformMetrics.totalPaintNs;
        waveform.maxPaintNs = qMax(waveform.maxPaintNs, waveformMetrics.maxPaintNs);
        waveform.maxRetainedEvents =
            qMax(waveform.maxRetainedEvents, waveformMetrics.maxRetainedEvents);

        const WidgetPaintMetrics phaseMetrics = panel->takePhasePaintMetrics();
        phase.paintCount += phaseMetrics.paintCount;
        phase.sampleVisits += phaseMetrics.sampleVisits;
        phase.bucketVisits += phaseMetrics.bucketVisits;
        phase.totalPaintNs += phaseMetrics.totalPaintNs;
        phase.maxPaintNs = qMax(phase.maxPaintNs, phaseMetrics.maxPaintNs);
        phase.maxRetainedEvents =
            qMax(phase.maxRetainedEvents, phaseMetrics.maxRetainedEvents);
    }
    const WidgetPaintMetrics singleChannel = m_singleChannelPage
        ? m_singleChannelPage->takeWaveformPaintMetrics() : WidgetPaintMetrics{};
    const WidgetPaintMetrics singlePhase = m_singlePhasePage
        ? m_singlePhasePage->takePaintMetrics() : WidgetPaintMetrics{};

    const double windowMs = static_cast<double>(windowNs) / 1000000.0;
    const double waveformMaxMs = static_cast<double>(waveform.maxPaintNs) / 1000000.0;
    const double phaseMaxMs = static_cast<double>(phase.maxPaintNs) / 1000000.0;
    const double maximumBatchMs = static_cast<double>(m_guiEventBatchMaxNs) / 1000000.0;
    m_guiPerformanceSummary = QStringLiteral(
        "GUI[%1ms] ev/b=%2/%3 batch tot/max=%4/%5ms; wave tot/max=%6/%7ms paints=%8 samples/events=%9/%10 max/ch=%11; phase tot/max=%12/%13ms paints=%14 samples/buckets=%15/%16; rows=%17")
        .arg(windowMs, 0, 'f', 0)
        .arg(m_guiEventBatchEvents)
        .arg(m_guiEventBatchCount)
        .arg(static_cast<double>(m_guiEventBatchTotalNs) / 1000000.0, 0, 'f', 2)
        .arg(maximumBatchMs, 0, 'f', 2)
        .arg(static_cast<double>(waveform.totalPaintNs) / 1000000.0, 0, 'f', 2)
        .arg(waveformMaxMs, 0, 'f', 2)
        .arg(waveform.paintCount)
        .arg(waveform.sampleVisits)
        .arg(waveform.eventVisits)
        .arg(waveform.maxRetainedEvents)
        .arg(static_cast<double>(phase.totalPaintNs) / 1000000.0, 0, 'f', 2)
        .arg(phaseMaxMs, 0, 'f', 2)
        .arg(phase.paintCount)
        .arg(phase.sampleVisits)
        .arg(phase.bucketVisits)
        .arg(m_eventModel ? m_eventModel->rowCount() : 0);
    if (singleChannel.paintCount > 0U) {
        m_guiPerformanceSummary += QStringLiteral(
            "; single_page tot/max=%1/%2ms paints=%3 samples=%4")
            .arg(static_cast<double>(singleChannel.totalPaintNs) / 1000000.0, 0, 'f', 2)
            .arg(static_cast<double>(singleChannel.maxPaintNs) / 1000000.0, 0, 'f', 2)
            .arg(singleChannel.paintCount)
            .arg(singleChannel.sampleVisits);
    }
    if (singlePhase.paintCount > 0U) {
        m_guiPerformanceSummary += QStringLiteral(
            "; single_phase tot/max=%1/%2ms paints=%3 samples/buckets=%4/%5")
            .arg(static_cast<double>(singlePhase.totalPaintNs) / 1000000.0, 0, 'f', 2)
            .arg(static_cast<double>(singlePhase.maxPaintNs) / 1000000.0, 0, 'f', 2)
            .arg(singlePhase.paintCount)
            .arg(singlePhase.sampleVisits)
            .arg(singlePhase.bucketVisits);
    }

    m_guiEventBatchCount = 0;
    m_guiEventBatchEvents = 0;
    m_guiEventBatchTotalNs = 0;
    m_guiEventBatchMaxNs = 0;
    refreshDiagnostics();
}

void MainWindow::consumeFrame(const ScopeFrame &frame)
{
    if (frame.fullCycleEnvelope) {
        if (frame.minimum.size() != 4 || frame.maximum.size() != 4) return;
        m_singlePhasePage->setEnvelopeFrame(frame);
        for (int channel = 0; channel < 4; ++channel) {
            if (frame.minimum[channel].size() != frame.maximum[channel].size()) return;
            const bool channelPhaseLocked =
                (frame.phaseLockMask & (1U << channel)) != 0U;
            m_panels[channel]->presentEnvelope(frame.minimum[channel], frame.maximum[channel],
                                                frame.sourceSampleCount,
                                                channelPhaseLocked);
        }
        m_status->setText(QStringLiteral(
            "全周期快照 #%1 · %2 原始点/通道 · 四通道包络已刷新 · PL锁相掩码=0x%3 · %4 · %5")
            .arg(frame.sequence)
            .arg(frame.sourceSampleCount)
            .arg(frame.phaseLockMask, 0, 16)
            .arg(QStringLiteral("仅锁相通道绘制相位包络"))
            .arg(m_transportSummary));
        return;
    }
    if (frame.samples.size() != 4) return;
    /* The short SCOPE NEXT window is retained for the matching PS FFT page;
       it is not stretched around the ellipse as if it represented 360 degrees. */
    if (!frame.phaseSynchronized) {
        m_singleChannelPage->setFrame(frame, QVector<QVector<PdPulse>>(4));
        m_status->setText(QStringLiteral("PS FFT 输入短窗 #%1；四通道实时相位图使用全周期快照。")
                              .arg(frame.sequence));
        return;
    }
    m_latestFramePulses.clear();
    m_latestFramePulses.resize(4);
    int pulseCount = 0;
    for (int channel = 0; channel < 4; ++channel) {
        const auto pulses = detectPulses(channel, frame.samples[channel], frame.sequence,
                                         frame.phaseSynchronized);
        m_latestFramePulses[channel] = pulses;
        pulseCount += pulses.size();
        m_panels[channel]->present(frame.samples[channel], pulses, frame.phaseSynchronized);
        m_singlePhasePage->presentFrame(channel, frame.samples[channel], pulses,
                                         frame.phaseSynchronized);
    }
    m_singleChannelPage->setFrame(frame, m_latestFramePulses);
    m_status->setText(QStringLiteral(
        "帧 #%1；波形候选 %2；PL峰值 %3；15秒事件表 %4 行；%5；%6")
        .arg(frame.sequence).arg(pulseCount).arg(m_phaseEventCount)
        .arg(m_eventModel->rowCount())
        .arg(frame.phaseSynchronized ? QStringLiteral("演示波形相位已同步")
                                    : QStringLiteral("原始波形与 PL 事件相位独立显示"))
        .arg(m_transportSummary));
}

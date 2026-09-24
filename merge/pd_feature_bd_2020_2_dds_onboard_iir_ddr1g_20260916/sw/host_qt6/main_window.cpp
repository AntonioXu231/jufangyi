#include "main_window.h"

#include "ellipse_panel.h"
#include "fft_engine.h"
#include "pd_analysis_worker.h"
#include "plot_widget.h"
#include "pd_reply_parse.h"
#include "prpd_panel.h"
#include "scope_stream.h"
#include "scope_widget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollArea>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QFrame>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace {

/* DDS 板测模式的采样率。SCOPE 帧头也会回传 fs，以帧头为准，这里只作兜底。 */
constexpr double kDefaultSampleRateHz = 26000000.0;
/* 单个编排阶段的超时。超时就明确报错退出，不无限等待。 */
constexpr qint64 kStageTimeoutMs = 10000;
/* PRPD 回合的总超时，防止某次回复丢失后链路被永久让出。 */
constexpr qint64 kPrpdRoundTimeoutMs = 5000;
/*
 * 板端日志实测：连续两次 CATALOG 之间 event_seq 前进约 101 条，而归档只有 16 槽。
 * 也就是说一条记录的存活时间只有回合间隔的 ~16%（回合 500 ms 时约 79 ms）。
 * 因此回合策略是"先试最新的、一遇失败就收手"，不做注定失败的补试；
 * 每回合只求一条含峰值的记录就够了（单条 8224 字节记录含约 1027 个峰值字）。
 */
constexpr int kEventProbeLimit = 3;
/* 连续两次 CATALOG 窗口不变判定为归档冻结，退避这么久再试，避免空刷链路。 */
constexpr qint64 kFrozenBackoffMs = 3000;
/* 单通道单批最多绘制的点数：一条记录就含上千个峰值字，全画会把环糊成实心团。 */
constexpr int kMaxPointsPerBatchPerChannel = 1200;
/*
 * 脉冲判据默认值。
 * 绝对阈值 120 码的依据：PD 脉冲增量是 +720/+600/+460/+300 码，而 DDS 背景
 * 相对 2048 的最大偏离只有 42 码（见设计文档 §10.6），120 码能干净地把背景挡在外面。
 * 自适应用 6×sigma（sigma = 1.4826×MAD），两个判据取较大者，界面上两个都显示。
 */
constexpr double kDefaultPulseThresholdCodes = 120.0;
constexpr double kDefaultPulseHighlightCodes = 300.0;
constexpr int kDefaultMaxPulsesDrawn = 256;
/* 日志合并：10 ms 内到达的多条合成一次追加；上限之外只计数不排队。 */
constexpr int kLogFlushIntervalMs = 120;
constexpr int kMaxPendingLogLines = 2000;

QSpinBox *makeSpin(QWidget *parent, int minimum, int maximum, int value, int step = 1)
{
    auto *result = new QSpinBox(parent);
    result->setRange(minimum, maximum);
    result->setSingleStep(step);
    result->setValue(value);
    return result;
}

QDoubleSpinBox *makeDoubleSpin(QWidget *parent, double minimum, double maximum,
                               double value, double step, const QString &suffix)
{
    auto *result = new QDoubleSpinBox(parent);
    result->setRange(minimum, maximum);
    result->setSingleStep(step);
    result->setDecimals(2);
    result->setValue(value);
    result->setSuffix(suffix);
    return result;
}

QString describeState(ScopeStream::State state)
{
    switch (state) {
    case ScopeStream::State::Idle: return QStringLiteral("空闲");
    case ScopeStream::State::CheckingFirmware: return QStringLiteral("检查固件能力");
    case ScopeStream::State::WaitingAcquisition: return QStringLiteral("等待采集运行");
    case ScopeStream::State::Enabling: return QStringLiteral("正在开启 SCOPE");
    case ScopeStream::State::Streaming: return QStringLiteral("取帧中");
    case ScopeStream::State::Paused: return QStringLiteral("已暂停");
    case ScopeStream::State::Fault: return QStringLiteral("故障");
    }
    return QStringLiteral("未知");
}

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("局放采集上位机 - Qt 6（实时示波器 + 相位环）"));
    /* 默认按 1280×720 级别（含 150% 缩放的全高清屏）布局：这是本次的目标下限，
       更大的屏幕可以直接最大化。 */
    resize(1320, 780);
    setStyleSheet(QStringLiteral(
        /*
         * 全局底色：不能只给 QMainWindow 设。子控件（页签页、滚动区视口）会回落到
         * 系统默认的浅色主题，于是控制区中间出现一条白带——上板截图里就能看到。
         */
        "QWidget { background:#101722; color:#e6f4ff; }"
        "QScrollArea { border:none; }"
        "QScrollBar:vertical { background:#152030; width:10px; margin:0; }"
        "QScrollBar::handle:vertical { background:#2f5d7d; min-height:20px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
        "QMainWindow { background:#101722; color:#e6f4ff; }"
        "QGroupBox { border:1px solid #057fc5; margin-top:10px; color:#dff5ff; }"
        "QGroupBox::title { subcontrol-origin:margin; left:9px; padding:0 4px; }"
        "QLineEdit,QSpinBox,QDoubleSpinBox,QComboBox { background:#202c3b; color:#f5fbff;"
        " border:1px solid #3984ad; padding:3px; }"
        "QPushButton { background:#007dbd; color:white; border:1px solid #24b9ff; padding:6px 10px; }"
        "QPushButton:hover { background:#009eea; }"
        "QPushButton:disabled { background:#2b3947; color:#7f8f9d; border-color:#3b4b59; }"
        "QCheckBox { color:#dff5ff; }"
        "QPlainTextEdit { background:#09101a; color:#c8f2ff; border:1px solid #146e9f; }"
        "QTabWidget::pane { border:1px solid #0585ce; }"
        "QTabBar::tab { background:#173149; color:white; padding:7px 16px; }"
        "QTabBar::tab:selected { background:#008ad1; }"));

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    /* ---------------------------------------------------------- 连接 */
    auto *networkBox = new QGroupBox(QStringLiteral("板端 TCP 连接"), central);
    auto *network = new QHBoxLayout(networkBox);
    m_host = new QLineEdit(QStringLiteral("192.168.1.10"), networkBox);
    m_port = makeSpin(networkBox, 1, 65535, 6001);
    m_connectionButton = new QPushButton(QStringLiteral("连接"), networkBox);
    m_state = new QLabel(QStringLiteral("未连接"), networkBox);
    network->addWidget(new QLabel(QStringLiteral("IP:"), networkBox));
    network->addWidget(m_host, 1);
    network->addWidget(new QLabel(QStringLiteral("端口:"), networkBox));
    network->addWidget(m_port);
    network->addWidget(m_connectionButton);
    network->addWidget(m_state);
    layout->addWidget(networkBox);
    connect(m_connectionButton, &QPushButton::clicked, this, &MainWindow::connectOrDisconnect);

    m_stream = new ScopeStream(&m_client, this);

    /* ---------------------------------------------------------- 页签 */
    auto *tabs = new QTabWidget(central);

    /* ===== 页签 1：示波器（波形 + 相位环） ===== */
    auto *livePage = new QWidget(tabs);
    auto *liveLayout = new QVBoxLayout(livePage);

    auto *scopeBox = new QGroupBox(QStringLiteral("示波器控制（波形持续取自板端 SCOPE 协议）"),
                                   livePage);
    auto *scopeGrid = new QGridLayout(scopeBox);

    m_scopeRunButton = new QPushButton(QStringLiteral("开始取帧"), scopeBox);
    m_scopePauseButton = new QPushButton(QStringLiteral("暂停"), scopeBox);
    m_scopeSingleButton = new QPushButton(QStringLiteral("单次"), scopeBox);
    connect(m_scopeRunButton, &QPushButton::clicked, this, &MainWindow::toggleScope);
    connect(m_scopePauseButton, &QPushButton::clicked, this, &MainWindow::scopePause);
    connect(m_scopeSingleButton, &QPushButton::clicked, this, &MainWindow::scopeSingleShot);
    scopeGrid->addWidget(m_scopeRunButton, 0, 0);
    scopeGrid->addWidget(m_scopePauseButton, 0, 1);
    scopeGrid->addWidget(m_scopeSingleButton, 0, 2);

    scopeGrid->addWidget(new QLabel(QStringLiteral("时基（样本/帧）:"), scopeBox), 0, 3);
    m_scopeSamples = makeSpin(scopeBox, 256, 2048, 1024, 4);
    m_scopeSamples->setToolTip(QStringLiteral(
        "板端约束：256..2048 且为 4 的倍数。26 MSPS 下 1024 点 = 39.38 µs 窗口。"));
    scopeGrid->addWidget(m_scopeSamples, 0, 4);

    scopeGrid->addWidget(new QLabel(QStringLiteral("波形模式:"), scopeBox), 0, 5);
    m_scopeMode = new QComboBox(scopeBox);
    m_scopeMode->addItems({QStringLiteral("实时（每帧刷新）"),
                           QStringLiteral("触发（命中才刷新，可对齐）"),
                           QStringLiteral("滚动（按到达顺序拼接）")});
    scopeGrid->addWidget(m_scopeMode, 0, 6, 1, 2);

    scopeGrid->addWidget(new QLabel(QStringLiteral("最小帧间隔 ms:"), scopeBox), 0, 8);
    m_scopeInterval = makeSpin(scopeBox, 0, 1000, 20, 5);
    m_scopeInterval->setToolTip(QStringLiteral(
        "给板端留出处理 DMA 与归档的时间。0 表示不设下限（易把 PS 打满）。"));
    scopeGrid->addWidget(m_scopeInterval, 0, 9);
    m_scopeAutoStart = new QCheckBox(QStringLiteral("必要时自动 START 0"), scopeBox);
    m_scopeAutoStart->setChecked(true);
    scopeGrid->addWidget(m_scopeAutoStart, 0, 10);

    scopeGrid->addWidget(new QLabel(QStringLiteral("触发通道:"), scopeBox), 1, 0);
    m_triggerChannel = new QComboBox(scopeBox);
    m_triggerChannel->addItems({QStringLiteral("CH0"), QStringLiteral("CH1"),
                                QStringLiteral("CH2"), QStringLiteral("CH3")});
    scopeGrid->addWidget(m_triggerChannel, 1, 1);
    scopeGrid->addWidget(new QLabel(QStringLiteral("边沿:"), scopeBox), 1, 2);
    m_triggerEdge = new QComboBox(scopeBox);
    m_triggerEdge->addItems({QStringLiteral("上升"), QStringLiteral("下降")});
    scopeGrid->addWidget(m_triggerEdge, 1, 3);
    scopeGrid->addWidget(new QLabel(QStringLiteral("触发电平（ADC 码）:"), scopeBox), 1, 4);
    m_triggerLevel = makeSpin(scopeBox, 0, 4095, 2048, 8);
    scopeGrid->addWidget(m_triggerLevel, 1, 5);
    scopeGrid->addWidget(new QLabel(QStringLiteral("迟滞:"), scopeBox), 1, 6);
    m_triggerHysteresis = makeSpin(scopeBox, 0, 512, 24, 4);
    m_triggerHysteresis->setToolTip(QStringLiteral(
        "信号必须先离开 level±迟滞 这条带，才允许再次触发，用于抑制阈值附近的抖动误触发。"));
    scopeGrid->addWidget(m_triggerHysteresis, 1, 7);
    m_triggerAlign = new QCheckBox(QStringLiteral("触发对齐（预触发"), scopeBox);
    m_triggerPre = makeSpin(scopeBox, 0, 90, 25, 5);
    auto *alignBox = new QWidget(scopeBox);
    auto *alignLayout = new QHBoxLayout(alignBox);
    alignLayout->setContentsMargins(0, 0, 0, 0);
    alignLayout->addWidget(m_triggerAlign);
    alignLayout->addWidget(m_triggerPre);
    alignLayout->addWidget(new QLabel(QStringLiteral("%）"), scopeBox));
    alignLayout->addStretch(1);
    scopeGrid->addWidget(alignBox, 1, 8, 1, 3);

    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        m_channelVisible[c] = new QCheckBox(ScopeWidget::channelName(c), scopeBox);
        m_channelVisible[c]->setChecked(true);
        m_channelGain[c] = makeDoubleSpin(scopeBox, 0.05, 50.0, 1.0, 0.05, QStringLiteral("×"));
        m_channelOffset[c] = makeSpin(scopeBox, -2048, 2048, 0, 8);
        /* 每个通道一行放三样：显隐、垂直增益、垂直偏移。
           标签改成 tooltip，省下一整行高度——纵向空间比标注更稀缺。 */
        m_channelGain[c]->setToolTip(QStringLiteral("%1 垂直增益（×）").arg(ScopeWidget::channelName(c)));
        m_channelOffset[c]->setToolTip(
            QStringLiteral("%1 垂直偏移（ADC 码）").arg(ScopeWidget::channelName(c)));
        scopeGrid->addWidget(m_channelVisible[c], 2, 0 + c * 3);
        scopeGrid->addWidget(m_channelGain[c], 2, 1 + c * 3);
        scopeGrid->addWidget(m_channelOffset[c], 2, 2 + c * 3);
    }

    m_autoScaleY = new QCheckBox(QStringLiteral("幅度自动量程"), scopeBox);
    m_autoScaleY->setChecked(true);
    auto *resetView = new QPushButton(QStringLiteral("复位视图"), scopeBox);
    auto *fitView = new QPushButton(QStringLiteral("显示全部"), scopeBox);
    connect(resetView, &QPushButton::clicked, this, [this] { m_scope->resetView(); });
    connect(fitView, &QPushButton::clicked, this, [this] { m_scope->fitAll(); });
    m_cursorButton = new QPushButton(QStringLiteral("游标 Δt 测量"), scopeBox);
    m_cursorButton->setCheckable(true);
    connect(m_cursorButton, &QPushButton::toggled, this, [this](bool on) {
        m_scope->setCursorsEnabled(on);
    });
    scopeGrid->addWidget(m_autoScaleY, 3, 0);
    connect(m_autoScaleY, &QCheckBox::toggled, this, [this](bool on) {
        /* 勾选 = 跟随数据自动定标；取消 = 冻结当前量程，之后用 Ctrl+滚轮微调。 */
        if (on) m_scope->setAutoScaleY(true);
        else m_scope->freezeCurrentRange();
    });
    scopeGrid->addWidget(resetView, 3, 1);
    scopeGrid->addWidget(fitView, 3, 2);
    scopeGrid->addWidget(m_cursorButton, 3, 3);
    scopeGrid->addWidget(new QLabel(QStringLiteral("滚动容量:"), scopeBox), 3, 4);
    m_rollCapacity = makeSpin(scopeBox, 4096, 262144, 32768, 4096);
    scopeGrid->addWidget(m_rollCapacity, 3, 5);
    auto *exportCsv = new QPushButton(QStringLiteral("导出 CSV"), scopeBox);
    auto *exportPng = new QPushButton(QStringLiteral("导出 PNG"), scopeBox);
    connect(exportCsv, &QPushButton::clicked, this, &MainWindow::exportScopeCsv);
    connect(exportPng, &QPushButton::clicked, this, &MainWindow::exportScopePng);
    scopeGrid->addWidget(exportCsv, 3, 6);
    scopeGrid->addWidget(exportPng, 3, 7);
    scopeGrid->addWidget(new QLabel(QStringLiteral("CSV 抽样上限:"), scopeBox), 3, 8);
    m_csvPoints = makeSpin(scopeBox, 0, 2000000, 20000, 5000);
    m_csvPoints->setToolTip(QStringLiteral("0 表示不抽样，导出视图内全部采样点。"));
    scopeGrid->addWidget(m_csvPoints, 3, 9);
    m_autoStartScopeOnConnect = new QCheckBox(QStringLiteral("连接后自动取帧"), scopeBox);
    m_autoStartScopeOnConnect->setChecked(true);
    m_autoStartScopeOnConnect->setToolTip(QStringLiteral(
        "连上就进入示波器状态：必要时自动发 START 0，再 SCOPE ON，然后持续取帧。\n"
        "日志会写明它实际发送了什么，不做无提示的板端状态变更。"));
    scopeGrid->addWidget(m_autoStartScopeOnConnect, 3, 10);

    m_scopeState = new QLabel(QStringLiteral("状态：空闲"), scopeBox);
    m_scopeStats = new QLabel(QStringLiteral("帧率 0.0 fps ｜ 等待开始"), scopeBox);
    /*
     * 状态读数是"显示"不是"控件"。它占着一整行会把下面"标注脉冲/检测阈值"那一行
     * 挤到滚动区之外——1280×720 上实测就是这样。所以它不放进控制网格，
     * 而是挂到底部常显信息行（见下面的 infoRow）。
     */
    /* 不进控制网格（见上），只在底部常显行出现。 */
    m_scopeState->hide();
    m_scopeStats->hide();
    /*
     * 控制区放进滚动区。
     * 实测：它的 11 列网格把窗口最小尺寸硬撑到 1550×869，在 1280×720 级别的屏
     * （全高清 + 150% 缩放）上整个界面放不下、相位环被切、日志被挤出屏幕。
     * 滚动区自身的最小尺寸很小，窗口不再被它撑开；内容超出时滚动查看。
     */
    auto *scopeScroll = new QScrollArea(livePage);
    scopeScroll->setWidgetResizable(true);
    scopeScroll->setFrameShape(QFrame::NoFrame);
    scopeScroll->setWidget(scopeBox);
    scopeScroll->setMaximumHeight(214);
    scopeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    liveLayout->addWidget(scopeScroll);

    /* 相位环 + 抓快照工具行 */
    auto *ringBox = new QGroupBox(QStringLiteral("实时相位环与快照抓取"), livePage);
    auto *ringBar = new QHBoxLayout(ringBox);
    m_prpdLiveEnabled = new QCheckBox(QStringLiteral("相位环持续刷新"), ringBox);
    m_prpdLiveEnabled->setChecked(true);
    m_prpdLiveEnabled->setToolTip(QStringLiteral(
        "在取帧间隙自动读一次事件归档（GET EVENT 在采集运行中板端明确允许），\n"
        "用 PL 的 12-bit 相位字段驱动四通道 360° 环。"));
    m_prpdInterval = makeSpin(ringBox, 200, 5000, 500, 100);
    m_prpdInterval->setSuffix(QStringLiteral(" ms"));
    m_ringAging = makeSpin(ringBox, 2, 120, 10, 1);
    m_ringAging->setSuffix(QStringLiteral(" s"));
    m_ringAging->setToolTip(QStringLiteral("环上只保留最近这段时间内的脉冲，形成持续刷新的效果。"));
    auto *refreshRing = new QPushButton(QStringLiteral("立即刷新一次"), ringBox);
    connect(refreshRing, &QPushButton::clicked, this, &MainWindow::startPrpdRound);

    m_grabSnapshotButton = new QPushButton(QStringLiteral("暂停并抓取快照"), ringBox);
    m_grabSnapshotButton->setToolTip(QStringLiteral(
        "停取帧 → STOP 回 IDLE（板端 GET SNAP 要求 IDLE）→ 按 seq 下载最新快照。\n"
        "抓完之后板端处于 IDLE，可直接跑 ANALYZE / FFT SNAP / PRPD BINS。"));
    m_resumeButton = new QPushButton(QStringLiteral("恢复取帧"), ringBox);
    connect(m_grabSnapshotButton, &QPushButton::clicked, this, &MainWindow::grabSnapshot);
    connect(m_resumeButton, &QPushButton::clicked, this, &MainWindow::resumeAfterSnapshot);

    ringBar->addWidget(m_prpdLiveEnabled);
    ringBar->addWidget(new QLabel(QStringLiteral("刷新间隔"), ringBox));
    ringBar->addWidget(m_prpdInterval);
    ringBar->addWidget(new QLabel(QStringLiteral("环老化"), ringBox));
    ringBar->addWidget(m_ringAging);
    ringBar->addWidget(refreshRing);
    ringBar->addSpacing(24);
    ringBar->addWidget(m_grabSnapshotButton);
    ringBar->addWidget(m_resumeButton);
    m_ringStats = new QLabel(QStringLiteral("相位环：等待数据"), ringBox);
    ringBar->addWidget(m_ringStats, 1);
    /*
     * 工具栏并入同一个控制滚动区（第 6 行），不再单独占一行纵向位置。
     * 每省下一行约 30 px，全部给波形与椭圆图谱；内容超高时滚动查看。
     */
    scopeGrid->addWidget(ringBox, 6, 0, 1, 11);

    /* ---------------------------------------------------------- PD 脉冲标注 */
    auto *pulseBox = new QGroupBox(QStringLiteral("局放脉冲标注与图谱联动"), livePage);
    auto *pulseGrid = new QGridLayout(pulseBox);
    pulseGrid->setContentsMargins(6, 4, 6, 4);
    pulseGrid->setHorizontalSpacing(6);

    int column = 0;
    m_pulseOverlay = new QCheckBox(QStringLiteral("标注脉冲"), pulseBox);
    m_pulseOverlay->setChecked(true);
    m_pulseOverlay->setToolTip(QStringLiteral(
        "在波形上画出检出的脉冲（竖直标线 + 极性三角标记）。\n"
        "检测在工作线程完成，界面只负责绘制。"));
    pulseGrid->addWidget(m_pulseOverlay, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("检测阈值"), pulseBox), 0, column++);
    m_pulseThreshold = new QDoubleSpinBox(pulseBox);
    m_pulseThreshold->setRange(0.0, 4095.0);
    m_pulseThreshold->setDecimals(0);
    m_pulseThreshold->setValue(kDefaultPulseThresholdCodes);
    m_pulseThreshold->setSuffix(QStringLiteral(" 码"));
    m_pulseThreshold->setToolTip(QStringLiteral(
        "绝对阈值（ADC 码，相对基线）。与自适应阈值 6×sigma 取较大者生效。\n"
        "背景在这里是 42 码量级，PD 脉冲是 300~720 码，120 足够分开。\n"
        "设为 0 表示只用自适应阈值。"));
    pulseGrid->addWidget(m_pulseThreshold, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("高亮阈值"), pulseBox), 0, column++);
    m_pulseHighlight = new QDoubleSpinBox(pulseBox);
    m_pulseHighlight->setRange(0.0, 4095.0);
    m_pulseHighlight->setDecimals(0);
    m_pulseHighlight->setValue(kDefaultPulseHighlightCodes);
    m_pulseHighlight->setSuffix(QStringLiteral(" 码"));
    m_pulseHighlight->setToolTip(QStringLiteral(
        "超过该幅值的脉冲在波形与椭圆图上用亮色 + 端点标记突出显示；\n"
        "低于它的只作背景（椭圆图上进密度网格）。"));
    pulseGrid->addWidget(m_pulseHighlight, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("纵轴单位"), pulseBox), 0, column++);
    m_pulseUnit = new QComboBox(pulseBox);
    m_pulseUnit->addItems({QStringLiteral("pC（板端口径）"), QStringLiteral("AD 码（波形域）")});
    m_pulseUnit->setToolTip(QStringLiteral(
        "事件包的幅度字段读作 Q8.8 时即板端声明的视在电荷量 pC。\n"
        "换算到 AD 码域：codes = 场值 × 256 / SCALE。"));
    pulseGrid->addWidget(m_pulseUnit, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("SCALE(Q8.8)"), pulseBox), 0, column++);
    m_scaleQ88 = new QDoubleSpinBox(pulseBox);
    m_scaleQ88->setRange(1.0, 65535.0);
    m_scaleQ88->setDecimals(0);
    m_scaleQ88->setValue(pdsample::eventpacket::kDefaultScaleQ88);
    m_scaleQ88->setToolTip(QStringLiteral(
        "板端每通道标定系数 SCALE（偏移 0x10，Q8.8）。上电默认 256 = 1.0。\n"
        "默认值只是占位标定，不是真实标定——界面会标出“pC 未标定”。\n"
        "⚠️ 板端 CONFIG 目前不回传 SCALE，这里需手动与板端一致。"));
    pulseGrid->addWidget(m_scaleQ88, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("脉冲样式"), pulseBox), 0, column++);
    m_pulseStyle = new QComboBox(pulseBox);
    m_pulseStyle->addItems({QStringLiteral("竖直对称线"), QStringLiteral("径向辐条")});
    m_pulseStyle->setToolTip(QStringLiteral(
        "竖直对称线＝参考图那种形态（跨过椭圆在该 x 处的上下边界，再向两端伸出）；\n"
        "径向辐条＝沿离心方向伸出，相位指向更直观。"));
    pulseGrid->addWidget(m_pulseStyle, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("最大标注"), pulseBox), 0, column++);
    m_pulseMaxDrawn = makeSpin(pulseBox, 1, 8192, kDefaultMaxPulsesDrawn, 64);
    m_pulseMaxDrawn->setToolTip(QStringLiteral(
        "单次最多精细绘制多少个脉冲（已确认的优先）。超出会在画面上明确写出截断。"));
    pulseGrid->addWidget(m_pulseMaxDrawn, 0, column++);

    pulseGrid->addWidget(new QLabel(QStringLiteral("频带标注"), pulseBox), 0, column++);
    m_bandEdit = new QLineEdit(QStringLiteral("20kHz-200kHz"), pulseBox);
    m_bandEdit->setMaximumWidth(130);
    m_bandEdit->setToolTip(QStringLiteral(
        "椭圆图头部显示的模拟前端频带，照参考图那种写法。\n"
        "板端协议未回传该信息（由模拟前端硬件决定），所以这里手动填写；\n"
        "纯标注，不参与任何计算。"));
    pulseGrid->addWidget(m_bandEdit, 0, column++);

    /*
     * 并入示波器控制区所在的滚动区（而不是独立占一行纵向位置）：
     * 单靠一个独立 GroupBox 会多吃掉约 60 px 净高，直接后果是波形与椭圆图谱被压扁。
     * 两者同属"采集/显示设置"，放一起也更合逻辑。
     */
    scopeGrid->addWidget(pulseBox, 5, 0, 1, 11);

    /*
     * 主体：上=四通道波形（含 PD 脉冲标注），下=四个椭圆图谱。
     * 两者由同一批事件驱动、上下同时常时更新，正是需求要的"波形与图谱同屏 +
     * 椭圆与相位的对应关系"。椭圆本身支持滚轮缩放/拖拽平移/双击复位。
     */
    auto *liveSplitter = new QSplitter(Qt::Vertical, livePage);
    m_scope = new ScopeWidget(liveSplitter);
    liveSplitter->addWidget(m_scope);

    auto *ellipseHost = new QWidget(liveSplitter);
    auto *ellipseRow = new QHBoxLayout(ellipseHost);
    ellipseRow->setContentsMargins(0, 0, 0, 0);
    ellipseRow->setSpacing(3);
    for (int channel = 0; channel < 4; ++channel) {
        auto *panel = new EllipsePanel(channel, ellipseHost);
        m_ellipses.append(panel);
        ellipseRow->addWidget(panel, 1);
        connect(panel, &EllipsePanel::eventPicked, this,
                [this, channel](int index, double phaseDeg, double adcCodes, bool positive) {
                    onEventPicked(channel, index, phaseDeg, adcCodes, positive);
                });
        /* 面板左下"显隐"按钮与示波器控制区的通道勾选框保持同步。 */
        connect(panel, &EllipsePanel::enableToggled, this, [this, channel](bool enabled) {
            if (m_channelVisible[channel] != nullptr)
                m_channelVisible[channel]->setChecked(enabled);
        });
    }
    liveSplitter->addWidget(ellipseHost);
    /* 波形给得比椭圆多：椭圆是"一眼看形态"，波形要能读细节。 */
    liveSplitter->setStretchFactor(0, 3);
    liveSplitter->setStretchFactor(1, 3);
    liveSplitter->setSizes({400, 400});
    liveLayout->addWidget(liveSplitter, 1);

    /*
     * 底部常显信息行（在滚动区之外，永远可见）：
     * 左＝游标读数，右＝脉冲统计。
     * 脉冲统计放在这里而不是滚动区里，是为了保证"标注脉冲"那一整行控件
     * 不需要滚动就能看到——控制控件被藏起来等于没有。
     */
    auto *infoRow = new QHBoxLayout;
    /* 状态读数与逐帧统计在这里常显（原先占控制网格一整行，会把脉冲控件挤出可视区）。 */
    m_scopeState->show();
    m_scopeStats->show();
    m_scopeState->setParent(livePage);
    m_scopeStats->setParent(livePage);
    m_scopeState->setWordWrap(true);
    m_scopeStats->setWordWrap(true);
    m_scopeState->setMinimumWidth(110);
    m_scopeStats->setMinimumWidth(190);
    infoRow->addWidget(m_scopeState, 0);
    m_cursorReadout = new QLabel(
        QStringLiteral("游标未放置（勾选“游标 Δt 测量”，在波形上右键拖拽放置 A、B）"), livePage);
    m_pulseStats = new QLabel(QStringLiteral("脉冲：等待数据"), livePage);
    /*
     * 这两行文本都很长，而 QLabel 在 wordWrap=false 时 minimumSizeHint 就等于整行文字宽度——
     * 放在滚动区之外会直接把**窗口最小宽度顶到 1668 px**（实测），
     * 1280×720 的屏上整个界面又放不下了。所以必须开换行并给一个小的最小宽度。
     */
    for (QLabel *label : {m_cursorReadout, m_pulseStats}) {
        label->setWordWrap(true);
        label->setMinimumWidth(200);
    }
    /*
     * 三样读数挤在一行：都开了换行且最小宽度很小，所以文本长了会自动折行，
     * 不会把窗口最小宽度顶起来。行数越少，控制区能拿到的高度越多——
     * 1280×720 下正是差这一行才需要滚动才能看到"标注脉冲"。
     */
    infoRow->addWidget(m_cursorReadout, 2);
    infoRow->addWidget(m_scopeStats, 3);
    infoRow->addWidget(m_pulseStats, 4);
    liveLayout->addLayout(infoRow);
    tabs->addTab(livePage, QStringLiteral("示波器：波形 + 椭圆图谱"));

    /* ===== 页签 1b：PRPD 散点（相位-幅值，标准直角坐标呈现） ===== */
    auto *scatterPage = new QWidget(tabs);
    auto *scatterLayout = new QGridLayout(scatterPage);
    scatterLayout->setContentsMargins(2, 2, 2, 2);
    scatterLayout->setSpacing(3);
    for (int channel = 0; channel < 4; ++channel) {
        auto *panel = new PrpdPanel(channel, scatterPage);
        m_prpdPanels.append(panel);
        scatterLayout->addWidget(panel, channel / 2, channel % 2);
    }
    tabs->addTab(scatterPage, QStringLiteral("PRPD 散点（相位-幅值）"));

    /* ===== 页签 2：归档记录波形 ===== */
    m_staticScope = new ScopeWidget(tabs);
    tabs->addTab(m_staticScope, QStringLiteral("归档记录波形（暂停抓取）"));

    /* ===== 页签 3：FFT ===== */
    m_spectrumPlot = new PlotWidget(tabs);
    tabs->addTab(m_spectrumPlot, QStringLiteral("1024 点 FFT"));

    /* ===== 页签 4：PRPD 相位直方图 ===== */
    auto *histPage = new QWidget(tabs);
    auto *histLayout = new QVBoxLayout(histPage);
    auto *histBar = new QHBoxLayout;
    m_prpdChannel = new QComboBox(histPage);
    m_prpdChannel->addItems({QStringLiteral("通道 0"), QStringLiteral("通道 1"),
                             QStringLiteral("通道 2"), QStringLiteral("通道 3")});
    auto *prpdRead = new QPushButton(QStringLiteral("读取 64 桶并绘图"), histPage);
    prpdRead->setToolTip(QStringLiteral(
        "PRPD 查询要求服务 IDLE。请先点“暂停并抓取快照”让板端回到 IDLE，再读取。"));
    connect(prpdRead, &QPushButton::clicked, this, &MainWindow::requestPrpdBins);
    histBar->addWidget(new QLabel(QStringLiteral("通道"), histPage));
    histBar->addWidget(m_prpdChannel);
    histBar->addWidget(prpdRead);
    histBar->addStretch(1);
    histLayout->addLayout(histBar);
    m_prpdPlot = new PlotWidget(histPage);
    histLayout->addWidget(m_prpdPlot, 1);
    tabs->addTab(histPage, QStringLiteral("PRPD 相位直方图"));

    /*
     * 主区用纵向 QSplitter（页签视图 | 日志）。
     * 之前页签与日志各自占固定比例，再叠上底部三个命令框，1020 px 的屏幕
     * 根本放不下：相位环被切掉、日志被挤出可视区。改成可拖动分隔后由用户分配。
     */
    auto *mainSplitter = new QSplitter(Qt::Vertical, central);
    mainSplitter->addWidget(tabs);
    layout->addWidget(mainSplitter, 1);

    /* 「命令与归档」独立成页签：把三个命令框从主纵向布局里搬走，
       否则它们在主区里要吃掉约 150 px 的净高。 */
    auto *cmdPage = new QWidget(tabs);
    auto *cmdLayout = new QVBoxLayout(cmdPage);
    tabs->addTab(cmdPage, QStringLiteral("命令与归档"));

    /* ---------------------------------------------------------- 采集与分析 */
    auto *controlBox = new QGroupBox(QStringLiteral("采集与分析命令"), central);
    auto *controls = new QGridLayout(controlBox);
    const auto commandButton = [this, controlBox](const QString &caption) {
        auto *result = new QPushButton(caption, controlBox);
        connect(result, &QPushButton::clicked, this, [this, caption] { send(caption); });
        return result;
    };
    m_packetCount = makeSpin(controlBox, 0, 1000000, 512);
    controls->addWidget(new QLabel(QStringLiteral("采集包数："), controlBox), 0, 0);
    controls->addWidget(m_packetCount, 0, 1);
    auto *start = new QPushButton(QStringLiteral("START"), controlBox);
    connect(start, &QPushButton::clicked, this, &MainWindow::startAcquisition);
    controls->addWidget(start, 0, 2);
    auto *continuous = new QPushButton(QStringLiteral("连续采集 START 0"), controlBox);
    connect(continuous, &QPushButton::clicked, this, [this] { send(QStringLiteral("START 0")); });
    controls->addWidget(continuous, 0, 3);
    controls->addWidget(commandButton(QStringLiteral("STOP")), 0, 4);
    controls->addWidget(commandButton(QStringLiteral("STATUS")), 0, 5);
    controls->addWidget(commandButton(QStringLiteral("CONFIG")), 0, 6);
    controls->addWidget(commandButton(QStringLiteral("CATALOG")), 1, 0);
    controls->addWidget(commandButton(QStringLiteral("FFT SELFTEST")), 1, 1);
    controls->addWidget(commandButton(QStringLiteral("PRPD SUMMARY")), 1, 2);
    controls->addWidget(commandButton(QStringLiteral("ANALYZE SNAP 0")), 1, 3);
    controls->addWidget(commandButton(QStringLiteral("REPORT")), 1, 4);
    controls->addWidget(commandButton(QStringLiteral("CLEAR")), 1, 5);
    cmdLayout->addWidget(controlBox);

    /* ---------------------------------------------------------- 归档下载 */
    auto *downloadBox = new QGroupBox(QStringLiteral("归档数据下载（CRC32 校验）"), central);
    auto *download = new QHBoxLayout(downloadBox);
    m_downloadKind = new QComboBox(downloadBox);
    m_downloadKind->addItems({QStringLiteral("SNAP"), QStringLiteral("EVENT")});
    m_downloadIndex = makeSpin(downloadBox, 0, 15, 0);
    m_downloadOffset = makeSpin(downloadBox, 0, 0x7fffffff, 0);
    m_downloadBytes = makeSpin(downloadBox, 1, 16384, 16384);
    m_downloadButton = new QPushButton(QStringLiteral("下载本段"), downloadBox);
    connect(m_downloadButton, &QPushButton::clicked, this, &MainWindow::requestDownload);
    m_wholeSnapshotButton = new QPushButton(QStringLiteral("下载完整 SNAP（指定槽）"), downloadBox);
    connect(m_wholeSnapshotButton, &QPushButton::clicked, this, [this] {
        /* 槽下标模式的整份下载同样要走 GET SNAP，板端要求 IDLE。 */
        if (scopeIsActive()) {
            appendLog(QStringLiteral("ERR"),
                      QStringLiteral("取帧进行中：请先点“暂停并抓取快照”，再整份下载。"));
            return;
        }
        const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("保存完整快照"),
            QStringLiteral("snapshot_%1.bin").arg(m_downloadIndex->value()),
            QStringLiteral("Binary data (*.bin);;All files (*)"));
        if (destination.isEmpty()) return;
        /* 这条路径拿不到归档 seq，标成无效值，避免显示成旧序号。 */
        m_snapshotTarget = 0xFFFFFFFFU;
        m_downloadProgress->setValue(0);
        appendLog(QStringLiteral(">>"),
                  QStringLiteral("下载完整 SNAP 槽 %1").arg(m_downloadIndex->value()));
        m_client.downloadWholeSnapshot(m_downloadIndex->value(), destination);
    });
    m_downloadProgress = new QProgressBar(downloadBox);
    m_downloadProgress->setRange(0, 100);
    download->addWidget(new QLabel(QStringLiteral("类型"), downloadBox));
    download->addWidget(m_downloadKind);
    download->addWidget(new QLabel(QStringLiteral("槽"), downloadBox));
    download->addWidget(m_downloadIndex);
    download->addWidget(new QLabel(QStringLiteral("偏移"), downloadBox));
    download->addWidget(m_downloadOffset);
    download->addWidget(new QLabel(QStringLiteral("字节"), downloadBox));
    download->addWidget(m_downloadBytes);
    download->addWidget(m_downloadButton);
    download->addWidget(m_wholeSnapshotButton);
    download->addWidget(m_downloadProgress, 1);
    cmdLayout->addWidget(downloadBox);

    /* ---------------------------------------------------------- 手动命令 */
    auto *manual = new QHBoxLayout;
    m_command = new QLineEdit(central);
    m_command->setPlaceholderText(QStringLiteral(
        "输入任意 TCP 命令，例如：SCOPE ON 512 / GET EVENT SEQ 12 0 128 / PRPD BINS 0 0 8"));
    auto *sendButton = new QPushButton(QStringLiteral("发送命令"), central);
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::sendManualCommand);
    connect(m_command, &QLineEdit::returnPressed, this, &MainWindow::sendManualCommand);
    manual->addWidget(m_command, 1);
    manual->addWidget(sendButton);
    cmdLayout->addLayout(manual);
    cmdLayout->addStretch(1);

    /* ---------------------------------------------------------- 日志 */
    m_log = new QPlainTextEdit(central);
    m_log->setReadOnly(true);
    m_log->document()->setMaximumBlockCount(3000);
    m_log->setMinimumHeight(48);
    mainSplitter->addWidget(m_log);

    /* ---------------------------------------------------------- 日志合并 */
    /*
     * SCOPE 帧 20 ms 一条、快照下载 190 条 DATA 头，若每条都 appendPlainText，
     * 文本布局的开销会与绘制争抢 GUI 线程。这里 120 ms 合并追加一次。
     */
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setSingleShot(true);
    m_logFlushTimer->setInterval(kLogFlushIntervalMs);
    connect(m_logFlushTimer, &QTimer::timeout, this, &MainWindow::flushLog);

    /* ---------------------------------------------------------- 分析工作线程 */
    /*
     * 把"解码 / 统计 / FFT / 脉冲检测 / 大文件 IO"搬出 GUI 线程。
     * 2026-09-23 上板实测的卡死就发生在这些地方：3.12 MB 快照要在事件循环里
     * 解出 200 万个 double 再遍历统计，界面在那几百毫秒里完全无响应。
     */
    PdAnalysisWorker::registerMetaTypes();
    m_analysisThread = new QThread(this);
    m_worker = new PdAnalysisWorker;
    m_worker->moveToThread(m_analysisThread);
    m_analysisThread->start();
    connect(m_worker, &PdAnalysisWorker::snapshotDecoded,
            this, &MainWindow::onSnapshotDecoded);
    connect(m_worker, &PdAnalysisWorker::eventBatchDecoded,
            this, &MainWindow::onEventBatchDecoded);
    connect(m_worker, &PdAnalysisWorker::spectrumReady, this, &MainWindow::onSpectrumReady);
    connect(m_worker, &PdAnalysisWorker::logLine, this,
            [this](const QString &line) { enqueueLog(QStringLiteral("[worker]"), line); });

    /*
     * 首次下发判据与显示设置。
     * 不调用的话椭圆图会一直停在"频带未配置"、高亮阈值用内置默认值，
     * 与界面上的控件显示不一致——用户会以为控件坏了。
     */
    applyPulseSettings();
    mainSplitter->setStretchFactor(0, 4);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({880, 84});
    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("就绪。先连接板端，再点“开始取帧”。"));

    /* ---------------------------------------------------------- 信号接线 */
    m_prpdRoundTimer = new QTimer(this);
    m_prpdRoundTimer->setInterval(m_prpdInterval->value());
    connect(m_prpdRoundTimer, &QTimer::timeout, this, [this] {
        if (m_prpdLiveEnabled->isChecked()) startPrpdRound();
    });

    m_snapshotTimer = new QTimer(this);
    m_snapshotTimer->setInterval(300);
    connect(m_snapshotTimer, &QTimer::timeout, this, &MainWindow::pumpSnapshotGrab);

    connect(&m_client, &PdTcpClient::connected, this, [this] {
        updateConnectionUi(true);
        m_prpdRoundTimer->start();
        startPrpdRound();
        /* "连上就是示波器"：默认自动开始取帧。这一条会在日志里写明，
           避免"没提示就改了板端状态"。 */
        if (m_autoStartScopeOnConnect->isChecked() && !m_stream->isRunning()) {
            appendLog(QStringLiteral(">>"),
                      QStringLiteral("连接后自动开始取帧（可在示波器控制里取消勾选）"));
            applyScopeSettings();
            m_liveFftCounter = 0;
            m_stream->start();
        }
    });
    connect(&m_client, &PdTcpClient::disconnected, this, [this] {
        m_prpdRoundTimer->stop();
        m_snapshotTimer->stop();
        m_snapshotStage = SnapshotStage::Idle;
        updateConnectionUi(false);
    });
    connect(&m_client, &PdTcpClient::textLine, this, &MainWindow::handleLine);
    connect(&m_client, &PdTcpClient::transportError, this,
            [this](const QString &e) { appendLog(QStringLiteral("ERR"), e); });
    connect(&m_client, &PdTcpClient::downloadProgress, this, [this](qint64 now, qint64 all) {
        m_downloadProgress->setValue(all == 0 ? 0 : static_cast<int>(now * 100 / all));
        /* 下载阶段按"有无进展"判超时：每收到一段就刷新计时。 */
        if (m_snapshotStage == SnapshotStage::Downloading)
            m_snapshotStageStartMs = QDateTime::currentMSecsSinceEpoch();
    });
    connect(&m_client, &PdTcpClient::downloadComplete, this,
            [this](const QString &path, quint32 bytes, quint32 crc, const QString &kind) {
        const QString check = crc == 0U ? QStringLiteral("每段 CRC32 已校验")
                                        : QStringLiteral("CRC32=%1").arg(crc, 8, 16, QLatin1Char('0'));
        appendLog(QStringLiteral("OK"), QStringLiteral("下载完成：%1，%2 字节，%3")
                  .arg(path).arg(bytes).arg(check));
        m_downloadProgress->setValue(100);
        if (kind == QStringLiteral("SNAP")) {
            /* SNAP 有两条来路：暂停抓快照编排，以及“下载完整 SNAP（指定槽）”按钮。
               两条都要把数据载入归档记录波形视图，区别只在收尾动作。 */
            const bool fromGrab = (m_snapshotStage == SnapshotStage::Downloading);
            if (fromGrab) {
                m_snapshotStage = SnapshotStage::Idle;
                m_snapshotTimer->stop();
                m_grabSnapshotButton->setEnabled(true);
                /* 板端现在处于 IDLE，归档读取本来就该放开。 */
                setArchiveControlsEnabled(true);
            }
            if (bytes >= 6144U && (bytes % 6U) == 0U) loadSnapshotPlots(path);
            if (fromGrab)
                appendLog(QStringLiteral("OK"),
                          QStringLiteral("快照抓取完成（seq=%1）。板端现处于 IDLE，可直接跑 "
                                         "ANALYZE / FFT SNAP / PRPD BINS；点“恢复取帧”继续示波器。")
                              .arg(m_snapshotTarget));
            return;
        }
        if (kind == QStringLiteral("EVENT")) {
            /*
             * 只提交解码。回合的推进放到 onEventBatchDecoded 里：
             * 解码现在是异步的，若在这里就 nextPrpdCandidate，会和回调里的推进
             * 撞成两次，回合一跳一跳地漏记录。
             */
            loadEventPrpd(path);
        }
    });
    connect(&m_client, &PdTcpClient::downloadFailed, this, [this](const QString &why) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("下载失败：%1").arg(why));
        if (m_eventDownloadInFlight) nextPrpdCandidate();
        if (m_snapshotStage != SnapshotStage::Idle) abortSnapshotGrab(why);
    });

    /* 实时帧 -> 波形控件 */
    /* ---- 脉冲标注设置：任一控件变动就重新下发判据 ---- */
    connect(m_pulseOverlay, &QCheckBox::toggled, this, &MainWindow::applyPulseSettings);
    connect(m_pulseHighlight, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::applyPulseSettings);
    connect(m_pulseUnit, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyPulseSettings);
    connect(m_scaleQ88, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::applyPulseSettings);
    connect(m_pulseStyle, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::applyPulseSettings);
    connect(m_pulseMaxDrawn, qOverload<int>(&QSpinBox::valueChanged),
            this, &MainWindow::applyPulseSettings);
    connect(m_bandEdit, &QLineEdit::textChanged, this, &MainWindow::applyPulseSettings);
    /* 检测阈值会改变判据，需要让工作线程重算静态数据的脉冲。 */
    connect(m_pulseThreshold, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
        applyPulseSettings();
        m_pulseStats->setText(QStringLiteral(
            "脉冲：判据已改；对实时帧立即生效，静态记录需重新抓取快照或重新导出。"));
    });
    connect(m_scope, &ScopeWidget::pulsePicked, this, &MainWindow::onPulsePicked);
    connect(m_staticScope, &ScopeWidget::pulsePicked, this, &MainWindow::onPulsePicked);

    connect(m_stream, &ScopeStream::liveFrame, this,
            [this](const pdsample::WaveformFrame &frame, int triggerIndex, bool valid) {
        m_scope->setLiveFrame(frame, triggerIndex, valid,
                              m_triggerLevel->value(), m_triggerChannel->currentIndex());
        if (m_triggerAlign->isChecked() && valid && triggerIndex >= 0) {
            /* 在半幅视图里把触发点固定到预触发比例位置，使重复事件稳定对齐。 */
            const double span = qMax(64.0, frame.sampleCount * 0.5);
            const double pre = m_triggerPre->value() / 100.0;
            const double start = qBound(0.0, triggerIndex - pre * span,
                                        qMax(0.0, frame.sampleCount - span));
            m_scope->setViewWindow(start, span);
        }
        /*
         * 实时帧只有 1024 点，检测是 O(N) 的直方图统计（约万次整数运算），
         * 留在 GUI 线程比每帧跨线程往返更划算；只有 520,000 点的静态记录
         * 才交给工作线程。
         */
        if (m_pulseOverlay->isChecked()) {
            const pddetect::Result pulses = pddetect::detect(frame, m_pulseSettings);
            m_livePulses = pddetect::flatten(pulses);
            m_scope->setPulses(m_livePulses);
            updatePulseStatistics();
        }

        ++m_liveFftCounter;
        if ((m_liveFftCounter % 4U) == 1U && frame.sampleCount >= 1024)
            refreshSpectrumFor(frame);
    });
    connect(m_stream, &ScopeStream::rollFrame, this,
            [this](const pdsample::WaveformFrame &frame) { m_scope->appendRollFrame(frame); });
    connect(m_stream, &ScopeStream::logLine, this, [this](const QString &line) {
        /* 消息本身已带 "[scope] " 前缀，这里不再加前缀，避免出现 [scope] [scope]。 */
        enqueueLog(QString(), line);
    });
    connect(m_stream, &ScopeStream::inFlightChanged, this, [this](bool) { pumpPrpdRound(); });
    connect(m_stream, &ScopeStream::stateChanged, this,
            [this](ScopeStream::State state, const QString &message) {
        m_scopeState->setText(QStringLiteral("状态：%1").arg(describeState(state)));
        const bool active = (state == ScopeStream::State::Streaming ||
                             state == ScopeStream::State::Enabling ||
                             state == ScopeStream::State::WaitingAcquisition);
        m_scopeRunButton->setText(active ? QStringLiteral("停止取帧")
                                         : QStringLiteral("开始取帧"));
        m_scopePauseButton->setEnabled(active);
        m_scopeSingleButton->setEnabled(!active);
        /* 取快照编排期间由它自己管归档控件，别被这里覆盖。 */
        if (m_snapshotStage == SnapshotStage::Idle) setArchiveControlsEnabled(!active);
        if (!message.isEmpty()) statusBar()->showMessage(message);
    });
    connect(m_stream, &ScopeStream::statisticsChanged, this, &MainWindow::updateScopeStatistics);

    /* 控件变更即时生效 */
    connect(m_scopeSamples, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_stream->setSamples(value);
        if (!m_triggerAlign->isChecked()) m_scope->setTimeSpanSamples(value);
    });
    const auto apply = [this] { applyScopeSettings(); };
    connect(m_scopeMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
    connect(m_scopeInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
    connect(m_scopeAutoStart, &QCheckBox::toggled, this, apply);
    connect(m_rollCapacity, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
    connect(m_triggerChannel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
    connect(m_triggerEdge, QOverload<int>::of(&QComboBox::currentIndexChanged), this, apply);
    connect(m_triggerLevel, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
    connect(m_triggerHysteresis, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
    connect(m_triggerAlign, &QCheckBox::toggled, this, apply);
    connect(m_triggerPre, QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        connect(m_channelVisible[c], &QCheckBox::toggled, this, apply);
        connect(m_channelGain[c], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, apply);
        connect(m_channelOffset[c], QOverload<int>::of(&QSpinBox::valueChanged), this, apply);
    }
    connect(m_scope, &ScopeWidget::cursorChanged, this,
            [this](const QString &readout) { m_cursorReadout->setText(readout); });

    connect(m_prpdLiveEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            m_prpdRoundTimer->start();
            startPrpdRound();
        } else {
            m_prpdRoundTimer->stop();
            if (m_prpdRound != PrpdRound::Idle) finishPrpdRound();
            m_ringStats->setText(QStringLiteral("相位环：已关闭自动刷新（可点“立即刷新一次”）"));
        }
    });
    connect(m_prpdInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        m_prpdRoundTimer->setInterval(value);
    });
    connect(m_ringAging, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [this](int) { applyRingSettings(); });

    applyScopeSettings();
    applyRingSettings();
    updateScopeStatistics();
}

/* ------------------------------------------------------------------ 通用 */

void MainWindow::appendLog(const QString &prefix, const QString &text)
{
    /* 全部走合并队列：QPlainTextEdit 每次追加都要重排文本，逐条追加会把 GUI 拖慢。 */
    enqueueLog(prefix, text);
}

void MainWindow::enqueueLog(const QString &prefix, const QString &text)
{
    if (m_pendingLog.size() < kMaxPendingLogLines)
        m_pendingLog.append(QStringLiteral("%1 %2").arg(prefix, text));
    else
        ++m_pendingLogDropped;
    if (m_logFlushTimer != nullptr && !m_logFlushTimer->isActive())
        m_logFlushTimer->start();
}

void MainWindow::flushLog()
{
    if (!m_pendingLog.isEmpty()) {
        if (m_pendingLogDropped > 0) {
            m_pendingLog.append(QStringLiteral("… 期间另有 %1 条日志因队列上限被丢弃")
                                    .arg(m_pendingLogDropped));
            m_pendingLogDropped = 0;
        }
        m_log->appendPlainText(m_pendingLog.join(QLatin1Char('\n')));
        m_pendingLog.clear();
    }
}

void MainWindow::send(const QString &command)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) return;
    appendLog(QStringLiteral(">>"), trimmed);
    m_client.sendCommand(trimmed);
}

void MainWindow::sendManualCommand()
{
    send(m_command->text());
    m_command->clear();
}

void MainWindow::connectOrDisconnect()
{
    if (m_client.isConnected()) {
        m_stream->stop();
        m_client.disconnectFromBoard();
        return;
    }
    appendLog(QStringLiteral(">>"),
              QStringLiteral("连接 %1:%2").arg(m_host->text()).arg(m_port->value()));
    m_client.connectToBoard(m_host->text(), static_cast<quint16>(m_port->value()));
}

void MainWindow::updateConnectionUi(bool connected)
{
    m_connectionButton->setText(connected ? QStringLiteral("断开") : QStringLiteral("连接"));
    m_state->setText(connected ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    appendLog(QStringLiteral("--"), connected ? QStringLiteral("TCP 已连接，等待板端欢迎行。")
                                              : QStringLiteral("TCP 已断开。"));
}

void MainWindow::startAcquisition()
{
    send(QStringLiteral("START %1").arg(m_packetCount->value()));
}

/* ------------------------------------------------------------------ 实时示波器 */

bool MainWindow::scopeIsActive() const
{
    return m_stream != nullptr && m_stream->isRunning();
}

void MainWindow::applyScopeSettings()
{
    m_stream->setSamples(m_scopeSamples->value());
    m_stream->setMinimumIntervalMs(m_scopeInterval->value());
    m_stream->setAutoStartAcquisition(m_scopeAutoStart->isChecked());
    m_stream->setDisplayMode(static_cast<ScopeStream::DisplayMode>(m_scopeMode->currentIndex()));

    ScopeStream::TriggerSettings trigger;
    trigger.channel = m_triggerChannel->currentIndex();
    trigger.rising = (m_triggerEdge->currentIndex() == 0);
    trigger.level = m_triggerLevel->value();
    trigger.hysteresis = m_triggerHysteresis->value();
    trigger.align = m_triggerAlign->isChecked();
    trigger.alignFraction = m_triggerPre->value() / 100.0;
    m_stream->setTrigger(trigger);

    m_scope->setRollCapacity(m_rollCapacity->value());
    m_scope->setTriggerOverlay(m_scopeMode->currentIndex() == 1, trigger.level, trigger.channel);
    m_scope->setAutoScaleY(m_autoScaleY->isChecked());
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        m_scope->setChannelVisible(c, m_channelVisible[c]->isChecked());
        m_scope->setChannelGain(c, m_channelGain[c]->value());
        m_scope->setChannelOffset(c, m_channelOffset[c]->value());
    }
}

void MainWindow::applyRingSettings()
{
    for (auto *panel : m_prpdPanels) panel->setAgingSeconds(m_ringAging->value());
}

void MainWindow::toggleScope()
{
    if (m_stream->isRunning()) {
        m_stream->stop();
        return;
    }
    applyScopeSettings();
    m_scope->clearData();
    m_liveFftCounter = 0;
    m_stream->start();
}

void MainWindow::scopePause()
{
    m_stream->pause();
}

void MainWindow::scopeSingleShot()
{
    applyScopeSettings();
    m_liveFftCounter = 0;
    m_stream->singleShot();
}

void MainWindow::updateScopeStatistics()
{
    const ScopeStream::Statistics &stats = m_stream->statistics();
    m_scopeStats->setText(
        QStringLiteral("帧率 %1 fps ｜ 已画帧 %2 ｜ seq 跳号 %3 ｜ CRC 失败 %4 ｜ 超时 %5 ｜ "
                       "协议错误 %6 ｜ 触发未命中 %7")
            .arg(stats.framesPerSecond, 0, 'f', 1)
            .arg(stats.frames)
            .arg(stats.sequenceGaps)
            .arg(stats.crcErrors)
            .arg(stats.timeouts)
            .arg(stats.protocolErrors)
            .arg(stats.triggerMisses));
}

void MainWindow::setArchiveControlsEnabled(bool enabled)
{
    m_downloadButton->setEnabled(enabled);
    m_wholeSnapshotButton->setEnabled(enabled);
    m_downloadKind->setEnabled(enabled);
    m_downloadIndex->setEnabled(enabled);
    m_downloadOffset->setEnabled(enabled);
    m_downloadBytes->setEnabled(enabled);
}

void MainWindow::exportScopeCsv()
{
    const pdsample::WaveformFrame frame = m_scope->displayFrame();
    if (frame.sampleCount <= 0) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("当前没有可导出的波形数据。"));
        return;
    }
    int first = 0, count = 0;
    m_scope->visibleSampleRange(first, count);
    if (count <= 0) { first = 0; count = frame.sampleCount; }

    int stride = 1;
    const int cap = m_csvPoints->value();
    if (cap > 0 && count > cap) stride = (count + cap - 1) / cap;

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前视图 CSV"),
        QStringLiteral("pd_scope_%1.csv").arg(QDateTime::currentSecsSinceEpoch()),
        QStringLiteral("CSV (*.csv);;All files (*)"));
    if (path.isEmpty()) return;

    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("无法写入文件：%1").arg(path));
        return;
    }
    /* 带 BOM，Excel 打开中文表头才不会乱码。 */
    output.write("\xEF\xBB\xBF");
    QTextStream stream(&output);
    stream << "# 局放采集上位机导出\n";
    stream << "# 采样率 = " << frame.sampleRateHz << " Hz\n";
    stream << "# 导出样本区间 = [" << first << ", " << (first + count) << ")，抽样步长 = " << stride << "\n";
    if (m_scope->mode() == ScopeWidget::Mode::Roll) {
        stream << "# 警告：滚动模式下横轴是“到达样本序号”，帧与帧之间存在未知间隙，"
                  "此列不代表连续时间。\n";
        stream << "# 如需连续时间轴，请用“暂停并抓取快照”后查看“归档记录波形”。\n";
    } else {
        stream << "# 该记录为单份时间连续数据，time_us 列可信。\n";
    }
    stream << "index,time_us,CH0,CH1,CH2,CH3\n";

    const double intervalUs = frame.sampleIntervalSec * 1e6;
    for (int i = first; i < first + count; i += stride) {
        stream << i << ',' << QString::number(i * intervalUs, 'f', 4);
        for (int c = 0; c < pdsample::kChannelCount; ++c)
            stream << ',' << static_cast<qint64>(frame.channel[c].at(i));
        stream << '\n';
    }
    output.close();
    appendLog(QStringLiteral("OK"), QStringLiteral("已导出 %1 行到 %2")
              .arg((count + stride - 1) / stride).arg(path));
}

void MainWindow::exportScopePng()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出当前视图 PNG"),
        QStringLiteral("pd_scope_%1.png").arg(QDateTime::currentSecsSinceEpoch()),
        QStringLiteral("PNG (*.png);;All files (*)"));
    if (path.isEmpty()) return;
    const QPixmap shot = m_scope->grab();
    if (!shot.save(path)) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("保存 PNG 失败：%1").arg(path));
        return;
    }
    appendLog(QStringLiteral("OK"), QStringLiteral("已保存视图截图：%1（%2×%3）")
              .arg(path).arg(shot.width()).arg(shot.height()));
}

/* ------------------------------------------------------------------ PRPD 回合 */

void MainWindow::startPrpdRound()
{
    if (m_prpdRound != PrpdRound::Idle) return;
    if (!m_client.isConnected()) return;
    if (m_snapshotStage != SnapshotStage::Idle) return; /* 暂停编排期间不抢链路 */
    /* 归档冻结退避：没有新事件时继续按间隔发 CATALOG 只是白刷链路与日志。 */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_prpdBackoffUntilMs) return;

    m_prpdRound = PrpdRound::NeedCatalog;
    m_prpdRoundStartMs = now;
    m_prpdRoundProbed = 0;
    m_prpdRoundFetched = 0;
    m_prpdAddedPoints = 0;
    /* 取帧中就先让出链路；等在途的 SCOPE 帧收完再发命令。 */
    if (m_stream->isRunning()) m_stream->setSuspendRequests(true);
    m_ringStatus = QStringLiteral("读取事件归档…");
    for (auto *panel : m_prpdPanels) panel->setStatusText(m_ringStatus);
    pumpPrpdRound();
}

void MainWindow::pumpPrpdRound()
{
    if (m_prpdRound == PrpdRound::Idle) return;
    if (!m_client.isConnected()) { finishPrpdRound(); return; }

    /* 回合超时保护：任何一步回复丢失都不能让链路被永久让出。 */
    if (QDateTime::currentMSecsSinceEpoch() - m_prpdRoundStartMs > kPrpdRoundTimeoutMs) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("PRPD 回合超时，恢复取帧等下一轮。"));
        /* 必须在这里把在途标志清掉：若某次回复丢失，这个标志会永久为真，
           不但回合停摆，连"暂停抓快照"的编排也会被它一直挡住。 */
        m_eventDownloadInFlight = false;
        m_eventDownloadSequence = 0xFFFFFFFFU;
        finishPrpdRound();
        return;
    }
    /* 链路忙：SCOPE 帧在途或已有别的下载在途，等信号再次驱动。 */
    if (m_stream->isFrameInFlight()) return;
    if (m_catalogRequestPending || m_eventDownloadInFlight) return;

    if (m_prpdRound == PrpdRound::NeedCatalog) {
        m_catalogRequestPending = true;
        /* 回合内的命令不进日志：一回合好几条会把有用信息刷掉；
           回合结束统一打一行摘要。 */
        m_client.sendCommand(QStringLiteral("CATALOG"));
        return;
    }
    /* 每回合只求"一条含峰值的记录"：单条记录就有上千个峰值字，
       再往下试只是把注定失败的往返堆上去。 */
    if (m_prpdRoundFetched == 0 && m_prpdRoundProbed < kEventProbeLimit &&
        !m_prpdEventQueue.isEmpty()) {
        fetchNextPrpdEvent();
        return;
    }
    m_prpdEventQueue.clear();
    finishPrpdRound();
}

void MainWindow::finishPrpdRound()
{
    const bool wasActive = (m_prpdRound != PrpdRound::Idle);
    m_prpdRound = PrpdRound::Idle;
    m_catalogRequestPending = false;
    m_prpdEventQueue.clear();
    if (m_stream->isSuspended()) m_stream->setSuspendRequests(false);
    if (wasActive && (m_prpdRoundProbed > 0 || m_prpdRoundFetched > 0))
        appendLog(QStringLiteral("PRPD"),
                  QStringLiteral("回合：探测 %1 条，取到 %2 条含峰值记录，新增 %3 点，耗时 %4 ms")
                      .arg(m_prpdRoundProbed).arg(m_prpdRoundFetched).arg(m_prpdAddedPoints)
                      .arg(QDateTime::currentMSecsSinceEpoch() - m_prpdRoundStartMs));
}

void MainWindow::nextPrpdCandidate()
{
    m_eventDownloadInFlight = false;
    m_eventDownloadSequence = 0xFFFFFFFFU;
    pumpPrpdRound();
}

void MainWindow::fetchNextPrpdEvent()
{
    if (m_eventDownloadInFlight || m_prpdEventQueue.isEmpty()) return;
    if (m_prpdRoundProbed >= kEventProbeLimit || m_prpdRoundFetched > 0) return;
    m_eventDownloadInFlight = true;
    ++m_prpdRoundProbed;
    m_eventDownloadSequence = m_prpdEventQueue.dequeue();
    /* 静默发送：回合流量不进日志。 */
    m_client.sendCommand(QStringLiteral("EVENT SEQ %1").arg(m_eventDownloadSequence));
}

/* ------------------------------------------------------------------ 暂停抓快照 */

void MainWindow::grabSnapshot()
{
    if (!m_client.isConnected()) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("未连接板端。"));
        return;
    }
    if (m_snapshotStage != SnapshotStage::Idle) return;

    /* 先让出链路并停止取帧，再让板端回到 IDLE。
       板端源码明确：GET SNAP 多段下载要求 IDLE（只有 GET EVENT 允许 RUNNING）。 */
    if (m_prpdRound != PrpdRound::Idle) finishPrpdRound();
    /* 回合的在途标志要一并清掉，否则本流程会被 pumpSnapshotGrab 一直挡住。 */
    m_eventDownloadInFlight = false;
    m_eventDownloadSequence = 0xFFFFFFFFU;
    /* 标记本次暂停的目的：到 IDLE 之后再决定是抓快照还是读直方图。
       另外必须先把阶段标记置好并锁住归档控件，再停取帧：
       m_stream->stop() 会同步发出 stateChanged，那里会按"非取帧"重新使能归档控件。 */
    m_pauseReason = PauseReason::Snapshot;
    m_snapshotStage = SnapshotStage::WaitIdle;
    m_snapshotStageStartMs = QDateTime::currentMSecsSinceEpoch();
    setArchiveControlsEnabled(false);
    m_grabSnapshotButton->setEnabled(false);
    m_stream->stop();
    m_snapshotTimer->start();
    appendLog(QStringLiteral(">>"),
              QStringLiteral("抓取快照：停止取帧 → STOP（板端 GET SNAP 要求 IDLE）"));
    send(QStringLiteral("STOP"));
}

void MainWindow::pumpSnapshotGrab()
{
    if (m_snapshotStage == SnapshotStage::Idle) { m_snapshotTimer->stop(); return; }
    if (!m_client.isConnected()) { abortSnapshotGrab(QStringLiteral("TCP 已断开")); return; }
    if (QDateTime::currentMSecsSinceEpoch() - m_snapshotStageStartMs > kStageTimeoutMs) {
        abortSnapshotGrab(QStringLiteral("该阶段 %1 ms 内无进展").arg(kStageTimeoutMs));
        return;
    }
    /* 链路正在被别人用（在途 SCOPE 帧或事件包）。这不是本阶段的故障，
       把阶段计时往后推，避免把"等链路"误判成"阶段超时"。 */
    if (m_stream->isFrameInFlight() || m_eventDownloadInFlight) {
        m_snapshotStageStartMs = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (m_catalogRequestPending) return;

    switch (m_snapshotStage) {
    case SnapshotStage::WaitIdle:
        send(QStringLiteral("STATUS"));
        break;
    case SnapshotStage::WaitCatalog:
        m_catalogRequestPending = true;
        send(QStringLiteral("CATALOG"));
        break;
    case SnapshotStage::Downloading:
        /*
         * 什么都不做：下载阶段由 downloadProgress 刷新计时、downloadComplete 收盘。
         * 这里以前会无条件 abort，于是明明在正常分段推进的下载被误判成"无进展"——
         * 2026-09-23 上板日志里 3120000 字节的快照下到第 38 段就被误杀，正是这个原因。
         * 超时判定只应发生在最上面的 elapsed 检查里。
         */
        break;
    case SnapshotStage::Idle:
        break;
    }
}

void MainWindow::abortSnapshotGrab(const QString &reason)
{
    m_snapshotTimer->stop();
    m_snapshotStage = SnapshotStage::Idle;
    m_pauseReason = PauseReason::None;
    m_catalogRequestPending = false;
    m_eventDownloadInFlight = false;
    m_eventDownloadSequence = 0xFFFFFFFFU;
    m_grabSnapshotButton->setEnabled(true);
    setArchiveControlsEnabled(true);
    appendLog(QStringLiteral("ERR"), QStringLiteral("快照抓取中止：%1").arg(reason));
    statusBar()->showMessage(QStringLiteral("快照抓取中止：%1").arg(reason));
}

void MainWindow::resumeAfterSnapshot()
{
    if (!m_client.isConnected()) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("未连接板端。"));
        return;
    }
    if (m_snapshotStage != SnapshotStage::Idle) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("快照抓取尚未结束。"));
        return;
    }
    appendLog(QStringLiteral(">>"),
              QStringLiteral("恢复取帧：自动 START 0 + SCOPE ON（已抓到的快照保留在归档，不受影响）"));
    applyScopeSettings();
    m_liveFftCounter = 0;
    m_stream->start();
}

/* ------------------------------------------------------------------ 板端文本回复 */

void MainWindow::handleLine(const QString &line)
{
    /* SCOPE 帧头每帧一条，进日志会把有用信息刷掉；只更新状态栏。 */
    if (line.startsWith(QStringLiteral("SCOPE V1 "))) {
        statusBar()->showMessage(QStringLiteral("实时取帧：%1").arg(line));
    } else if (line.startsWith(QStringLiteral("DATA V2 ")) && m_client.isWholeSnapshotDownload()) {
        /*
         * 整份 3,120,000 字节快照要发约 190 条 DATA 头；逐条进日志会把有用信息淹掉，
         * 而进度条与分段 CRC 校验已经在表达同一件事。只更新状态栏。
         */
        statusBar()->showMessage(QStringLiteral("快照分段下载中：%1").arg(line));
    } else {
        appendLog(QStringLiteral("<<"), line);
    }

    /* ---- 抓快照编排：等 IDLE ---- */
    if (m_snapshotStage == SnapshotStage::WaitIdle) {
        const pdreply::StatusInfo status = pdreply::parseStatus(line);
        if (status.ok) {
            m_snapshotStageStartMs = QDateTime::currentMSecsSinceEpoch();
            if (status.state == 0) {
                if (m_pauseReason == PauseReason::Bins) {
                    /* 目的只是让板端进 IDLE 以便 PRPD 查询：到达即发，然后交回用户。 */
                    m_snapshotStage = SnapshotStage::Idle;
                    m_snapshotTimer->stop();
                    m_pauseReason = PauseReason::None;
                    m_grabSnapshotButton->setEnabled(true);
                    sendPrpdBins(m_pendingBinsChannel);
                    appendLog(QStringLiteral("OK"),
                              QStringLiteral("板端已 IDLE，已发送 64 桶查询（通道 %1）。"
                                             "点“恢复取帧”回到示波器模式。")
                                  .arg(m_pendingBinsChannel));
                    return;
                }
                m_snapshotStage = SnapshotStage::WaitCatalog;
                pumpSnapshotGrab();
            }
            return;
        }
    } else if (m_snapshotStage == SnapshotStage::WaitCatalog) {
        const pdreply::Catalog snap = pdreply::parseCatalog(line);
        if (snap.ok) {
            m_catalogRequestPending = false;
            const quint32 first = snap.snapFirst;
            const quint32 next = snap.snapNext;
            if (next <= first) {
                abortSnapshotGrab(QStringLiteral("归档中还没有可用快照（snap_seq 区间为空）。"
                                                 "请先跑一段采集让事件触发自动快照。"));
                return;
            }
            m_snapshotTarget = next - 1U; /* 取最新的一份 */
            m_snapshotPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath(QStringLiteral("pd_snapshot_seq%1.bin")
                                               .arg(m_snapshotTarget));
            m_snapshotStage = SnapshotStage::Downloading;
            m_snapshotStageStartMs = QDateTime::currentMSecsSinceEpoch();
            appendLog(QStringLiteral(">>"),
                      QStringLiteral("下载最新快照 seq=%1 → %2")
                          .arg(m_snapshotTarget).arg(m_snapshotPath));
            m_downloadProgress->setValue(0);
            m_client.downloadWholeSnapshotBySequence(m_snapshotTarget, m_snapshotPath);
            return;
        }
    }

    /* ---- 相位环：PRPD 回合的 CATALOG ---- */
    const pdreply::Catalog catalog = pdreply::parseCatalog(line);
    if (m_catalogRequestPending && catalog.ok) {
        const quint32 first = catalog.eventFirst;
        const quint32 next = catalog.eventNext;
        m_catalogRequestPending = false;
        if (m_prpdRound != PrpdRound::Idle) m_prpdRound = PrpdRound::FetchingEvents;

        /* 归档冻结检测：连续两次窗口一模一样，说明没有新事件产生
           （采集已停，或事件流本身没有推进）。此前会在这种情况下按间隔
           一直空发 CATALOG，把日志刷满却拿不到任何新数据。 */
        if (first == m_lastEventFirst && next == m_lastEventNext) {
            ++m_frozenWindowCount;
            if (m_frozenWindowCount >= 2) {
                m_prpdBackoffUntilMs = QDateTime::currentMSecsSinceEpoch() + kFrozenBackoffMs;
                m_ringStatus = QStringLiteral("事件归档已冻结（无新事件）");
                for (auto *panel : m_prpdPanels) panel->setStatusText(m_ringStatus);
                m_ringStats->setText(
                    QStringLiteral("相位环：归档冻结，%1 s 后重试（点“立即刷新一次”可强制）")
                        .arg(kFrozenBackoffMs / 1000));
            }
        } else {
            m_frozenWindowCount = 0;
            m_prpdBackoffUntilMs = 0;
            m_lastEventFirst = first;
            m_lastEventNext = next;
        }

        if (next > first && m_frozenWindowCount < 2) {
            /* 板端序号重置后需要整体重载（例如重新上电或 CLEAR）。 */
            if (!m_loadedPrpdSequences.isEmpty() &&
                next <= *std::max_element(m_loadedPrpdSequences.begin(),
                                          m_loadedPrpdSequences.end())) {
                m_loadedPrpdSequences.clear();
                for (auto *panel : m_prpdPanels) panel->clear();
            }
            /*
             * 候选按"最新优先"排队。实测归档寿命只有几十毫秒
             * （两回合之间 event_seq 前进约 101 条，归档仅 16 槽），
             * 所以先试最新的；一遇失败或取到一条就收手，
             * 不做注定失效的补试——那只是白占链路往返。
             */
            int queued = 0;
            for (quint32 upper = next; upper > first && queued < kEventProbeLimit; --upper) {
                const quint32 sequence = upper - 1U;
                if (m_loadedPrpdSequences.contains(sequence) ||
                    m_prpdEventQueue.contains(sequence))
                    continue;
                m_prpdEventQueue.enqueue(sequence);
                ++queued;
            }
        }
        pumpPrpdRound();
        return;
    }

    /* ---- 相位环：事件元数据 → 下载该事件包 ---- */
    const pdreply::EventMeta event = pdreply::parseEventMeta(line);
    if (event.ok && event.sequence == m_eventDownloadSequence) {
        const quint32 bytes = event.bytes;
        const quint32 peaks = event.peaks;
        /* 元数据已确认这个序号有效，记下来就不会重复探测它。 */
        m_loadedPrpdSequences.insert(m_eventDownloadSequence);
        /*
         * peaks=0 的记录里只有一个周期统计字，没有任何峰值事件。
         * 板端日志里这类记录占了相当比例（bytes=8 peaks=0 cycles=1），
         * 直接跳过就省掉一次毫无收益的二进制下载。
         */
        if (peaks == 0U || bytes <= 8U) {
            nextPrpdCandidate();
            return;
        }
        const QString target = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("pd_prpd_event_%1.bin")
                                                 .arg(m_eventDownloadSequence));
        m_client.downloadRecordBySequence(QStringLiteral("EVENT"), m_eventDownloadSequence,
                                          0U, bytes, target);
        return;
    }
    if (m_eventDownloadInFlight &&
        (line.startsWith(QStringLiteral("ERR EVENT")) ||
         line.startsWith(QStringLiteral("ERR GET EVENT")))) {
        /* 序号已被环形覆盖：换下一条候选。不清空已有画面——覆盖是预期行为。 */
        nextPrpdCandidate();
        return;
    }

    /* ---- PRPD 直方图（需要 IDLE） ---- */
    const pdreply::PrpdBins bins = pdreply::parsePrpdBins(line);
    if (bins.ok && bins.channel == m_prpdChannel->currentIndex()) {
        if (m_prpdBins.size() != 64) m_prpdBins.fill(0U, 64);
        if (bins.first + bins.count <= m_prpdBins.size()) {
            for (int i = 0; i < bins.count; ++i)
                m_prpdBins[bins.first + i] = bins.values.at(i);
            m_prpdPlot->setBars(m_prpdBins,
                                QStringLiteral("PRPD 相位分布 - 通道 %1").arg(bins.channel),
                                QStringLiteral("相位桶（0..63）"), QStringLiteral("峰值事件数"));
        }
    }
    if (line.startsWith(QStringLiteral("STATUS ")) || line.startsWith(QStringLiteral("REPORT ")))
        statusBar()->showMessage(line);
}

/* ------------------------------------------------------------------ 归档下载与绘图 */

void MainWindow::requestDownload()
{
    if (scopeIsActive()) {
        appendLog(QStringLiteral("ERR"),
                  QStringLiteral("取帧进行中：请先点“暂停并抓取快照”，板端同一时刻只允许一个二进制传输。"));
        return;
    }
    const QString kind = m_downloadKind->currentText();
    const QString suggested = QStringLiteral("%1_%2_%3.bin")
        .arg(kind.toLower()).arg(m_downloadIndex->value()).arg(m_downloadOffset->value());
    const QString destination = QFileDialog::getSaveFileName(this, QStringLiteral("保存归档分段"),
                                                              suggested,
                                                              QStringLiteral("Binary data (*.bin);;All files (*)"));
    if (destination.isEmpty()) return;
    m_downloadProgress->setValue(0);
    appendLog(QStringLiteral(">>"), QStringLiteral("GET %1 %2 %3 %4")
              .arg(kind).arg(m_downloadIndex->value()).arg(m_downloadOffset->value())
              .arg(m_downloadBytes->value()));
    m_client.downloadRecord(kind, m_downloadIndex->value(), m_downloadOffset->value(),
                            m_downloadBytes->value(), destination);
}

void MainWindow::sendPrpdBins(int channel)
{
    /* 板端一次最多回 8 个桶，64 桶需要 8 次请求。 */
    for (int first = 0; first < 64; first += 8)
        send(QStringLiteral("PRPD BINS %1 %2 8").arg(channel).arg(first));
}

void MainWindow::requestPrpdBins()
{
    if (!m_client.isConnected()) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("未连接板端。"));
        return;
    }
    if (m_snapshotStage != SnapshotStage::Idle) {
        appendLog(QStringLiteral("ERR"), QStringLiteral("已有一个暂停流程在进行中。"));
        return;
    }
    const int channel = m_prpdChannel->currentIndex();
    m_prpdBins.fill(0U, 64);

    /* 板端 PRPD 查询要求 IDLE，而"取帧中"就意味着 RUNNING。
       以前这里只回一句"请先暂停"，用户点几次都拿不到数据；
       现在直接替用户走完"停取帧 -> STOP -> 等 IDLE -> 发查询"。 */
    if (!scopeIsActive()) {
        sendPrpdBins(channel);
        return;
    }
    appendLog(QStringLiteral(">>"),
              QStringLiteral("读取相位直方图需要板端 IDLE：自动停取帧并 STOP，"
                             "到 IDLE 后自动发查询。"));
    if (m_prpdRound != PrpdRound::Idle) finishPrpdRound();
    m_eventDownloadInFlight = false;
    m_eventDownloadSequence = 0xFFFFFFFFU;
    m_pendingBinsChannel = channel;
    m_pauseReason = PauseReason::Bins;
    m_snapshotStage = SnapshotStage::WaitIdle;
    m_snapshotStageStartMs = QDateTime::currentMSecsSinceEpoch();
    m_snapshotTimer->start();
    m_stream->stop();
    send(QStringLiteral("STOP"));
}

void MainWindow::loadSnapshotPlots(const QString &path)
{
    /*
     * 全流程交给工作线程：读 3.12 MB 文件 → decodeFrame 解 200 万个 double →
     * measure 再遍历 200 万点 → 脉冲检测 → 4 次 FFT。
     * 之前这些都在 GUI 线程里连着做，一次阻塞数百毫秒，是"暂停抓取之后卡死"的主因。
     */
    appendLog(QStringLiteral(">>"), QStringLiteral("归档快照解码已提交工作线程：%1").arg(path));
    QMetaObject::invokeMethod(m_worker, "decodeSnapshotFile", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(double, kDefaultSampleRateHz),
                              Q_ARG(qint64, static_cast<qint64>(m_snapshotTarget)),
                              Q_ARG(QString, QStringLiteral("归档快照（时间连续）")),
                              Q_ARG(bool, true));
}


void MainWindow::refreshSpectrumFor(const pdsample::WaveformFrame &frame)
{
    /* FFT 也搬去工作线程：4 通道 radix-2 FFT 虽只需微秒级，但没必要占着 GUI 线程。 */
    if (m_worker == nullptr) return;
    WaveformFramePtr ptr(new pdsample::WaveformFrame(frame));
    QMetaObject::invokeMethod(m_worker, "computeSpectrum", Qt::QueuedConnection,
                              Q_ARG(WaveformFramePtr, ptr));
}

void MainWindow::loadEventPrpd(const QString &path)
{
    /*
     * 事件包解码（8224 字节 → 约 1027 个字）与文件 IO 一起交给工作线程；
     * 拿到结果后由 onEventBatchDecoded 分发给椭圆图谱与散点图。
     */
    if (m_worker == nullptr) return;
    QMetaObject::invokeMethod(m_worker, "decodeEventFile", Qt::QueuedConnection,
                              Q_ARG(QString, path),
                              Q_ARG(quint32, m_eventDownloadSequence));
}


/* ==================================================================
 * 工作线程回传处理
 *
 * 这三个槽全部在 GUI 线程执行，只做"把成品交给控件"，不做任何解码/计算。
 * 所有 O(样本数) 的活都在 PdAnalysisWorker 里完成。
 * ================================================================== */

MainWindow::~MainWindow()
{
    /* 有序收线程：worker 的槽可能还在跑，先 quit 再等，避免析构时对象已亡。 */
    if (m_analysisThread != nullptr) {
        m_analysisThread->quit();
        if (!m_analysisThread->wait(5000))
            m_analysisThread->terminate();
    }
}

void MainWindow::onSnapshotDecoded(WaveformFramePtr frame, pdsample::ChannelStats stats,
                                   pddetect::Result pulses, qint64 sequence,
                                   const QString &sourceName, const QString &error)
{
    Q_UNUSED(sequence);
    if (!error.isEmpty() || frame.isNull()) {
        appendLog(QStringLiteral("ERR"),
                  error.isEmpty() ? QStringLiteral("快照解码返回空帧，未绘图。") : error);
        return;
    }

    appendLog(QStringLiteral("OK"),
              QStringLiteral("%1：%2 点，min/max = CH0 %3/%4，CH1 %5/%6，CH2 %7/%8，CH3 %9/%10")
                  .arg(sourceName)
                  .arg(frame->sampleCount)
                  .arg(stats.minimum[0], 0, 'f', 0).arg(stats.maximum[0], 0, 'f', 0)
                  .arg(stats.minimum[1], 0, 'f', 0).arg(stats.maximum[1], 0, 'f', 0)
                  .arg(stats.minimum[2], 0, 'f', 0).arg(stats.maximum[2], 0, 'f', 0)
                  .arg(stats.minimum[3], 0, 'f', 0).arg(stats.maximum[3], 0, 'f', 0));

    m_staticScope->setStaticTrace(*frame, sourceName);
    m_staticPulses = pddetect::flatten(pulses);
    m_staticScope->setPulses(m_pulseOverlay->isChecked() ? m_staticPulses
                                                         : QVector<pddetect::Pulse>());

    appendLog(QStringLiteral("OK"),
              QStringLiteral("已载入“归档记录波形”：%1 个采样点，可缩放/平移/加游标/点选脉冲；"
                             "检出脉冲 %2 个（已确认 %3 个）。%4")
                  .arg(frame->sampleCount)
                  .arg(m_staticPulses.size())
                  .arg(pulses.confirmedPulses)
                  .arg(pulses.note));
    updatePulseStatistics();
}

void MainWindow::onEventBatchDecoded(QVector<pdsample::PeakEvent> events, quint32 sequence,
                                     int unrecognised, const QString &error)
{
    if (!error.isEmpty()) {
        appendLog(QStringLiteral("ERR"), error);
        /* 解码失败也要把回合推进，否则链路被这一条卡死。 */
        if (m_eventDownloadInFlight) nextPrpdCandidate();
        return;
    }
    if (unrecognised > 0) {
        m_unrecognisedEvents += static_cast<quint64>(unrecognised);
        appendLog(QStringLiteral("ERR"),
                  QStringLiteral("事件包中有 %1 个字 type 不属于 0/1，说明事件包格式与上位机"
                                 "理解不一致，请核对 pd_feature_core.v 与 PL 的事件包契约。")
                      .arg(unrecognised));
    }

    accountEventSequences(events);

    QVector<QVector<pdsample::PeakEvent>> perChannel(pdsample::kChannelCount);
    for (const pdsample::PeakEvent &event : events) {
        if (event.channel < 0 || event.channel >= pdsample::kChannelCount) continue;
        perChannel[event.channel].append(event);
    }

    int received = 0;
    int drawn = 0;
    QString perChannelText;
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        QVector<pdsample::PeakEvent> batch = perChannel.at(c);
        received += batch.size();
        if (batch.size() > kMaxPointsPerBatchPerChannel) {
            const int stride =
                (batch.size() + kMaxPointsPerBatchPerChannel - 1) / kMaxPointsPerBatchPerChannel;
            QVector<pdsample::PeakEvent> thinned;
            thinned.reserve(batch.size() / stride + 1);
            for (int i = 0; i < batch.size(); i += stride) thinned.append(batch.at(i));
            batch = thinned;
        }
        if (c < m_ellipses.size()) m_ellipses[c]->appendEvents(batch);

        /* 散点图与椭圆图用同一批点：前者是直角坐标，后者是相位刻度盘。 */
        QVector<QPointF> scatter;
        scatter.reserve(batch.size());
        for (const pdsample::PeakEvent &event : batch)
            scatter.append(QPointF(event.phaseDeg, event.q88));
        if (c < m_prpdPanels.size()) m_prpdPanels[c]->appendPoints(scatter);

        drawn += batch.size();
        perChannelText += QStringLiteral("%1%2")
                              .arg(c == 0 ? QString() : QStringLiteral("/"))
                              .arg(batch.size());
    }
    m_prpdAddedPoints += drawn;

    m_ringStatus = QStringLiteral("seq=%1 ｜ 接收 %2 绘制 %3 ｜ 各通道 %4")
                       .arg(sequence).arg(received).arg(drawn).arg(perChannelText);
    for (auto *panel : m_prpdPanels) panel->setStatusText(m_ringStatus);
    for (auto *panel : m_ellipses) panel->setStatusText(m_ringStatus);

    if (m_ringStats != nullptr) {
        QString ellipses;
        for (int c = 0; c < m_ellipses.size(); ++c)
            ellipses += QStringLiteral("%1%2")
                            .arg(c == 0 ? QString() : QStringLiteral("/"))
                            .arg(m_ellipses.at(c)->eventCount());
        m_ringStats->setText(
            QStringLiteral("相位环/椭圆：累计 %1 点（只保留最近 %2 s）｜椭圆 %3")
                .arg(m_prpdAddedPoints)
                .arg(m_ringAging->value())
                .arg(ellipses));
    }
    updatePulseStatistics();

    /* 解码完成，回合在这里才推进（下载完成时只提交解码）。 */
    if (m_eventDownloadInFlight) {
        ++m_prpdRoundFetched;
        nextPrpdCandidate();
    }
}

void MainWindow::onSpectrumReady(QVector<QVector<QPointF>> curves, QStringList names,
                                 const QString &peakNote, const QString &error)
{
    if (!error.isEmpty()) {
        appendLog(QStringLiteral("ERR"), error);
        return;
    }
    if (curves.isEmpty()) return;
    m_spectrumPlot->setLines(curves, names,
                             QStringLiteral("1024 点 FFT（去直流 + 周期 Hann；"
                                            "幅值口径与板端一致）"),
                             QStringLiteral("频率 Hz"),
                             QStringLiteral("幅值 dB（相对 1 ADC 码）"));
    statusBar()->showMessage(peakNote);
}

/* ---------------------------------------------------------- 联动 */

void MainWindow::onPulsePicked(int index, int channel, double code, double timeSec)
{
    Q_UNUSED(index);
    if (channel < 0 || channel >= m_ellipses.size()) return;

    /*
     * ⚠️ 必须说清的一点：这不是"同一个事件"的证明。
     * 事件包只有 (相位, 幅值, 通道)，DDR 快照只有 (样本码, 通道)，两者**没有共同时间戳**，
     * 而且 SCOPE 帧与帧之间本来就不连续。所以这里只能按"同通道 + 幅值最接近"互相印证。
     * 这个匹配有物理依据（默认标定下事件包 q 的原始值等于该脉冲的 AD 码幅度），
     * 但它不是同一事件的时间配对，界面与文档都不把它说成后者。
     */
    const bool hit = m_ellipses.at(channel)->highlightNearestByAmplitude(code);
    if (hit) {
        appendLog(QStringLiteral("LINK"),
                  QStringLiteral("波形脉冲（CH%1，%2 码，%3 µs）→ 椭圆图谱已高亮同通道"
                                 "幅值最接近的事件（按幅值就近匹配，非同一事件的时间配对）。")
                      .arg(channel)
                      .arg(code, 0, 'f', 0)
                      .arg(timeSec * 1e6, 0, 'f', 2));
    } else {
        appendLog(QStringLiteral("LINK"),
                  QStringLiteral("椭圆图谱上暂无可匹配的 CH%1 事件（先让采集跑一会儿，"
                                 "或检查该通道门限）。").arg(channel));
    }
}

void MainWindow::onEventPicked(int channel, int index, double phaseDeg, double adcCodes,
                               bool positive)
{
    Q_UNUSED(index);
    const double magnitude = std::fabs(adcCodes);
    int matched = 0;

    /* 反向联动：在波形上找同通道、幅值最接近的脉冲并选中。两个波形视图都试。 */
    const auto linkOne = [&](ScopeWidget *widget, const QVector<pddetect::Pulse> &list) {
        int best = -1;
        double bestDistance = 0.0;
        for (int i = 0; i < list.size(); ++i) {
            if (list.at(i).channel != channel) continue;
            const double distance = std::fabs(list.at(i).absCode - magnitude);
            if (best < 0 || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        if (best >= 0) {
            widget->setSelectedPulse(best);
            ++matched;
        }
    };
    linkOne(m_scope, m_livePulses);
    linkOne(m_staticScope, m_staticPulses);

    appendLog(QStringLiteral("LINK"),
              QStringLiteral("椭圆事件（CH%1，相位 %2°，%3 码，%4）→ %5")
                  .arg(channel)
                  .arg(phaseDeg, 0, 'f', 2)
                  .arg(magnitude, 0, 'f', 0)
                  .arg(positive ? QStringLiteral("正极性") : QStringLiteral("负极性"))
                  .arg(matched > 0
                           ? QStringLiteral("波形上已选中幅值最接近的脉冲")
                           : QStringLiteral("波形上没有可匹配的脉冲（实时窗口仅含 1024 点，"
                                            "事件可能落在窗口之外）")));
}

/* ---------------------------------------------------------- 配置下发 */

void MainWindow::applyPulseSettings()
{
    if (m_worker == nullptr || m_pulseThreshold == nullptr) return;

    pddetect::Settings settings;
    settings.absoluteThresholdCodes = m_pulseThreshold->value();
    settings.adaptiveMultiplier = 6.0;
    settings.mergeGapSamples = 16;
    settings.maxPulsesPerChannel = 2048;
    m_pulseSettings = settings;

    /* 工作线程里的判据必须走队列更新——直接调用会在 GUI 线程里改工作线程的成员。 */
    QMetaObject::invokeMethod(m_worker, "setPulseSettings", Qt::QueuedConnection,
                              Q_ARG(pddetect::Settings, settings));

    const double scale = m_scaleQ88->value();
    const bool showPc = (m_pulseUnit->currentIndex() == 0);
    const double highlightCodes = m_pulseHighlight->value();
    const QString band = m_bandEdit->text();
    const bool vertical = (m_pulseStyle->currentIndex() == 0);

    for (auto *panel : m_ellipses) {
        panel->setHighlightThresholdCodes(highlightCodes);
        panel->setScaleQ88(scale);
        panel->setShowPicoCoulomb(showPc);
        panel->setBandLabel(band);
        panel->setPulseStyle(vertical ? EllipsePanel::PulseStyle::Vertical
                                      : EllipsePanel::PulseStyle::Radial);
    }

    /* 散点图的纵轴是原始 Q8.8，阈值要换算到同一域：raw = codes × SCALE / 256。 */
    const double prpdThreshold = highlightCodes * scale / 256.0;
    for (auto *panel : m_prpdPanels) {
        panel->setHighlightThreshold(prpdThreshold);
        panel->setBandLabel(band);
    }

    for (ScopeWidget *widget : {m_scope, m_staticScope}) {
        widget->setPulseOverlayVisible(m_pulseOverlay->isChecked());
        widget->setPulseHighlightCodes(highlightCodes);
        widget->setMaximumPulsesDrawn(m_pulseMaxDrawn->value());
    }
    /* 关掉标注时把已有标注清掉，避免"关了还留着线"的错觉。 */
    if (!m_pulseOverlay->isChecked()) {
        m_scope->setPulses(QVector<pddetect::Pulse>());
        m_staticScope->setPulses(QVector<pddetect::Pulse>());
    } else {
        m_scope->setPulses(m_livePulses);
        m_staticScope->setPulses(m_staticPulses);
    }
    updatePulseStatistics();
}

void MainWindow::accountEventSequences(const QVector<pdsample::PeakEvent> &events)
{
    /*
     * evt_seq 是每通道自增的（pd_feature_core.v:549-550），此前完全没用上。
     *
     * 只在**同一条记录内**比较才有意义：上位机是刻意跳着取记录来避开归档轮转的
     * （一条记录只活约 79 ms），所以跨记录的序号必然跳，那不是丢事件。
     * 而记录内同一通道的连续事件序号应当连号——不连号就说明 PL→PS→归档这条链丢了事件。
     */
    quint32 previous[pdsample::kChannelCount] = {0, 0, 0, 0};
    bool seen[pdsample::kChannelCount] = {false, false, false, false};
    for (const pdsample::PeakEvent &event : events) {
        const int c = event.channel;
        if (c < 0 || c >= pdsample::kChannelCount) continue;
        if (seen[c]) {
            const quint32 expected = previous[c] + 1U;
            if (event.evtSeq != expected) {
                /* 25 位自然回绕不算跳号。 */
                const bool wrapped = (event.evtSeq == 0U) && (previous[c] != 0U);
                if (!wrapped) ++m_evtSeqGaps;
            }
        }
        previous[c] = event.evtSeq;
        seen[c] = true;
        ++m_evtSeqTotal;
    }
}

void MainWindow::updatePulseStatistics()
{
    if (m_pulseStats == nullptr) return;

    double livePeak = 0.0;
    int liveConfirmed = 0;
    for (const pddetect::Pulse &pulse : m_livePulses) {
        livePeak = std::max(livePeak, pulse.absCode);
        if (pulse.confirmed) ++liveConfirmed;
    }
    double staticPeak = 0.0;
    int staticConfirmed = 0;
    for (const pddetect::Pulse &pulse : m_staticPulses) {
        staticPeak = std::max(staticPeak, pulse.absCode);
        if (pulse.confirmed) ++staticConfirmed;
    }

    QString channelDetail;
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        int count = 0;
        for (const pddetect::Pulse &pulse : m_livePulses)
            if (pulse.channel == c) ++count;
        channelDetail += QStringLiteral("%1%2").arg(c == 0 ? QString() : QStringLiteral("/"))
                             .arg(count);
    }

    m_pulseStats->setText(
        QStringLiteral("脉冲：实时 %1 个（确认 %2，各通道 %3，峰 %4 码）｜"
                       "归档 %5 个（确认 %6，峰 %7 码）｜判据 max(%8 码, 6×sigma)｜"
                       "事件字 %9，记录内跳号 %10，不识别 %11")
            .arg(m_livePulses.size())
            .arg(liveConfirmed)
            .arg(channelDetail)
            .arg(livePeak, 0, 'f', 0)
            .arg(m_staticPulses.size())
            .arg(staticConfirmed)
            .arg(staticPeak, 0, 'f', 0)
            .arg(m_pulseThreshold->value(), 0, 'f', 0)
            .arg(m_evtSeqTotal)
            .arg(m_evtSeqGaps)
            .arg(m_unrecognisedEvents));
}

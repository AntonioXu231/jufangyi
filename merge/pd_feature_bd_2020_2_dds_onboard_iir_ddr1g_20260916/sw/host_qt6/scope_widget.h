#pragma once

#include "pd_pulse_detect.h"
#include "pd_sample_codec.h"

#include <QPoint>
#include <QVector>
#include <QWidget>

class QTimer;

/*
 * 内置示波器绘图控件。
 *
 * 与原先的 PlotWidget 的区别（后者保留给静态分析图使用）：
 *   - 有鼠标交互：滚轮缩放时间轴、Ctrl+滚轮缩放幅度轴、Shift+滚轮平移、
 *     左键拖拽平移、右键拖拽放置/移动测量游标、双击复位视图；
 *   - X 轴按采样间隔换算成真实时间单位（ns/µs/ms），Y 轴用 ADC 码；
 *   - 用 min/max 包络抽稀绘制长记录：每个像素列画该列样本的最小值到最大值，
 *     这样即使 520,000 点的快照压到 1000 像素宽，DDS 的 4 样点窄脉冲也不会被
 *     等间隔抽稀丢掉（等间隔抽稀会直接漏掉脉冲，这是原实现的实际风险）；
 *   - 支持三种显示模式，语义边界在 UI 上明确标注，不把不连续的数据画成连续波形；
 *   - 带触发电平线与触发点标记，以及双游标 Δt 测量。
 */
class ScopeWidget final : public QWidget
{
    Q_OBJECT
public:
    enum class Mode {
        Static, /* 一份时间连续的记录（快照/CSV），X 轴覆盖整段 */
        Live,   /* 单帧实时窗口，X 轴宽度 = 时基（样本数） */
        Roll    /* 追加式滚动历史；帧与帧之间不连续，用间隙标记明确画出 */
    };

    explicit ScopeWidget(QWidget *parent = nullptr);

    /* ---- 数据入口 ---- */
    void setStaticTrace(const pdsample::WaveformFrame &frame, const QString &label);
    void setLiveFrame(const pdsample::WaveformFrame &frame, int triggerIndex,
                      bool triggerValid, double triggerLevel, int triggerChannel);
    void appendRollFrame(const pdsample::WaveformFrame &frame);
    void clearData();

    /* ---- 视图控制 ---- */
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    void setTimeSpanSamples(double samples);
    double timeSpanSamples() const { return m_viewSpan; }
    void setChannelVisible(int channel, bool visible);
    bool channelVisible(int channel) const;
    void setChannelOffset(int channel, double codes);
    double channelOffset(int channel) const;
    void setChannelGain(int channel, double gain);
    void setAutoScaleY(bool enabled);
    /* 关闭自动量程时冻结当前量程，而不是跳到某个固定窗口。 */
    void freezeCurrentRange();
    void setYRange(double minimumCode, double maximumCode);
    void setRollCapacity(int samples);
    void setTriggerOverlay(bool visible, double level, int channel);

    void resetView();
    void fitAll();
    /* 显式设定视图窗口（触发对齐时用），并标记为用户已调整，避免下一帧被自动重置。 */
    void setViewWindow(double startSample, double spanSamples);
    void zoomTime(double factor, int anchorPixel);
    void zoomAmplitude(double factor);
    void panTime(double deltaSamples);

    void setCursorsEnabled(bool enabled);
    void clearCursors();

    /* ---- PD 脉冲标注 ---- */
    /* 传入对当前显示数据做检测得到的脉冲（工作线程算好）。空列表表示清除标注。 */
    void setPulses(const QVector<pddetect::Pulse> &pulses);
    const QVector<pddetect::Pulse> &pulses() const { return m_pulses; }
    void setPulseOverlayVisible(bool visible);
    bool pulseOverlayVisible() const { return m_pulseOverlay; }
    /* 超过该幅值（AD 码）的脉冲用高亮几何画出，其余只画细短标。 */
    void setPulseHighlightCodes(double codes);
    void setSelectedPulse(int index);
    int selectedPulse() const { return m_selectedPulse; }
    /* 单次最多精细绘制多少个脉冲，防止噪声记录把渲染拖死。 */
    void setMaximumPulsesDrawn(int count);
    /*
     * 渲染节流：数据到达不必每次重绘。
     * 实时帧 20 ms 一帧、解码 33 ms 一次，若不合并会出现"画得比来得慢"的堆积。
     */
    void setMinimumRenderIntervalMs(int milliseconds);

    /* 供主窗口读取：当前视图窗口对应的样本区间，用于 CSV 导出。 */
    void visibleSampleRange(int &first, int &count) const;

    /* 导出用：返回当前显示的整段数据。滚动模式返回拼接后的滚动缓冲
       （注意其横轴是到达顺序，不是时间；CSV 里会写清这一点）。 */
    pdsample::WaveformFrame displayFrame() const;

    /* 把当前画面存成 PNG；直接复用 QWidget::grab()，见主窗口调用处。 */
    static const QColor &channelColor(int channel);
    static QString channelName(int channel);

signals:
    void viewChanged();
    void cursorChanged(const QString &readout);
    /* 用户点中一个脉冲（与椭圆图侧的联动由主窗口完成）。 */
    void pulsePicked(int index, int channel, double code, double timeSec);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct PlotArea {
        QRectF rect;
        double xMin = 0.0;   /* 视图左端对应的样本下标（可为小数） */
        double xMax = 0.0;   /* 视图右端 */
        double yMin = 0.0;
        double yMax = 0.0;
        double pixelToSample(double x) const;
        double sampleToPixel(double sample) const;
        double codeToPixel(double code) const;
    };

    PlotArea computePlotArea() const;
    void resolveAutoRange(double &yMin, double &yMax) const;
    int totalSamples() const;
    const QVector<double> *channelData(int channel) const;

    void drawGrid(QPainter &painter, const PlotArea &area) const;
    void drawTraces(QPainter &painter, const PlotArea &area) const;
    void drawTriggerOverlay(QPainter &painter, const PlotArea &area) const;
    void drawPulses(QPainter &painter, const PlotArea &area) const;
    void drawCursors(QPainter &painter, const PlotArea &area) const;
    void drawLegend(QPainter &painter, const PlotArea &area) const;
    void emitCursorReadout();
    /* 数据版本变化：包络缓存据此失效。 */
    void bumpDataVersion();
    /* 按最小渲染间隔合并重绘请求。 */
    void requestRepaint();
    /*
     * min/max 包络抽稀，结果按 (数据版本, 视图范围, 像素列数) 缓存。
     * 旧实现每次 paint 都要重算——520,000 点 × 4 通道 = 208 万次比较，
     * 而拖拽/游标/实时刷新都会触发重绘，这是卡顿的主因之一。
     */
    void ensureEnvelope(int channel, const PlotArea &area, int first, int last,
                        int columns) const;
    int pickPulse(const QPointF &widgetPoint) const;

    /* ---- 显示数据 ---- */
    Mode m_mode = Mode::Static;
    pdsample::WaveformFrame m_frame; /* Static / Live 用 */
    /* 采样率单独保存：滚动模式下 m_frame 为空，但横轴换算与导出仍需要它。 */
    double m_sampleRateHz = 0.0;
    QString m_label;
    int m_triggerIndex = -1;
    bool m_triggerValid = false;
    double m_triggerLevel = pdsample::kMidCode;
    int m_triggerChannel = 0;
    bool m_triggerOverlay = false;

    /* 滚动历史：按"到达顺序"追加，不假设帧间时间连续。 */
    QVector<double> m_roll[pdsample::kChannelCount];
    QVector<qint64> m_rollSequence;
    QVector<int> m_rollFrameStart; /* 每帧在 m_roll 中的起始下标 */
    int m_rollCapacity = 65536;

    /* ---- 视图状态 ---- */
    double m_viewStart = 0.0;
    double m_viewSpan = 1024.0;
    bool m_viewInitialised = false;
    /* 用户手动缩放/平移过之后，新到帧不再自动重置视图，否则实时模式下
       用户刚放大就看到画面被跳回去。 */
    bool m_userAdjustedView = false;
    bool m_autoScaleY = true;
    double m_yMin = 0.0;
    double m_yMax = 4095.0;

    /* ---- 每通道显示属性 ---- */
    bool m_visible[pdsample::kChannelCount] = {true, true, true, true};
    double m_offset[pdsample::kChannelCount] = {0.0, 0.0, 0.0, 0.0};
    double m_gain[pdsample::kChannelCount] = {1.0, 1.0, 1.0, 1.0};

    /* ---- 游标 ---- */
    bool m_cursorsEnabled = false;
    bool m_cursorSet[2] = {false, false};
    double m_cursorSample[2] = {0.0, 0.0};
    int m_draggingCursor = -1;

    /* ---- PD 脉冲标注 ---- */
    QVector<pddetect::Pulse> m_pulses;
    bool m_pulseOverlay = true;
    double m_pulseHighlightCodes = 120.0;
    int m_selectedPulse = -1;
    int m_maxPulsesDrawn = 256;

    /* ---- 渲染节流 ---- */
    quint64 m_dataVersion = 1;
    int m_minRenderIntervalMs = 30;
    qint64 m_lastPaintRequestMs = 0;
    bool m_renderPending = false;

    /* ---- 包络缓存（mutable：在 const 的 drawTraces 里填充） ---- */
    struct EnvelopeCache {
        bool valid = false;
        int first = -1;
        int last = -1;
        int columns = -1;
        quint64 version = 0;
        QVector<double> columnMin; /* 原始 ADC 码，未加增益/偏移 */
        QVector<double> columnMax;
        QVector<uchar> minFirst;   /* 1 = 该列最小值出现在最大值之前 */
    };
    mutable EnvelopeCache m_envelope[pdsample::kChannelCount];

    /* ---- 交互状态 ---- */
    bool m_panning = false;
    bool m_movedWhilePressed = false;
    QPoint m_pressPosition;
    QPoint m_lastMouse;
    double m_lastFrameIntervalSec = 0.0;
    qint64 m_lastArrivalMs = 0;
};

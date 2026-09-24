#pragma once

#include "pd_sample_codec.h"

#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

/*
 * 单通道椭圆图谱（对应参考图一 / 图二那种椭圆显示）。
 *
 * ------------------------------------------------------------------ 几何含义
 *
 * 椭圆不是装饰，它是**工频相位刻度盘**：把椭圆参数化后，
 * 相位 θ 与椭圆上一点一一对应，
 *
 *      x(θ) = cx + Rx · sin θ
 *      y(θ) = cy − Ry · cos θ            （θ = 0 在正上方，顺时针增加）
 *
 * 于是一个发生在相位 θ 的局放脉冲，就在 x(θ) 处画一条**竖直对称线**：
 * 该 x 处椭圆的上下边界是 cy ± Ry·|cos θ|，竖线再向两端各伸出与幅值成正比的长度。
 *
 * 这个模型与参考图完全吻合，也正是它"上下对称、中间长两端短"的原因：
 * |cos θ| 在 θ=0/180（x = cx，中轴）处为 1 → 竖线最长；
 * 在 θ=90/270（x = cx ± Rx，椭圆两端）处为 0 → 竖线最短。
 *
 * ------------------------------------------------------------------ 数据来源
 *
 * 事件来自 PL 特征事件包的峰值包（见 pd_sample_codec.h 的位域说明），
 * 每个事件带相位窗号、Q8.8 视在电荷量、极性与每通道自增序号。
 *
 * ⚠️ 必须说清的一点：事件包与 DDR 快照**没有共同的时间戳**，而 SCOPE 帧之间
 *    也不连续。因此本控件上的事件点与波形控件上的脉冲**无法严格配对**，
 *    只能按"同通道 + 幅值最接近"互相印证（默认标定下事件包的 q 原始值等于
 *    该脉冲的 AD 码幅度，所以这个匹配是有物理依据的，但仍不是同一事件的证明）。
 *
 * ------------------------------------------------------------------ 性能
 *
 * 不逐点 drawXxx，也不在 paint 里做三角函数：
 *   - 相位 → sin/cos 走 4096 项查表（与 12-bit 相位窗号一一对应）；
 *   - 背景事件预先聚合成 (相位桶 × 幅值桶) 密度网格，只在数据变化时重建，
 *     paint 只遍历非空格子；
 *   - 只有"已确认脉冲"才用真实几何精细绘制，并设有绘制上限。
 */
class EllipsePanel final : public QWidget
{
    Q_OBJECT
public:
    enum class PulseStyle {
        Vertical, /* 参考图那种竖直对称线（默认） */
        Radial    /* 沿椭圆径向的辐条，相位指向更直观 */
    };
    Q_ENUM(PulseStyle)

    explicit EllipsePanel(int channel, QWidget *parent = nullptr);

    /* ---- 数据 ---- */
    void setEvents(const QVector<pdsample::PeakEvent> &events);
    void appendEvents(const QVector<pdsample::PeakEvent> &events);
    void clear();
    int eventCount() const { return m_count; }
    double peakAdcCodes() const { return m_peakCodes; }
    int confirmedCount() const { return m_confirmedIndices.size(); }

    /* ---- 判据与单位 ---- */
    /* 超过该 AD 码幅值的脉冲视为"已确认"，用高亮几何绘制；其余进密度网格当背景。 */
    void setHighlightThresholdCodes(double codes);
    double highlightThresholdCodes() const { return m_highlightThreshold; }
    /* 板端每通道标定系数 SCALE（Q8.8，默认 256 = 1.0），用于 pC ↔ AD 码换算。 */
    void setScaleQ88(double scaleQ88);
    /* 纵轴单位：true = pC（板端口径），false = AD 码（波形域）。 */
    void setShowPicoCoulomb(bool showPc);
    bool showPicoCoulomb() const { return m_showPc; }
    void setAmplitudeFullScaleCodes(double codes); /* 0 = 用当前数据峰值自动定标 */
    void setBandLabel(const QString &band) ;
    void setStatusText(const QString &text);
    void setPulseStyle(PulseStyle style);
    void setEnabledChannel(bool enabled);
    bool enabledChannel() const { return m_enabled; }

    /* ---- 视图 ---- */
    void resetView();
    void zoomBy(double factor, const QPointF &widgetAnchor);
    void zoomAmplitudeBy(double factor);
    double zoomFactor() const { return m_zoom; }
    double amplitudeZoom() const { return m_amplitudeZoom; }

    /* ---- 与波形控件的联动 ---- */
    /* 按幅值就近高亮同通道的一个事件；返回是否命中。 */
    bool highlightNearestByAmplitude(double adcCodes);
    bool highlightNearestByPhase(double phaseDeg);
    void clearHighlight();
    int highlightedIndex() const { return m_highlighted; }

signals:
    void eventPicked(int index, double phaseDeg, double adcCodes, bool positive);
    void viewChanged();
    void enableToggled(bool enabled);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    /* 重建缓存：峰值、已确认索引、密度网格。数据或阈值变化时调用。 */
    void rebuildCaches();
    void prune();
    void compactIfNeeded();

    QRectF plotRect() const;
    QPointF centre() const;
    double radiusX() const;
    double radiusY() const;
    /* 相位 θ 对应椭圆上的点。 */
    QPointF ellipsePoint(double phaseDeg) const;
    double amplitudeToPixels(double adcCodes) const;
    double ampFullScale() const;
    QString formatValue(double adcCodes) const;
    QString unitName() const;
    void drawHeader(QPainter &painter);
    void drawDial(QPainter &painter);
    void drawDensity(QPainter &painter);
    void drawConfirmed(QPainter &painter);
    void drawHighlight(QPainter &painter);
    void drawControls(QPainter &painter);
    int pickEvent(const QPointF &widgetPoint) const;

    int m_channel = 0;

    /* 数据（带 head 偏移，避免 remove(0,n) 的 O(N) 搬移）。 */
    QVector<pdsample::PeakEvent> m_events;
    QVector<qint64> m_times;
    int m_head = 0;
    int m_count = 0;
    double m_agingSeconds = 10.0;
    int m_maximumEvents = 60000;

    /* 缓存 */
    double m_peakCodes = 0.0;
    QVector<int> m_confirmedIndices; /* 超过阈值的元素下标（0..count-1，不含 head） */
    QVector<int> m_density;          /* kPhaseBins × kAmpBins */
    QVector<int> m_densityCells;     /* 非空格子的线性下标 */
    int m_densityMax = 0;
    bool m_cacheDirty = true;

    /* 配置 */
    double m_highlightThreshold = 120.0;
    double m_scaleQ88 = pdsample::eventpacket::kDefaultScaleQ88;
    bool m_showPc = true;
    double m_fullScaleCodes = 0.0;
    double m_amplitudeZoom = 1.0;
    QString m_band;
    QString m_status;
    PulseStyle m_style = PulseStyle::Vertical;

    /* 视图 */
    double m_zoom = 1.0;
    double m_panX = 0.0;
    double m_panY = 0.0;

    /* 交互 */
    bool m_enabled = true;
    bool m_panning = false;
    QPoint m_lastMouse;
    bool m_movedWhilePressed = false;
    int m_pressedControl = -1;
    int m_highlighted = -1;
};

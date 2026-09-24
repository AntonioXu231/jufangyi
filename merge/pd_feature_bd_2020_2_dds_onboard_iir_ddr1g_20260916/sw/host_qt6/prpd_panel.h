#pragma once

#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

/*
 * 单通道 PRPD 相位-幅值散点（x = 相位 0..360°，y = 幅值）。
 *
 * 与椭圆图谱的分工：
 *   - 本控件是标准的 PRPD 直角坐标呈现，相位轴线性，便于读相位分布与幅值谱；
 *   - EllipsePanel 是"椭圆图谱"呈现（相位刻度盘 + 竖直脉冲线），视觉对比更直观。
 *   两者由同一批峰值事件驱动，互为印证。
 *
 * ------------------------------------------------------------------ 性能（2026-09-23 重构）
 *
 * 旧实现的 paintEvent 每次都做两件昂贵的事：
 *   1) peakAmplitude() 遍历全部点求峰值（O(N) 每次重绘）；
 *   2) 主循环里对**每个点**调用 cos/sin、构造一个 QPen、再 drawPoint——
 *      QPainter 逐点调用是最慢的绘制方式，2 万点足以让 Debug 构建掉到个位数 fps。
 *
 * 现在：
 *   - 峰值 / 最新点 / 密度网格都在数据入口算好并缓存，paint 里没有 O(N) 循环；
 *   - 背景点聚合成 (相位桶 × 幅值桶) 密度网格，只遍历非空格子；
 *   - 三角函数改用 4096 项查表（与 12-bit 相位窗号对应），热循环内无三角函数。
 */
class PrpdPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit PrpdPanel(int channel, QWidget *parent = nullptr);

    void setPoints(const QVector<QPointF> &points);
    void appendPoints(const QVector<QPointF> &points);
    void clear();

    /* 只保留最近 seconds 秒内的点；0 表示不老化（一直堆积）。 */
    void setAgingSeconds(double seconds);
    double agingSeconds() const { return m_agingSeconds; }

    /* 超过该幅值（原始 Q8.8）的点用亮色绘制，作为"已确认脉冲"的标记。 */
    void setHighlightThreshold(double q88) { m_highlightThreshold = q88; rebuildCaches(); update(); }
    void setBandLabel(const QString &band) { m_band = band; update(); }

    /* 右上角状态文本：数据来源、刷新时间等。 */
    void setStatusText(const QString &text);

    int pointCount() const { return m_count; }
    double peakAmplitude() const { return m_peak; }
    int highlightedCount() const { return m_highlighted.size(); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void prune();
    void compactIfNeeded();
    /* 重建缓存：峰值、最新点、已高亮点、密度网格。数据/阈值变化时调用。 */
    void rebuildCaches();

    int m_channel = 0;
    QVector<QPointF> m_points; /* x = 相位角(度)，y = 原始 Q8.8 幅值 */
    QVector<qint64> m_times;   /* 与 m_points 一一对应的到达时刻（ms） */
    /* 头部偏移：避免 remove(0,n) 的 O(N) 内存搬移。 */
    int m_head = 0;
    int m_count = 0;
    double m_agingSeconds = 10.0;
    int m_maximumPoints = 60000;
    QString m_status;
    QString m_band;

    /* ---- 缓存 ---- */
    double m_peak = 0.0;
    int m_newest = -1;             /* 相对下标（0..m_count-1） */
    QVector<int> m_highlighted;    /* 超过阈值的相对下标 */
    QVector<int> m_density;        /* kPhaseBins × kAmpBins */
    QVector<int> m_densityCells;   /* 非空格子的线性下标 */
    int m_densityMax = 0;
    double m_highlightThreshold = 0.0;
};

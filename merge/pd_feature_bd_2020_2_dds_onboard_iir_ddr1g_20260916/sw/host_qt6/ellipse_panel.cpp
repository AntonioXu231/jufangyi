#include "ellipse_panel.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

const QColor kBackground(11, 17, 28);
const QColor kHeaderBackground(0, 110, 175);
const QColor kFrame(0, 150, 210);
const QColor kGrid(46, 66, 88);
const QColor kTextDim(150, 175, 190);

/* 每通道颜色，与参考图一致：1 黄 / 2 青 / 3 红 / 4 品红。 */
const QColor kChannelColor[4] = {
    QColor(240, 216, 60),
    QColor(120, 230, 180),
    QColor(240, 120, 90),
    QColor(205, 120, 240),
};

constexpr int kHeaderHeight = 24;
constexpr int kControlHeight = 18;

/* 密度网格：按 (相位桶 × 幅值桶) 聚合，与视图无关，所以缩放/平移不必重建。 */
constexpr int kPhaseBins = 144; /* 2.5°/桶 */
constexpr int kAmpBins = 48;

/* 精细绘制的已确认脉冲上限；超过的部分仍计入密度网格，但不再单独画线。 */
constexpr int kMaxConfirmedDrawn = 512;

/*
 * 相位 → sin/cos 查表。
 * 12-bit 相位窗号正好 4096 个取值，与表长一一对应，
 * 因此热循环里**没有三角函数调用**——这是相对旧实现最关键的提速点
 * （旧 PrpdPanel 对每个点各调一次 cos 与 sin，2 万点就是 4 万次三角函数）。
 */
struct TrigTable {
    double sinValue[4096];
    double cosValue[4096];
    TrigTable()
    {
        for (int i = 0; i < 4096; ++i) {
            const double theta = kPi * 2.0 * i / 4096.0;
            sinValue[i] = std::sin(theta);
            cosValue[i] = std::cos(theta);
        }
    }
};

const TrigTable &trig()
{
    static const TrigTable table;
    return table;
}

} // namespace

EllipsePanel::EllipsePanel(int channel, QWidget *parent)
    : QWidget(parent), m_channel(channel & 3)
{
    /*
     * 最小高度压到 118：这是为了在 1280×720 这类屏上，
     * 上方控制区能拿到足够高度、把"标注脉冲/检测阈值"那一行放进可视区
     * （否则要向下滚动一行才看得到，等于没有）。
     * 相位盘本身在 118 高时仍有约 60 px 可画，看得清。
     */
    setMinimumSize(230, 118);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setToolTip(QStringLiteral(
        "滚轮：缩放图谱（以鼠标为锚点）\n"
        "Ctrl+滚轮：只缩放幅度轴\n"
        "左键拖拽：平移；双击：复位视图\n"
        "左键点击脉冲：选中并在波形侧高亮同幅值脉冲\n"
        "底部按钮：复位 / 适配 / 显隐"));
}

/* ------------------------------------------------------------------ 数据 */

void EllipsePanel::setEvents(const QVector<pdsample::PeakEvent> &events)
{
    m_events.clear();
    m_times.clear();
    m_head = 0;
    m_count = 0;
    m_events.reserve(events.size());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const pdsample::PeakEvent &event : events) {
        m_events.append(event);
        m_times.append(now);
    }
    m_count = m_events.size();
    prune();
    rebuildCaches();
    update();
}

void EllipsePanel::appendEvents(const QVector<pdsample::PeakEvent> &events)
{
    if (events.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_events.reserve(m_events.size() + events.size());
    m_times.reserve(m_times.size() + events.size());
    for (const pdsample::PeakEvent &event : events) {
        m_events.append(event);
        m_times.append(now);
    }
    m_count = m_events.size() - m_head;
    prune();
    compactIfNeeded();
    rebuildCaches();
    update();
}

void EllipsePanel::clear()
{
    m_events.clear();
    m_times.clear();
    m_head = 0;
    m_count = 0;
    m_highlighted = -1;
    rebuildCaches();
    update();
}

void EllipsePanel::prune()
{
    if (m_count <= 0) return;
    int drop = 0;
    if (m_agingSeconds > 0.0) {
        const qint64 cutoff =
            QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(m_agingSeconds * 1000.0);
        while (drop < m_count && m_times.at(m_head + drop) < cutoff) ++drop;
    }
    const int excess = m_count - drop - m_maximumEvents;
    if (excess > 0) drop += excess;
    if (drop > 0) {
        m_head += drop;
        m_count -= drop;
        if (m_highlighted >= 0) {
            m_highlighted -= drop;
            if (m_highlighted < 0) m_highlighted = -1;
        }
    }
}

void EllipsePanel::compactIfNeeded()
{
    if (m_head < 8192 || m_head * 2 < m_events.size()) return;
    m_events.remove(0, m_head);
    m_times.remove(0, m_head);
    m_head = 0;
    m_count = m_events.size();
}

void EllipsePanel::rebuildCaches()
{
    m_confirmedIndices.clear();
    m_density.fill(0, kPhaseBins * kAmpBins);
    m_densityCells.clear();
    m_densityMax = 0;
    m_peakCodes = 0.0;
    if (m_count <= 0) return;

    /*
     * 两趟扫描。
     * 必须先算出峰值，才能定幅度满量程；否则分桶会用上一轮的满量程
     * （首次甚至是 1.0），把全部幅值都挤进最高一档——分桶就白做了。
     */
    for (int i = 0; i < m_count; ++i) {
        const pdsample::PeakEvent &event = m_events.at(m_head + i);
        const double magnitude = std::fabs(event.adcCodes);
        m_peakCodes = std::max(m_peakCodes, magnitude);
        if (magnitude >= m_highlightThreshold) m_confirmedIndices.append(i);
    }

    const double fullScale = ampFullScale();
    for (int i = 0; i < m_count; ++i) {
        const pdsample::PeakEvent &event = m_events.at(m_head + i);
        const double magnitude = std::fabs(event.adcCodes);
        if (magnitude >= m_highlightThreshold) continue; /* 已确认的走精细几何 */

        int phaseBin = static_cast<int>(event.phaseWindow) * kPhaseBins / 4096;
        phaseBin = qBound(0, phaseBin, kPhaseBins - 1);
        int ampBin = static_cast<int>(magnitude / fullScale * kAmpBins);
        ampBin = qBound(0, ampBin, kAmpBins - 1);

        const int cell = phaseBin * kAmpBins + ampBin;
        if (m_density.at(cell) == 0) m_densityCells.append(cell);
        m_densityMax = std::max(m_densityMax, ++m_density[cell]);
    }
}

/* ------------------------------------------------------------------ 配置 */

void EllipsePanel::setHighlightThresholdCodes(double codes)
{
    if (qFuzzyCompare(m_highlightThreshold, codes)) return;
    m_highlightThreshold = codes;
    rebuildCaches();
    update();
}

void EllipsePanel::setScaleQ88(double scaleQ88)
{
    if (scaleQ88 <= 0.0 || qFuzzyCompare(m_scaleQ88, scaleQ88)) return;
    m_scaleQ88 = scaleQ88;
    rebuildCaches();
    update();
}

void EllipsePanel::setShowPicoCoulomb(bool showPc)
{
    if (m_showPc == showPc) return;
    m_showPc = showPc;
    update();
}

void EllipsePanel::setAmplitudeFullScaleCodes(double codes)
{
    if (qFuzzyCompare(m_fullScaleCodes, codes)) return;
    m_fullScaleCodes = codes;
    rebuildCaches();
    update();
}

void EllipsePanel::setBandLabel(const QString &band)
{
    m_band = band;
    update();
}

void EllipsePanel::setStatusText(const QString &text)
{
    m_status = text;
    update();
}

void EllipsePanel::setPulseStyle(PulseStyle style)
{
    m_style = style;
    update();
}

/* ------------------------------------------------------------------ 视图 */

void EllipsePanel::resetView()
{
    m_zoom = 1.0;
    m_amplitudeZoom = 1.0;
    m_panX = 0.0;
    m_panY = 0.0;
    update();
    emit viewChanged();
}

void EllipsePanel::zoomBy(double factor, const QPointF &widgetAnchor)
{
    if (factor <= 0.0) return;
    const double target = qBound(0.2, m_zoom * factor, 40.0);
    const double applied = target / m_zoom;
    if (qFuzzyCompare(applied, 1.0)) return;

    /*
     * 让鼠标下的那个点在缩放前后停在原处：
     * 椭圆相对坐标 u = (p − c) / R，缩放后要满足 p = c' + u·R'，
     * 故 c' = p − (p − c)·f，换成 pan 就是 p − (p − c)·f − c_base。
     */
    const QRectF area = plotRect();
    const double baseCx = area.center().x();
    const double baseCy = area.center().y();
    const double cx = baseCx + m_panX;
    const double cy = baseCy + m_panY;
    m_panX = widgetAnchor.x() - (widgetAnchor.x() - cx) * applied - baseCx;
    m_panY = widgetAnchor.y() - (widgetAnchor.y() - cy) * applied - baseCy;
    m_zoom = target;
    update();
    emit viewChanged();
}

void EllipsePanel::zoomAmplitudeBy(double factor)
{
    if (factor <= 0.0) return;
    const double target = qBound(0.2, m_amplitudeZoom * factor, 40.0);
    if (qFuzzyCompare(target, m_amplitudeZoom)) return;
    m_amplitudeZoom = target;
    update();
    emit viewChanged();
}

/* ------------------------------------------------------------------ 几何 */

QRectF EllipsePanel::plotRect() const
{
    /*
     * 顶部只留标题行，底部只留按钮条。
     * "点数/已确认/标定状态"这些小字挪到底部按钮条右侧——纵向空间比横向稀缺得多，
     * 多占一行就等于相位盘少十几像素。
     */
    return QRectF(rect()).adjusted(36, kHeaderHeight + 5, -12, -(kControlHeight + 7));
}

QPointF EllipsePanel::centre() const
{
    const QRectF area = plotRect();
    return QPointF(area.center().x() + m_panX, area.center().y() + m_panY);
}

double EllipsePanel::radiusX() const
{
    return plotRect().width() * 0.46 * m_zoom;
}

double EllipsePanel::radiusY() const
{
    return plotRect().height() * 0.42 * m_zoom;
}

QPointF EllipsePanel::ellipsePoint(double phaseDeg) const
{
    const int index = static_cast<int>(std::lround(phaseDeg / 360.0 * 4096.0)) & 4095;
    const QPointF c = centre();
    return QPointF(c.x() + radiusX() * trig().sinValue[index],
                   c.y() - radiusY() * trig().cosValue[index]);
}

double EllipsePanel::ampFullScale() const
{
    if (m_fullScaleCodes > 0.0) return m_fullScaleCodes;
    return std::max(m_peakCodes, 1.0);
}

double EllipsePanel::amplitudeToPixels(double adcCodes) const
{
    const double fullScale = ampFullScale();
    if (fullScale <= 0.0) return 0.0;
    const double fraction = qBound(0.0, std::fabs(adcCodes) / fullScale, 4.0);
    return radiusY() * 0.75 * fraction * m_amplitudeZoom;
}

/* ------------------------------------------------------------------ 单位 */

QString EllipsePanel::unitName() const
{
    return m_showPc ? QStringLiteral("pC") : QStringLiteral("码");
}

QString EllipsePanel::formatValue(double adcCodes) const
{
    /*
     * 换算依据（pd_axil_regs.v:360 + pd_feature_core.v:390,435）：
     *   场值 qRaw 读作 Q8.8 即为 pC → pC = adcCodes × scaleQ88 / 65536
     *   默认 scaleQ88 = 256（Q8.8 = 1.0）时，pC = adcCodes / 256。
     * 注意默认值只是占位标定，界面上会明确标出"未标定"。
     */
    if (m_showPc)
        return QString::number(adcCodes * m_scaleQ88 / 65536.0, 'f', 3);
    return QString::number(adcCodes, 'f', 0);
}

/* ------------------------------------------------------------------ 绘制 */

void EllipsePanel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kBackground);

    const QColor trace = kChannelColor[m_channel % 4];

    drawHeader(painter);

    const QRectF area = plotRect();
    painter.setPen(QPen(kFrame, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area);

    /*
     * 相位基准环先画：即使还没有事件，它也要在，作为读相位的参照。
     * 只在有数据时才画会让人误以为"图谱坏了"。
     */
    drawDial(painter);

    if (m_count <= 0) {
        painter.setPen(kTextDim);
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        /* 放在椭圆内部正中：椭圆轮廓（相位基准）完整可见，也不压 0°/180° 标注。 */
        painter.drawText(QRectF(area.left(), area.center().y() - 10, area.width(), 20),
                         Qt::AlignCenter,
                         QStringLiteral("等待峰值事件（采集中自动读取事件归档）"));
        drawControls(painter);
        return;
    }

    if (!m_enabled) {
        painter.setPen(kTextDim);
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("本通道已隐藏（点左下“显隐”恢复）"));
        drawControls(painter);
        return;
    }

    drawDensity(painter);
    drawConfirmed(painter);
    drawHighlight(painter);
    drawControls(painter);

    /* 缩放倍率角标：放大后必须让人知道自己在看什么比例。 */
    painter.setPen(kTextDim);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    painter.drawText(QRectF(area.right() - 150, area.top() + 2, 148, 14),
                     Qt::AlignRight,
                     QStringLiteral("图谱 ×%1 ｜ 幅度 ×%2")
                         .arg(m_zoom, 0, 'f', 2)
                         .arg(m_amplitudeZoom, 0, 'f', 2));
}

void EllipsePanel::drawHeader(QPainter &painter)
{
    const QColor trace = kChannelColor[m_channel % 4];
    const QRectF header(0, 0, width(), kHeaderHeight);

    QColor headerColor = kHeaderBackground;
    headerColor.setAlpha(230);
    painter.fillRect(header, headerColor);

    /* 通道徽章：彩色边框 + 通道号，与参考图一致。 */
    const QRectF badge(4, 3, 22, kHeaderHeight - 6);
    painter.setBrush(QColor(8, 14, 24, 220));
    painter.setPen(QPen(trace, 1.5));
    painter.drawRect(badge);
    painter.setPen(trace);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 11, QFont::Bold));
    painter.drawText(badge, Qt::AlignCenter, QString::number(m_channel + 1));

    /* 峰值（当前保留窗口内的最大幅值） */
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 12, QFont::Bold));
    painter.drawText(QRectF(32, 1, 170, kHeaderHeight - 2), Qt::AlignVCenter | Qt::AlignLeft,
                     QStringLiteral("%1 %2").arg(formatValue(m_peakCodes), unitName()));

    /* 右侧：频带标签 */
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    painter.setPen(QColor(225, 240, 250));
    painter.drawText(QRectF(width() - 132, 1, 128, kHeaderHeight - 2),
                     Qt::AlignVCenter | Qt::AlignRight,
                     m_band.isEmpty() ? QStringLiteral("频带未配置") : m_band);

}

void EllipsePanel::drawDial(QPainter &painter)
{
    const QColor trace = kChannelColor[m_channel % 4];
    const QPointF c = centre();
    const double rx = radiusX();
    const double ry = radiusY();

    /* 中轴线（相位 0/180 与 90/270 的定位依据） */
    painter.setPen(QPen(kGrid, 1, Qt::DashLine));
    painter.drawLine(QPointF(plotRect().left(), c.y()), QPointF(plotRect().right(), c.y()));
    painter.drawLine(QPointF(c.x(), plotRect().top()), QPointF(c.x(), plotRect().bottom()));

    /* 参考椭圆：相位刻度盘本体 */
    QColor ellipseColor = trace;
    ellipseColor.setAlpha(150);
    painter.setPen(QPen(ellipseColor, 1.3));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(c.x() - rx, c.y() - ry, rx * 2.0, ry * 2.0));

    /* 每 30° 一个刻度：短径向刻线，用来读相位 */
    QPainterPath ticks;
    for (int step = 0; step < 12; ++step) {
        const int index = step * 4096 / 12;
        const double sx = trig().sinValue[index];
        const double sy = trig().cosValue[index];
        ticks.moveTo(c.x() + rx * sx * 0.94, c.y() - ry * sy * 0.94);
        ticks.lineTo(c.x() + rx * sx * 1.06, c.y() - ry * sy * 1.06);
    }
    QColor tickColor = trace;
    tickColor.setAlpha(110);
    painter.setPen(QPen(tickColor, 1));
    painter.drawPath(ticks);

    /* 四个象限的相位标注，直接回答"椭圆与相位如何对应" */
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    painter.setPen(kTextDim);
    const QRectF area = plotRect();
    painter.drawText(QRectF(c.x() - 16, area.top() - 1, 32, 12), Qt::AlignCenter,
                     QStringLiteral("0°"));
    painter.drawText(QRectF(c.x() - 16, area.bottom() - 11, 32, 12), Qt::AlignCenter,
                     QStringLiteral("180°"));
    painter.drawText(QRectF(area.right() - 34, c.y() - 6, 32, 12), Qt::AlignCenter,
                     QStringLiteral("90°"));
    painter.drawText(QRectF(area.left() + 2, c.y() - 6, 32, 12), Qt::AlignCenter,
                     QStringLiteral("270°"));
}

void EllipsePanel::drawDensity(QPainter &painter)
{
    if (m_densityCells.isEmpty() || m_densityMax <= 0) return;

    const QColor trace = kChannelColor[m_channel % 4];
    const QPointF c = centre();
    const double rx = radiusX();
    const double ry = radiusY();

    /*
     * 每个非空格子画两个短刻（椭圆上下两侧各一），位置就是该(相位,幅值)对应的
     * 脉冲线端点。这样背景点自然形成"椭圆轮廓变厚 + 向外发毛"的观感，
     * 与参考图二放大后的形态一致。
     */
    QPainterPath dots;
    for (int cell : m_densityCells) {
        const int phaseBin = cell / kAmpBins;
        const int ampBin = cell % kAmpBins;
        const double phaseDeg = (phaseBin + 0.5) * 360.0 / kPhaseBins;
        const int index = static_cast<int>(std::lround(phaseDeg / 360.0 * 4096.0)) & 4095;
        const double sinValue = trig().sinValue[index];
        const double cosValue = trig().cosValue[index];

        const double x = c.x() + rx * sinValue;
        const double half = std::fabs(ry * cosValue);
        const double extension = radiusY() * 0.75 *
                                 ((ampBin + 0.5) / kAmpBins) * m_amplitudeZoom;

        dots.moveTo(x, c.y() - half - extension);
        dots.lineTo(x, c.y() - half - extension + 2.0);
        dots.moveTo(x, c.y() + half + extension - 2.0);
        dots.lineTo(x, c.y() + half + extension);
    }

    QColor densityColor = trace;
    densityColor.setAlpha(120);
    painter.setPen(QPen(densityColor, 1.4));
    painter.drawPath(dots);
}

void EllipsePanel::drawConfirmed(QPainter &painter)
{
    if (m_confirmedIndices.isEmpty()) return;

    const QColor trace = kChannelColor[m_channel % 4];
    const QPointF c = centre();
    const double rx = radiusX();
    const double ry = radiusY();

    const int drawn = std::min<int>(m_confirmedIndices.size(), kMaxConfirmedDrawn);

    QPainterPath lines;
    QPainterPath markers;
    for (int k = 0; k < drawn; ++k) {
        const pdsample::PeakEvent &event = m_events.at(m_head + m_confirmedIndices.at(k));
        const int index = static_cast<int>(event.phaseWindow) & 4095;
        const double sinValue = trig().sinValue[index];
        const double cosValue = trig().cosValue[index];

        const double x = c.x() + rx * sinValue;
        const double half = std::fabs(ry * cosValue);
        const double extension = amplitudeToPixels(event.adcCodes);

        if (m_style == PulseStyle::Vertical) {
            /* 参考图那种竖直对称线：跨过椭圆在该 x 处的上下边界，再各伸出与幅值成正比的长度。 */
            lines.moveTo(x, c.y() - half - extension);
            lines.lineTo(x, c.y() + half + extension);
        } else {
            /*
             * 径向辐条：从椭圆上的相位点沿径向（离心方向）伸出，长度∝幅值。
             * 极性决定伸向外侧还是内侧，相位指向比竖线更直观。
             */
            const double px = rx * sinValue;   /* 相对圆心的位置 */
            const double py = -ry * cosValue;
            const double length = std::sqrt(px * px + py * py);
            const double unitX = length > 1e-6 ? px / length : 0.0;
            const double unitY = length > 1e-6 ? py / length : 0.0;
            const double direction = event.positive ? 1.0 : -1.0;
            lines.moveTo(c.x() + px, c.y() + py);
            lines.lineTo(c.x() + px + unitX * extension * direction,
                         c.y() + py + unitY * extension * direction);
        }

        /* 端点标记：上方为正向、下方为负向，用来区分半周极性。 */
        const double end = event.positive ? (c.y() - half - extension)
                                         : (c.y() + half + extension);
        markers.addEllipse(QPointF(x, end), 2.0, 2.0);
    }

    QColor lineColor = trace;
    lineColor.setAlpha(235);
    painter.setPen(QPen(lineColor, 1.5));
    painter.drawPath(lines);

    painter.setPen(Qt::NoPen);
    painter.setBrush(trace.lighter(130));
    painter.drawPath(markers);
    painter.setBrush(Qt::NoBrush);
}

void EllipsePanel::drawHighlight(QPainter &painter)
{
    if (m_highlighted < 0 || m_highlighted >= m_count) return;
    const pdsample::PeakEvent &event = m_events.at(m_head + m_highlighted);

    const QPointF c = centre();
    const double rx = radiusX();
    const double ry = radiusY();
    const int index = static_cast<int>(event.phaseWindow) & 4095;
    const double sinValue = trig().sinValue[index];
    const double cosValue = trig().cosValue[index];
    const double x = c.x() + rx * sinValue;
    const double half = std::fabs(ry * cosValue);
    const double extension = amplitudeToPixels(event.adcCodes);

    /* 十字准星 + 白色加粗线，明确"就是这一条" */
    painter.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DotLine));
    painter.drawLine(QPointF(plotRect().left(), c.y() - half - extension),
                     QPointF(plotRect().right(), c.y() - half - extension));
    painter.drawLine(QPointF(plotRect().left(), c.y() + half + extension),
                     QPointF(plotRect().right(), c.y() + half + extension));

    painter.setPen(QPen(Qt::white, 2.4));
    painter.drawLine(QPointF(x, c.y() - half - extension), QPointF(x, c.y() + half + extension));

    /* 从圆心到该相位点的径向线：把"相位 θ"这件事画出来 */
    painter.setPen(QPen(QColor(255, 255, 255, 120), 1, Qt::DashLine));
    painter.drawLine(c, QPointF(x, c.y() - ry * cosValue));
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawEllipse(QPointF(x, c.y() - ry * cosValue), 3.0, 3.0);

    /* 读数框 */
    const QString readout =
        QStringLiteral("CH%1 ｜ 相位 %2° ｜ %3 %4 ｜ %5")
            .arg(m_channel + 1)
            .arg(event.phaseDeg, 0, 'f', 2)
            .arg(formatValue(std::fabs(event.adcCodes)), unitName())
            .arg(event.positive ? QStringLiteral("正极性") : QStringLiteral("负极性"))
            .arg(QStringLiteral("AD 码 %1").arg(std::fabs(event.adcCodes), 0, 'f', 0));

    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    const QFontMetrics metrics(painter.font());
    const QRectF textRect = metrics.boundingRect(readout).adjusted(-6, -3, 6, 3);
    QRectF box(QPointF(0.0, 0.0), textRect.size());
    box.moveTopLeft(QPointF(plotRect().left() + 4, plotRect().bottom() - box.height() - 20));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(8, 14, 24, 225));
    painter.drawRect(box);
    painter.setPen(QPen(kFrame, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, readout);
}

void EllipsePanel::drawControls(QPainter &painter)
{
    /* 参考图左下角那三个小按钮：复位 / 适配 / 显隐。做成可点区域，不用子控件。 */
    const QRectF area = plotRect();
    const QString labels[3] = {QStringLiteral("复位"), QStringLiteral("适配"),
                               QStringLiteral("显隐")};
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    double x = area.left();
    for (int i = 0; i < 3; ++i) {
        const QRectF box(x, area.bottom() + 3, 40, kControlHeight - 4);
        const bool highlighted = (i == 2 && m_enabled);
        painter.setPen(QPen(highlighted ? kFrame : kGrid, 1));
        painter.setBrush(QColor(20, 30, 44));
        painter.drawRect(box);
        painter.setPen(highlighted ? QColor(210, 235, 250) : kTextDim);
        painter.drawText(box, Qt::AlignCenter, labels[i]);
        x += 44;
    }
    painter.setPen(kTextDim);
    QString detail =
        QStringLiteral("高亮阈值 %1 %2 ｜ %3 点 ｜ 已确认 %4")
            .arg(formatValue(m_highlightThreshold), unitName())
            .arg(m_count)
            .arg(m_confirmedIndices.size());
    if (m_showPc && qFuzzyCompare(m_scaleQ88, pdsample::eventpacket::kDefaultScaleQ88))
        detail += QStringLiteral(" ｜ pC 未标定(SCALE=1.0)");
    if (!m_status.isEmpty()) detail += QStringLiteral(" ｜ ") + m_status;
    painter.drawText(QRectF(x + 4, area.bottom() + 3, area.right() - x - 4, kControlHeight - 4),
                     Qt::AlignVCenter | Qt::AlignLeft, detail);
}

void EllipsePanel::setEnabledChannel(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    update();
    emit enableToggled(m_enabled);
}

/* ------------------------------------------------------------------ 联动 */

bool EllipsePanel::highlightNearestByAmplitude(double adcCodes)
{
    if (m_count <= 0) return false;
    int best = -1;
    double bestDistance = 0.0;
    const double magnitude = std::fabs(adcCodes);
    for (int i = 0; i < m_count; ++i) {
        const pdsample::PeakEvent &event = m_events.at(m_head + i);
        const double distance = std::fabs(std::fabs(event.adcCodes) - magnitude);
        if (best < 0 || distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    if (best < 0) return false;
    m_highlighted = best;
    update();
    return true;
}

bool EllipsePanel::highlightNearestByPhase(double phaseDeg)
{
    if (m_count <= 0) return false;
    int best = -1;
    double bestDistance = 0.0;
    for (int i = 0; i < m_count; ++i) {
        double delta = std::fabs(m_events.at(m_head + i).phaseDeg - phaseDeg);
        if (delta > 180.0) delta = 360.0 - delta;
        if (best < 0 || delta < bestDistance) {
            best = i;
            bestDistance = delta;
        }
    }
    if (best < 0) return false;
    m_highlighted = best;
    update();
    return true;
}

void EllipsePanel::clearHighlight()
{
    if (m_highlighted < 0) return;
    m_highlighted = -1;
    update();
}

int EllipsePanel::pickEvent(const QPointF &widgetPoint) const
{
    if (m_count <= 0) return -1;
    const QPointF c = centre();
    const double rx = radiusX();
    const double ry = radiusY();
    const double tolerance = 20.0;

    int best = -1;
    double bestDistance = tolerance;
    /*
     * 已确认的优先命中（它们才是"看得见的线"）；一个都没命中时再退回全部点，
     * 这样点空白处也不会莫名其妙选中一个背景点。
     */
    const bool useConfirmed = !m_confirmedIndices.isEmpty();
    const int count = useConfirmed ? m_confirmedIndices.size() : m_count;
    for (int k = 0; k < count; ++k) {
        const int index = useConfirmed ? m_confirmedIndices.at(k) : k;
        const pdsample::PeakEvent &event = m_events.at(m_head + index);
        const int window = static_cast<int>(event.phaseWindow) & 4095;
        const double x = c.x() + rx * trig().sinValue[window];
        const double half = std::fabs(ry * trig().cosValue[window]);
        const double extension = amplitudeToPixels(event.adcCodes);
        /* 点到竖线段的距离：x 距离与 y 落在区间内的处理。 */
        const double dx = std::fabs(widgetPoint.x() - x);
        if (dx > tolerance) continue;
        const double yLow = c.y() - half - extension;
        const double yHigh = c.y() + half + extension;
        double dy = 0.0;
        if (widgetPoint.y() < yLow) dy = yLow - widgetPoint.y();
        else if (widgetPoint.y() > yHigh) dy = widgetPoint.y() - yHigh;
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ 交互 */

void EllipsePanel::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) return;
    const double factor = std::pow(1.2, steps);
    if (event->modifiers() & Qt::ControlModifier)
        zoomAmplitudeBy(factor);
    else
        zoomBy(factor, event->position());
    event->accept();
}

void EllipsePanel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const QPointF position = event->position();

    /* 底部三个按钮：命中则改动作，不进入平移/拾取。 */
    const QRectF area = plotRect();
    m_pressedControl = -1;
    if (position.y() >= area.bottom() + 3 && position.y() <= area.bottom() + kControlHeight) {
        for (int i = 0; i < 3; ++i) {
            const QRectF box(area.left() + i * 44.0, area.bottom() + 3, 40, kControlHeight - 4);
            if (!box.contains(position)) continue;
            m_pressedControl = i;
            if (i == 0) resetView();
            else if (i == 1) {
                m_zoom = 1.0;
                m_amplitudeZoom = 1.0;
                m_panX = 0.0;
                m_panY = 0.0;
                m_fullScaleCodes = 0.0;
                rebuildCaches();
                update();
                emit viewChanged();
            } else {
                setEnabledChannel(!m_enabled);
            }
            return;
        }
    }

    m_panning = true;
    m_movedWhilePressed = false;
    m_lastMouse = position.toPoint();
    setCursor(Qt::ClosedHandCursor);
}

void EllipsePanel::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_panning) return;
    const QPoint position = event->position().toPoint();
    const QPoint delta = position - m_lastMouse;
    if (std::abs(delta.x()) + std::abs(delta.y()) > 2) m_movedWhilePressed = true;
    m_panX += delta.x();
    m_panY += delta.y();
    m_lastMouse = position;
    update();
}

void EllipsePanel::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const bool wasPanning = m_panning;
    m_panning = false;
    setCursor(Qt::CrossCursor);
    if (!wasPanning || m_movedWhilePressed) {
        if (m_movedWhilePressed) emit viewChanged();
        return;
    }

    const int index = pickEvent(event->position());
    if (index < 0) {
        clearHighlight();
        return;
    }
    m_highlighted = index;
    update();
    const pdsample::PeakEvent &event2 = m_events.at(m_head + index);
    emit eventPicked(index, event2.phaseDeg, event2.adcCodes, event2.positive);
}

void EllipsePanel::mouseDoubleClickEvent(QMouseEvent *)
{
    resetView();
}

#include "prpd_panel.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace {

const QColor kBackground(13, 19, 31);
const QColor kHeaderBackground(0, 137, 212);
const QColor kFrame(0, 179, 242);
const QColor kGrid(46, 66, 88);
const QColor kTextDim(150, 175, 190);

const QColor kTrace[4] = {
    QColor(240, 216, 60),
    QColor(120, 230, 180),
    QColor(240, 120, 90),
    QColor(205, 120, 240),
};

constexpr int kHeaderHeight = 28;

/* 密度网格：按 (相位桶 × 幅值桶) 聚合，与控件尺寸无关，缩放窗口不必重建。 */
constexpr int kPhaseBins = 180; /* 2°／桶 */
constexpr int kAmpBins = 64;

/* 精细绘制的已高亮上限：超过就只画前若干个，避免噪声把渲染拖死。 */
constexpr int kMaxHighlightDrawn = 400;

} // namespace

PrpdPanel::PrpdPanel(int channel, QWidget *parent) : QWidget(parent), m_channel(channel & 3)
{
    setMinimumSize(240, 130);
    setToolTip(QStringLiteral(
        "标准 PRPD 直角坐标：横轴 相位 0..360°，纵轴 幅值（原始 Q8.8）。\n"
        "背景点以密度网格绘制；超过阈值的点用亮色标出。\n"
        "同页的椭圆图谱是同一批事件的另一种呈现。"));
}

/* ------------------------------------------------------------------ 数据 */

void PrpdPanel::setPoints(const QVector<QPointF> &points)
{
    m_points.clear();
    m_times.clear();
    m_head = 0;
    m_count = 0;
    if (!points.isEmpty()) {
        m_points.reserve(points.size());
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (const QPointF &point : points) {
            m_points.append(point);
            m_times.append(now);
        }
    }
    m_count = m_points.size();
    prune();
    rebuildCaches();
    update();
}

void PrpdPanel::appendPoints(const QVector<QPointF> &points)
{
    if (points.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_points.reserve(m_points.size() + points.size());
    m_times.reserve(m_times.size() + points.size());
    for (const QPointF &point : points) {
        m_points.append(point);
        m_times.append(now);
    }
    m_count = m_points.size() - m_head;
    prune();
    compactIfNeeded();
    rebuildCaches();
    update();
}

void PrpdPanel::clear()
{
    m_points.clear();
    m_times.clear();
    m_head = 0;
    m_count = 0;
    rebuildCaches();
    update();
}

void PrpdPanel::setAgingSeconds(double seconds)
{
    m_agingSeconds = seconds;
    prune();
    rebuildCaches();
    update();
}

void PrpdPanel::setStatusText(const QString &text)
{
    if (m_status == text) return;
    m_status = text;
    update();
}

void PrpdPanel::prune()
{
    if (m_count <= 0) return;
    int drop = 0;
    if (m_agingSeconds > 0.0) {
        const qint64 cutoff =
            QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(m_agingSeconds * 1000.0);
        while (drop < m_count && m_times.at(m_head + drop) < cutoff) ++drop;
    }
    const int excess = m_count - drop - m_maximumPoints;
    if (excess > 0) drop += excess;
    if (drop > 0) {
        m_head += drop;
        m_count -= drop;
    }
}

void PrpdPanel::compactIfNeeded()
{
    if (m_head < 8192 || m_head * 2 < m_points.size()) return;
    m_points.remove(0, m_head);
    m_times.remove(0, m_head);
    m_head = 0;
    m_count = m_points.size();
}

void PrpdPanel::rebuildCaches()
{
    m_peak = 0.0;
    m_newest = -1;
    m_highlighted.clear();
    m_density.fill(0, kPhaseBins * kAmpBins);
    m_densityCells.clear();
    m_densityMax = 0;
    if (m_count <= 0) return;

    /* 第一趟：峰值 + 高亮点 + 最新点。 */
    qint64 newestTime = 0;
    for (int i = 0; i < m_count; ++i) {
        const QPointF &point = m_points.at(m_head + i);
        const double magnitude = std::fabs(point.y());
        m_peak = std::max(m_peak, magnitude);
        if (m_highlightThreshold > 0.0 && magnitude >= m_highlightThreshold)
            m_highlighted.append(i);
        const qint64 time = m_times.at(m_head + i);
        if (time >= newestTime) {
            newestTime = time;
            m_newest = i;
        }
    }

    /* 第二趟：背景点入密度网格。必须先有峰值才能定纵轴满量程，所以分两趟。 */
    const double reference = std::max(m_peak, 1e-3);
    for (int i = 0; i < m_count; ++i) {
        const QPointF &point = m_points.at(m_head + i);
        const double magnitude = std::fabs(point.y());
        if (m_highlightThreshold > 0.0 && magnitude >= m_highlightThreshold)
            continue; /* 高亮的走精细几何 */

        int phaseBin = static_cast<int>(point.x() / 360.0 * kPhaseBins);
        phaseBin = qBound(0, phaseBin, kPhaseBins - 1);
        int ampBin = static_cast<int>((point.y() / reference + 1.0) * 0.5 * kAmpBins);
        ampBin = qBound(0, ampBin, kAmpBins - 1);

        const int cell = phaseBin * kAmpBins + ampBin;
        if (m_density.at(cell) == 0) m_densityCells.append(cell);
        m_densityMax = std::max(m_densityMax, ++m_density[cell]);
    }
}

/* ------------------------------------------------------------------ 绘制 */

void PrpdPanel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor trace = kTrace[m_channel % 4];
    painter.fillRect(rect(), kBackground);

    /* ---- 头部：通道号 + 峰值 + 点数 + 状态 ---- */
    const QRectF header(0, 0, width(), kHeaderHeight);
    QColor headerColor = kHeaderBackground;
    headerColor.setAlpha(225);
    painter.fillRect(header, headerColor);

    const QRectF badge(4, 3, 22, kHeaderHeight - 6);
    painter.setBrush(QColor(8, 14, 24, 220));
    painter.setPen(QPen(trace, 1.5));
    painter.drawRect(badge);
    painter.setPen(trace);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 11, QFont::Bold));
    painter.drawText(badge, Qt::AlignCenter, QString::number(m_channel + 1));

    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 10, QFont::Bold));
    painter.drawText(QRectF(32, 1, 200, kHeaderHeight - 2), Qt::AlignVCenter | Qt::AlignLeft,
                     QStringLiteral("峰 %1 raw ｜ %2 点 ｜ 高亮 %3")
                         .arg(m_peak, 0, 'f', 1)
                         .arg(m_count)
                         .arg(m_highlighted.size()));

    if (!m_band.isEmpty()) {
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        painter.setPen(QColor(225, 240, 250));
        painter.drawText(QRectF(width() - 132, 1, 128, kHeaderHeight - 2),
                         Qt::AlignVCenter | Qt::AlignRight, m_band);
    }

    const QRectF area = rect().adjusted(34, kHeaderHeight + 6, -14, -26);

    /* ---- 坐标框与刻度 ---- */
    painter.setPen(QPen(kFrame, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area);
    painter.setPen(QPen(kGrid, 1));
    for (int degree = 90; degree < 360; degree += 90) {
        const qreal x = area.left() + area.width() * degree / 360.0;
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    const qreal midY = area.center().y();
    painter.setPen(QPen(QColor(90, 120, 140), 1, Qt::DashLine));
    painter.drawLine(QPointF(area.left(), midY), QPointF(area.right(), midY));

    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
    painter.setPen(kTextDim);
    painter.drawText(area.left(), height() - 6, QStringLiteral("0°"));
    painter.drawText(static_cast<int>(area.center().x()) - 12, height() - 6,
                     QStringLiteral("180°"));
    painter.drawText(static_cast<int>(area.right()) - 26, height() - 6, QStringLiteral("360°"));

    if (m_count <= 0) {
        painter.drawText(area, Qt::AlignCenter,
                         QStringLiteral("PRPD 相位-幅值散点 ｜ 等待峰值事件\n"
                                        "（采集运行中时自动读取事件归档）"));
        return;
    }

    const double reference = std::max(m_peak, 1e-3);
    const auto toPixel = [&area, reference, midY](double phaseDeg, double value) {
        const double x = area.left() + area.width() * qBound(0.0, phaseDeg, 360.0) / 360.0;
        const double normalized = qBound(-1.0, value / reference, 1.0);
        const double y = midY - normalized * area.height() * 0.48;
        return QPointF(x, y);
    };

    /* ---- 背景：密度网格，只遍历非空格子 ---- */
    if (!m_densityCells.isEmpty() && m_densityMax > 0) {
        const double cellWidth = area.width() / kPhaseBins;
        const double cellHeight = area.height() / kAmpBins;
        for (int cell : m_densityCells) {
            const int phaseBin = cell / kAmpBins;
            const int ampBin = cell % kAmpBins;
            const double phaseDeg = (phaseBin + 0.5) * 360.0 / kPhaseBins;
            const double value = ((ampBin + 0.5) / kAmpBins * 2.0 - 1.0) * reference;
            const QPointF point = toPixel(phaseDeg, value);

            QColor color = trace;
            color.setAlpha(70 + 150 * m_density.at(cell) / m_densityMax);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRect(QRectF(point.x(), point.y() - cellHeight * 0.5,
                                    qMax(1.0, cellWidth), qMax(1.0, cellHeight)));
        }
        painter.setBrush(Qt::NoBrush);
    }

    /* ---- 前景：超过阈值的点，用亮色真实几何绘制 ---- */
    const int drawn = std::min<int>(m_highlighted.size(), kMaxHighlightDrawn);
    if (drawn > 0) {
        QColor color = trace.lighter(140);
        painter.setPen(QPen(color, 1.6));
        for (int k = 0; k < drawn; ++k) {
            const QPointF &sample = m_points.at(m_head + m_highlighted.at(k));
            painter.drawPoint(toPixel(sample.x(), sample.y()));
        }
    }

    /* ---- 最新点：让"正在更新"可见 ---- */
    if (m_newest >= 0) {
        const QPointF &sample = m_points.at(m_head + m_newest);
        painter.setPen(QPen(Qt::white, 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(toPixel(sample.x(), sample.y()), 3.5, 3.5);
    }

    if (drawn < m_highlighted.size()) {
        painter.setPen(QColor(240, 190, 90));
        painter.drawText(QRectF(area.left() + 4, area.bottom() - 16, area.width() - 8, 14),
                         Qt::AlignLeft,
                         QStringLiteral("高亮点已按上限截断：%1 个只画 %2 个")
                             .arg(m_highlighted.size())
                             .arg(drawn));
    }

    if (!m_status.isEmpty()) {
        painter.setPen(kTextDim);
        painter.drawText(QRectF(area.left() + 4, area.top() + 2, area.width() - 8, 14),
                         Qt::AlignLeft, m_status);
    }
}

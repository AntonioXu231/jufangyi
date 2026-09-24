#include "plot_widget.h"

#include <QPainter>
#include <QPainterPath>

namespace {
const QVector<QColor> kColors = {
    QColor(20, 105, 205), QColor(210, 75, 40), QColor(25, 150, 90), QColor(155, 70, 175)
};
}

PlotWidget::PlotWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(260);
    setAutoFillBackground(true);
}

void PlotWidget::setLines(const QVector<QVector<QPointF>> &series, const QStringList &names,
                          const QString &title, const QString &xLabel, const QString &yLabel)
{
    m_mode = Mode::Lines;
    m_lines = series;
    m_bars.clear();
    m_names = names;
    m_title = title;
    m_xLabel = xLabel;
    m_yLabel = yLabel;
    update();
}

void PlotWidget::setBars(const QVector<quint32> &values, const QString &title,
                         const QString &xLabel, const QString &yLabel)
{
    m_mode = Mode::Bars;
    m_bars = values;
    m_lines.clear();
    m_names.clear();
    m_title = title;
    m_xLabel = xLabel;
    m_yLabel = yLabel;
    update();
}

void PlotWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), Qt::white);
    const QRectF area = rect().adjusted(64, 34, -24, -48);
    painter.setPen(QPen(QColor(75, 75, 75), 1));
    painter.drawRect(area);
    painter.drawText(QRectF(0, 4, width(), 24), Qt::AlignCenter, m_title);
    painter.drawText(QRectF(area.left(), height() - 30, area.width(), 20), Qt::AlignCenter, m_xLabel);
    painter.save();
    painter.translate(17, area.center().y()); painter.rotate(-90);
    painter.drawText(QRectF(-area.height() / 2, -12, area.height(), 20), Qt::AlignCenter, m_yLabel);
    painter.restore();
    if (m_mode == Mode::Empty) {
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("等待数据"));
        return;
    }

    qreal xmin = 0.0, xmax = 1.0, ymin = 0.0, ymax = 1.0;
    if (m_mode == Mode::Lines) {
        bool first = true;
        for (const auto &one : m_lines) for (const auto &p : one) {
            if (first) { xmin = xmax = p.x(); ymin = ymax = p.y(); first = false; }
            else { xmin = qMin(xmin, p.x()); xmax = qMax(xmax, p.x()); ymin = qMin(ymin, p.y()); ymax = qMax(ymax, p.y()); }
        }
    } else {
        xmax = qMax(1, m_bars.size() - 1); ymax = 1.0;
        for (const quint32 value : m_bars) ymax = qMax(ymax, static_cast<qreal>(value));
    }
    if (qFuzzyCompare(xmin, xmax)) xmax = xmin + 1.0;
    if (qFuzzyCompare(ymin, ymax)) { ymin -= 1.0; ymax += 1.0; }
    const auto map = [&area, xmin, xmax, ymin, ymax](QPointF p) {
        return QPointF(area.left() + (p.x() - xmin) * area.width() / (xmax - xmin),
                       area.bottom() - (p.y() - ymin) * area.height() / (ymax - ymin));
    };
    painter.setPen(QPen(QColor(160, 160, 160), 1, Qt::DashLine));
    for (int i = 1; i < 4; ++i) {
        const qreal y = area.top() + i * area.height() / 4.0;
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    painter.setPen(QColor(50, 50, 50));
    painter.drawText(5, static_cast<int>(area.top()) + 4, QString::number(ymax, 'g', 5));
    painter.drawText(5, static_cast<int>(area.bottom()), QString::number(ymin, 'g', 5));
    if (m_mode == Mode::Bars) {
        const qreal width = area.width() / qMax(1, m_bars.size());
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(30, 120, 210));
        for (int i = 0; i < m_bars.size(); ++i) {
            const qreal h = m_bars[i] * area.height() / ymax;
            painter.drawRect(QRectF(area.left() + i * width + 1, area.bottom() - h,
                                    qMax(1.0, width - 2), h));
        }
        return;
    }
    for (int series = 0; series < m_lines.size(); ++series) {
        const auto &points = m_lines[series];
        if (points.isEmpty()) continue;
        QPainterPath path(map(points.front()));
        for (int i = 1; i < points.size(); ++i) path.lineTo(map(points[i]));
        painter.setPen(QPen(kColors[series % kColors.size()], 1.2));
        painter.drawPath(path);
        if (series < m_names.size()) {
            painter.drawText(area.right() - 70, area.top() + 16 + series * 16, m_names[series]);
        }
    }
}

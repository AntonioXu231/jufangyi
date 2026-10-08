#include "fft_widget.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace {
constexpr double kDbFloor = -100.0;
}

FftWidget::FftWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(140);
}

void FftWidget::setSpectrum(const ScopeSpectrum &spectrum, const QColor &traceColor)
{
    m_traceColor = traceColor;
    m_sequence = spectrum.sequence;
    m_fromArchive = spectrum.fromArchive;
    m_startSample = spectrum.startSample;
    m_dcCode = spectrum.dcCode;
    if (spectrum.sampleRateHz > 0U) m_sampleRateHz = spectrum.sampleRateHz;
    m_binHz = spectrum.binHz;
    m_peakHz = spectrum.peakHz;
    if (spectrum.magnitudes.size() != 513 || m_binHz == 0U) {
        m_db.clear();
        update();
        return;
    }
    m_db.resize(spectrum.magnitudes.size());
    m_peakDb = kDbFloor;
    for (int bin = 0; bin < spectrum.magnitudes.size(); ++bin) {
        const double amplitude = static_cast<double>(spectrum.magnitudes[bin]) / 2048.0;
        const double db = 20.0 * std::log10(qMax(amplitude, 1.0e-12));
        m_db[bin] = qMax(kDbFloor, db);
    }
    m_peakDb = 20.0 * std::log10(qMax(1.0e-12,
        static_cast<double>(spectrum.amplitudeCode) / 2048.0));
    update();
}

void FftWidget::setWaiting(const QColor &traceColor)
{
    m_traceColor = traceColor;
    m_fromArchive = false;
    m_startSample = 0U;
    m_db.clear();
    m_peakHz = 0.0;
    m_peakDb = kDbFloor;
    update();
}

void FftWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(8, 15, 25));
    const QRectF area = rect().adjusted(58, 27, -18, -34);
    painter.setPen(QPen(QColor(58, 105, 130), 1));
    painter.drawRect(area);
    painter.setPen(QPen(QColor(52, 81, 100), 1, Qt::DashLine));
    for (int db = -20; db >= -80; db -= 20) {
        const double y = area.top() + (-db / 100.0) * area.height();
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    for (int i = 1; i < 4; ++i) {
        const double x = area.left() + area.width() * i / 4.0;
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    painter.setPen(QColor(185, 215, 230));
    painter.drawText(4, static_cast<int>(area.top() + 5), QStringLiteral("幅值 dBFS"));
    painter.drawText(22, static_cast<int>(area.top() + 12), QStringLiteral("0"));
    painter.drawText(13, static_cast<int>(area.bottom()), QStringLiteral("-100"));
    painter.drawText(area.left(), height() - 8, QStringLiteral("0 Hz"));
    painter.drawText(area.right() - 112, height() - 8,
                     QStringLiteral("%1 MHz").arg(m_sampleRateHz / 2.0e6, 0, 'f', 2));
    if (m_db.isEmpty()) {
        painter.drawText(area, Qt::AlignCenter, QStringLiteral("等待 PS 返回与当前波形帧匹配的 FFT 频谱"));
        return;
    }
    const QString sourceLabel = m_fromArchive
        ? QStringLiteral("SNAP #%1 起点 %2").arg(m_sequence).arg(m_startSample)
        : QStringLiteral("实时帧 #%1").arg(m_sequence);
    painter.drawText(area.left(), 16,
        QStringLiteral("PS FFT %1 点 · %2 · Δf %3 Hz · DC %4 · 主峰 %5 Hz (%6 dBFS)")
            .arg((m_db.size() - 1) * 2).arg(sourceLabel)
            .arg(m_binHz, 0, 'f', 1)
            .arg(m_dcCode)
            .arg(m_peakHz)
            .arg(m_peakDb, 0, 'f', 1));
    QPainterPath path;
    for (int bin = 0; bin < m_db.size(); ++bin) {
        const double x = area.left() + bin * area.width() / qMax(1, m_db.size() - 1);
        const double y = area.bottom() - (m_db[bin] - kDbFloor) * area.height() / -kDbFloor;
        if (bin == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    painter.setClipRect(area);
    painter.setPen(QPen(m_traceColor, 1.3));
    painter.drawPath(path);
    if (m_peakHz > 0.0 && m_peakDb > kDbFloor) {
        const int peakBin = qBound(0, qRound(m_peakHz / m_binHz), m_db.size() - 1);
        const double x = area.left() + peakBin * area.width() / qMax(1, m_db.size() - 1);
        const double y = area.bottom() - (m_db[peakBin] - kDbFloor) * area.height() / -kDbFloor;
        painter.setPen(QPen(m_traceColor.lighter(145), 1, Qt::DashLine));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        painter.setBrush(m_traceColor);
        painter.drawEllipse(QPointF(x, y), 4.0, 4.0);
    }
}

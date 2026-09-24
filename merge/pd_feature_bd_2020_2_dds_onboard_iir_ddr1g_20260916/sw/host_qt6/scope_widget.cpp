#include "scope_widget.h"

#include <QDateTime>
#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {

const QColor kBackground(13, 19, 31);
const QColor kGrid(38, 62, 88);
const QColor kGridStrong(58, 96, 130);
const QColor kAxisText(178, 208, 224);
const QColor kMidLine(96, 122, 148);
const QColor kTriggerLine(255, 196, 64);
const QColor kCursorAColor(120, 220, 255);
const QColor kCursorBColor(255, 140, 200);
const QColor kGapMarker(170, 110, 200);

const QColor kTrace[4] = {
    QColor(255, 215, 0), QColor(53, 224, 192), QColor(255, 107, 90), QColor(224, 96, 255)
};

/* 按数量级选择时间单位，避免刻度上出现 0.000039 这种不可读的数字。 */
QString timeUnitFor(double seconds, double &scale)
{
    const double magnitude = std::fabs(seconds);
    if (magnitude < 1e-6) { scale = 1e9; return QStringLiteral("ns"); }
    if (magnitude < 1e-3) { scale = 1e6; return QStringLiteral("µs"); }
    if (magnitude < 1.0) { scale = 1e3; return QStringLiteral("ms"); }
    scale = 1.0;
    return QStringLiteral("s");
}

} // namespace

double ScopeWidget::PlotArea::pixelToSample(double x) const
{
    if (rect.width() <= 0.0) return xMin;
    return xMin + (x - rect.left()) * (xMax - xMin) / rect.width();
}

double ScopeWidget::PlotArea::sampleToPixel(double sample) const
{
    if (xMax <= xMin) return rect.left();
    return rect.left() + (sample - xMin) * rect.width() / (xMax - xMin);
}

double ScopeWidget::PlotArea::codeToPixel(double code) const
{
    if (yMax <= yMin) return rect.center().y();
    return rect.bottom() - (code - yMin) * rect.height() / (yMax - yMin);
}

const QColor &ScopeWidget::channelColor(int channel)
{
    return kTrace[qBound(0, channel, 3)];
}

QString ScopeWidget::channelName(int channel)
{
    return QStringLiteral("CH%1").arg(qBound(0, channel, 3));
}

ScopeWidget::ScopeWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(160);
    setMinimumWidth(220);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setToolTip(QStringLiteral(
        "滚轮：缩放时间轴（以鼠标位置为锚点）\n"
        "Ctrl+滚轮：缩放幅度轴\n"
        "Shift+滚轮 / 左键拖拽：平移\n"
        "右键拖拽：放置并拖动测量游标 A/B\n"
        "双击：复位视图\n"
        "A 键：开启/关闭游标"));
}

/* ------------------------------------------------------------------ 数据入口 */

void ScopeWidget::setStaticTrace(const pdsample::WaveformFrame &frame, const QString &label)
{
    m_mode = Mode::Static;
    m_frame = frame;
    m_sampleRateHz = frame.sampleRateHz;
    m_label = label;
    m_triggerIndex = -1;
    m_triggerValid = false;
    m_viewInitialised = false;
    m_userAdjustedView = false;
    m_viewStart = 0.0;
    m_viewSpan = qMax(16, frame.sampleCount);
    bumpDataVersion();
    requestRepaint();
    emit viewChanged();
}

void ScopeWidget::setLiveFrame(const pdsample::WaveformFrame &frame, int triggerIndex,
                              bool triggerValid, double triggerLevel, int triggerChannel)
{
    m_mode = Mode::Live;
    m_frame = frame;
    m_sampleRateHz = frame.sampleRateHz;
    m_triggerIndex = triggerIndex;
    m_triggerValid = triggerValid;
    m_triggerLevel = triggerLevel;
    m_triggerChannel = qBound(0, triggerChannel, 3);
    m_label = QStringLiteral("实时窗口");

    if (!m_viewInitialised || !m_userAdjustedView) {
        m_viewStart = 0.0;
        m_viewSpan = qMax(16, frame.sampleCount);
        m_viewInitialised = true;
    }
    /* 帧长变化（用户改了时基）时，把视图夹回数据范围，避免看到空白。 */
    if (m_viewStart + m_viewSpan > frame.sampleCount)
        m_viewStart = qMax(0.0, frame.sampleCount - m_viewSpan);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastArrivalMs > 0 && now > m_lastArrivalMs)
        m_lastFrameIntervalSec = (now - m_lastArrivalMs) / 1000.0;
    m_lastArrivalMs = now;

    bumpDataVersion();
    requestRepaint();
    emit viewChanged();
}

void ScopeWidget::appendRollFrame(const pdsample::WaveformFrame &frame)
{
    m_mode = Mode::Roll;
    m_label = QStringLiteral("滚动历史");
    if (frame.sampleRateHz > 0.0) m_sampleRateHz = frame.sampleRateHz;

    const int count = frame.sampleCount;
    if (count <= 0) return;

    /* 先整帧追加，再按"整帧"为单位从头部裁掉，保证帧边界与起始下标始终一致。 */
    m_rollFrameStart.append(m_roll[0].size());
    m_rollSequence.append(frame.sequence);
    for (int c = 0; c < pdsample::kChannelCount; ++c) m_roll[c] += frame.channel[c];

    int trim = 0;
    while (m_rollFrameStart.size() > 1 && (m_roll[0].size() - trim) > m_rollCapacity) {
        /* 丢掉第 0 帧：其占用 [0, start1) 共 start1 个样本。 */
        trim = m_rollFrameStart.at(1);
        m_rollFrameStart.removeFirst();
        m_rollSequence.removeFirst();
    }
    if (trim > 0) {
        for (int c = 0; c < pdsample::kChannelCount; ++c) m_roll[c].remove(0, trim);
        for (int i = 0; i < m_rollFrameStart.size(); ++i) m_rollFrameStart[i] -= trim;
        if (m_userAdjustedView) m_viewStart = qMax(0.0, m_viewStart - trim);
    }

    if (!m_viewInitialised || !m_userAdjustedView) {
        /* 未手动缩放时，始终显示最新的一个时基宽度，形成"滚动"观感。 */
        m_viewSpan = qMax(16, count);
        m_viewStart = qMax(0.0, static_cast<double>(m_roll[0].size()) - m_viewSpan);
        m_viewInitialised = true;
    } else {
        const double total = m_roll[0].size();
        m_viewStart = qBound(0.0, m_viewStart, qMax(0.0, total - m_viewSpan));
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastArrivalMs > 0 && now > m_lastArrivalMs)
        m_lastFrameIntervalSec = (now - m_lastArrivalMs) / 1000.0;
    m_lastArrivalMs = now;

    bumpDataVersion();
    requestRepaint();
    emit viewChanged();
}

void ScopeWidget::clearData()
{
    m_frame = pdsample::WaveformFrame();
    for (int c = 0; c < pdsample::kChannelCount; ++c) m_roll[c].clear();
    m_rollSequence.clear();
    m_rollFrameStart.clear();
    m_triggerIndex = -1;
    m_triggerValid = false;
    m_cursorSet[0] = m_cursorSet[1] = false;
    m_viewInitialised = false;
    m_userAdjustedView = false;
    m_viewStart = 0.0;
    m_viewSpan = 1024.0;
    bumpDataVersion();
    requestRepaint();
    emit viewChanged();
}

/* ------------------------------------------------------------------ 视图控制 */

void ScopeWidget::setMode(Mode mode)
{
    if (m_mode == mode) return;
    m_mode = mode;
    m_viewInitialised = false;
    m_userAdjustedView = false;
    update();
    emit viewChanged();
}

void ScopeWidget::setTimeSpanSamples(double samples)
{
    m_viewSpan = qBound(8.0, samples, static_cast<double>(qMax(8, totalSamples())));
    const double total = totalSamples();
    m_viewStart = qBound(0.0, m_viewStart, qMax(0.0, total - m_viewSpan));
    m_userAdjustedView = false;
    update();
    emit viewChanged();
}

void ScopeWidget::setChannelVisible(int channel, bool visible)
{
    if (channel < 0 || channel >= pdsample::kChannelCount) return;
    m_visible[channel] = visible;
    update();
}

bool ScopeWidget::channelVisible(int channel) const
{
    return channel >= 0 && channel < pdsample::kChannelCount && m_visible[channel];
}

void ScopeWidget::setChannelOffset(int channel, double codes)
{
    if (channel < 0 || channel >= pdsample::kChannelCount) return;
    m_offset[channel] = codes;
    update();
}

double ScopeWidget::channelOffset(int channel) const
{
    return (channel >= 0 && channel < pdsample::kChannelCount) ? m_offset[channel] : 0.0;
}

void ScopeWidget::setChannelGain(int channel, double gain)
{
    if (channel < 0 || channel >= pdsample::kChannelCount) return;
    m_gain[channel] = qBound(0.05, gain, 50.0);
    update();
}

void ScopeWidget::setAutoScaleY(bool enabled)
{
    m_autoScaleY = enabled;
    update();
}

void ScopeWidget::freezeCurrentRange()
{
    double yMin = 0.0, yMax = 0.0;
    resolveAutoRange(yMin, yMax);
    m_yMin = yMin;
    m_yMax = yMax;
    m_autoScaleY = false;
    update();
    emit viewChanged();
}

void ScopeWidget::setYRange(double minimumCode, double maximumCode)
{
    if (maximumCode <= minimumCode) return;
    m_autoScaleY = false;
    m_yMin = minimumCode;
    m_yMax = maximumCode;
    update();
}

void ScopeWidget::setRollCapacity(int samples)
{
    m_rollCapacity = qMax(1024, samples);
}

void ScopeWidget::setTriggerOverlay(bool visible, double level, int channel)
{
    m_triggerOverlay = visible;
    m_triggerLevel = level;
    m_triggerChannel = qBound(0, channel, 3);
    update();
}

void ScopeWidget::resetView()
{
    m_userAdjustedView = false;
    m_viewInitialised = false;
    m_viewStart = 0.0;
    m_viewSpan = qMax(16, totalSamples());
    m_autoScaleY = true;
    update();
    emit viewChanged();
}

void ScopeWidget::fitAll()
{
    m_userAdjustedView = false;
    m_viewStart = 0.0;
    m_viewSpan = qMax(16, totalSamples());
    update();
    emit viewChanged();
}

void ScopeWidget::setViewWindow(double startSample, double spanSamples)
{
    const double total = qMax(1, totalSamples());
    m_viewSpan = qBound(4.0, spanSamples, total);
    m_viewStart = qBound(0.0, startSample, qMax(0.0, total - m_viewSpan));
    m_viewInitialised = true;
    m_userAdjustedView = true;
    update();
    emit viewChanged();
}

void ScopeWidget::zoomTime(double factor, int anchorPixel)
{
    if (factor <= 0.0) return;
    const PlotArea area = computePlotArea();
    const double anchorSample = area.pixelToSample(anchorPixel);
    const double ratio = (anchorSample - m_viewStart) / qMax(1e-9, m_viewSpan);

    const double minimumSpan = 4.0;
    const double maximumSpan = qMax(minimumSpan, static_cast<double>(totalSamples()));
    m_viewSpan = qBound(minimumSpan, m_viewSpan * factor, maximumSpan);
    m_viewStart = anchorSample - ratio * m_viewSpan;
    const double total = totalSamples();
    m_viewStart = qBound(0.0, m_viewStart, qMax(0.0, total - m_viewSpan));
    m_userAdjustedView = true;
    update();
    emit viewChanged();
}

void ScopeWidget::zoomAmplitude(double factor)
{
    if (factor <= 0.0) return;
    const double centre = (m_yMin + m_yMax) * 0.5;
    double half = (m_yMax - m_yMin) * 0.5 * factor;
    half = qBound(1.0, half, 200000.0);
    m_autoScaleY = false;
    m_yMin = centre - half;
    m_yMax = centre + half;
    update();
}

void ScopeWidget::panTime(double deltaSamples)
{
    const double total = totalSamples();
    m_viewStart = qBound(0.0, m_viewStart + deltaSamples, qMax(0.0, total - m_viewSpan));
    m_userAdjustedView = true;
    update();
    emit viewChanged();
}

void ScopeWidget::setCursorsEnabled(bool enabled)
{
    m_cursorsEnabled = enabled;
    if (!enabled) {
        m_cursorSet[0] = m_cursorSet[1] = false;
        emit cursorChanged(QStringLiteral("游标已关闭"));
    }
    update();
}

void ScopeWidget::clearCursors()
{
    m_cursorSet[0] = m_cursorSet[1] = false;
    m_draggingCursor = -1;
    update();
    emit cursorChanged(QStringLiteral("游标已清除"));
}

void ScopeWidget::visibleSampleRange(int &first, int &count) const
{
    const int total = totalSamples();
    if (total <= 0) { first = 0; count = 0; return; }
    first = qBound(0, static_cast<int>(std::floor(m_viewStart)), total - 1);
    count = qMin(total - first,
                 qMax(1, static_cast<int>(std::ceil(m_viewSpan))));
}

pdsample::WaveformFrame ScopeWidget::displayFrame() const
{
    if (m_mode != Mode::Roll) return m_frame;

    pdsample::WaveformFrame merged;
    merged.sequence = m_rollSequence.isEmpty() ? -1 : m_rollSequence.last();
    merged.sampleCount = m_roll[0].size();
    merged.sampleRateHz = m_sampleRateHz;
    merged.sampleIntervalSec = m_sampleRateHz > 0.0 ? 1.0 / m_sampleRateHz : 0.0;
    for (int c = 0; c < pdsample::kChannelCount; ++c) merged.channel[c] = m_roll[c];
    return merged;
}

/* ------------------------------------------------------------------ 内部工具 */

int ScopeWidget::totalSamples() const
{
    if (m_mode == Mode::Roll) return m_roll[0].size();
    return m_frame.sampleCount;
}

const QVector<double> *ScopeWidget::channelData(int channel) const
{
    if (channel < 0 || channel >= pdsample::kChannelCount) return nullptr;
    if (m_mode == Mode::Roll) return &m_roll[channel];
    if (m_frame.sampleCount <= 0) return nullptr;
    return &m_frame.channel[channel];
}

void ScopeWidget::resolveAutoRange(double &yMin, double &yMax) const
{
    yMin = 1e30;
    yMax = -1e30;
    const int total = totalSamples();
    if (total <= 0) { yMin = 0.0; yMax = 4096.0; return; }

    const int first = qBound(0, static_cast<int>(std::floor(m_viewStart)), total - 1);
    const int last = qMin(total - 1, static_cast<int>(std::ceil(m_viewStart + m_viewSpan)));
    const int step = qMax(1, (last - first) / 4096); /* 自动量程不需要逐点精度 */

    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        if (!m_visible[c]) continue;
        const QVector<double> *data = channelData(c);
        if (data == nullptr) continue;
        for (int i = first; i <= last; i += step) {
            const double value = (data->at(i) - pdsample::kMidCode) * m_gain[c]
                                 + pdsample::kMidCode + m_offset[c];
            yMin = qMin(yMin, value);
            yMax = qMax(yMax, value);
        }
    }
    if (yMin > yMax) { yMin = 0.0; yMax = 4096.0; return; }

    /* 留 8% 余量，避免曲线贴边；全平信号给一个固定最小跨度，否则会除零或者放大噪声。 */
    const double span = yMax - yMin;
    const double pad = span > 1.0 ? span * 0.08 : 4.0;
    yMin -= pad;
    yMax += pad;
    if (yMax - yMin < 8.0) {
        const double centre = (yMin + yMax) * 0.5;
        yMin = centre - 4.0;
        yMax = centre + 4.0;
    }
}

ScopeWidget::PlotArea ScopeWidget::computePlotArea() const
{
    PlotArea area;
    area.rect = QRectF(rect()).adjusted(66.0, 26.0, -20.0, -46.0);
    if (area.rect.width() < 20.0) area.rect.setWidth(20.0);
    if (area.rect.height() < 20.0) area.rect.setHeight(20.0);

    const double total = qMax(1, totalSamples());
    area.xMin = qBound(0.0, m_viewStart, qMax(0.0, total - 1.0));
    area.xMax = qMin(total, area.xMin + m_viewSpan);
    if (area.xMax - area.xMin < 1.0) area.xMax = area.xMin + 1.0;

    if (m_autoScaleY) {
        double yMin = 0.0, yMax = 0.0;
        resolveAutoRange(yMin, yMax);
        area.yMin = yMin;
        area.yMax = yMax;
    } else {
        area.yMin = m_yMin;
        area.yMax = m_yMax;
    }
    if (area.yMax <= area.yMin) area.yMax = area.yMin + 1.0;
    return area;
}

/* ------------------------------------------------------------------ 绘制 */

void ScopeWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), kBackground);

    const PlotArea area = computePlotArea();
    drawGrid(painter, area);

    if (totalSamples() <= 0) {
        painter.setPen(kAxisText);
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 11));
        painter.drawText(area.rect, Qt::AlignCenter,
                         QStringLiteral("等待数据。请先连接板端并执行 START 0 连续采集，"
                                        "再开启实时示波器。"));
        return;
    }

    drawTraces(painter, area);
    drawPulses(painter, area);
    if (m_triggerOverlay) drawTriggerOverlay(painter, area);
    drawCursors(painter, area);
    drawLegend(painter, area);
}

void ScopeWidget::drawGrid(QPainter &painter, const PlotArea &area) const
{
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));

    /* 背景格线：10 纵格 × 8 横格，与普通示波器的分格读数习惯一致。 */
    painter.setPen(QPen(kGrid, 1, Qt::DotLine));
    for (int i = 1; i < 10; ++i) {
        const double x = area.rect.left() + i * area.rect.width() / 10.0;
        painter.drawLine(QPointF(x, area.rect.top()), QPointF(x, area.rect.bottom()));
    }
    for (int i = 1; i < 8; ++i) {
        const double y = area.rect.top() + i * area.rect.height() / 8.0;
        painter.drawLine(QPointF(area.rect.left(), y), QPointF(area.rect.right(), y));
    }
    painter.setPen(QPen(kGridStrong, 1));
    painter.drawRect(area.rect);

    /* 2048 = 12-bit offset-binary 的名义零电平，画出来方便目测偏置。 */
    painter.setPen(QPen(kMidLine, 1, Qt::DashLine));
    const double midY = area.codeToPixel(pdsample::kMidCode);
    if (midY > area.rect.top() && midY < area.rect.bottom())
        painter.drawLine(QPointF(area.rect.left(), midY), QPointF(area.rect.right(), midY));

    /* 纵向刻度：滚动模式下横轴是到达顺序而非时间，必须明确区分。 */
    const bool timeAxis = (m_mode != Mode::Roll);
    const double sampleInterval = m_sampleRateHz > 0.0 ? 1.0 / m_sampleRateHz : 0.0;
    const double spanSamples = qMax(1.0, area.xMax - area.xMin);
    double unitScale = 1.0;
    const QString unit = timeAxis ? timeUnitFor(spanSamples * sampleInterval, unitScale)
                                  : QString();

    painter.setPen(kAxisText);
    for (int i = 0; i <= 10; ++i) {
        const double x = area.rect.left() + i * area.rect.width() / 10.0;
        const double sample = area.xMin + i * spanSamples / 10.0;
        QString text;
        if (timeAxis) {
            const double seconds = sample * sampleInterval;
            text = QString::number(seconds * unitScale, 'g', 4);
            if (i == 10) text += ' ' + unit;
        } else {
            text = QString::number(static_cast<qint64>(sample));
        }
        const QRectF box(x - 40, area.rect.bottom() + 4, 80, 16);
        painter.drawText(box, (i == 0) ? Qt::AlignLeft : (i == 10 ? Qt::AlignRight : Qt::AlignHCenter),
                         text);
    }
    const QString xTitle = (m_mode == Mode::Roll)
        ? QStringLiteral("到达样本序号（帧间不连续，此轴不代表时间）")
        : QStringLiteral("时间（相对记录起点，采样间隔 %1 ns）")
              .arg(sampleInterval * 1e9, 0, 'g', 4);
    painter.drawText(QRectF(area.rect.left(), height() - 22, area.rect.width(), 18),
                     Qt::AlignCenter, xTitle);

    /* 横向刻度：直接用 ADC 码，和板端 ANALYZE/EVENT 报的原始码同量纲。 */
    for (int i = 0; i <= 8; ++i) {
        const double y = area.rect.top() + i * area.rect.height() / 8.0;
        const double code = area.yMax - i * (area.yMax - area.yMin) / 8.0;
        painter.drawText(QRectF(2, y - 8, 60, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(code, 'f', 0));
    }
    painter.save();
    painter.translate(14, area.rect.center().y());
    painter.rotate(-90);
    painter.drawText(QRectF(-area.rect.height() / 2, -12, area.rect.height(), 20),
                     Qt::AlignCenter, QStringLiteral("ADC 码（12-bit）"));
    painter.restore();

    if (m_lastFrameIntervalSec > 0.0 && m_mode != Mode::Static) {
        painter.setPen(kAxisText);
        painter.drawText(QRectF(area.rect.right() - 240, 2, 240, 18), Qt::AlignRight,
                         QStringLiteral("帧到达间隔 %1 ms").arg(m_lastFrameIntervalSec * 1e3, 0, 'f', 1));
    }
}

void ScopeWidget::drawTraces(QPainter &painter, const PlotArea &area) const
{
    const int first = qBound(0, static_cast<int>(std::floor(area.xMin)), qMax(0, totalSamples() - 1));
    const int last = qMin(totalSamples() - 1, static_cast<int>(std::ceil(area.xMax)));
    if (last < first) return;

    const double samplesPerPixel = (area.xMax - area.xMin) / qMax(1.0, area.rect.width());
    const bool envelope = samplesPerPixel > 2.0;

    painter.setClipRect(area.rect);
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        if (!m_visible[c]) continue;
        const QVector<double> *data = channelData(c);
        if (data == nullptr || data->size() <= last) continue;

        const auto display = [this, c, data](int index) {
            return (data->at(index) - pdsample::kMidCode) * m_gain[c]
                   + pdsample::kMidCode + m_offset[c];
        };

        QPainterPath path;
        if (!envelope) {
            for (int i = first; i <= last; ++i) {
                const QPointF point(area.sampleToPixel(i), area.codeToPixel(display(i)));
                if (i == first) path.moveTo(point); else path.lineTo(point);
            }
        } else {
            /*
             * min/max 包络抽稀：每像素列取列内最小与最大显示值，并按两者在数据中
             * 的先后顺序连线。这样 520,000 点压到 1000 像素时，DDS 的 4 个样点窄
             * 脉冲仍会被画成一根竖线；等间隔抽稀会直接跳过脉冲。
             *
             * 抽稀结果按 (数据版本, 视图范围, 像素列数) 缓存：这一趟是 O(可见样本数)，
             * 520,000 点 × 4 通道 = 208 万次比较，而拖拽/游标/实时刷新都会重绘。
             * 旧实现每次 paint 都重算，是卡顿的主因之一。
             */
            const int columns = static_cast<int>(area.rect.width());
            ensureEnvelope(c, area, first, last, columns);
            const EnvelopeCache &cache = m_envelope[c];
            bool started = false;
            for (int column = 0; column < cache.columnMin.size(); ++column) {
                const double rawMin = cache.columnMin.at(column);
                const double rawMax = cache.columnMax.at(column);
                if (rawMin > rawMax) continue;
                const double s0 = area.xMin + column * samplesPerPixel;
                const double x = area.sampleToPixel(s0 + samplesPerPixel * 0.5);
                const QPointF a(x, area.codeToPixel((rawMin - pdsample::kMidCode) * m_gain[c]
                                                    + pdsample::kMidCode + m_offset[c]));
                const QPointF b(x, area.codeToPixel((rawMax - pdsample::kMidCode) * m_gain[c]
                                                    + pdsample::kMidCode + m_offset[c]));
                /* 先出现的是最小值还是最大值，由数据决定；增益为负时显示值会互换，
                   所以顺序标志必须按"原码"记录（这里已经这样做了）。 */
                const bool minFirst = cache.minFirst.at(column) != 0;
                const QPointF p = minFirst ? a : b;
                const QPointF q = minFirst ? b : a;
                if (!started) { path.moveTo(p); started = true; }
                else path.lineTo(p);
                path.lineTo(q);
            }
        }
        painter.setPen(QPen(kTrace[c], 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
    painter.setClipping(false);
}

void ScopeWidget::drawTriggerOverlay(QPainter &painter, const PlotArea &area) const
{
    if (totalSamples() <= 0) return;
    painter.setClipRect(area.rect);

    const double y = area.codeToPixel(m_triggerLevel);
    painter.setPen(QPen(kTriggerLine, 1, Qt::DashLine));
    if (y > area.rect.top() && y < area.rect.bottom())
        painter.drawLine(QPointF(area.rect.left(), y), QPointF(area.rect.right(), y));
    painter.drawText(QRectF(area.rect.right() - 190, area.rect.top() + 2, 188, 16),
                     Qt::AlignRight,
                     QStringLiteral("T 电平 %1 (%2)")
                         .arg(m_triggerLevel, 0, 'f', 0).arg(channelName(m_triggerChannel)));

    if (m_triggerValid && m_triggerIndex >= 0 && m_triggerIndex < totalSamples()) {
        const double x = area.sampleToPixel(m_triggerIndex);
        if (x >= area.rect.left() && x <= area.rect.right()) {
            painter.setPen(QPen(kTriggerLine, 1.5));
            painter.drawLine(QPointF(x, area.rect.top()), QPointF(x, area.rect.bottom()));
            QPolygonF marker;
            marker << QPointF(x, area.rect.top())
                   << QPointF(x - 5, area.rect.top() - 8)
                   << QPointF(x + 5, area.rect.top() - 8);
            painter.setBrush(kTriggerLine);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(marker);
            painter.setBrush(Qt::NoBrush);
        }
    }
    painter.setClipping(false);
}

void ScopeWidget::drawCursors(QPainter &painter, const PlotArea &area) const
{
    if (!m_cursorsEnabled) return;
    painter.setClipRect(area.rect);
    const QColor colors[2] = {kCursorAColor, kCursorBColor};
    for (int index = 0; index < 2; ++index) {
        if (!m_cursorSet[index]) continue;
        const double x = area.sampleToPixel(m_cursorSample[index]);
        if (x < area.rect.left() || x > area.rect.right()) continue;
        painter.setPen(QPen(colors[index], 1.2, Qt::DashLine));
        painter.drawLine(QPointF(x, area.rect.top()), QPointF(x, area.rect.bottom()));
        painter.drawText(QRectF(x + 3, area.rect.bottom() - 18, 40, 16),
                         index == 0 ? QStringLiteral("A") : QStringLiteral("B"));
    }
    painter.setClipping(false);
}

void ScopeWidget::drawLegend(QPainter &painter, const PlotArea &area) const
{
    painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    int x = static_cast<int>(area.rect.left()) + 6;
    for (int c = 0; c < pdsample::kChannelCount; ++c) {
        const QColor color = m_visible[c] ? kTrace[c] : QColor(90, 100, 112);
        painter.setPen(color);
        const QString text = QStringLiteral("%1 ×%2%3")
            .arg(channelName(c))
            .arg(m_gain[c], 0, 'f', 2)
            .arg(m_offset[c] != 0.0 ? QStringLiteral(" %1%2")
                                          .arg(m_offset[c] > 0 ? QStringLiteral("+") : QString())
                                          .arg(m_offset[c], 0, 'f', 0)
                                      : QString());
        painter.drawText(x, static_cast<int>(area.rect.top()) - 8, text);
        x += 108;
    }

    /* 把数据来源与"是否时间连续"写在画面里，避免看的人误读。 */
    const QString note = (m_mode == Mode::Roll)
        ? QStringLiteral("滚动：连续帧按到达顺序拼接，帧间存在未知间隙（每帧取自 DDR 环 24 KiB 之后的窗口），"
                         "本视图用于波形形态监测，不代表连续时间")
        : (m_mode == Mode::Static ? QStringLiteral("静态记录：单份时间连续数据（快照/CSV），横轴可信")
                                  : QStringLiteral("实时单帧：每次刷新为 DDR 环上的新窗口"));
    painter.setPen(kAxisText);
    painter.drawText(QRectF(static_cast<qreal>(area.rect.left()), 2, area.rect.width() - 250, 18),
                     Qt::AlignLeft, QStringLiteral("%1 · %2").arg(m_label, note));
}

/* ------------------------------------------------------------------ 交互 */

void ScopeWidget::wheelEvent(QWheelEvent *event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) { event->ignore(); return; }
    const double factor = std::pow(0.8, steps);
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        zoomAmplitude(factor);
    } else if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        panTime(-steps * m_viewSpan * 0.15);
    } else {
        zoomTime(factor, static_cast<int>(event->position().x()));
    }
    event->accept();
}

void ScopeWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton && m_cursorsEnabled) {
        const PlotArea area = computePlotArea();
        const double sample = area.pixelToSample(event->position().x());
        /* 落在已有游标附近就拖动它，否则按 A→B→A 的顺序放置。 */
        int nearest = -1;
        double bestDistance = 24.0;
        for (int index = 0; index < 2; ++index) {
            if (!m_cursorSet[index]) continue;
            const double distance = std::fabs(area.sampleToPixel(m_cursorSample[index])
                                              - event->position().x());
            if (distance < bestDistance) { bestDistance = distance; nearest = index; }
        }
        if (nearest < 0) nearest = m_cursorSet[0] ? (m_cursorSet[1] ? 0 : 1) : 0;
        m_draggingCursor = nearest;
        m_cursorSet[nearest] = true;
        m_cursorSample[nearest] = sample;
        emitCursorReadout();
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_panning = true;
        m_movedWhilePressed = false;
        m_pressPosition = event->pos();
        m_lastMouse = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ScopeWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_draggingCursor >= 0) {
        const PlotArea area = computePlotArea();
        m_cursorSample[m_draggingCursor] =
            qBound(0.0, area.pixelToSample(event->position().x()),
                   static_cast<double>(qMax(1, totalSamples() - 1)));
        emitCursorReadout();
        update();
        event->accept();
        return;
    }
    if (m_panning) {
        const PlotArea area = computePlotArea();
        const double samplesPerPixel = qMax(1e-9, (area.xMax - area.xMin) / qMax(1.0, area.rect.width()));
        const double delta = -(event->pos().x() - m_lastMouse.x()) * samplesPerPixel;
        if ((event->pos() - m_pressPosition).manhattanLength() > 3) m_movedWhilePressed = true;
        m_lastMouse = event->pos();
        panTime(delta);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ScopeWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_draggingCursor >= 0) { m_draggingCursor = -1; event->accept(); return; }
    if (m_panning) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
        /*
         * 没有拖动就当作"点选脉冲"：左键拖拽是平移，单击是拾取，
         * 两者共用左键，所以必须靠位移量区分，不能只看按键。
         */
        if (!m_movedWhilePressed) {
            const int index = pickPulse(event->position());
            m_selectedPulse = index;
            update();
            if (index >= 0) {
                const pddetect::Pulse &pulse = m_pulses.at(index);
                emit pulsePicked(index, pulse.channel, pulse.code, pulse.timeSec);
            }
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ScopeWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    resetView();
    event->accept();
}

void ScopeWidget::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_A:
        setCursorsEnabled(!m_cursorsEnabled);
        break;
    case Qt::Key_C:
        clearCursors();
        break;
    case Qt::Key_F:
        fitAll();
        break;
    case Qt::Key_R:
        resetView();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void ScopeWidget::emitCursorReadout()
{
    if (!m_cursorSet[0] || !m_cursorSet[1]) {
        emit cursorChanged(QStringLiteral("游标：仅放置了一个，再拖一次放置另一个"));
        return;
    }
    const double sampleInterval = m_sampleRateHz > 0.0 ? 1.0 / m_sampleRateHz : 0.0;
    const double deltaSamples = std::fabs(m_cursorSample[1] - m_cursorSample[0]);
    const double deltaSeconds = deltaSamples * sampleInterval;
    QString text = QStringLiteral("Δt = %1 %2（%3 个采样点）")
                       .arg(deltaSeconds * 1e6, 0, 'f', 3)
                       .arg(QStringLiteral("µs"))
                       .arg(deltaSamples, 0, 'f', 0);
    if (deltaSeconds > 0.0)
        text += QStringLiteral("，等效 %1 kHz").arg(1.0 / deltaSeconds / 1e3, 0, 'f', 2);

    /* 幅度差取当前触发通道（若无触发则取 CH0）在游标处的瞬时值。 */
    const QVector<double> *data = channelData(m_triggerChannel);
    if (data != nullptr) {
        const int a = qBound(0, static_cast<int>(m_cursorSample[0] + 0.5), data->size() - 1);
        const int b = qBound(0, static_cast<int>(m_cursorSample[1] + 0.5), data->size() - 1);
        text += QStringLiteral("；%1 幅度差 %2 码")
                    .arg(channelName(m_triggerChannel))
                    .arg(data->at(b) - data->at(a), 0, 'f', 0);
    }
    emit cursorChanged(text);
}


/* ==================================================================
 * PD 脉冲标注 / 渲染节流 / 包络缓存
 *
 * 这三件事是同一目的：让"标注局放脉冲"在实时刷新下也能成立。
 *   1) 脉冲检测在**工作线程**做（pd_analysis_worker），这里只负责画；
 *   2) 画之前先按视图与上限筛，避免噪声记录把渲染拖死；
 *   3) 波形本体走缓存抽稀，重绘成本从 O(样本数) 降到 O(像素列数)。
 * ================================================================== */

void ScopeWidget::setPulses(const QVector<pddetect::Pulse> &pulses)
{
    m_pulses = pulses;
    m_selectedPulse = -1;
    update();
}

void ScopeWidget::setPulseOverlayVisible(bool visible)
{
    if (m_pulseOverlay == visible) return;
    m_pulseOverlay = visible;
    update();
}

void ScopeWidget::setPulseHighlightCodes(double codes)
{
    m_pulseHighlightCodes = codes;
    update();
}

void ScopeWidget::setSelectedPulse(int index)
{
    if (m_selectedPulse == index) return;
    m_selectedPulse = index;
    update();
}

void ScopeWidget::setMaximumPulsesDrawn(int count)
{
    m_maxPulsesDrawn = qMax(1, count);
    update();
}

void ScopeWidget::setMinimumRenderIntervalMs(int milliseconds)
{
    m_minRenderIntervalMs = qMax(0, milliseconds);
}

void ScopeWidget::bumpDataVersion()
{
    ++m_dataVersion;
}

void ScopeWidget::requestRepaint()
{
    if (m_minRenderIntervalMs <= 0) {
        update();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPaintRequestMs >= m_minRenderIntervalMs) {
        m_lastPaintRequestMs = now;
        update();
        return;
    }
    /*
     * 距上次重绘还不够一个最小间隔：排一次延迟重绘。
     * 数据到达的频率由板端决定（SCOPE 帧最低 20 ms，解码可能更慢），
     * 不合并就会出现"画得比来得慢"的绘制事件堆积，界面表现为延迟越拖越大。
     */
    if (m_renderPending) return;
    m_renderPending = true;
    const int delay = static_cast<int>(m_minRenderIntervalMs - (now - m_lastPaintRequestMs));
    QTimer::singleShot(qMax(1, delay), this, [this] {
        m_renderPending = false;
        m_lastPaintRequestMs = QDateTime::currentMSecsSinceEpoch();
        update();
    });
}

void ScopeWidget::ensureEnvelope(int channel, const PlotArea &area, int first, int last,
                                 int columns) const
{
    EnvelopeCache &cache = m_envelope[channel];
    if (cache.valid && cache.version == m_dataVersion && cache.first == first &&
        cache.last == last && cache.columns == columns)
        return;

    const QVector<double> *data = channelData(channel);
    const double samplesPerPixel =
        (area.xMax - area.xMin) / qMax(1.0, area.rect.width());

    cache.valid = true;
    cache.version = m_dataVersion;
    cache.first = first;
    cache.last = last;
    cache.columns = columns;
    cache.columnMin.clear();
    cache.columnMax.clear();
    cache.minFirst.clear();

    if (data == nullptr || data->size() <= last || columns <= 0 || samplesPerPixel <= 0.0)
        return;

    cache.columnMin.resize(columns + 1);
    cache.columnMax.resize(columns + 1);
    cache.minFirst.resize(columns + 1);

    const double *samples = data->constData();
    for (int column = 0; column <= columns; ++column) {
        const double s0 = area.xMin + column * samplesPerPixel;
        const int i0 = qBound(first, static_cast<int>(std::floor(s0)), last);
        const int i1 = qBound(first, static_cast<int>(std::ceil(s0 + samplesPerPixel)), last);
        double minimum = 1e30;
        double maximum = -1e30;
        int indexOfMin = i0;
        int indexOfMax = i0;
        for (int i = i0; i <= i1; ++i) {
            const double value = samples[i];
            if (value < minimum) { minimum = value; indexOfMin = i; }
            if (value > maximum) { maximum = value; indexOfMax = i; }
        }
        cache.columnMin[column] = minimum;
        cache.columnMax[column] = maximum;
        cache.minFirst[column] = static_cast<uchar>(indexOfMin <= indexOfMax ? 1 : 0);
    }
}

void ScopeWidget::drawPulses(QPainter &painter, const PlotArea &area) const
{
    if (!m_pulseOverlay || m_pulses.isEmpty() || totalSamples() <= 0) return;

    /* 先筛出落在视图内、且通道可见的脉冲。 */
    QVector<int> candidates;
    candidates.reserve(qMin(m_pulses.size(), 4096));
    for (int i = 0; i < m_pulses.size(); ++i) {
        const pddetect::Pulse &pulse = m_pulses.at(i);
        if (pulse.channel < 0 || pulse.channel >= pdsample::kChannelCount) continue;
        if (!m_visible[pulse.channel]) continue;
        if (pulse.index < 0 || pulse.index >= totalSamples()) continue;
        if (pulse.refinedIndex < area.xMin || pulse.refinedIndex > area.xMax) continue;
        candidates.append(i);
    }
    if (candidates.isEmpty()) return;

    /* 太多时优先保留"已确认"的，再按幅值取前若干个——绝不静默丢掉高亮。 */
    bool truncated = false;
    if (candidates.size() > m_maxPulsesDrawn) {
        std::stable_sort(candidates.begin(), candidates.end(), [this](int a, int b) {
            const pddetect::Pulse &pa = m_pulses.at(a);
            const pddetect::Pulse &pb = m_pulses.at(b);
            if (pa.confirmed != pb.confirmed) return pa.confirmed;
            return pa.absCode > pb.absCode;
        });
        candidates.resize(m_maxPulsesDrawn);
        truncated = true;
    }

    painter.setClipRect(area.rect);
    for (int index : candidates) {
        const pddetect::Pulse &pulse = m_pulses.at(index);
        const int c = pulse.channel;
        const double x = area.sampleToPixel(pulse.refinedIndex);

        /* 幅度轴上的显示位置要经过与波形一致的增益/偏移变换，否则标注会错位。 */
        const double baselineCode =
            (pulse.baseline - pdsample::kMidCode) * m_gain[c] + pdsample::kMidCode + m_offset[c];
        const double peakCode = (pulse.baseline + pulse.code - pdsample::kMidCode) * m_gain[c] +
                                pdsample::kMidCode + m_offset[c];
        const double yBase = area.codeToPixel(baselineCode);
        const double yPeak = area.codeToPixel(peakCode);

        const bool selected = (index == m_selectedPulse);
        QColor color = kTrace[c];
        if (!pulse.confirmed) color.setAlpha(115);

        painter.setPen(QPen(color, selected ? 2.2 : (pulse.confirmed ? 1.4 : 0.9)));
        painter.drawLine(QPointF(x, yBase), QPointF(x, yPeak));

        if (pulse.confirmed || selected) {
            /* 三角标记指向极性方向：正向上、负向下。 */
            const double apex = pulse.positive ? yPeak - 7.0 : yPeak + 7.0;
            const double base = pulse.positive ? yPeak - 1.0 : yPeak + 1.0;
            const QPolygonF marker({QPointF(x, apex), QPointF(x - 4.0, base),
                                    QPointF(x + 4.0, base)});
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawPolygon(marker);
            painter.setBrush(Qt::NoBrush);
        }

        if (selected) {
            const QString readout =
                QStringLiteral("CH%1 ｜ %2 码 ｜ %3 ｜ %4")
                    .arg(c)
                    .arg(pulse.code, 0, 'f', 0)
                    .arg(pulse.positive ? QStringLiteral("正极性") : QStringLiteral("负极性"))
                    .arg(m_sampleRateHz > 0.0
                             ? QStringLiteral("%1 µs").arg(pulse.timeSec * 1e6, 0, 'f', 2)
                             : QStringLiteral("时间未知"));

            painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
            const QFontMetrics metrics(painter.font());
            const QRectF textRect = metrics.boundingRect(readout).adjusted(-6, -3, 6, 3);
            QRectF box(QPointF(0.0, 0.0), textRect.size());
            box.moveTopLeft(QPointF(area.rect.left() + 6, area.rect.top() + 6));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(8, 14, 24, 230));
            painter.drawRect(box);
            painter.setPen(QPen(color, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(box);
            painter.setPen(Qt::white);
            painter.drawText(box, Qt::AlignCenter, readout);
        }
    }

    if (truncated) {
        painter.setPen(QColor(240, 190, 90));
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 8));
        painter.drawText(QRectF(area.rect.left() + 6, area.rect.bottom() - 18,
                                area.rect.width() - 12, 16),
                         Qt::AlignLeft,
                         QStringLiteral("脉冲标注已按上限截断（本视图 %1 个，只画 %2 个，"
                                        "已确认优先）")
                             .arg(candidates.size() == 0 ? 0 : m_maxPulsesDrawn)
                             .arg(m_maxPulsesDrawn));
    }
    painter.setClipping(false);
}

int ScopeWidget::pickPulse(const QPointF &widgetPoint) const
{
    if (m_pulses.isEmpty() || totalSamples() <= 0) return -1;
    const PlotArea area = computePlotArea();

    int best = -1;
    double bestDistance = 14.0;
    for (int i = 0; i < m_pulses.size(); ++i) {
        const pddetect::Pulse &pulse = m_pulses.at(i);
        if (pulse.channel < 0 || pulse.channel >= pdsample::kChannelCount) continue;
        if (!m_visible[pulse.channel]) continue;
        if (pulse.index < 0 || pulse.index >= totalSamples()) continue;

        const double x = area.sampleToPixel(pulse.refinedIndex);
        if (x < area.rect.left() - 14.0 || x > area.rect.right() + 14.0) continue;

        const int c = pulse.channel;
        const double baselineCode =
            (pulse.baseline - pdsample::kMidCode) * m_gain[c] + pdsample::kMidCode + m_offset[c];
        const double peakCode = (pulse.baseline + pulse.code - pdsample::kMidCode) * m_gain[c] +
                                pdsample::kMidCode + m_offset[c];
        const double yOne = area.codeToPixel(baselineCode);
        const double yTwo = area.codeToPixel(peakCode);
        const double yLow = qMin(yOne, yTwo);
        const double yHigh = qMax(yOne, yTwo);

        double dy = 0.0;
        if (widgetPoint.y() < yLow) dy = yLow - widgetPoint.y();
        else if (widgetPoint.y() > yHigh) dy = widgetPoint.y() - yHigh;
        const double dx = std::fabs(widgetPoint.x() - x);
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

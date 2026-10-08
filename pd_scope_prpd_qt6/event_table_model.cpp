#include "event_table_model.h"

#include <QDateTime>
#include <QTimer>

#include <cmath>

namespace {
constexpr int kColumnCount = 7;
}

EventTableModel::EventTableModel(QObject *parent) : QAbstractTableModel(parent)
{
    m_expiryTimer = new QTimer(this);
    m_expiryTimer->setInterval(200);
    connect(m_expiryTimer, &QTimer::timeout, this, &EventTableModel::expireEvents);
}

int EventTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_events.size());
}

int EventTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : kColumnCount;
}

QVariant EventTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        static_cast<size_t>(index.row()) >= m_events.size()) return {};
    const PdPulse &pulse = m_events[static_cast<size_t>(index.row())];
    const bool passes = std::abs(pulse.amplitude) >= m_threshold;
    if (role == Qt::TextAlignmentRole && index.column() != 0)
        return static_cast<int>(Qt::AlignCenter);
    if (role == Qt::UserRole) {
        switch (index.column()) {
        case 1: return QVariant::fromValue<qulonglong>(pulse.frameSequence);
        case 2: return pulse.packetWordIndex;
        case 3: return pulse.channel + 1;
        case 4: return pulse.phaseValid ? pulse.phaseDeg : -1.0;
        case 5: return pulse.amplitude;
        case 6: return passes;
        default: return pulse.receivedAtMs;
        }
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0:
        return QDateTime::fromMSecsSinceEpoch(pulse.receivedAtMs)
            .toString(QStringLiteral("HH:mm:ss.zzz"));
    case 1: return QVariant::fromValue<qulonglong>(pulse.frameSequence);
    case 2: return pulse.packetWordIndex;
    case 3: return QStringLiteral("CH%1").arg(pulse.channel + 1);
    case 4: return pulse.phaseValid
                    ? QStringLiteral("%1°").arg(pulse.phaseDeg, 0, 'f', 2)
                    : QStringLiteral("未锁相");
    case 5: return QString::number(pulse.amplitude, 'f', 0);
    case 6: return passes ? QStringLiteral("通过") : QStringLiteral("低于阈值");
    default: return {};
    }
}

QVariant EventTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole) return {};
    if (orientation == Qt::Vertical) return section + 1;
    static const char *headers[kColumnCount] = {
        "主机接收时间", "PL 包序号", "包内字序号", "通道", "相位", "幅值 (Q8.8 码)", "主机阈值"
    };
    if (section >= 0 && section < kColumnCount)
        return QString::fromUtf8(headers[section]);
    return {};
}

void EventTableModel::appendEvents(const QVector<PdPulse> &events)
{
    if (events.isEmpty()) return;
    const int first = rowCount();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    beginInsertRows({}, first, first + events.size() - 1);
    for (PdPulse event : events) {
        if (event.receivedAtMs <= 0) event.receivedAtMs = now;
        m_events.push_back(event);
    }
    endInsertRows();
    prune(now);
    if (!m_events.empty() && !m_expiryTimer->isActive()) m_expiryTimer->start();
}

void EventTableModel::setThreshold(double rawQ88)
{
    m_threshold = qMax(0.0, rawQ88);
    if (!m_events.empty())
        emit dataChanged(index(0, 6), index(rowCount() - 1, 6), {Qt::DisplayRole, Qt::UserRole});
}

void EventTableModel::clear()
{
    if (m_events.empty()) return;
    beginResetModel();
    m_events.clear();
    endResetModel();
    m_expiryTimer->stop();
}

void EventTableModel::expireEvents()
{
    prune(QDateTime::currentMSecsSinceEpoch());
    if (m_events.empty()) m_expiryTimer->stop();
}

void EventTableModel::prune(qint64 nowMs)
{
    const qint64 cutoff = nowMs - kRetentionMs;
    int expired = 0;
    while (static_cast<size_t>(expired) < m_events.size() &&
           m_events[static_cast<size_t>(expired)].receivedAtMs <= cutoff) ++expired;
    if (expired > 0) {
        beginRemoveRows({}, 0, expired - 1);
        for (int i = 0; i < expired; ++i) m_events.pop_front();
        endRemoveRows();
    }
}

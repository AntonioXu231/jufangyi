#pragma once

#include "scope_types.h"

#include <QAbstractTableModel>

#include <deque>

class QTimer;

class EventTableModel final : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit EventTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void appendEvents(const QVector<PdPulse> &events);
    void setThreshold(double rawQ88);
    void clear();

private slots:
    void expireEvents();

private:
    static constexpr qint64 kRetentionMs = 15000;
    void prune(qint64 nowMs);

    std::deque<PdPulse> m_events;
    QTimer *m_expiryTimer = nullptr;
    double m_threshold = 0.0;
};

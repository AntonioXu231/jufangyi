#pragma once

#include "scope_types.h"

#include <QObject>
#include <QTimer>

/*
 * Temporary real-time source for UI verification.  Its frameReady contract is
 * deliberately independent of the generator: replace this class later with a
 * TCP SCOPE V1 client without changing plotting widgets.
 */
class DemoSource final : public QObject
{
    Q_OBJECT
public:
    explicit DemoSource(QObject *parent = nullptr);
    void start();
    void stop();
    bool isRunning() const;

signals:
    void frameReady(const ScopeFrame &frame);

private slots:
    void makeFrame();

private:
    QTimer m_timer;
    quint64 m_sequence = 0;
};

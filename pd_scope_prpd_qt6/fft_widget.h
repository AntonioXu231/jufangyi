#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include "scope_types.h"

class FftWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit FftWidget(QWidget *parent = nullptr);
    void setSpectrum(const ScopeSpectrum &spectrum, const QColor &traceColor);
    void setWaiting(const QColor &traceColor);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<double> m_db;
    QColor m_traceColor = QColor(255, 216, 0);
    double m_sampleRateHz = 26000000.0;
    double m_binHz = 0.0;
    double m_peakHz = 0.0;
    double m_peakDb = -120.0;
    quint64 m_sequence = 0;
    quint32 m_dcCode = 0;
    quint32 m_startSample = 0;
    bool m_fromArchive = false;
};

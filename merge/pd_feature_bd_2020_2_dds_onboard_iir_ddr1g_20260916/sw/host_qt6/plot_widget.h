#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

class PlotWidget final : public QWidget
{
    Q_OBJECT
public:
    explicit PlotWidget(QWidget *parent = nullptr);

    void setLines(const QVector<QVector<QPointF>> &series, const QStringList &names,
                  const QString &title, const QString &xLabel, const QString &yLabel);
    void setBars(const QVector<quint32> &values, const QString &title,
                 const QString &xLabel, const QString &yLabel);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    enum class Mode { Empty, Lines, Bars };
    Mode m_mode = Mode::Empty;
    QVector<QVector<QPointF>> m_lines;
    QVector<quint32> m_bars;
    QStringList m_names;
    QString m_title;
    QString m_xLabel;
    QString m_yLabel;
};

#pragma once

#include <chrono>
#include <vector>

#include <QColor>
#include <QString>
#include <QWidget>

// A small 60-second line chart: one value per second, newest at the right edge.
// Hovering shows a crosshair and the sample under the pointer.
class HistoryChart : public QWidget {
public:
    HistoryChart(QString title, QString unit, QColor color, int capacity,
                 QWidget* parent = nullptr);

    // values[i] was sampled at times[i]; the last entry is the newest sample.
    void SetData(std::vector<double> values,
                 std::vector<std::chrono::system_clock::time_point> times);

    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF PlotRect() const;
    double SlotToX(double slot, const QRectF& plot) const;
    int HoveredIndex(const QRectF& plot) const;

    QString title_;
    QString unit_;
    QColor color_;
    int capacity_;
    std::vector<double> values_;
    std::vector<std::chrono::system_clock::time_point> times_;
    double yMax_ = 1.0;
    double yStep_ = 0.25;
    int hoverX_ = -1;
};

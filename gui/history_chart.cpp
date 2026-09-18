#include "history_chart.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <utility>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

namespace {
constexpr double kTicksWanted = 4.0;

// Plain fixed notation so large speeds stay readable as integers.
QString FormatValue(double value) {
    const int precision = value >= 1.0 ? 0 : value >= 0.1 ? 1 : 2;
    return QString::number(value, 'f', precision);
}

// Enough decimals to tell neighbouring ticks apart (0.25 needs two).
QString FormatTick(double value, double step) {
    int decimals = 0;
    if (step < 1.0) {
        decimals = static_cast<int>(std::ceil(-std::log10(step) - 1e-9));
        const double scaled = step * std::pow(10.0, decimals);
        if (std::abs(scaled - std::round(scaled)) > 1e-6) ++decimals;
    }
    return QString::number(value, 'f', decimals);
}

// Rounds a tick step up to 1, 2, 2.5 or 5 times a power of ten.
double NiceStep(double raw) {
    if (raw <= 0.0) return 1.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double fraction = raw / magnitude;
    for (const double nice : {1.0, 2.0, 2.5, 5.0}) {
        if (fraction <= nice) return nice * magnitude;
    }
    return 10.0 * magnitude;
}

QString ClockLabel(std::chrono::system_clock::time_point timestamp) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return QString::fromLatin1(buffer);
}
}  // namespace

HistoryChart::HistoryChart(QString title, QString unit, QColor color, int capacity,
                           QWidget* parent)
    : QWidget(parent),
      title_(std::move(title)),
      unit_(std::move(unit)),
      color_(std::move(color)),
      capacity_(std::max(2, capacity)) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize HistoryChart::minimumSizeHint() const { return {180, 130}; }

void HistoryChart::SetData(std::vector<double> values,
                           std::vector<std::chrono::system_clock::time_point> times) {
    values_ = std::move(values);
    times_ = std::move(times);
    if (static_cast<int>(values_.size()) > capacity_) {
        values_.erase(values_.begin(), values_.end() - capacity_);
    }
    if (times_.size() != values_.size()) times_.resize(values_.size());
    double maximum = 0.0;
    for (double& value : values_) {
        value = std::max(0.0, value);
        maximum = std::max(maximum, value);
    }
    // An idle chart still gets a readable 0..1 axis.
    const double top = maximum > 0.0 ? maximum : 1.0;
    yStep_ = NiceStep(top / kTicksWanted);
    yMax_ = yStep_ * std::max(1.0, std::ceil(top / yStep_ - 1e-9));
    update();
}

QRectF HistoryChart::PlotRect() const {
    const QFontMetrics metrics(font());
    const double labelWidth = std::max(
        metrics.horizontalAdvance(FormatTick(yMax_, yStep_)),
        metrics.horizontalAdvance(QStringLiteral("0.00")));
    const double left = labelWidth + 10.0;
    const double top = metrics.height() + 8.0;
    const double bottom = metrics.height() + 6.0;
    return QRectF(left, top, std::max(1.0, width() - left - 8.0),
                  std::max(1.0, height() - top - bottom));
}

// Slot 0 is the oldest position on the axis, capacity_-1 is "now". Half a slot
// of padding on both ends keeps the first and last points off the frame.
double HistoryChart::SlotToX(double slot, const QRectF& plot) const {
    return plot.left() + (slot + 0.5) / capacity_ * plot.width();
}

int HistoryChart::HoveredIndex(const QRectF& plot) const {
    if (hoverX_ < 0 || values_.empty()) return -1;
    const double slot = (hoverX_ - plot.left()) / plot.width() * capacity_ - 0.5;
    const long long rounded = std::llround(slot);
    const long long firstSlot = capacity_ - static_cast<long long>(values_.size());
    if (rounded < firstSlot || rounded >= capacity_) return -1;
    return static_cast<int>(rounded - firstSlot);
}

void HistoryChart::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette& colors = palette();
    const QFontMetrics metrics(font());
    const QRectF plot = PlotRect();

    // No background fill: the chart sits directly on its card.
    painter.setPen(colors.color(QPalette::WindowText));
    painter.drawText(QRectF(0, 2, width(), metrics.height()), Qt::AlignHCenter,
                     QStringLiteral("%1 (%2)").arg(title_, unit_));

    // Grid and axis labels.
    const QColor grid = colors.color(QPalette::Mid);
    const QColor labelColor = colors.color(QPalette::Disabled, QPalette::WindowText);
    const int tickCount = static_cast<int>(std::lround(yMax_ / yStep_));
    for (int tick = 0; tick <= tickCount; ++tick) {
        const double value = yStep_ * tick;
        const double y = plot.bottom() - value / yMax_ * plot.height();
        painter.setPen(QPen(grid, 1.0));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(labelColor);
        painter.drawText(QRectF(0, y - metrics.height() / 2.0, plot.left() - 5.0,
                                metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, FormatTick(value, yStep_));
    }
    for (int secondsAgo = 0; secondsAgo < capacity_; secondsAgo += 15) {
        const double x = SlotToX(capacity_ - 1 - secondsAgo, plot);
        painter.setPen(QPen(grid, 1.0));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(labelColor);
        const QString label = secondsAgo == 0
            ? QStringLiteral("now") : QStringLiteral("-%1 s").arg(secondsAgo);
        const double labelWidth = metrics.horizontalAdvance(label);
        const double labelX = std::clamp(x - labelWidth / 2.0, 0.0,
                                         width() - labelWidth);
        painter.drawText(QPointF(labelX, plot.bottom() + metrics.ascent() + 3.0),
                         label);
    }
    painter.setPen(QPen(grid, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);

    if (values_.empty()) {
        painter.setPen(labelColor);
        painter.drawText(plot, Qt::AlignCenter,
                         QStringLiteral("Collecting the first sample..."));
        return;
    }

    // Shaded line.
    const int firstSlot = capacity_ - static_cast<int>(values_.size());
    QPainterPath line;
    for (std::size_t index = 0; index < values_.size(); ++index) {
        const QPointF point(SlotToX(firstSlot + static_cast<double>(index), plot),
                            plot.bottom() - values_[index] / yMax_ * plot.height());
        if (index == 0) line.moveTo(point);
        else line.lineTo(point);
    }
    QPainterPath area = line;
    area.lineTo(SlotToX(capacity_ - 1, plot), plot.bottom());
    area.lineTo(SlotToX(firstSlot, plot), plot.bottom());
    area.closeSubpath();
    painter.save();
    painter.setClipRect(plot);
    QColor fill = color_;
    fill.setAlphaF(0.18f);
    painter.fillPath(area, fill);
    painter.setPen(QPen(color_, 2.0));
    painter.drawPath(line);
    painter.restore();

    // Hover crosshair and readout.
    const int hovered = HoveredIndex(plot);
    if (hovered < 0) return;
    const double x = SlotToX(firstSlot + static_cast<double>(hovered), plot);
    const double y = plot.bottom() - values_[hovered] / yMax_ * plot.height();
    painter.setPen(QPen(colors.color(QPalette::WindowText), 1.0, Qt::DashLine));
    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color_);
    painter.drawEllipse(QPointF(x, y), 3.5, 3.5);

    const int secondsAgo = static_cast<int>(values_.size()) - 1 - hovered;
    const QString timeLine = QStringLiteral("%1 (%2 s ago)")
        .arg(ClockLabel(times_[hovered])).arg(secondsAgo);
    const QString valueLine = FormatValue(values_[hovered]) + ' ' + unit_;
    const double boxWidth = std::max({metrics.horizontalAdvance(title_),
                                      metrics.horizontalAdvance(timeLine),
                                      metrics.horizontalAdvance(valueLine)}) + 12.0;
    const double boxHeight = metrics.height() * 3.0 + 10.0;
    double boxX = x + 10.0;
    if (boxX + boxWidth > plot.right()) boxX = x - 10.0 - boxWidth;
    boxX = std::max(boxX, plot.left());
    const QRectF box(boxX, plot.top() + 4.0, boxWidth, boxHeight);
    painter.setBrush(colors.color(QPalette::ToolTipBase));
    painter.setPen(QPen(grid, 1.0));
    painter.drawRoundedRect(box, 4.0, 4.0);
    const double textX = box.left() + 6.0;
    double baseline = box.top() + 5.0 + metrics.ascent();
    painter.setPen(colors.color(QPalette::ToolTipText));
    painter.drawText(QPointF(textX, baseline), title_);
    baseline += metrics.height();
    painter.drawText(QPointF(textX, baseline), timeLine);
    baseline += metrics.height();
    painter.setPen(color_);
    painter.drawText(QPointF(textX, baseline), valueLine);
}

void HistoryChart::mouseMoveEvent(QMouseEvent* event) {
    hoverX_ = static_cast<int>(event->position().x());
    update();
}

void HistoryChart::leaveEvent(QEvent*) {
    hoverX_ = -1;
    update();
}

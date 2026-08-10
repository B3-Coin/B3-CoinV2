// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/b3chartwidget.h>

#include <qt/b3fixed.h>
#include <qt/b3marketmodel.h>
#include <qt/b3theme.h>

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kMinVisible{5.0};
constexpr double kMaxZoomOutFactor{2.0}; // at most 2x the series length
constexpr int kAxisWidth{72};
constexpr int kAxisHeight{22};
constexpr double kVolumeFraction{0.18};
} // namespace

B3ChartWidget::B3ChartWidget(QWidget* parent)
    : QWidget{parent}
{
    setMouseTracking(true);
    setMinimumSize(240, 160);
    setFocusPolicy(Qt::StrongFocus);
}

void B3ChartWidget::setSeries(B3CandleSeries* series)
{
    if (m_series) disconnect(m_series, nullptr, this, nullptr);
    m_series = series;
    if (m_series) {
        connect(m_series, &B3CandleSeries::resetted, this, [this] { onSeriesReset(); });
        connect(m_series, &B3CandleSeries::updated, this, [this](int index) { onSeriesUpdated(index); });
        connect(m_series, &QObject::destroyed, this, [this] {
            m_series = nullptr;
            update();
        });
    }
    resetView();
}

void B3ChartWidget::setMode(Mode mode)
{
    m_mode = mode;
    update();
}

void B3ChartWidget::resetView()
{
    const int n = m_series ? m_series->count() : 0;
    m_visible = std::clamp(60.0, kMinVisible, std::max(kMinVisible, static_cast<double>(std::max(n, 1))));
    m_start = std::max(0.0, n - m_visible);
    m_follow_tail = true;
    clampView();
    update();
}

QPair<int, int> B3ChartWidget::visibleRange() const
{
    const int n = m_series ? m_series->count() : 0;
    const int first = std::clamp(static_cast<int>(std::floor(m_start)), 0, std::max(0, n));
    const int last = std::clamp(static_cast<int>(std::ceil(m_start + m_visible)) + 1, first, n);
    return {first, last};
}

void B3ChartWidget::zoomBy(double factor, double anchor_fraction)
{
    if (factor <= 0.0) return;
    const int n = m_series ? m_series->count() : 0;
    const double anchor_index = m_start + m_visible * std::clamp(anchor_fraction, 0.0, 1.0);
    m_visible /= factor;
    const double max_visible = std::max(kMinVisible, n * kMaxZoomOutFactor);
    m_visible = std::clamp(m_visible, kMinVisible, max_visible);
    m_start = anchor_index - m_visible * std::clamp(anchor_fraction, 0.0, 1.0);
    m_follow_tail = false;
    clampView();
    update();
}

void B3ChartWidget::panByCandles(double candles)
{
    m_start += candles;
    m_follow_tail = false;
    clampView();
    update();
}

void B3ChartWidget::clampView()
{
    const int n = m_series ? m_series->count() : 0;
    // Keep at least one candle-width of data on screen at either edge.
    const double min_start = -m_visible + 1.0;
    const double max_start = std::max(min_start, static_cast<double>(n) - 1.0);
    m_start = std::clamp(m_start, min_start, max_start);
    if (n > 0 && m_start + m_visible >= n) m_follow_tail = true;
}

void B3ChartWidget::onSeriesReset()
{
    resetView();
}

void B3ChartWidget::onSeriesUpdated(int index)
{
    Q_UNUSED(index);
    if (m_follow_tail) {
        const int n = m_series ? m_series->count() : 0;
        m_start = std::max(0.0, n - m_visible);
    }
    update();
}

void B3ChartWidget::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (steps != 0.0) {
        const double anchor = width() > kAxisWidth
            ? std::clamp(event->position().x() / std::max(1, width() - kAxisWidth), 0.0, 1.0)
            : 0.5;
        zoomBy(std::pow(1.2, steps), anchor);
    }
    event->accept();
}

void B3ChartWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_drag_origin = event->pos();
        m_drag_start = m_start;
        setCursor(Qt::ClosedHandCursor);
    }
    QWidget::mousePressEvent(event);
}

void B3ChartWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_hover = event->pos();
    m_hovering = true;
    if (m_dragging) {
        const int plot_width = std::max(1, width() - kAxisWidth);
        const double candles_per_px = m_visible / plot_width;
        m_start = m_drag_start - (event->pos().x() - m_drag_origin.x()) * candles_per_px;
        m_follow_tail = false;
        clampView();
    }
    update();
    QWidget::mouseMoveEvent(event);
}

void B3ChartWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        unsetCursor();
    }
    QWidget::mouseReleaseEvent(event);
}

void B3ChartWidget::leaveEvent(QEvent* event)
{
    m_hovering = false;
    update();
    QWidget::leaveEvent(event);
}

void B3ChartWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), B3Theme::kSurface);

    const int n = m_series ? m_series->count() : 0;
    if (n == 0) {
        painter.setPen(B3Theme::kTextSecondary);
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No market data available.\nCharts activate when a market data backend is connected."));
        return;
    }

    const QRect plot(0, 0, width() - kAxisWidth, height() - kAxisHeight);
    const QRect volume_area(0, plot.height() - static_cast<int>(plot.height() * kVolumeFraction),
                            plot.width(), static_cast<int>(plot.height() * kVolumeFraction));
    const QRect price_area(0, 0, plot.width(), volume_area.top());

    const auto [first, last] = visibleRange();
    if (first >= last) return;

    // Autoscale Y to the visible slice only.
    qint64 min_price = m_series->at(first).low;
    qint64 max_price = m_series->at(first).high;
    qint64 max_volume = 1;
    for (int i = first; i < last; ++i) {
        const B3Candle& candle = m_series->at(i);
        min_price = std::min(min_price, candle.low);
        max_price = std::max(max_price, candle.high);
        max_volume = std::max(max_volume, candle.volume);
    }
    if (max_price == min_price) {
        // One-point/flat data: give the scale artificial headroom so the
        // candle is drawable.
        max_price += 1;
        min_price -= (min_price > 0 ? 1 : 0);
    }

    const double candle_width = static_cast<double>(plot.width()) / m_visible;
    const auto xAt = [&](int index) {
        return (index - m_start) * candle_width + candle_width / 2.0;
    };
    const auto yAt = [&](qint64 price) {
        const double t = static_cast<double>(price - min_price) / static_cast<double>(max_price - min_price);
        return price_area.bottom() - t * (price_area.height() - 8) - 4;
    };

    // Grid + price axis labels.
    painter.setFont(QFont(font().family(), 8));
    const QFontMetrics fm = painter.fontMetrics();
    const int grid_lines = 4;
    for (int g = 0; g <= grid_lines; ++g) {
        const qint64 price = min_price + (max_price - min_price) * g / grid_lines;
        const int y = static_cast<int>(yAt(price));
        painter.setPen(QColor(B3Theme::kBorder));
        painter.drawLine(0, y, plot.width(), y);
        painter.setPen(B3Theme::kTextMuted);
        painter.drawText(plot.width() + 4, y + fm.ascent() / 2,
                         B3Fixed::format(price, m_series->priceDecimals()));
    }

    // Time axis: a few labels across the visible range.
    painter.setPen(B3Theme::kTextMuted);
    const int time_labels = std::max(2, plot.width() / 140);
    for (int t = 0; t < time_labels; ++t) {
        const int index = first + (last - 1 - first) * t / std::max(1, time_labels - 1);
        const QDateTime when = QDateTime::fromSecsSinceEpoch(m_series->at(index).timestamp);
        const QString label = when.toString(QStringLiteral("dd MMM hh:mm"));
        const int x = std::clamp(static_cast<int>(xAt(index)) - fm.horizontalAdvance(label) / 2,
                                 0, plot.width() - fm.horizontalAdvance(label));
        painter.drawText(x, height() - 6, label);
    }

    // Volume bars.
    for (int i = first; i < last; ++i) {
        const B3Candle& candle = m_series->at(i);
        const double h = static_cast<double>(candle.volume) / static_cast<double>(max_volume) * (volume_area.height() - 2);
        QColor bar = candle.close >= candle.open ? B3Theme::kPositive : B3Theme::kNegative;
        bar.setAlphaF(0.35);
        painter.fillRect(QRectF(xAt(i) - candle_width * 0.3, volume_area.bottom() - h,
                                std::max(1.0, candle_width * 0.6), h),
                         bar);
    }

    // Price marks.
    if (m_mode == Mode::Line) {
        painter.setPen(QPen(B3Theme::kAccent, 1.5));
        QPointF prev;
        bool have_prev = false;
        for (int i = first; i < last; ++i) {
            const QPointF point(xAt(i), yAt(m_series->at(i).close));
            if (have_prev) painter.drawLine(prev, point);
            prev = point;
            have_prev = true;
        }
    } else {
        const double body_width = std::max(1.0, candle_width * 0.6);
        for (int i = first; i < last; ++i) {
            const B3Candle& candle = m_series->at(i);
            const bool up = candle.close >= candle.open;
            const QColor color = up ? B3Theme::kPositive : B3Theme::kNegative;
            const double x = xAt(i);
            painter.setPen(QPen(color, 1.0));
            painter.drawLine(QPointF(x, yAt(candle.high)), QPointF(x, yAt(candle.low)));
            const double top = yAt(std::max(candle.open, candle.close));
            const double bottom = yAt(std::min(candle.open, candle.close));
            painter.fillRect(QRectF(x - body_width / 2.0, top,
                                    body_width, std::max(1.0, bottom - top)),
                             color);
        }
    }

    // Crosshair + tooltip.
    if (m_hovering && plot.contains(m_hover)) {
        painter.setPen(QPen(B3Theme::kTextMuted, 1, Qt::DashLine));
        painter.drawLine(m_hover.x(), 0, m_hover.x(), plot.height());
        painter.drawLine(0, m_hover.y(), plot.width(), m_hover.y());

        const int index = std::clamp(static_cast<int>(std::floor(m_start + m_hover.x() / candle_width)),
                                     first, last - 1);
        const B3Candle& candle = m_series->at(index);
        const QString tip = tr("%1\nO %2  H %3  L %4  C %5\nVol %6")
            .arg(QDateTime::fromSecsSinceEpoch(candle.timestamp).toString(QStringLiteral("dd MMM yyyy hh:mm")),
                 B3Fixed::format(candle.open, m_series->priceDecimals()),
                 B3Fixed::format(candle.high, m_series->priceDecimals()),
                 B3Fixed::format(candle.low, m_series->priceDecimals()),
                 B3Fixed::format(candle.close, m_series->priceDecimals()),
                 B3Fixed::format(candle.volume, m_series->quantityDecimals()));
        const QRect tip_rect = fm.boundingRect(QRect(0, 0, 320, 80), Qt::AlignLeft, tip)
                                   .adjusted(0, 0, 12, 8);
        QPoint tip_pos = m_hover + QPoint(14, 10);
        if (tip_pos.x() + tip_rect.width() > plot.width()) tip_pos.setX(m_hover.x() - tip_rect.width() - 14);
        if (tip_pos.y() + tip_rect.height() > plot.height()) tip_pos.setY(m_hover.y() - tip_rect.height() - 10);
        painter.setPen(Qt::NoPen);
        QColor tip_bg = B3Theme::kCard;
        tip_bg.setAlphaF(0.95);
        painter.setBrush(tip_bg);
        painter.drawRoundedRect(QRect(tip_pos, tip_rect.size()), 6, 6);
        painter.setPen(B3Theme::kTextPrimary);
        painter.drawText(QRect(tip_pos + QPoint(6, 4), tip_rect.size()), Qt::AlignLeft | Qt::AlignTop, tip);
    }
}

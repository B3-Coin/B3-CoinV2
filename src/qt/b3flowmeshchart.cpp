// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshchart.h>
#include <qt/b3theme.h>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

using namespace B3FlowMeshMarketData;
B3FlowMeshChart::B3FlowMeshChart(QWidget* parent) : QWidget{parent} { setMouseTracking(true); setMinimumHeight(280); setObjectName(QStringLiteral("certifiedAuctionChart")); }
void B3FlowMeshChart::setSnapshot(const std::optional<Snapshot>& s) { if (m_snapshot == s) return; m_snapshot = s; update(); }
void B3FlowMeshChart::setMode(Mode mode) { if (m_mode == mode) return; m_mode = mode; update(); }
void B3FlowMeshChart::setInverted(bool inverted) { if (m_inverted == inverted) return; m_inverted = inverted; update(); }
void B3FlowMeshChart::setLoading(bool loading) { if (m_loading == loading) return; m_loading = loading; update(); }
void B3FlowMeshChart::setStale(bool stale) { if (m_stale == stale) return; m_stale = stale; update(); }
int B3FlowMeshChart::pricePointCount() const { return m_snapshot ? static_cast<int>(std::count_if(m_snapshot->history.begin(), m_snapshot->history.end(), [this](const auto& t) { return t.cleared && t.quantity > 0 && (!m_inverted || t.price > 0); })) : 0; }
QString B3FlowMeshChart::emptyMessage() const {
    if (!m_snapshot) return m_loading ? tr("Reading certified market data…") : tr("Select a market to view certified auctions");
    if (!m_snapshot->units.known) return tr("Asset precision unavailable\nPrices are hidden until token units are verified.");
    if (m_inverted && m_mode == Mode::Prices && std::any_of(m_snapshot->history.begin(), m_snapshot->history.end(), [](const auto& t) { return t.cleared && t.quantity > 0 && t.price == 0; })) return tr("No finite inverse trade price\nZero-price trades remain visible in history.");
    if (m_mode == Mode::Prices) {
        if (m_snapshot->remote && !m_snapshot->execution_result_verified) return m_snapshot->history_available ? tr("No trades reported in this history window\nEndpoint-reported history is not independently verified.") : tr("Trade history unavailable\nMissing history does not prove that no trades occurred.");
        return m_snapshot->history_available ? tr("No trades yet\nWaiting for the first confirmed trade in this history window.") : tr("Trade history unavailable\nThis node has no certified history to display.");
    }
    return m_inverted && !m_snapshot->depth.empty() ? tr("No finite inverse depth price\nZero-price samples remain visible in the liquidity table.") : tr("No orders yet\nOrders appear after certification.");
}
void B3FlowMeshChart::mouseMoveEvent(QMouseEvent* e) { m_pointer = e->position(); update(); }
void B3FlowMeshChart::leaveEvent(QEvent*) { m_pointer = {-1, -1}; update(); }
void B3FlowMeshChart::paintEvent(QPaintEvent*)
{
    QPainter p{this}; p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), B3Theme::kSurface);
    const QRectF plot{12, 26, static_cast<qreal>(std::max(20, width() - 104)), static_cast<qreal>(std::max(20, height() - 66))};
    p.setPen(B3Theme::kBorder);
    for (int i{0}; i <= 4; ++i) { const qreal y{plot.top() + plot.height() * i / 4}; p.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y}); }
    for (int i{1}; i < 5; ++i) { const qreal x{plot.left() + plot.width() * i / 5}; p.drawLine(QPointF{x, plot.top()}, QPointF{x, plot.bottom()}); }
    std::vector<const Depth*> depths;
    if (m_snapshot) for (const auto& d : m_snapshot->depth) if (!m_inverted || d.price > 0) depths.push_back(&d);
    if (m_inverted) std::reverse(depths.begin(), depths.end());
    const bool empty{!m_snapshot || !m_snapshot->units.known || (m_mode == Mode::Prices ? pricePointCount() == 0 : depths.empty())};
    if (empty) { p.setPen(B3Theme::kTextSecondary); p.drawText(plot.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage()); return; }
    const auto& s{*m_snapshot}; const auto units{s.units};
    // Floating point is confined to pixel coordinates. All labels, tables and
    // order inputs use exact integer/rational helpers, never these values.
    const auto price_coordinate = [&](CAmount price) { return m_inverted ? 1.0 / static_cast<double>(price) : static_cast<double>(price); };
    const QString price_units{(m_inverted ? QStringLiteral("%1 / B3") : QStringLiteral("B3 / %1")).arg(units.ticker)};
    const QString quantity_units{m_inverted ? QStringLiteral("B3 gross") : units.ticker};
    p.setFont(QFont{font().family(), 9});
    if (m_mode == Mode::Prices) {
        std::vector<Trade> trades; for (const auto& t : s.history) if (t.cleared && t.quantity > 0 && (!m_inverted || t.price > 0)) trades.push_back(t);
        CAmount low{trades.front().price}, high{low}, max_volume{1};
        for (const auto& t : trades) { if (price_coordinate(t.price) < price_coordinate(low)) low = t.price; if (price_coordinate(t.price) > price_coordinate(high)) high = t.price; max_volume = std::max(max_volume, m_inverted ? t.notional : t.quantity); }
        const uint64_t first{trades.front().sequence}, last{trades.back().sequence};
        const auto x = [&](uint64_t seq) { return first == last ? plot.center().x() : plot.left() + plot.width() * static_cast<double>(seq - first) / static_cast<double>(last - first); };
        const auto y = [&](CAmount price) { return price_coordinate(low) == price_coordinate(high) ? plot.center().y() : plot.bottom() - 26 - (plot.height() - 42) * (price_coordinate(price) - price_coordinate(low)) / (price_coordinate(high) - price_coordinate(low)); };
        QPainterPath path;
        for (size_t i{0}; i < trades.size(); ++i) { const QPointF point{x(trades[i].sequence), y(trades[i].price)}; if (i == 0) path.moveTo(point); else path.lineTo(point); }
        p.setPen(QPen{B3Theme::kAccent, 2}); p.drawPath(path);
        for (const auto& t : trades) { p.setBrush(B3Theme::kAccent); p.drawEllipse(QPointF{x(t.sequence), y(t.price)}, 2.6, 2.6); const qreal h{18.0 * static_cast<double>(m_inverted ? t.notional : t.quantity) / max_volume}; p.fillRect(QRectF{x(t.sequence) - 2, plot.bottom() - h, 4, h}, QColor{241, 189, 71, 80}); }
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(QRectF{plot.right() + 6, y(high) - 8, 78, 18}, FormatDisplayPrice(high, units.decimals, m_inverted));
        if (low != high) p.drawText(QRectF{plot.right() + 6, y(low) - 8, 78, 18}, FormatDisplayPrice(low, units.decimals, m_inverted));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignLeft, QStringLiteral("#%1").arg(first));
        if (first != last) p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignRight, QStringLiteral("#%1").arg(last));
        p.drawText(QRectF{plot.left(), 2, plot.width(), 18}, (s.remote && !s.execution_result_verified ? tr("%1 · endpoint-reported history") : tr("%1 · certified microblock sequence")).arg(price_units));
        if (m_inverted && std::any_of(s.history.begin(), s.history.end(), [](const auto& t) { return t.cleared && t.quantity > 0 && t.price == 0; })) { p.setPen(B3Theme::kWarning); p.drawText(plot.adjusted(8, 30, -8, -8), Qt::AlignTop | Qt::AlignLeft, tr("Zero-price trades have no inverse price")); }
        if (plot.contains(m_pointer)) {
            const auto closest{std::min_element(trades.begin(), trades.end(), [&](const auto& a, const auto& b) { return std::abs(x(a.sequence) - m_pointer.x()) < std::abs(x(b.sequence) - m_pointer.x()); })};
            p.setPen(QPen{B3Theme::kTextMuted, 1, Qt::DashLine}); p.drawLine(QPointF{x(closest->sequence), plot.top()}, QPointF{x(closest->sequence), plot.bottom()});
            const QString tip{tr("#%1   %2 %3   ·   %4 %5").arg(closest->sequence).arg(FormatDisplayPrice(closest->price, units.decimals, m_inverted), price_units, FormatDepthQuantity(closest->price, closest->quantity, units.decimals, m_inverted), quantity_units)};
            const QRectF box{plot.left() + 8, plot.top() + 8, std::min<qreal>(plot.width() - 16, p.fontMetrics().horizontalAdvance(tip) + 16), 26}; p.fillRect(box, B3Theme::kCardHover); p.setPen(B3Theme::kTextPrimary); p.drawText(box.adjusted(8, 0, -8, 0), Qt::AlignVCenter, tip);
        }
    } else {
        const CAmount first{depths.front()->price}, last{depths.back()->price};
        double max_qty{1}; CAmount max_price{0}, max_base{0};
        const auto quantity_coordinate = [&](const Depth& d, CAmount q) { return m_inverted ? static_cast<double>(d.price) * static_cast<double>(q) : static_cast<double>(q); };
        for (const auto* d : depths) for (const CAmount q : {d->demand, d->supply}) if (quantity_coordinate(*d, q) >= max_qty) { max_qty = quantity_coordinate(*d, q); max_price = d->price; max_base = q; }
        const auto x = [&](CAmount price) { return price_coordinate(first) == price_coordinate(last) ? plot.center().x() : plot.left() + plot.width() * (price_coordinate(price) - price_coordinate(first)) / (price_coordinate(last) - price_coordinate(first)); };
        const auto y = [&](double quantity) { return plot.bottom() - plot.height() * quantity / max_qty; };
        for (const bool demand : {true, false}) {
            // EvaluateCurve uses an exact, floored linear interpolant between
            // breakpoints. Segments are a visual guide; table values are exact
            // integer evaluations, and pixels are never fed into an order.
            QPainterPath path; for (size_t i{0}; i < depths.size(); ++i) { const auto& d{*depths[i]}; const CAmount quantity{demand != m_inverted ? d.demand : d.supply}; const QPointF point{x(d.price), y(quantity_coordinate(d, quantity))}; if (!i) path.moveTo(point); else path.lineTo(point); }
            p.setPen(QPen{demand ? B3Theme::kPositive : B3Theme::kNegative, 2}); p.setBrush(Qt::NoBrush); p.drawPath(path);
        }
        p.setPen(B3Theme::kTextSecondary); p.drawText(QRectF{plot.left(), 2, plot.width(), 18}, s.curves_complete ? tr("Aggregate demand / supply · %1").arg(quantity_units) : tr("PARTIAL curve page · not total market liquidity"));
        p.drawText(QRectF{plot.right() + 6, plot.top(), 78, 18}, FormatDepthQuantity(max_price, max_base, units.decimals, m_inverted));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignLeft, FormatDisplayPrice(first, units.decimals, m_inverted));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignRight, FormatDisplayPrice(last, units.decimals, m_inverted) + QLatin1Char(' ') + price_units);
    }
    if (m_stale) { p.setPen(B3Theme::kWarning); p.drawText(plot.adjusted(8, 30, -8, -8), Qt::AlignTop | Qt::AlignRight, tr("STALE · retained certified data")); }
}

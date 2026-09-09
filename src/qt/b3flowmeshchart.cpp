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
void B3FlowMeshChart::setSnapshot(const std::optional<Snapshot>& s) { m_snapshot = s; update(); }
void B3FlowMeshChart::setMode(Mode mode) { m_mode = mode; update(); }
void B3FlowMeshChart::setLoading(bool loading) { m_loading = loading; update(); }
void B3FlowMeshChart::setStale(bool stale) { m_stale = stale; update(); }
int B3FlowMeshChart::pricePointCount() const { return m_snapshot ? static_cast<int>(std::count_if(m_snapshot->history.begin(), m_snapshot->history.end(), [](const auto& t) { return t.cleared && t.quantity > 0; })) : 0; }
QString B3FlowMeshChart::emptyMessage() const {
    if (!m_snapshot) return m_loading ? tr("Reading certified market data…") : tr("Select a market to view certified auctions");
    if (!m_snapshot->units.known) return tr("Asset precision unavailable\nPrices are hidden until token units are verified.");
    if (m_mode == Mode::Prices) return m_snapshot->history_available ? tr("No certified trades in this history window\nA price appears only after a real uniform-price auction clears.") : tr("Certified history is unavailable on this node\nNo price history is invented or inferred from balances.");
    return tr("No standing demand or supply curves\nThe first accepted orders will appear after certification.");
}
void B3FlowMeshChart::mouseMoveEvent(QMouseEvent* e) { m_pointer = e->position(); update(); }
void B3FlowMeshChart::leaveEvent(QEvent*) { m_pointer = {-1, -1}; update(); }
void B3FlowMeshChart::paintEvent(QPaintEvent*)
{
    QPainter p{this}; p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), B3Theme::kSurface);
    const QRectF plot{12, 26, std::max(20, width() - 104), std::max(20, height() - 66)};
    p.setPen(B3Theme::kBorder);
    for (int i{0}; i <= 4; ++i) { const qreal y{plot.top() + plot.height() * i / 4}; p.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y}); }
    for (int i{1}; i < 5; ++i) { const qreal x{plot.left() + plot.width() * i / 5}; p.drawLine(QPointF{x, plot.top()}, QPointF{x, plot.bottom()}); }
    const bool empty{!m_snapshot || !m_snapshot->units.known || (m_mode == Mode::Prices ? pricePointCount() == 0 : m_snapshot->depth.empty())};
    if (empty) { p.setPen(B3Theme::kTextSecondary); p.drawText(plot.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage()); return; }
    const auto& s{*m_snapshot}; const auto units{s.units};
    p.setFont(QFont{font().family(), 9});
    if (m_mode == Mode::Prices) {
        std::vector<Trade> trades; for (const auto& t : s.history) if (t.cleared && t.quantity > 0) trades.push_back(t);
        CAmount low{trades.front().price}, high{low}, max_volume{1};
        for (const auto& t : trades) { low = std::min(low, t.price); high = std::max(high, t.price); max_volume = std::max(max_volume, t.quantity); }
        const uint64_t first{trades.front().sequence}, last{trades.back().sequence};
        const auto x = [&](uint64_t seq) { return first == last ? plot.center().x() : plot.left() + plot.width() * static_cast<double>(seq - first) / static_cast<double>(last - first); };
        const auto y = [&](CAmount price) { return low == high ? plot.center().y() : plot.bottom() - 26 - (plot.height() - 42) * static_cast<double>(price - low) / static_cast<double>(high - low); };
        QPainterPath path;
        for (size_t i{0}; i < trades.size(); ++i) { const QPointF point{x(trades[i].sequence), y(trades[i].price)}; if (i == 0) path.moveTo(point); else path.lineTo(point); }
        p.setPen(QPen{B3Theme::kAccent, 2}); p.drawPath(path);
        for (const auto& t : trades) { p.setBrush(B3Theme::kAccent); p.drawEllipse(QPointF{x(t.sequence), y(t.price)}, 2.6, 2.6); const qreal h{18.0 * static_cast<double>(t.quantity) / max_volume}; p.fillRect(QRectF{x(t.sequence) - 2, plot.bottom() - h, 4, h}, QColor{241, 189, 71, 80}); }
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(QRectF{plot.right() + 6, y(high) - 8, 78, 18}, FormatPrice(high, units.decimals));
        if (low != high) p.drawText(QRectF{plot.right() + 6, y(low) - 8, 78, 18}, FormatPrice(low, units.decimals));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignLeft, QStringLiteral("#%1").arg(first));
        if (first != last) p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignRight, QStringLiteral("#%1").arg(last));
        p.drawText(QRectF{plot.left(), 2, plot.width(), 18}, tr("B3 / %1 · certified microblock sequence").arg(units.ticker));
        if (plot.contains(m_pointer)) {
            const auto closest{std::min_element(trades.begin(), trades.end(), [&](const auto& a, const auto& b) { return std::abs(x(a.sequence) - m_pointer.x()) < std::abs(x(b.sequence) - m_pointer.x()); })};
            p.setPen(QPen{B3Theme::kTextMuted, 1, Qt::DashLine}); p.drawLine(QPointF{x(closest->sequence), plot.top()}, QPointF{x(closest->sequence), plot.bottom()});
            const QString tip{tr("#%1   %2 B3   ·   %3 %4").arg(closest->sequence).arg(FormatPrice(closest->price, units.decimals), FormatAmount(closest->quantity, units.decimals), units.ticker)};
            const QRectF box{plot.left() + 8, plot.top() + 8, std::min<qreal>(plot.width() - 16, p.fontMetrics().horizontalAdvance(tip) + 16), 26}; p.fillRect(box, B3Theme::kCardHover); p.setPen(B3Theme::kTextPrimary); p.drawText(box.adjusted(8, 0, -8, 0), Qt::AlignVCenter, tip);
        }
    } else {
        const CAmount first{s.depth.front().price}, last{s.depth.back().price}; CAmount max_qty{1};
        for (const auto& d : s.depth) max_qty = std::max({max_qty, d.demand, d.supply});
        const auto x = [&](CAmount price) { return first == last ? plot.center().x() : plot.left() + plot.width() * static_cast<double>(price - first) / static_cast<double>(last - first); };
        const auto y = [&](CAmount quantity) { return plot.bottom() - plot.height() * static_cast<double>(quantity) / static_cast<double>(max_qty); };
        for (const bool demand : {true, false}) {
            // EvaluateCurve uses an exact, floored linear interpolant between
            // breakpoints. Segments are a visual guide; table values are exact
            // integer evaluations, and pixels are never fed into an order.
            QPainterPath path; for (size_t i{0}; i < s.depth.size(); ++i) { const auto& d{s.depth[i]}; const QPointF point{x(d.price), y(demand ? d.demand : d.supply)}; if (!i) path.moveTo(point); else path.lineTo(point); }
            p.setPen(QPen{demand ? B3Theme::kPositive : B3Theme::kNegative, 2}); p.setBrush(Qt::NoBrush); p.drawPath(path);
        }
        p.setPen(B3Theme::kTextSecondary); p.drawText(QRectF{plot.left(), 2, plot.width(), 18}, s.curves_complete ? tr("Aggregate demand / supply · %1").arg(units.ticker) : tr("PARTIAL curve page · not total market liquidity"));
        p.drawText(QRectF{plot.right() + 6, plot.top(), 78, 18}, FormatAmount(max_qty, units.decimals));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignLeft, FormatPrice(first, units.decimals));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignRight, FormatPrice(last, units.decimals) + QStringLiteral(" B3"));
    }
    if (m_stale) { p.setPen(B3Theme::kWarning); p.drawText(plot.adjusted(8, 30, -8, -8), Qt::AlignTop | Qt::AlignRight, tr("STALE · retained certified data")); }
}

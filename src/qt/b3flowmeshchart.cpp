// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshchart.h>
#include <qt/b3theme.h>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <algorithm>
#include <cmath>

using namespace B3FlowMeshMarketData;
B3FlowMeshChart::B3FlowMeshChart(QWidget* parent) : QWidget{parent} { setMouseTracking(true); setMinimumHeight(280); setObjectName(QStringLiteral("certifiedAuctionChart")); }
void B3FlowMeshChart::setSnapshot(const std::optional<Snapshot>& s)
{
    if (m_snapshot == s) return;
    const bool same_history{m_snapshot && s && m_snapshot->certified == s->certified &&
        m_snapshot->history_available == s->history_available && m_snapshot->history == s->history};
    const bool same_plot{same_history && m_snapshot->units == s->units && m_snapshot->depth == s->depth &&
        m_snapshot->remote == s->remote && m_snapshot->execution_result_verified == s->execution_result_verified &&
        m_snapshot->curves_complete == s->curves_complete && m_snapshot->history_truncated == s->history_truncated &&
        m_snapshot->history_page_partial == s->history_page_partial && m_snapshot->event_gap == s->event_gap};
    // Keep the latest observation, but runtime-only tip reconciliation and
    // queue changes belong to the panel's status/action gates. They do not
    // change chart pixels or require rebuilding the retained auction candles.
    m_snapshot = s;
    if (!same_history) rebuildCandles();
    if (!same_plot) update();
}
void B3FlowMeshChart::setMode(Mode mode) { if (m_mode == mode) return; m_mode = mode; update(); }
void B3FlowMeshChart::setInverted(bool inverted) { if (m_inverted == inverted) return; m_inverted = inverted; rebuildCandles(); update(); }
void B3FlowMeshChart::setLoading(bool loading) { if (m_loading == loading) return; m_loading = loading; update(); }
void B3FlowMeshChart::setStale(bool stale) { if (m_stale == stale) return; m_stale = stale; update(); }
void B3FlowMeshChart::setCandleInterval(uint64_t microblocks)
{
    if ((microblocks != 1 && microblocks != 5 && microblocks != 20) || m_candle_interval == microblocks) return;
    m_candle_interval = microblocks; rebuildCandles(); update();
}
void B3FlowMeshChart::rebuildCandles()
{
    m_candles = m_snapshot && m_snapshot->certified && m_snapshot->history_available ? AggregateCandles(m_snapshot->history, m_candle_interval, m_inverted) : std::vector<Candle>{};
}
std::vector<B3FlowMeshChart::Candle> B3FlowMeshChart::AggregateCandles(const std::vector<Trade>& history, uint64_t microblocks, bool inverse)
{
    if (microblocks == 0) return {};
    std::vector<const Trade*> ordered;
    ordered.reserve(history.size());
    for (const auto& trade : history) ordered.push_back(&trade);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->sequence < b->sequence; });
    std::vector<Candle> candles;
    std::optional<uint64_t> prior_sequence;
    for (const auto* trade : ordered) {
        const auto& t{*trade};
        if ((prior_sequence && *prior_sequence == t.sequence) || !MoneyRange(t.price) || !MoneyRange(t.quantity) || !MoneyRange(t.notional) || !MoneyRange(t.fee)) return {};
        prior_sequence = t.sequence;
        if (!t.cleared) {
            if (t.quantity != 0 || t.notional != 0 || t.fee != 0) return {};
            continue;
        }
        if (t.quantity == 0 || t.price > MAX_MONEY / t.quantity || t.price * t.quantity != t.notional || t.fee > t.notional) return {};
        const uint64_t bucket{t.sequence - t.sequence % microblocks};
        if (candles.empty() || candles.back().bucket_sequence != bucket) {
            Candle c;
            c.bucket_sequence = bucket; c.first_sequence = c.last_sequence = t.sequence;
            c.open = c.high = c.low = c.close = t.price;
            candles.push_back(c);
        }
        auto& c{candles.back()};
        c.last_sequence = t.sequence; c.close = t.price;
        c.high = std::max(c.high, t.price); c.low = std::min(c.low, t.price);
        c.base_volume += static_cast<uint64_t>(t.quantity);
        c.quote_volume += static_cast<uint64_t>(t.notional);
        ++c.trades;
    }
    if (inverse) {
        std::erase_if(candles, [](const auto& c) { return c.low == 0; });
        for (auto& c : candles) std::swap(c.high, c.low);
    }
    return candles;
}
QString B3FlowMeshChart::FormatCandleVolume(const Candle& candle, int base_decimals, bool inverse)
{
    if (base_decimals < 0 || base_decimals > 18) return QStringLiteral("—");
    auto value{inverse ? candle.quote_volume : candle.base_volume};
    QString text;
    do { text.prepend(QChar{static_cast<ushort>('0' + static_cast<unsigned>(value % 10))}); value /= 10; } while (value != 0);
    const int decimals{inverse ? 9 : base_decimals};
    if (decimals > 0) {
        text = text.rightJustified(decimals + 1, QLatin1Char('0'));
        text.insert(text.size() - decimals, QLatin1Char('.'));
        while (text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    }
    return text;
}
int B3FlowMeshChart::pricePointCount() const
{
    size_t count{0}; for (const auto& candle : m_candles) count += candle.trades;
    return static_cast<int>(count);
}
QString B3FlowMeshChart::emptyMessage() const {
    if (!m_snapshot) return m_loading ? tr("Reading certified market data…") : tr("Select a market to view certified auctions");
    if (!m_snapshot->units.known) return tr("Asset precision unavailable\nPrices are hidden until token units are verified.");
    if (m_inverted && m_mode == Mode::Prices && std::any_of(m_snapshot->history.begin(), m_snapshot->history.end(), [](const auto& t) { return t.cleared && t.quantity > 0 && t.price == 0; })) return tr("No finite inverse candle price\nCandles containing zero-price trades cannot be inverted. Trades remain visible in history.");
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
    p.setFont(QFont{font().family(), 9});
    int axis_width{88};
    if (m_snapshot && m_mode == Mode::Prices) for (const auto& candle : m_candles) for (const auto price : {candle.low, candle.high}) {
        axis_width = std::max(axis_width, p.fontMetrics().horizontalAdvance(FormatDisplayPrice(price, m_snapshot->units.decimals, m_inverted)) + 12);
    }
    axis_width = std::min(axis_width, std::max(88, width() / 3));
    const QRectF plot{12, m_mode == Mode::Prices ? 70.0 : 26.0, static_cast<qreal>(std::max(20, width() - axis_width - 16)), static_cast<qreal>(std::max(20, height() - (m_mode == Mode::Prices ? 130 : 66)))};
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
    if (m_mode == Mode::Prices) {
        CAmount canonical_low{std::min(m_candles.front().low, m_candles.front().high)}, canonical_high{std::max(m_candles.front().low, m_candles.front().high)};
        util::Unsigned128 max_volume{1};
        for (const auto& candle : m_candles) {
            canonical_low = std::min(canonical_low, std::min(candle.low, candle.high));
            canonical_high = std::max(canonical_high, std::max(candle.low, candle.high));
            max_volume = std::max(max_volume, m_inverted ? candle.quote_volume : candle.base_volume);
        }
        const auto& first{m_candles.front()}; const auto& last{m_candles.back()};
        const long double span{static_cast<long double>((last.bucket_sequence - first.bucket_sequence) / m_candle_interval) + 1};
        const qreal slot_width{static_cast<qreal>(plot.width() / span)};
        const qreal body_width{std::clamp(slot_width * 0.62, 1.0, 20.0)};
        const QRectF volume_plot{plot.left(), plot.bottom() - plot.height() * 0.20, plot.width(), plot.height() * 0.20};
        const QRectF price_plot{plot.left(), plot.top() + 8, plot.width(), plot.height() * 0.74 - 16};
        const auto x = [&](const Candle& candle) {
            return plot.left() + static_cast<qreal>((static_cast<long double>((candle.bucket_sequence - first.bucket_sequence) / m_candle_interval) + 0.5L) * slot_width);
        };
        const auto y = [&](CAmount price) -> qreal {
            if (canonical_low == canonical_high) return price_plot.center().y();
            // Subtract integers before conversion, including reciprocal ranges:
            // (1/p - 1/high) / (1/low - 1/high). Large adjacent raw ticks
            // must not collapse into a zero floating-point denominator.
            const long double fraction{m_inverted ?
                static_cast<long double>(canonical_high - price) / (canonical_high - canonical_low) * canonical_low / price :
                static_cast<long double>(price - canonical_low) / (canonical_high - canonical_low)};
            return price_plot.bottom() - static_cast<qreal>(fraction) * price_plot.height();
        };
        for (const auto& candle : m_candles) {
            const bool rising{m_inverted ? candle.close < candle.open : candle.close > candle.open};
            const QColor color{candle.close == candle.open ? B3Theme::kTextSecondary : rising ? B3Theme::kPositive : B3Theme::kNegative};
            p.setPen(QPen{color, 1.4});
            p.drawLine(QPointF{x(candle), y(candle.high)}, QPointF{x(candle), y(candle.low)});
            const qreal body_top{std::min(y(candle.open), y(candle.close))};
            const qreal body_height{std::abs(y(candle.close) - y(candle.open))};
            if (body_height < 1.5) p.drawLine(QPointF{x(candle) - body_width / 2, body_top}, QPointF{x(candle) + body_width / 2, body_top});
            else p.fillRect(QRectF{x(candle) - body_width / 2, body_top, body_width, body_height}, color);
            const qreal volume_height{volume_plot.height() * static_cast<qreal>(static_cast<long double>(m_inverted ? candle.quote_volume : candle.base_volume) / static_cast<long double>(max_volume))};
            QColor volume_color{color}; volume_color.setAlpha(100);
            p.fillRect(QRectF{x(candle) - body_width / 2, plot.bottom() - volume_height, body_width, volume_height}, volume_color);
        }
        const CAmount high{m_inverted ? canonical_low : canonical_high}, low{m_inverted ? canonical_high : canonical_low};
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(QRectF{plot.right() + 6, y(high) - 8, static_cast<qreal>(axis_width - 6), 18}, FormatDisplayPrice(high, units.decimals, m_inverted));
        if (low != high) p.drawText(QRectF{plot.right() + 6, y(low) - 8, static_cast<qreal>(axis_width - 6), 18}, FormatDisplayPrice(low, units.decimals, m_inverted));
        p.drawText(QRectF{plot.left(), plot.bottom() + 5, plot.width(), 18}, Qt::AlignLeft, QStringLiteral("#%1").arg(first.first_sequence));
        if (first.first_sequence != last.last_sequence) p.drawText(QRectF{plot.left(), plot.bottom() + 5, plot.width(), 18}, Qt::AlignRight, QStringLiteral("#%1").arg(last.last_sequence));
        p.drawText(QRectF{plot.left(), plot.bottom() + 26, plot.width(), 18}, Qt::AlignCenter, tr("Microblock sequence · %1 / candle · gaps unfilled").arg(m_candle_interval));
        const QString provenance{s.remote && !s.execution_result_verified ? tr("Endpoint-reported · unverified execution") : tr("Certified execution history")};
        p.drawText(QRectF{plot.left(), 2, static_cast<qreal>(width() - 24), 18}, tr("%1 · %2").arg(price_units, provenance));
        const Candle* selected{&last};
        if (plot.contains(m_pointer)) {
            selected = &*std::min_element(m_candles.begin(), m_candles.end(), [&](const auto& a, const auto& b) { return std::abs(x(a) - m_pointer.x()) < std::abs(x(b) - m_pointer.x()); });
            p.setPen(QPen{B3Theme::kTextMuted, 1, Qt::DashLine});
            p.drawLine(QPointF{x(*selected), plot.top()}, QPointF{x(*selected), plot.bottom()});
            p.drawLine(QPointF{plot.left(), m_pointer.y()}, QPointF{plot.right(), m_pointer.y()});
        }
        const auto label = [&](CAmount price) { return FormatDisplayPrice(price, units.decimals, m_inverted); };
        const QStringList ohlc{tr("O %1").arg(label(selected->open)), tr("H %1").arg(label(selected->high)), tr("L %1").arg(label(selected->low)), tr("C %1").arg(label(selected->close))};
        p.setPen(B3Theme::kTextPrimary);
        const qreal field_width{(width() - 24) / 2.0};
        for (int i{0}; i < ohlc.size(); ++i) p.drawText(QRectF{plot.left() + (i % 2) * field_width, 24.0 + (i / 2) * 18, field_width, 18}, p.fontMetrics().elidedText(ohlc[i], Qt::ElideRight, static_cast<int>(field_width - 8)));
        const QString volume{tr("Vol %1 %2 · #%3–%4 · %5 clearing(s)").arg(FormatCandleVolume(*selected, units.decimals, m_inverted), quantity_units).arg(selected->first_sequence).arg(selected->last_sequence).arg(selected->trades)};
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(volume_plot.adjusted(2, -20, -2, -2), Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(volume, Qt::ElideRight, static_cast<int>(plot.width() - 4)));
        QStringList notices;
        if (s.history_truncated || s.history_page_partial || s.event_gap) notices.push_back(tr("Partial history window"));
        if (m_inverted && std::any_of(s.history.begin(), s.history.end(), [](const auto& t) { return t.cleared && t.quantity > 0 && t.price == 0; })) notices.push_back(tr("Zero-price candles cannot be inverted"));
        if (!notices.empty()) { p.setPen(B3Theme::kWarning); p.drawText(plot.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, notices.join(QStringLiteral(" · "))); }
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

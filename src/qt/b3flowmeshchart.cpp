// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshchart.h>
#include <qt/b3theme.h>
#include <flowmesh/observed_time.h>
#include <QDateTime>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QTimeZone>
#include <QUrl>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QToolTip>
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
namespace {
constexpr double MIN_TIME_SLOTS{10}, MAX_TIME_SLOTS{500}, DEFAULT_TIME_SLOTS{60};
QString Host(const QString& source)
{
    const QUrl url{source};
    return url.isValid() && !url.host().isEmpty() ? url.host() : source;
}
} // namespace
void B3FlowMeshChart::setTimeSeries(TimeSeries series)
{
    if (m_time == series) return;
    // Another interval, source or mode starts at the newest candle again.
    if (m_time.interval != series.interval || m_time.source != series.source || m_time.available != series.available) {
        m_time_right.reset();
        m_time_slots = DEFAULT_TIME_SLOTS;
    }
    m_time = std::move(series);
    update();
}
void B3FlowMeshChart::setNowMs(const int64_t now_ms)
{
    const auto interval{m_time.interval};
    const bool same_bucket{flowmesh::CandleOpenMs(now_ms, interval) == flowmesh::CandleOpenMs(m_now_ms, interval)};
    m_now_ms = now_ms;
    // Only a new bucket can move the axis.
    if (!same_bucket && timeMode() && !m_time_right) update();
}
int64_t B3FlowMeshChart::timeRight() const
{
    if (m_time_right) return *m_time_right;
    const int64_t step{flowmesh::CandleIntervalMs(m_time.interval)};
    const int64_t now_open{flowmesh::CandleOpenMs(m_now_ms, m_time.interval)};
    if (m_time.candles.empty()) return now_open;
    const int64_t last{m_time.candles.back().open_ms};
    // Now, while the newest candle is still well inside the window; after it
    // the axis is blank, never filled with invented bars.
    if (now_open >= last && (now_open - last) / step <= static_cast<int64_t>(m_time_slots) - 5) return now_open;
    return last;
}
std::pair<int64_t, int64_t> B3FlowMeshChart::timeWindow() const
{
    const int64_t step{flowmesh::CandleIntervalMs(m_time.interval)};
    const int64_t right{timeRight()};
    const int64_t slots{std::max<int64_t>(1, std::llround(m_time_slots))};
    return {std::max<int64_t>(0, right - (slots - 1) * step), right};
}
void B3FlowMeshChart::zoomTime(const double factor, const double anchor_fraction)
{
    if (factor <= 0) return;
    const int64_t step{flowmesh::CandleIntervalMs(m_time.interval)};
    const auto [left, right]{timeWindow()};
    const double anchor{std::clamp(anchor_fraction, 0.0, 1.0)};
    const double anchor_bucket{static_cast<double>(left) + anchor * static_cast<double>(right - left)};
    const double slots{std::clamp(m_time_slots / factor, MIN_TIME_SLOTS, MAX_TIME_SLOTS)};
    if (slots == m_time_slots) return;
    m_time_slots = slots;
    if (anchor < 1.0 || m_time_right) {
        const double new_right{anchor_bucket + (1.0 - anchor) * (slots - 1) * static_cast<double>(step)};
        m_time_right = flowmesh::CandleOpenMs(static_cast<int64_t>(std::llround(new_right)) + step / 2, m_time.interval);
    }
    update();
}
void B3FlowMeshChart::panTime(const double slots)
{
    const int64_t step{flowmesh::CandleIntervalMs(m_time.interval)};
    const int64_t right{timeRight() - static_cast<int64_t>(std::llround(slots)) * step};
    // Reaching the newest bucket again follows the tail.
    m_time_right.reset();
    const int64_t tail{timeRight()};
    if (right < tail) m_time_right = std::max<int64_t>(0, right);
    update();
}
void B3FlowMeshChart::resetTimeView()
{
    m_time_right.reset();
    m_time_slots = DEFAULT_TIME_SLOTS;
    update();
}
QString B3FlowMeshChart::TimeAxisLabel(const int64_t open_ms, const flowmesh::CandleInterval interval)
{
    const auto time{QDateTime::fromMSecsSinceEpoch(open_ms, QTimeZone::UTC)};
    switch (interval) {
    case flowmesh::CandleInterval::M1:
    case flowmesh::CandleInterval::M5:
    case flowmesh::CandleInterval::M15:
        return time.time().hour() == 0 && time.time().minute() == 0 ? time.toString(QStringLiteral("MMM d")) : time.toString(QStringLiteral("HH:mm"));
    case flowmesh::CandleInterval::H1:
    case flowmesh::CandleInterval::H4:
        return time.time().hour() == 0 ? time.toString(QStringLiteral("MMM d")) : time.toString(QStringLiteral("HH:mm"));
    case flowmesh::CandleInterval::D1:
        return time.toString(QStringLiteral("MMM d"));
    }
    return time.toString(QStringLiteral("HH:mm"));
}
QString B3FlowMeshChart::ProvenanceText(const bool remote, const QString& source)
{
    return remote ? tr("Prices, volumes and times reported by %1 · not verified").arg(Host(source))
                  : tr("Certified execution · times observed by this node (not certified)");
}
std::pair<B3FlowMeshChart::Between, uint64_t> B3FlowMeshChart::SpanBetween(const flowmesh::ObservedCandle& earlier, const flowmesh::ObservedCandle& later)
{
    const uint64_t missing{later.first_sequence > earlier.last_sequence ? later.first_sequence - earlier.last_sequence - 1 : 0};
    if (missing > 0) return {Between::GAP, missing};
    if (later.flags & flowmesh::CANDLE_SEAM_BEFORE) return {Between::SEAM, 0};
    return {Between::QUIET, 0};
}
QString B3FlowMeshChart::AuctionOrderLabel(const uint64_t auctions)
{
    return auctions == 1 ? tr("Every auction") : tr("Every %1 auctions").arg(auctions);
}
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
QString B3FlowMeshChart::FormatCandleVolume(const Candle& candle, int base_decimals, bool /*inverse*/)
{
    if (base_decimals < 0 || base_decimals > 18) return QStringLiteral("—");
    // Price inversion does not change the volume asset. In B3/rUSD view
    // this is rUSD turnover, not B3 notional relabelled as rUSD.
    auto value{candle.base_volume};
    QString text;
    do { text.prepend(QChar{static_cast<ushort>('0' + static_cast<unsigned>(value % 10))}); value /= 10; } while (value != 0);
    const int decimals{base_decimals};
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
bool B3FlowMeshChart::event(QEvent* e)
{
    if (e->type() != QEvent::ToolTip) return QWidget::event(e);
    const QString text{m_exact_prices.isEmpty() ? toolTip() : toolTip().isEmpty() ? m_exact_prices : toolTip() + QStringLiteral("\n\n") + m_exact_prices};
    if (text.isEmpty()) {
        QToolTip::hideText();
        e->ignore();
    } else {
        QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), text, this);
    }
    return true;
}
void B3FlowMeshChart::mouseMoveEvent(QMouseEvent* e)
{
    m_pointer = e->position();
    if (m_dragging && timeMode()) {
        const double slot_width{std::max(1.0, (width() - 116.0) / std::max(1.0, m_time_slots))};
        const double slots{(m_pointer.x() - m_drag_origin.x()) / slot_width};
        const int64_t step{flowmesh::CandleIntervalMs(m_time.interval)};
        m_time_right = std::max<int64_t>(0, m_drag_right - static_cast<int64_t>(std::llround(slots)) * step);
        // Past the newest bucket: follow the tail again.
        const auto chosen{m_time_right};
        m_time_right.reset();
        if (*chosen < timeRight()) m_time_right = chosen;
    }
    update();
}
void B3FlowMeshChart::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && timeMode() && m_mode == Mode::Prices) {
        m_dragging = true;
        m_drag_origin = e->position();
        m_drag_right = timeRight();
    }
    QWidget::mousePressEvent(e);
}
void B3FlowMeshChart::mouseReleaseEvent(QMouseEvent* e)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(e);
}
void B3FlowMeshChart::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (timeMode()) resetTimeView();
    QWidget::mouseDoubleClickEvent(e);
}
void B3FlowMeshChart::wheelEvent(QWheelEvent* e)
{
    if (!timeMode() || m_mode != Mode::Prices || e->angleDelta().y() == 0) { QWidget::wheelEvent(e); return; }
    const double anchor{std::clamp((e->position().x() - 12.0) / std::max(1.0, width() - 116.0), 0.0, 1.0)};
    zoomTime(e->angleDelta().y() > 0 ? 1.25 : 0.8, anchor);
    e->accept();
}
void B3FlowMeshChart::leaveEvent(QEvent*) { m_pointer = {-1, -1}; update(); }
void B3FlowMeshChart::paintEvent(QPaintEvent*)
{
    QPainter p{this}; p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), B3Theme::kSurface);
    p.setFont(QFont{font().family(), 9});
    m_exact_prices.clear();
    int axis_width{88};
    if (m_snapshot && m_mode == Mode::Prices) for (const auto& candle : m_candles) for (const auto price : {candle.low, candle.high}) {
        axis_width = std::max(axis_width, p.fontMetrics().horizontalAdvance(FormatDisplayPrice(price, m_snapshot->units, m_inverted)) + 12);
    }
    axis_width = std::min(axis_width, std::max(88, width() / 3));
    const QRectF plot{12, m_mode == Mode::Prices ? 70.0 : 26.0, static_cast<qreal>(std::max(20, width() - axis_width - 16)), static_cast<qreal>(std::max(20, height() - (m_mode == Mode::Prices ? 130 : 66)))};
    p.setPen(B3Theme::kBorder);
    for (int i{0}; i <= 4; ++i) { const qreal y{plot.top() + plot.height() * i / 4}; p.drawLine(QPointF{plot.left(), y}, QPointF{plot.right(), y}); }
    for (int i{1}; i < 5; ++i) { const qreal x{plot.left() + plot.width() * i / 5}; p.drawLine(QPointF{x, plot.top()}, QPointF{x, plot.bottom()}); }
    std::vector<const Depth*> depths;
    if (m_snapshot) for (const auto& d : m_snapshot->depth) if (!m_inverted || d.price > 0) depths.push_back(&d);
    if (m_inverted) std::reverse(depths.begin(), depths.end());
    if (m_mode == Mode::Prices && timeMode() && m_snapshot && m_snapshot->units.known) {
        QStringList exact;
        paintTime(p, plot, axis_width, exact);
        const QString price_units{(m_inverted ? QStringLiteral("%1 / B3") : QStringLiteral("B3 / %1")).arg(m_snapshot->units.ticker)};
        if (!exact.isEmpty()) m_exact_prices = tr("Exact prices (%1): %2").arg(price_units, exact.join(QStringLiteral(" · ")));
        if (m_stale) { p.setPen(B3Theme::kWarning); p.drawText(plot.adjusted(8, 30, -8, -8), Qt::AlignTop | Qt::AlignRight, tr("STALE · retained data")); }
        return;
    }
    const bool empty{!m_snapshot || !m_snapshot->units.known || (m_mode == Mode::Prices ? pricePointCount() == 0 : depths.empty())};
    if (empty) { p.setPen(B3Theme::kTextSecondary); p.drawText(plot.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, emptyMessage()); return; }
    const auto& s{*m_snapshot}; const auto units{s.units};
    // Floating point is confined to pixel coordinates. All labels, tables and
    // order inputs use exact integer/rational helpers, never these values.
    const auto price_coordinate = [&](CAmount price) { return m_inverted ? 1.0 / static_cast<double>(price) : static_cast<double>(price); };
    const QString price_units{(m_inverted ? QStringLiteral("%1 / B3") : QStringLiteral("B3 / %1")).arg(units.ticker)};
    const QString quantity_units{m_inverted ? QStringLiteral("B3 gross") : units.ticker};
    // Every price label: the shared display formatter. Rounded ones are
    // listed exactly for the hover tooltip.
    QStringList exact;
    const auto label = [&](const QString& name, CAmount price) {
        const auto shown{DisplayPrice(price, units, m_inverted)};
        if (shown.approximate) exact << name + QLatin1Char(' ') + shown.exact;
        return shown.text;
    };
    if (m_mode == Mode::Prices) {
        CAmount canonical_low{std::min(m_candles.front().low, m_candles.front().high)}, canonical_high{std::max(m_candles.front().low, m_candles.front().high)};
        util::Unsigned128 max_volume{1};
        for (const auto& candle : m_candles) {
            canonical_low = std::min(canonical_low, std::min(candle.low, candle.high));
            canonical_high = std::max(canonical_high, std::max(candle.low, candle.high));
            max_volume = std::max(max_volume, candle.base_volume);
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
            const qreal volume_height{volume_plot.height() * static_cast<qreal>(static_cast<long double>(candle.base_volume) / static_cast<long double>(max_volume))};
            QColor volume_color{color}; volume_color.setAlpha(100);
            p.fillRect(QRectF{x(candle) - body_width / 2, plot.bottom() - volume_height, body_width, volume_height}, volume_color);
        }
        const CAmount high{m_inverted ? canonical_low : canonical_high}, low{m_inverted ? canonical_high : canonical_low};
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(QRectF{plot.right() + 6, y(high) - 8, static_cast<qreal>(axis_width - 6), 18}, label(tr("axis high"), high));
        if (low != high) p.drawText(QRectF{plot.right() + 6, y(low) - 8, static_cast<qreal>(axis_width - 6), 18}, label(tr("axis low"), low));
        p.drawText(QRectF{plot.left(), plot.bottom() + 5, plot.width(), 18}, Qt::AlignLeft, QStringLiteral("#%1").arg(first.first_sequence));
        if (first.first_sequence != last.last_sequence) p.drawText(QRectF{plot.left(), plot.bottom() + 5, plot.width(), 18}, Qt::AlignRight, QStringLiteral("#%1").arg(last.last_sequence));
        p.drawText(QRectF{plot.left(), plot.bottom() + 26, plot.width(), 18}, Qt::AlignCenter,
                   tr("Auction order · %1 per candle · no time reported · gaps unfilled").arg(AuctionOrderLabel(m_candle_interval).toLower()));
        const QString provenance{s.remote && !s.execution_result_verified ? tr("Endpoint-reported · unverified execution") : tr("Certified execution history")};
        p.drawText(QRectF{plot.left(), 2, static_cast<qreal>(width() - 24), 18}, tr("%1 · %2").arg(price_units, provenance));
        const Candle* selected{&last};
        if (plot.contains(m_pointer)) {
            selected = &*std::min_element(m_candles.begin(), m_candles.end(), [&](const auto& a, const auto& b) { return std::abs(x(a) - m_pointer.x()) < std::abs(x(b) - m_pointer.x()); });
            p.setPen(QPen{B3Theme::kTextMuted, 1, Qt::DashLine});
            p.drawLine(QPointF{x(*selected), plot.top()}, QPointF{x(*selected), plot.bottom()});
            p.drawLine(QPointF{plot.left(), m_pointer.y()}, QPointF{plot.right(), m_pointer.y()});
        }
        const QStringList ohlc{tr("O %1").arg(label(tr("O"), selected->open)), tr("H %1").arg(label(tr("H"), selected->high)),
                               tr("L %1").arg(label(tr("L"), selected->low)), tr("C %1").arg(label(tr("C"), selected->close))};
        p.setPen(B3Theme::kTextPrimary);
        const qreal field_width{(width() - 24) / 2.0};
        for (int i{0}; i < ohlc.size(); ++i) p.drawText(QRectF{plot.left() + (i % 2) * field_width, 24.0 + (i / 2) * 18, field_width, 18}, p.fontMetrics().elidedText(ohlc[i], Qt::ElideRight, static_cast<int>(field_width - 8)));
        const QString volume{tr("Vol %1 %2 · #%3–%4 · %5 clearing(s)").arg(FormatCandleVolume(*selected, units.decimals, m_inverted), units.ticker).arg(selected->first_sequence).arg(selected->last_sequence).arg(selected->trades)};
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(volume_plot.adjusted(2, -20, -2, -2), Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(volume, Qt::ElideRight, static_cast<int>(plot.width() - 4)));
        QStringList notices;
        if (s.history_truncated || s.history_page_partial || s.event_gap) notices.push_back(tr("Partial history window"));
        if (m_time.reason == QStringLiteral("endpoint_does_not_advertise_observed_time")) notices.push_back(tr("This server does not report times (older version)"));
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
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignLeft, label(tr("axis left"), first));
        p.drawText(QRectF{plot.left(), plot.bottom() + 8, plot.width(), 20}, Qt::AlignRight, label(tr("axis right"), last) + QLatin1Char(' ') + price_units);
    }
    if (!exact.isEmpty()) m_exact_prices = tr("Exact prices (%1): %2").arg(price_units, exact.join(QStringLiteral(" · ")));
    if (m_stale) { p.setPen(B3Theme::kWarning); p.drawText(plot.adjusted(8, 30, -8, -8), Qt::AlignTop | Qt::AlignRight, tr("STALE · retained certified data")); }
}
void B3FlowMeshChart::paintTime(QPainter& p, const QRectF& plot, const int axis_width, QStringList& exact)
{
    const auto& s{*m_snapshot};
    const auto& units{s.units};
    const auto interval{m_time.interval};
    const int64_t step{flowmesh::CandleIntervalMs(interval)};
    const auto [left, right]{timeWindow()};
    const double slots{static_cast<double>((right - left) / step + 1)};
    const qreal slot_width{static_cast<qreal>(plot.width() / slots)};
    const qreal body_width{std::clamp(slot_width * 0.62, 1.0, 18.0)};
    const QRectF volume_plot{plot.left(), plot.bottom() - plot.height() * 0.20, plot.width(), plot.height() * 0.20};
    const QRectF price_plot{plot.left(), plot.top() + 8, plot.width(), plot.height() * 0.74 - 16};
    const QString price_units{(m_inverted ? QStringLiteral("%1 / B3") : QStringLiteral("B3 / %1")).arg(units.ticker)};
    const auto label = [&](const QString& name, CAmount price) {
        const auto shown{DisplayPrice(price, units, m_inverted)};
        if (shown.approximate) exact << name + QLatin1Char(' ') + shown.exact;
        return shown.text;
    };
    const auto x = [&](int64_t open) { return plot.left() + static_cast<qreal>((static_cast<long double>(open - left) / step + 0.5L) * slot_width); };
    const auto& candles{m_time.candles};
    // Only what is on screen (and each side's neighbour, for the space
    // between) is measured and drawn.
    const auto first{std::lower_bound(candles.begin(), candles.end(), left, [](const auto& c, int64_t v) { return c.open_ms < v; })};
    const auto last{std::upper_bound(candles.begin(), candles.end(), right, [](int64_t v, const auto& c) { return v < c.open_ms; })};
    const bool priced_inverse_ok{!m_inverted};
    std::optional<CAmount> low, high;
    util::Unsigned128 max_volume{1};
    const auto consider = [&](CAmount price) {
        if (m_inverted && price == 0) return;
        low = std::min(low.value_or(price), price);
        high = std::max(high.value_or(price), price);
    };
    for (auto it{first}; it != last; ++it) {
        if (it->clearings == 0) continue;
        consider(it->low); consider(it->high);
        max_volume = std::max(max_volume, it->base_volume);
    }
    // A quiet stretch entering the window is drawn at the previous close.
    if (first != candles.begin() && std::prev(first)->clearings > 0) consider(std::prev(first)->close);
    (void)priced_inverse_ok;
    const auto y = [&](CAmount price) -> qreal {
        if (!low || !high || *low == *high) return price_plot.center().y();
        const CAmount canonical_low{*low}, canonical_high{*high};
        const long double fraction{m_inverted ?
            static_cast<long double>(canonical_high - price) / (canonical_high - canonical_low) * canonical_low / std::max<CAmount>(price, 1) :
            static_cast<long double>(price - canonical_low) / (canonical_high - canonical_low)};
        return price_plot.bottom() - static_cast<qreal>(fraction) * price_plot.height();
    };
    // Between two candles: a proven quiet stretch (consecutive sequences,
    // no seam) is a flat line at the earlier close; anything else is a
    // hatched gap: auctions without observed time, or a clock seam.
    p.save();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1));
    const auto begin_pair{first == candles.begin() ? first : std::prev(first)};
    // The close a quiet stretch continues: the nearest priced candle before.
    std::optional<CAmount> carried_close;
    for (auto it{begin_pair}; it != candles.begin();) {
        --it;
        if (it->clearings > 0) { carried_close = it->close; break; }
    }
    for (auto it{begin_pair}; it != candles.end() && it->open_ms <= right; ++it) {
        if (it->clearings > 0) carried_close = it->close;
        const auto next{std::next(it)};
        if (next == candles.end()) break;
        if (next->open_ms < left) continue;
        const qreal from{x(it->open_ms) + slot_width / 2}, to{x(next->open_ms) - slot_width / 2};
        const auto [between, missing]{SpanBetween(*it, *next)};
        if (between == Between::QUIET) {
            if (to > from && carried_close && !(m_inverted && *carried_close == 0)) {
                p.setPen(QPen{B3Theme::kTextMuted, 1, Qt::DotLine});
                p.drawLine(QPointF{from, y(*carried_close)}, QPointF{to, y(*carried_close)});
            }
            continue;
        }
        const QRectF gap{from, price_plot.top(), std::max<qreal>(3, to - from), plot.bottom() - price_plot.top()};
        QColor hatch{B3Theme::kTextMuted}; hatch.setAlpha(90);
        p.fillRect(gap, QBrush{hatch, Qt::BDiagPattern});
        if (gap.width() > 110) {
            p.setPen(B3Theme::kTextSecondary);
            const QString text{missing ? tr("%1 auction(s) without observed time").arg(missing) : tr("clock seam")};
            p.drawText(gap.adjusted(4, 4, -4, -4), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, text);
        }
    }
    for (auto it{first}; it != last; ++it) {
        const auto& c{*it};
        if (c.clearings == 0 || (m_inverted && c.low == 0)) {
            // Entries without a match: a short neutral mark, never a price.
            p.setPen(QPen{B3Theme::kTextMuted, 1});
            const qreal base{plot.bottom() - 2};
            p.drawLine(QPointF{x(c.open_ms) - body_width / 2, base}, QPointF{x(c.open_ms) + body_width / 2, base});
            continue;
        }
        const bool rising{m_inverted ? c.close < c.open : c.close > c.open};
        QColor color{c.close == c.open ? B3Theme::kTextSecondary : rising ? B3Theme::kPositive : B3Theme::kNegative};
        const bool partial{(c.flags & flowmesh::CANDLE_PARTIAL) != 0};
        if (partial) color.setAlpha(140);
        const CAmount top_price{m_inverted ? c.low : c.high}, bottom_price{m_inverted ? c.high : c.low};
        p.setPen(QPen{color, 1.4});
        p.drawLine(QPointF{x(c.open_ms), y(top_price)}, QPointF{x(c.open_ms), y(bottom_price)});
        const qreal body_top{std::min(y(c.open), y(c.close))};
        const qreal body_height{std::abs(y(c.close) - y(c.open))};
        if (body_height < 1.5) p.drawLine(QPointF{x(c.open_ms) - body_width / 2, body_top}, QPointF{x(c.open_ms) + body_width / 2, body_top});
        else if (partial) { p.setPen(QPen{color, 1, Qt::DotLine}); p.setBrush(Qt::NoBrush); p.drawRect(QRectF{x(c.open_ms) - body_width / 2, body_top, body_width, body_height}); }
        else p.fillRect(QRectF{x(c.open_ms) - body_width / 2, body_top, body_width, body_height}, color);
        const qreal volume_height{volume_plot.height() * static_cast<qreal>(static_cast<long double>(c.base_volume) / static_cast<long double>(max_volume))};
        QColor volume_color{color}; volume_color.setAlpha(90);
        p.fillRect(QRectF{x(c.open_ms) - body_width / 2, plot.bottom() - volume_height, body_width, volume_height}, volume_color);
    }
    p.restore();
    p.setPen(B3Theme::kTextSecondary);
    if (low && high) {
        const CAmount shown_high{m_inverted ? *low : *high}, shown_low{m_inverted ? *high : *low};
        p.drawText(QRectF{plot.right() + 6, y(shown_high) - 8, static_cast<qreal>(axis_width - 6), 18}, label(tr("axis high"), shown_high));
        if (shown_low != shown_high) p.drawText(QRectF{plot.right() + 6, y(shown_low) - 8, static_cast<qreal>(axis_width - 6), 18}, label(tr("axis low"), shown_low));
    }
    // UTC axis: a label every k buckets, k keeping labels about 72 px apart.
    const int64_t every{std::max<int64_t>(1, static_cast<int64_t>(std::ceil(72.0 / std::max<qreal>(slot_width, 0.1))))};
    for (int64_t open{left - left % (every * step)}; open <= right; open += every * step) {
        if (open < left) continue;
        const qreal at{x(open)};
        p.setPen(B3Theme::kBorder);
        p.drawLine(QPointF{at, plot.bottom()}, QPointF{at, plot.bottom() + 3});
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(QRectF{at - 40, plot.bottom() + 5, 80, 18}, Qt::AlignHCenter, TimeAxisLabel(open, interval));
    }
    const QString caption{tr("%1 · UTC · time observed by %2, not certified")
        .arg(QString::fromLatin1(flowmesh::CandleIntervalName(interval)), m_time.remote ? Host(m_time.source) : tr("this node"))};
    p.drawText(QRectF{plot.left(), plot.bottom() + 26, plot.width(), 18}, Qt::AlignCenter,
               p.fontMetrics().elidedText(caption, Qt::ElideRight, static_cast<int>(plot.width())));
    p.drawText(QRectF{plot.left(), 2, static_cast<qreal>(width() - 24), 18}, tr("%1 · %2").arg(price_units, ProvenanceText(m_time.remote, m_time.source)));
    // The hovered (else newest shown) candle.
    const flowmesh::ObservedCandle* selected{nullptr};
    if (first != last) {
        selected = &*std::prev(last);
        if (plot.contains(m_pointer)) {
            selected = &*std::min_element(first, last, [&](const auto& a, const auto& b) { return std::abs(x(a.open_ms) - m_pointer.x()) < std::abs(x(b.open_ms) - m_pointer.x()); });
            p.setPen(QPen{B3Theme::kTextMuted, 1, Qt::DashLine});
            p.drawLine(QPointF{x(selected->open_ms), plot.top()}, QPointF{x(selected->open_ms), plot.bottom()});
            p.drawLine(QPointF{plot.left(), m_pointer.y()}, QPointF{plot.right(), m_pointer.y()});
        }
    }
    const qreal field_width{(width() - 24) / 2.0};
    if (selected && selected->clearings > 0) {
        const QStringList ohlc{tr("O %1").arg(label(tr("O"), selected->open)), tr("H %1").arg(label(tr("H"), m_inverted ? selected->low : selected->high)),
                               tr("L %1").arg(label(tr("L"), m_inverted ? selected->high : selected->low)), tr("C %1").arg(label(tr("C"), selected->close))};
        p.setPen(B3Theme::kTextPrimary);
        for (int i{0}; i < ohlc.size(); ++i) p.drawText(QRectF{plot.left() + (i % 2) * field_width, 24.0 + (i / 2) * 18, field_width, 18}, p.fontMetrics().elidedText(ohlc[i], Qt::ElideRight, static_cast<int>(field_width - 8)));
    }
    if (selected) {
        QStringList notes;
        if (selected->flags & flowmesh::CANDLE_PARTIAL) notes << tr("partial: some auctions here have no observed time");
        if (selected->flags & flowmesh::CANDLE_LATE) notes << tr("includes late-applied auctions within their bound");
        if (selected->flags & flowmesh::CANDLE_SEAM_BEFORE) notes << tr("time continuity with the previous candle not established");
        if (selected->flags & flowmesh::CANDLE_CLOCK) notes << tr("observer clock adjusted");
        Candle volume_only; volume_only.base_volume = selected->base_volume;
        const QString info{tr("%1 UTC · Vol %2 %3 · %4 auction(s), %5 matched%6")
            .arg(QDateTime::fromMSecsSinceEpoch(selected->open_ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                 FormatCandleVolume(volume_only, units.decimals, m_inverted), units.ticker)
            .arg(selected->entries).arg(selected->clearings)
            .arg(notes.isEmpty() ? QString{} : QStringLiteral(" · ") + notes.join(QStringLiteral(" · ")))};
        p.setPen(B3Theme::kTextSecondary);
        p.drawText(volume_plot.adjusted(2, -20, -2, -2), Qt::AlignTop | Qt::AlignLeft, p.fontMetrics().elidedText(info, Qt::ElideRight, static_cast<int>(plot.width() - 4)));
    }
    if (first == last) {
        p.setPen(B3Theme::kTextSecondary);
        const QString message{m_time.loading ? tr("Reading observed-time candles…")
            : !m_time.reason.isEmpty() ? tr("Observed-time candles unavailable (%1)").arg(m_time.reason)
            : candles.empty() ? tr("No auctions observed in time yet\nTimes start when this source first observed an auction; earlier history has none.")
                              : tr("No observed auction in this window\nScroll, zoom out or choose a longer interval.")};
        p.drawText(price_plot.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, message);
    }
    if (m_time_right) {
        p.setPen(B3Theme::kTextMuted);
        p.drawText(plot.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignRight, tr("Viewing history · double-click for the latest"));
    }
}

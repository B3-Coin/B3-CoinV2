// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3assettransfer.h>
#include <flowmesh/clearing.h>
#include <flowmesh/market.h>
#include <util/strencodings.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace B3FlowMeshMarketData {
namespace {
[[noreturn]] void Fail(const char* text) { throw std::runtime_error{text}; }
bool ValidUnits(const Units& u) { return u.known && u.decimals >= 0 && u.decimals <= 18 && u.quantity_step > 0 && u.price_step > 0; }
std::optional<CAmount> Error(QString* error, const QString& text) { if (error) *error = text; return std::nullopt; }
const UniValue& F(const UniValue& v, const char* key) { if (!v.isObject()) Fail("Invalid certified market-data object."); return v.find_value(key); }
QString Text(const UniValue& v) {
    if (!v.isStr() || v.get_str().size() > 512) Fail("Invalid market-data text.");
    const auto s{QString::fromStdString(v.get_str())};
    for (const auto c : s) if (c.unicode() < 32 || c.unicode() == 127) Fail("Control characters in market data.");
    return s;
}
QString Id(const UniValue& v, bool zero = false) {
    const auto s{Text(v).toLower()};
    if (s.size() != 64 || !IsHex(s.toStdString()) || (!zero && s == QString(64, QLatin1Char('0')))) Fail("Invalid certified market-data identity.");
    return s;
}
uint64_t Count(const UniValue& v) { if (!v.isNum()) Fail("Expected an exact market-data integer."); return v.getInt<uint64_t>(); }
CAmount Amount(const UniValue& v) { const auto n{Count(v)}; if (n > MAX_MONEY) Fail("Market-data amount exceeds the consensus range."); return static_cast<CAmount>(n); }
bool Flag(const UniValue& v) { if (!v.isBool()) Fail("Invalid market-data flag."); return v.get_bool(); }
std::vector<Curve> Curves(const UniValue& v, size_t limit) {
    if (!v.isArray() || v.size() > limit) Fail("Invalid or oversized certified curve page.");
    std::vector<Curve> out; std::set<std::pair<QString, QString>> ids;
    for (const auto& item : v.getValues()) {
        Curve c; c.account = Id(F(item, "account_id")); c.side = Text(F(item, "side"));
        if ((c.side != QStringLiteral("bid") && c.side != QStringLiteral("ask")) || !ids.emplace(c.account, c.side).second) Fail("Invalid or duplicate certified account curve.");
        const auto& points{F(item, "points")};
        if (!points.isArray() || points.empty() || points.size() > flowmesh::HARD_MAX_CURVE_POINTS) Fail("Invalid curve breakpoint count.");
        std::vector<flowmesh::ClearingEngine::Breakpoint> checked;
        for (const auto& p : points.getValues()) { const Point point{Amount(F(p, "price")), Amount(F(p, "quantity"))}; c.points.push_back(point); checked.push_back({point.price, point.quantity}); }
        uint256 base; base.begin()[0] = 1;
        const flowmesh::ClearingEngine engine{base, modern::NativeAsset(), flowmesh::HARD_MAX_CURVE_POINTS};
        if (!engine.CurveIsValid(c.side == QStringLiteral("bid") ? flowmesh::ClearingEngine::Side::BID : flowmesh::ClearingEngine::Side::ASK, checked)) Fail("Noncanonical certified curve.");
        c.filled = Amount(F(item, "filled_quantity")); c.remaining = Amount(F(item, "remaining_quantity")); c.reserved = Amount(F(item, "reserved_amount"));
        const CAmount maximum{c.side == QStringLiteral("bid") ? c.points.front().quantity : c.points.back().quantity};
        if (c.filled > maximum || c.remaining != maximum - c.filled) Fail("Certified curve remaining amount is inconsistent.");
        out.push_back(std::move(c));
    }
    return out;
}
}

QString FormatAmount(CAmount raw, int decimals)
{
    if (raw < 0 || decimals < 0 || decimals > 18) return QStringLiteral("—");
    QString s{QString::number(raw)};
    if (decimals == 0) return s;
    s = s.rightJustified(decimals + 1, QLatin1Char('0')); s.insert(s.size() - decimals, QLatin1Char('.'));
    while (s.endsWith(QLatin1Char('0'))) s.chop(1);
    if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    return s;
}
QString FormatPrice(CAmount raw, int decimals)
{
    if (raw < 0 || decimals < 0 || decimals > 18) return QStringLiteral("—");
    if (decimals <= 9) return FormatAmount(raw, 9 - decimals);
    return raw == 0 ? QStringLiteral("0") : QString::number(raw) + QString(decimals - 9, QLatin1Char('0'));
}
std::optional<CAmount> ParseQuantity(const QString& text, const Units& units, QString* error)
{
    if (!ValidUnits(units)) return Error(error, QStringLiteral("Verified asset precision is unavailable. Trading is disabled rather than guessing units."));
    const auto n{B3AssetTransfer::ParseAmount(text, units.decimals, error)};
    if (n && *n % units.quantity_step != 0) return Error(error, QStringLiteral("Quantity must be an exact multiple of %1 %2.").arg(FormatAmount(units.quantity_step, units.decimals), units.ticker));
    return n;
}
std::optional<CAmount> ParsePrice(const QString& text, const Units& units, QString* error)
{
    if (!ValidUnits(units)) return Error(error, QStringLiteral("Verified asset precision is required to enter B3 per token."));
    std::optional<CAmount> price;
    if (units.decimals <= 9) price = B3AssetTransfer::ParseAmount(text, 9 - units.decimals, error);
    else {
        // Coarse price grids for assets with >9 decimals are represented by
        // exact trailing zeroes, never by a rounded division or floating point.
        const int zeroes{units.decimals - 9};
        if (text.size() <= zeroes || !text.endsWith(QString(zeroes, QLatin1Char('0')))) return Error(error, QStringLiteral("Price must be on the exact %1 B3/token grid.").arg(FormatPrice(units.price_step, units.decimals)));
        price = B3AssetTransfer::ParseAmount(text.left(text.size() - zeroes), 0, error);
    }
    if (price && *price % units.price_step != 0) return Error(error, QStringLiteral("Price must be an exact multiple of %1 B3 per token.").arg(FormatPrice(units.price_step, units.decimals)));
    return price;
}
std::optional<CAmount> Notional(CAmount price, CAmount quantity)
{
    if (price <= 0 || quantity <= 0 || !MoneyRange(price) || !MoneyRange(quantity) || price > MAX_MONEY / quantity) return std::nullopt;
    return price * quantity;
}
std::optional<CAmount> FeeExample(CAmount notional)
{
    if (!MoneyRange(notional)) return std::nullopt;
    return notional / 10'000; // floor(notional * 100 / 1,000,000), without overflow.
}

std::vector<Depth> Aggregate(const std::vector<Curve>& curves)
{
    if (curves.size() > 128) Fail("Oversized curve page.");
    std::set<CAmount> prices; std::vector<std::vector<flowmesh::ClearingEngine::Breakpoint>> prepared;
    uint256 base; base.begin()[0] = 1; const flowmesh::ClearingEngine engine{base, modern::NativeAsset(), flowmesh::HARD_MAX_CURVE_POINTS};
    for (const auto& c : curves) {
        std::vector<flowmesh::ClearingEngine::Breakpoint> points;
        for (const auto& p : c.points) { prices.insert(p.price); points.push_back({p.price, p.quantity}); }
        if ((c.side != QStringLiteral("bid") && c.side != QStringLiteral("ask")) || !engine.CurveIsValid(c.side == QStringLiteral("bid") ? flowmesh::ClearingEngine::Side::BID : flowmesh::ClearingEngine::Side::ASK, points) || c.filled < 0 || !MoneyRange(c.filled)) Fail("Invalid curve supplied for display aggregation.");
        prepared.push_back(std::move(points));
    }
    std::vector<Depth> depths;
    for (const auto price : prices) {
        Depth d; d.price = price;
        for (size_t i{0}; i < curves.size(); ++i) {
            const auto& c{curves[i]};
            const CAmount n{std::max<CAmount>(0, flowmesh::ClearingEngine::EvaluateCurve(prepared[i], price) - c.filled)};
            auto& total{c.side == QStringLiteral("bid") ? d.demand : d.supply};
            if (n > MAX_MONEY - total) Fail("Aggregate certified liquidity exceeds the supported display range.");
            total += n;
        }
        depths.push_back(d);
    }
    return depths;
}

Snapshot Parse(const UniValue& value)
{
    Snapshot s;
    if (Text(F(value, "matching_model")) != QStringLiteral("uniform-price-curve-auction")) Fail("Unsupported FlowMesh matching model.");
    s.market = Id(F(value, "market_id")); s.base = Id(F(value, "base_asset_id")); s.domain = Id(F(value, "domain")); s.config = Id(F(value, "execution_config_id"));
    if (Text(F(value, "quote_asset")) != QStringLiteral("B3") || flowmesh::ComputeFlowMeshMarketId(*uint256::FromHex(s.domain.toStdString()), *uint256::FromHex(s.base.toStdString()))->GetHex() != s.market.toStdString()) Fail("Certified market identity is not derived from this base asset and domain.");
    const auto& v{F(value, "snapshot")};
    s.certified = Flag(F(v, "certified")); s.next_sequence = Count(F(v, "next_microblock_sequence")); s.head = Id(F(v, "last_microblock_hash"), !s.certified); s.state_root = Id(F(v, "state_root"), !s.certified);
    s.epoch = Count(F(v, "epoch")); s.running = Flag(F(v, "running")); s.paused = Flag(F(v, "paused")); s.handoff = Flag(F(v, "pending_handoff")); s.observer = Flag(F(v, "observer_only"));
    s.halt = Text(F(v, "halt")); s.error = Text(F(v, "error")); s.active_seats = Count(F(v, "active_seats")); s.quorum_required = Count(F(v, "quorum_required")); s.pending_actions = Count(F(v, "pending_actions"));
    if (!F(value, "unchanged").isNull()) s.unchanged = Flag(F(value, "unchanged"));
    const auto& metadata{F(value, "base_metadata")};
    if (!metadata.isNull()) {
        s.units.asset = s.base; s.units.known = Flag(F(metadata, "known"));
        s.units.test_only = Flag(F(metadata, "test_only"));
        if (s.units.known) {
            const auto decimals{Count(F(metadata, "decimals"))}; if (decimals > 18) Fail("Invalid verified token precision."); s.units.decimals = static_cast<int>(decimals);
            s.units.ticker = Text(F(metadata, "ticker")); s.units.name = Text(F(metadata, "name")); s.units.source = Text(F(metadata, "source"));
            if (s.units.source.isEmpty()) Fail("Token metadata has no provenance.");
            if (s.units.ticker.isEmpty()) s.units.ticker = QStringLiteral("Asset %1…").arg(s.base.left(8));
        }
    }
    s.units.quantity_step = Amount(F(value, "quantity_lot_raw")); s.units.price_step = Amount(F(value, "price_tick_raw"));
    if (s.units.quantity_step != 1 || s.units.price_step != 1) Fail("Unsupported v1 market unit grid.");
    if (s.active_seats > 1'000'000 || s.quorum_required > s.active_seats) Fail("Invalid configured validator threshold.");
    if (s.unchanged) return s;
    const auto& liquidity{F(value, "liquidity")}; s.curves_complete = Flag(F(liquidity, "complete")); s.curves = Curves(F(liquidity, "curves"), 128); s.depth = Aggregate(s.curves);
    if (s.curves_complete && Count(F(liquidity, "total_curves")) != s.curves.size()) Fail("Complete liquidity count does not match its curves.");
    const auto& account{F(value, "account")};
    if (!account.isNull()) {
        s.account = Id(F(account, "account_id")); s.account_sequence = Count(F(account, "next_sequence"));
        s.base_available = Amount(F(account, "base_available")); s.base_reserved = Amount(F(account, "base_reserved")); s.b3_available = Amount(F(account, "b3_available_atoms")); s.b3_reserved = Amount(F(account, "b3_reserved_atoms"));
        s.own_curves = Curves(F(account, "curves"), 2);
        for (const auto& c : s.own_curves) if (c.account != s.account) Fail("Own curve belongs to a different account.");
    }
    const auto& history{F(value, "history")}; s.history_truncated = Flag(F(history, "truncated")); s.history_available = Flag(F(history, "available"));
    if (!F(history, "next_before_sequence").isNull()) { Count(F(history, "next_before_sequence")); s.history_page_partial = true; }
    if (Text(F(history, "scope")) != QStringLiteral("bounded-certified-log-cache")) Fail("Unsupported trade-history provenance.");
    const auto& entries{F(history, "entries")}; if (!entries.isArray() || entries.size() > 100) Fail("Oversized certified history page.");
    std::set<uint64_t> sequences;
    for (const auto& v : entries.getValues()) {
        Trade t; t.sequence = Count(F(v, "sequence")); t.hash = Id(F(v, "microblock_hash")); t.epoch = Count(F(v, "epoch"));
        if (!s.certified || t.sequence >= s.next_sequence || !sequences.insert(t.sequence).second) Fail("Trade history lies outside the certified snapshot.");
        t.anchor_height = F(v, "anchor_height").getInt<int64_t>(); if (t.anchor_height < 0) Fail("Invalid certified anchor height.");
        t.cleared = Flag(F(v, "cleared")); t.price = Amount(F(v, "price")); t.quantity = Amount(F(v, "quantity")); t.notional = Amount(F(v, "notional_atoms")); t.fee = Amount(F(v, "fee_atoms"));
        t.own_fills_known = Flag(F(v, "account_fills_known")); t.own_buy = Amount(F(v, "account_bid_fill")); t.own_sell = Amount(F(v, "account_ask_fill"));
        if (t.cleared ? (t.quantity == 0 || t.price > MAX_MONEY / t.quantity || t.price * t.quantity != t.notional || t.fee > t.notional) : (t.quantity != 0 || t.notional != 0 || t.fee != 0)) Fail("Certified clearing amounts are inconsistent.");
        if (t.own_buy > t.quantity || t.own_sell > t.quantity || (!t.own_fills_known && (t.own_buy || t.own_sell))) Fail("Invalid account fill data.");
        s.history.push_back(t);
    }
    std::sort(s.history.begin(), s.history.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
    return s;
}

bool AdmissionReady(const Snapshot& s, int64_t response_age, int64_t certificate_age, int64_t queued_without_progress)
{
    (void)certificate_age; // Idle markets do not produce empty heartbeat certificates.
    // Local UI precaution, not a claim that the committee is offline. Only
    // time with outstanding requests counts; an idle chart does not pause it.
    if (s.pending_actions > 0 && queued_without_progress >= 30'000) return false;
    return response_age >= 0 && response_age <= 3000 && s.certified && s.running && !s.paused && !s.handoff && s.halt == QStringLiteral("none") && s.error.isEmpty() && s.active_seats >= 4 && s.active_seats <= 1'000'000 && s.quorum_required == (s.active_seats * 2) / 3 + 1;
}
QString StatusText(const Snapshot& s, int64_t response_age, int64_t certificate_age, int64_t queued_without_progress)
{
    if (response_age < 0 || response_age > 3000) return QStringLiteral("Connection stale · last certified data retained; new orders are disabled.");
    if (!s.running) return QStringLiteral("Market service offline · waiting for the node.");
    if (s.halt != QStringLiteral("none") || !s.error.isEmpty()) return QStringLiteral("Market halted · %1").arg(!s.error.isEmpty() ? s.error : s.halt);
    if (s.handoff) return QStringLiteral("Validator handoff · new orders paused; certified settlement remains available.");
    if (s.paused) return QStringLiteral("Market paused · quorum or checkpoint progress required.");
    if (!s.certified) return QStringLiteral("Awaiting first certified microblock · no price or fills yet.");
    if (s.pending_actions > 0 && queued_without_progress >= 30'000) return QStringLiteral("Queued requests have not certified for 30 seconds · new submissions paused locally. Existing requests are not canceled.");
    if (certificate_age < 0 || certificate_age > 10000) return QStringLiteral("Certified state · awaiting another auction; current quorum is not independently proven.");
    return QStringLiteral("Certified updates observed · runtime status alone is not proof of current quorum.");
}
} // namespace B3FlowMeshMarketData

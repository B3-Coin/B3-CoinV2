// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3assettransfer.h>
#include <flowmesh/clearing.h>
#include <flowmesh/market.h>
#include <util/strencodings.h>
#include <util/int128.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace B3FlowMeshMarketData {
namespace {
using Wide = util::Unsigned128;
struct Ratio { Wide numerator, denominator; };
Wide Gcd(Wide a, Wide b) { while (b != 0) { const Wide r{a % b}; a = b; b = r; } return a; }
Wide Pow10(int n) { Wide result{1}; while (n-- > 0) result *= 10; return result; }
QString WideText(Wide n)
{
    QString result;
    do { result.prepend(QChar{static_cast<ushort>('0' + static_cast<unsigned>(n % 10))}); n /= 10; } while (n != 0);
    return result;
}
std::optional<Wide> PositiveInteger(const QString& text)
{
    if (text.isEmpty() || text.size() > 38) return std::nullopt;
    const Wide maximum{~Wide{0}};
    Wide value{0};
    for (const auto c : text) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return std::nullopt;
        const unsigned digit{static_cast<unsigned>(c.unicode() - '0')};
        if (value > (maximum - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return value == 0 ? std::nullopt : std::optional<Wide>{value};
}
std::optional<Ratio> PositiveRatio(const QString& input)
{
    const QString& text{input};
    const auto slash{text.indexOf(QLatin1Char('/'))};
    if (slash >= 0) {
        const auto n{PositiveInteger(text.left(slash))}, d{PositiveInteger(text.mid(slash + 1))};
        if (!n || !d) return std::nullopt;
        return Ratio{*n, *d};
    }
    const auto dot{text.indexOf(QLatin1Char('.'))};
    const int decimals{dot < 0 ? 0 : static_cast<int>(text.size() - dot - 1)};
    if (decimals > 36 || (dot >= 0 && (dot == 0 || decimals == 0))) return std::nullopt;
    const auto n{PositiveInteger(dot < 0 ? text : text.left(dot) + text.mid(dot + 1))};
    if (!n) return std::nullopt;
    return Ratio{*n, Pow10(decimals)};
}
std::optional<Ratio> InverseRatio(CAmount raw, int decimals)
{
    if (raw <= 0 || !MoneyRange(raw) || decimals < 0 || decimals > 18) return std::nullopt;
    Ratio r{KILO_COIN, Wide{static_cast<uint64_t>(raw)} * Pow10(decimals)};
    const Wide divisor{Gcd(r.numerator, r.denominator)};
    r.numerator /= divisor; r.denominator /= divisor;
    return r;
}
QString RatioText(const Ratio& ratio, bool exact)
{
    QString result{WideText(ratio.numerator / ratio.denominator)};
    Wide remainder{ratio.numerator % ratio.denominator};
    if (remainder == 0) return result;
    result += QLatin1Char('.');
    // Formatting only. Truncation is explicitly marked and never accepted as
    // an executable price. A reduced fraction represents every canonical tick.
    for (int i{0}; i < 18 && remainder != 0; ++i) {
        remainder *= 10;
        result += QChar{static_cast<ushort>('0' + static_cast<unsigned>(remainder / ratio.denominator))};
        remainder %= ratio.denominator;
    }
    while (result.endsWith(QLatin1Char('0'))) result.chop(1);
    if (result.endsWith(QLatin1Char('.'))) result.chop(1);
    if (remainder == 0) return result;
    if (exact || result == QStringLiteral("0")) return WideText(ratio.numerator) + QLatin1Char('/') + WideText(ratio.denominator);
    return QStringLiteral("≈") + result;
}
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
QString FormatDisplayPrice(CAmount raw, int decimals, bool inverse)
{
    if (!inverse) return FormatPrice(raw, decimals);
    const auto ratio{InverseRatio(raw, decimals)};
    return ratio ? RatioText(*ratio, false) : QStringLiteral("—");
}
QString ExactInversePrice(CAmount raw, int decimals)
{
    const auto ratio{InverseRatio(raw, decimals)};
    return ratio ? RatioText(*ratio, true) : QStringLiteral("—");
}
QString FormatDepthQuantity(CAmount price, CAmount quantity, int decimals, bool inverse)
{
    if (!inverse) return FormatAmount(quantity, decimals);
    if (!MoneyRange(price) || !MoneyRange(quantity)) return QStringLiteral("—");
    QString result{WideText(Wide{static_cast<uint64_t>(price)} * static_cast<uint64_t>(quantity))};
    result = result.rightJustified(10, QLatin1Char('0'));
    result.insert(result.size() - 9, QLatin1Char('.'));
    while (result.endsWith(QLatin1Char('0'))) result.chop(1);
    if (result.endsWith(QLatin1Char('.'))) result.chop(1);
    return result;
}
std::optional<CAmount> ParseDisplayPrice(const QString& text, const Units& units, bool inverse, QString* error)
{
    if (!inverse) return ParsePrice(text, units, error);
    if (!ValidUnits(units)) return Error(error, QStringLiteral("Verified asset precision is required to enter token per B3."));
    const auto ratio{PositiveRatio(text)};
    if (!ratio) return Error(error, QStringLiteral("Enter a positive decimal or exact fraction, e.g. 2 or 1000/901. Approximate prices cannot be submitted."));
    // raw = 10^9 * denominator / (10^decimals * numerator). Cancel factors
    // BEFORE multiplying, so even extreme decimal/fraction inputs cannot wrap.
    Wide a{KILO_COIN}, b{ratio->denominator}, c{Pow10(units.decimals)}, d{ratio->numerator};
    for (auto* numerator : {&a, &b}) for (auto* denominator : {&c, &d}) {
        const Wide divisor{Gcd(*numerator, *denominator)};
        *numerator /= divisor; *denominator /= divisor;
    }
    if (c != 1 || d != 1) return Error(error, QStringLiteral("That inverse price is not on the exact market grid. Use an exact quoted fraction; no price was rounded."));
    if (a > static_cast<uint64_t>(MAX_MONEY) || b > static_cast<uint64_t>(MAX_MONEY) / a) return Error(error, QStringLiteral("The inverse price exceeds the supported market range."));
    const CAmount price{static_cast<CAmount>(a * b)};
    if (price <= 0 || price % units.price_step != 0) return Error(error, QStringLiteral("That inverse price is not on the exact market grid; no price was rounded."));
    return price;
}
std::optional<DisplayLimit> ParseDisplayLimit(const QString& text, const Units& units, bool inverse, bool display_buy, QString* error)
{
    const auto reject = [&](const QString& message) -> std::optional<DisplayLimit> {
        if (error) *error = message;
        return std::nullopt;
    };
    // A limit bid needs its existing (price + 1, zero quantity) endpoint.
    // This is the existing action shape, not a new price grid restriction.
    const bool canonical_bid{display_buy != inverse};
    const CAmount maximum_price{MAX_MONEY - (canonical_bid ? 1 : 0)};
    if (!inverse) {
        const auto price{ParsePrice(text, units, error)};
        if (!price) return std::nullopt;
        if (*price > maximum_price) return reject(QStringLiteral("The limit exceeds the supported canonical order range."));
        return DisplayLimit{*price, false, FormatPrice(*price, units.decimals)};
    }
    if (!ValidUnits(units) || units.price_step > MAX_MONEY) return reject(QStringLiteral("Verified asset precision and a valid canonical price grid are required."));
    const auto ratio{PositiveRatio(text)};
    if (!ratio) return reject(QStringLiteral("Enter a positive decimal or exact fraction. Approximate prices cannot be submitted."));
    // Displayed r = numerator / denominator in colored units per B3.
    // Canonical grid index k = 10^9 * denominator /
    //                         (10^decimals * numerator * price_step).
    // Cross-cancel before either product. Reject unrepresentable products;
    // never saturate or use floating point to choose a signed tick.
    Wide numerators[]{Wide{KILO_COIN}, ratio->denominator};
    Wide denominators[]{Pow10(units.decimals), ratio->numerator, Wide{static_cast<uint64_t>(units.price_step)}};
    for (auto& numerator : numerators) for (auto& denominator : denominators) {
        const Wide divisor{Gcd(numerator, denominator)};
        numerator /= divisor;
        denominator /= divisor;
    }
    const Wide maximum{~Wide{0}};
    Wide numerator{1}, denominator{1};
    for (const Wide factor : numerators) {
        if (numerator > maximum / factor) return reject(QStringLiteral("The inverse limit exceeds the bounded exact-arithmetic range."));
        numerator *= factor;
    }
    for (const Wide factor : denominators) {
        if (denominator > maximum / factor) return reject(QStringLiteral("The inverse limit exceeds the bounded exact-arithmetic range."));
        denominator *= factor;
    }
    Wide ticks{numerator / denominator};
    const bool adjusted{numerator % denominator != 0};
    // BUY B3 is a canonical ask: ceil raises minimum B3 received per token,
    // so the executable reciprocal cannot exceed the displayed maximum.
    // SELL B3 is a canonical bid: floor lowers maximum B3 spent per token,
    // so the executable reciprocal cannot fall below the displayed minimum.
    if (display_buy && adjusted) {
        if (ticks == maximum) return reject(QStringLiteral("The inverse limit exceeds the supported market range."));
        ++ticks;
    }
    if (ticks == 0 || ticks > static_cast<uint64_t>(maximum_price / units.price_step)) {
        return reject(QStringLiteral("No executable canonical tick satisfies this limit within the supported market range."));
    }
    const CAmount price{static_cast<CAmount>(ticks * static_cast<uint64_t>(units.price_step))};
    return DisplayLimit{price, adjusted, ExactInversePrice(price, units.decimals)};
}
QString CanonicalSide(bool buy, bool inverse) { return buy != inverse ? QStringLiteral("bid") : QStringLiteral("ask"); }
bool DisplayBuy(const QString& side, bool inverse)
{
    if (side != QStringLiteral("bid") && side != QStringLiteral("ask")) Fail("Invalid order side.");
    return (side == QStringLiteral("bid")) != inverse;
}
std::optional<CAmount> ReplacementBudget(CAmount available, CAmount reserved)
{
    if (!MoneyRange(available) || !MoneyRange(reserved) || available > MAX_MONEY - reserved) return std::nullopt;
    return available + reserved;
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
    // Older local RPCs omit this additive status field; their paused/error
    // fields still fail closed. Never infer reconciliation from error text.
    if (v.exists("chain_reconciling")) s.chain_reconciling = Flag(F(v, "chain_reconciling"));
    s.halt = Text(F(v, "halt")); s.error = Text(F(v, "error")); s.active_seats = Count(F(v, "active_seats")); s.quorum_required = Count(F(v, "quorum_required")); s.pending_actions = Count(F(v, "pending_actions"));
    if (!F(value, "unchanged").isNull()) s.unchanged = Flag(F(value, "unchanged"));
    const auto& verification{F(value, "verification")};
    if (!verification.isNull()) {
        const auto source{Text(F(verification, "source"))};
        if (source != QStringLiteral("remote_endpoint") && source != QStringLiteral("local_engine")) Fail("Unsupported market verification source.");
        s.remote = source == QStringLiteral("remote_endpoint");
        s.endpoint = Text(F(verification, "endpoint"));
        s.certificate_verified = Flag(F(verification, "certificate_verified"));
        s.account_state_verified = Flag(F(verification, "account_state_verified"));
        s.execution_result_verified = Flag(F(verification, "execution_result_verified"));
        s.b3_checkpoint_confirmed = Flag(F(verification, "b3_checkpoint_confirmed"));
        s.event_gap = Flag(F(verification, "event_gap"));
        if (s.remote && (s.endpoint.isEmpty() || !s.certificate_verified || !s.account_state_verified)) Fail("Remote market data lacks locally verified certificate and whole-state evidence.");
    }
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
    return response_age >= 0 && response_age <= 3000 && s.certified && (!s.remote || (s.certificate_verified && s.account_state_verified && !s.endpoint.isEmpty())) && s.running && !s.chain_reconciling && !s.paused && !s.handoff && s.halt == QStringLiteral("none") && s.error.isEmpty() && s.active_seats >= 4 && s.active_seats <= 1'000'000 && s.quorum_required == (s.active_seats * 2) / 3 + 1;
}
QString StatusText(const Snapshot& s, int64_t response_age, int64_t certificate_age, int64_t queued_without_progress)
{
    if (response_age < 0 || response_age > 3000) return QStringLiteral("Connection stale · last certified data retained; new orders are disabled.");
    if (!s.running) return QStringLiteral("Market service offline · waiting for the node.");
    if (s.chain_reconciling && s.halt == QStringLiteral("none")) return QStringLiteral("Reconciling B3 tip · retrying; certified data retained and new actions disabled.");
    if (s.halt != QStringLiteral("none") || !s.error.isEmpty()) return QStringLiteral("Market halted · %1").arg(!s.error.isEmpty() ? s.error : s.halt);
    if (s.handoff) return QStringLiteral("Validator handoff · new orders paused; certified settlement remains available.");
    if (s.paused) return QStringLiteral("Market paused · quorum or checkpoint progress required.");
    if (!s.certified) return QStringLiteral("Awaiting first certified microblock · no price or fills yet.");
    if (s.remote && (!s.certificate_verified || !s.account_state_verified)) return QStringLiteral("Remote evidence not verified · new actions disabled.");
    if (s.pending_actions > 0 && queued_without_progress >= 30'000) return QStringLiteral("Queued requests have not certified for 30 seconds · new submissions paused locally. Existing requests are not canceled.");
    if (s.remote) return QStringLiteral("Remote client · certificate and whole-state verified locally; newest network head is not proven. %1%2")
        .arg(s.b3_checkpoint_confirmed ? QStringLiteral("Head checkpoint confirmed on B3; this is not proof of a payout.") : QStringLiteral("Quorum-attested head, not B3-checkpoint confirmed."),
             s.event_gap ? QStringLiteral(" Event history has a gap; missing action outcomes remain unknown.") : QString{});
    if (certificate_age < 0 || certificate_age > 10000) return QStringLiteral("Certified state · awaiting another auction; current quorum is not independently proven.");
    return QStringLiteral("Certified updates observed · runtime status alone is not proof of current quorum.");
}
} // namespace B3FlowMeshMarketData

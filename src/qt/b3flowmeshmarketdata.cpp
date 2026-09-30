// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshmarketdata.h>
#include <qt/b3assettransfer.h>
#include <flowmesh/clearing.h>
#include <flowmesh/market.h>
#include <flowmesh/market_view.h>
#include <flowmesh/microblock.h>
#include <util/strencodings.h>
#include <util/int128.h>
#include <QObject>
#include <QStringList>
#include <algorithm>
#include <limits>
#include <map>
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
//! The exact decimal when it ends within 18 places, else the reduced
//! fraction: both parse back to the same canonical tick.
QString ExactRatioText(const Ratio& ratio)
{
    QString result{WideText(ratio.numerator / ratio.denominator)};
    Wide remainder{ratio.numerator % ratio.denominator};
    if (remainder == 0) return result;
    result += QLatin1Char('.');
    for (int i{0}; i < 18 && remainder != 0; ++i) {
        remainder *= 10;
        result += QChar{static_cast<ushort>('0' + static_cast<unsigned>(remainder / ratio.denominator))};
        remainder %= ratio.denominator;
    }
    if (remainder == 0) return result;
    return WideText(ratio.numerator) + QLatin1Char('/') + WideText(ratio.denominator);
}
struct Rounded {
    QString text;
    bool exact{true};
    //! The decimal place it rounded at, before trailing zeros were trimmed.
    int places{0};
};
//! Display rounding of a positive ratio, half up: to `significant` digits,
//! never into the integer part, and to at most `max_places` decimal places.
//! Trailing zeros are trimmed. A denominator whose remainder cannot be
//! multiplied by ten is not rounded (nullopt): callers show the exact form.
std::optional<Rounded> RoundRatio(const Ratio& ratio, int significant, int max_places)
{
    if (ratio.denominator == 0 || ratio.denominator > ~Wide{0} / 10) return std::nullopt;
    Rounded out;
    Wide integer{ratio.numerator / ratio.denominator}, remainder{ratio.numerator % ratio.denominator};
    int digits{integer == 0 ? 0 : static_cast<int>(WideText(integer).size())};
    QString fraction;
    while (out.places < max_places && digits < significant) {
        remainder *= 10;
        const unsigned digit{static_cast<unsigned>(remainder / ratio.denominator)};
        remainder %= ratio.denominator;
        fraction += QChar{static_cast<ushort>('0' + digit)};
        ++out.places;
        if (digits > 0 || digit != 0) ++digits;
    }
    out.exact = remainder == 0;
    if (remainder >= ratio.denominator - remainder && remainder != 0) {
        qsizetype i{fraction.size()};
        for (; i > 0 && fraction.at(i - 1) == QLatin1Char('9'); --i) fraction[i - 1] = QLatin1Char('0');
        if (i > 0) fraction[i - 1] = QChar{static_cast<ushort>(fraction.at(i - 1).unicode() + 1)};
        else ++integer;
    }
    while (fraction.endsWith(QLatin1Char('0'))) fraction.chop(1);
    out.text = WideText(integer) + (fraction.isEmpty() ? QString{} : QLatin1Char('.') + fraction);
    return out;
}
//! Adjacent canonical ticks p and p + step have reciprocals about 1 / (p /
//! step) apart, relatively: one more digit than p / step has tells them apart.
int InverseSignificantDigits(CAmount raw, CAmount step)
{
    const bool known{step > 0};
    const uint64_t ticks{std::max<uint64_t>(static_cast<uint64_t>(raw) / static_cast<uint64_t>(known ? step : 1), 1)};
    const int digits{static_cast<int>(QString::number(ticks).size()) + 1};
    return std::min(digits, known ? MAX_PRICE_SIGNIFICANT_DIGITS : FALLBACK_PRICE_SIGNIFICANT_DIGITS);
}
std::optional<Rounded> RoundInverse(CAmount raw, int decimals, CAmount step)
{
    const auto ratio{InverseRatio(raw, decimals)};
    if (!ratio) return std::nullopt;
    return RoundRatio(*ratio, InverseSignificantDigits(raw, step), 18);
}
// Only formatting needs this wider-than-128-bit denominator. Each digit's
// product/carry is bounded by ten times a uint64_t; no economic value is cast.
QString MultiplyDecimal(const QString& value, uint64_t factor)
{
    QString result;
    Wide carry{0};
    for (qsizetype i{value.size()}; i > 0;) {
        carry += Wide{static_cast<unsigned>(value.at(--i).unicode() - '0')} * factor;
        result.prepend(QChar{static_cast<ushort>('0' + static_cast<unsigned>(carry % 10))});
        carry /= 10;
    }
    while (carry != 0) {
        result.prepend(QChar{static_cast<ushort>('0' + static_cast<unsigned>(carry % 10))});
        carry /= 10;
    }
    return result;
}
//! Empty `exact` when there is no spread to show.
PriceText LimitSpread(CAmount bid, CAmount ask, int decimals, CAmount step, bool inverse)
{
    if (ask < bid || (inverse && (bid == 0 || ask == 0))) return {};
    if (!inverse) return DisplayPrice(ask - bid, decimals, false, step);
    if (bid == ask) return {QStringLiteral("0"), QStringLiteral("0"), false};
    // 10^9 * (ask - bid) / (10^decimals * bid * ask). The raw-price
    // product fits 128 bits, but multiplication by token precision may not.
    Wide numerator{Wide{KILO_COIN} * static_cast<uint64_t>(ask - bid)};
    Wide denominator{Wide{static_cast<uint64_t>(bid)} * static_cast<uint64_t>(ask)};
    Wide scale{Pow10(decimals)};
    for (auto* factor : {&denominator, &scale}) {
        const Wide divisor{Gcd(numerator, *factor)};
        numerator /= divisor;
        *factor /= divisor;
    }
    const Wide maximum{~Wide{0}};
    // The long division multiplies a remainder by ten. Beyond that bound an
    // exact reduced fraction avoids both overflow and approximation.
    if (denominator > (maximum / 10) / scale) {
        const QString exact{WideText(numerator) + QLatin1Char('/') + MultiplyDecimal(WideText(denominator), static_cast<uint64_t>(scale))};
        return {exact, exact, false};
    }
    const Ratio spread{numerator, denominator * scale};
    PriceText out;
    out.exact = out.text = ExactRatioText(spread);
    // Display only: as many decimal places as the two best prices show, so
    // the spread reads against them. A spread finer than that stays exact.
    const auto bid_shown{RoundInverse(bid, decimals, step)}, ask_shown{RoundInverse(ask, decimals, step)};
    if (!bid_shown || !ask_shown) return out;
    const auto rounded{RoundRatio(spread, std::numeric_limits<int>::max(), std::max(bid_shown->places, ask_shown->places))};
    if (rounded && !rounded->exact && rounded->text != QStringLiteral("0")) {
        out.text = QStringLiteral("≈") + rounded->text;
        out.approximate = true;
    }
    return out;
}
enum class Rounding { DOWN, UP, NEAREST };
//! A canonical price in the displayed orientation times 10^places, rounded
//! in `rounding`'s direction (NEAREST: half up); `whole` tells whether
//! nothing was rounded. nullopt for an inverse zero, invalid arguments, and
//! B3 per token of an asset with more than nine decimals (a whole number:
//! callers format it exactly).
std::optional<Wide> ScaledPrice(const CAmount raw, const int decimals, const bool inverse, const int places, const Rounding rounding, bool* whole = nullptr)
{
    if (raw < 0 || !MoneyRange(raw) || decimals < 0 || decimals > 18 || places < 0 || places > 18) return std::nullopt;
    std::optional<Ratio> ratio;
    if (inverse) ratio = InverseRatio(raw, decimals);
    else if (decimals <= 9) ratio = Ratio{Wide{static_cast<uint64_t>(raw)}, Pow10(9 - decimals)};
    if (!ratio) return std::nullopt;
    // Bounded: a numerator below 2^63 (or 10^9 inverted) times 10^18 fits.
    const Wide scaled{ratio->numerator * Pow10(places)};
    Wide units{scaled / ratio->denominator};
    const Wide remainder{scaled % ratio->denominator};
    if (whole) *whole = remainder == 0;
    if (remainder != 0 && (rounding == Rounding::UP || (rounding == Rounding::NEAREST && remainder >= ratio->denominator - remainder))) ++units;
    return units;
}
//! `units` x 10^-places with exactly `places` decimal places.
QString ScaledText(const Wide units, const int places)
{
    QString text{WideText(units)};
    if (places <= 0) return text;
    text = text.rightJustified(places + 1, QLatin1Char('0'));
    text.insert(text.size() - places, QLatin1Char('.'));
    return text;
}
QString RoundedPrice(const CAmount raw, const int decimals, const bool inverse, const int places, const Rounding rounding, bool* exact)
{
    if (raw < 0 || !MoneyRange(raw) || decimals < 0 || decimals > 18 || places < 0 || places > 18) return {};
    if (!inverse && decimals > 9) {
        // A whole number of B3 per token: only zeros follow the point.
        if (exact) *exact = true;
        return PadDecimals(FormatPrice(raw, decimals), places);
    }
    const auto units{ScaledPrice(raw, decimals, inverse, places, rounding, exact)};
    return units ? ScaledText(*units, places) : QString{};
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
TickPage Ticks(const UniValue& v, const Snapshot& s)
{
    TickPage page;
    try {
        page.available = Flag(F(v, "available"));
        const auto source{Text(F(v, "source"))};
        if (source != QStringLiteral("local_engine") && source != QStringLiteral("endpoint_reported")) Fail("Unknown tick source.");
        page.remote = source == QStringLiteral("endpoint_reported");
        if (!page.available) {
            page.reason = Text(F(v, "unavailable_reason")).left(160);
            return page;
        }
        page.gap = Flag(F(v, "gap")); page.more = Flag(F(v, "more"));
        const auto& rows{F(v, "ticks")};
        if (!rows.isArray() || rows.size() > MAX_HELD_TICKS) Fail("Oversized tick page.");
        for (const auto& row : rows.getValues()) {
            Tick t; t.sequence = Count(F(row, "sequence")); t.hash = Id(F(row, "microblock_hash"));
            t.kind = Text(F(row, "kind")); t.side = Text(F(row, "imbalance_side")); t.cleared = Flag(F(row, "cleared"));
            t.price = Amount(F(row, "price")); t.volume = Amount(F(row, "volume")); t.imbalance = Amount(F(row, "imbalance"));
            if (t.kind != QStringLiteral("execution") && t.kind != QStringLiteral("settlement") && t.kind != QStringLiteral("handoff")) Fail("Unknown tick kind.");
            if (t.side != QStringLiteral("none") && t.side != QStringLiteral("bid") && t.side != QStringLiteral("ask") && t.side != QStringLiteral("unknown")) Fail("Unknown tick side.");
            if (t.cleared ? t.kind != QStringLiteral("execution") || t.volume == 0 : t.price != 0 || t.volume != 0 || t.imbalance != 0 || t.side != QStringLiteral("none")) Fail("Inconsistent tick.");
            if (t.sequence >= s.next_sequence || (!page.ticks.empty() && t.sequence != page.ticks.back().sequence + 1)) Fail("Ticks are not consecutive within the snapshot.");
            page.ticks.push_back(std::move(t));
        }
    } catch (const std::exception&) {
        page = {};
        page.reason = QStringLiteral("malformed");
    }
    return page;
}
uint8_t ObservedFlags(const UniValue& v)
{
    if (!v.isArray() || v.size() > 16) Fail("Invalid observed-time flags.");
    uint8_t out{0};
    for (const auto& flag : v.getValues()) {
        const auto name{Text(flag)};
        if (name == QStringLiteral("clock_clamped")) out |= flowmesh::OBSERVED_CLOCK_CLAMPED;
        else if (name == QStringLiteral("clock_step")) out |= flowmesh::OBSERVED_CLOCK_STEP;
        else if (name == QStringLiteral("clock_suspect")) out |= flowmesh::OBSERVED_CLOCK_SUSPECT;
        else if (name == QStringLiteral("restart")) out |= flowmesh::OBSERVED_RESTART;
        else if (name == QStringLiteral("no_clearing_data")) out |= flowmesh::OBSERVED_NO_CLEARING;
        else if (name == QStringLiteral("implausible")) out |= flowmesh::OBSERVED_IMPLAUSIBLE;
    }
    return out;
}
int64_t Millis(const UniValue& v)
{
    const auto n{Count(v)};
    if (n > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) Fail("Invalid observed time.");
    return static_cast<int64_t>(n);
}
ObservedTimes ObservedOf(const UniValue& v, const Snapshot& s)
{
    ObservedTimes out;
    try {
        if (Text(F(v, "source_kind")) != QStringLiteral("operator_observed") || Flag(F(v, "certified"))) Fail("Observed time is never certified.");
        out.available = Flag(F(v, "available"));
        out.source = Text(F(v, "source")).left(256);
        if (!out.available) {
            out.reason = Text(F(v, "unavailable_reason")).left(160);
            return out;
        }
        out.clock_suspect = Flag(F(v, "clock_suspect"));
        const auto& rows{F(v, "rows")};
        if (!rows.isArray() || rows.size() > MAX_HELD_TICKS + 101) Fail("Oversized observed-time page.");
        for (const auto& row : rows.getValues()) {
            flowmesh::ObservedStamp stamp;
            stamp.sequence = Count(F(row, "sequence"));
            stamp.hash = *uint256::FromHex(Id(F(row, "microblock_hash")).toStdString());
            if (stamp.sequence >= s.next_sequence || (!out.rows.empty() && stamp.sequence <= out.rows.back().sequence)) Fail("Observed times out of order.");
            const auto mode{Text(F(row, "mode"))};
            if (mode == QStringLiteral("live")) { stamp.mode = flowmesh::ObservedMode::LIVE; stamp.ms = Millis(F(row, "observed_ms")); }
            else if (mode == QStringLiteral("delayed")) { stamp.mode = flowmesh::ObservedMode::DELAYED; stamp.ms = Millis(F(row, "applied_here_ms")); }
            else if (mode == QStringLiteral("untimed")) stamp.mode = flowmesh::ObservedMode::UNTIMED;
            else Fail("Unknown observed-time mode.");
            stamp.flags = ObservedFlags(F(row, "flags"));
            out.rows.push_back(stamp);
        }
    } catch (const std::exception&) {
        out = {};
        out.reason = QStringLiteral("malformed");
    }
    return out;
}
Indicative IndicativeOf(const UniValue& v, const Snapshot& s)
{
    Indicative out;
    try {
        if (Flag(F(v, "certified"))) Fail("An indicative clearing is never certified.");
        out.available = Flag(F(v, "available"));
        out.head = Id(F(v, "head"), true);
        const auto& draft{F(v, "draft")};
        if (!draft.isNull()) {
            out.has_draft = true; out.draft_side = Text(F(draft, "side"));
            if (out.draft_side != QStringLiteral("bid") && out.draft_side != QStringLiteral("ask")) Fail("Unknown draft side.");
            out.draft_price = Amount(F(draft, "price")); out.draft_quantity = Amount(F(draft, "quantity"));
            out.replaces = Flag(F(draft, "replaces_standing_curve"));
        }
        if (!out.available) {
            out.reason = Text(F(v, "unavailable_reason")).left(160);
            return out;
        }
        // A view of another head than this snapshot's is not shown.
        if (out.head != s.head) Fail("Indicative clearing of another head.");
        out.cleared = Flag(F(v, "cleared")); out.price = Amount(F(v, "price")); out.volume = Amount(F(v, "volume"));
        out.imbalance = Amount(F(v, "imbalance")); out.side = Text(F(v, "imbalance_side"));
        if (out.side != QStringLiteral("none") && out.side != QStringLiteral("bid") && out.side != QStringLiteral("ask")) Fail("Unknown indicative side.");
        if (!out.cleared && (out.price != 0 || out.volume != 0)) Fail("Inconsistent indicative clearing.");
        for (const auto& [key, field] : {std::pair{"best_bid", &out.best_bid}, std::pair{"best_ask", &out.best_ask},
                                         std::pair{"draft_fill_min", &out.fill_min}, std::pair{"draft_fill_max", &out.fill_max}}) {
            if (!F(v, key).isNull()) *field = Amount(F(v, key));
        }
        if (out.fill_min.has_value() != out.fill_max.has_value() || (out.fill_min && (!out.has_draft || *out.fill_min > *out.fill_max ||
            *out.fill_max > out.volume || *out.fill_max > out.draft_quantity))) Fail("Inconsistent indicative fill.");
    } catch (const std::exception&) {
        const bool draft{out.has_draft};
        const auto side{out.draft_side};
        const auto price{out.draft_price}, quantity{out.draft_quantity};
        out = {};
        out.reason = QStringLiteral("malformed");
        out.has_draft = draft; out.draft_side = side; out.draft_price = price; out.draft_quantity = quantity;
    }
    return out;
}
flowmesh::LevelGrouping GroupingOf(const UniValue& v)
{
    flowmesh::LevelGrouping out;
    out.exact = Flag(F(v, "exact"));
    if (out.exact) return out;
    const auto orientation{Text(F(v, "orientation"))};
    if (orientation != QStringLiteral("canonical") && orientation != QStringLiteral("inverse")) Fail("Unknown level grouping orientation.");
    out.inverse = orientation == QStringLiteral("inverse");
    const auto mantissa{Count(F(v, "mantissa"))}, decimals{Count(F(v, "decimals"))};
    const auto& exponent{F(v, "exponent")};
    if (!exponent.isNum() || mantissa > 5 || decimals > 18) Fail("Invalid level grouping.");
    const auto power{exponent.getInt<int64_t>()};
    if (power < -64 || power > 64) Fail("Invalid level grouping exponent.");
    out.mantissa = static_cast<uint8_t>(mantissa); out.exponent = static_cast<int32_t>(power); out.decimals = static_cast<uint8_t>(decimals);
    if (!flowmesh::LevelGroupingError(out).empty()) Fail("Invalid level grouping.");
    return out;
}
BookLevelsSide LevelsSideOf(const UniValue& v, const bool bid, const BookLevels& levels)
{
    BookLevelsSide side;
    side.available = Flag(F(v, "available")); side.more = Flag(F(v, "more"));
    if (!F(v, "truncated_reason").isNull()) side.truncated = Text(F(v, "truncated_reason")).left(64);
    const auto& rows{F(v, "levels")};
    if (!rows.isArray() || rows.size() > levels.depth) Fail("Oversized book levels.");
    if (!side.available) {
        side.reason = Text(F(v, "unavailable_reason")).left(64);
        if (!rows.empty()) Fail("Levels of an unavailable side.");
        return side;
    }
    Wide running{0};
    for (const auto& row : rows.getValues()) {
        BookLevel level;
        const auto bucket{Text(F(row, "bucket"))};
        const auto index{bucket == QStringLiteral("0") ? std::optional<Wide>{0} : PositiveInteger(bucket)};
        if (!index) Fail("Invalid book level bucket.");
        level.bucket = *index; level.zero_price = Flag(F(row, "zero_price"));
        level.low = Amount(F(row, "low")); level.high = Amount(F(row, "high"));
        level.size = Amount(F(row, "size")); level.cumulative = Amount(F(row, "cumulative")); level.own = Amount(F(row, "own"));
        const auto curves{Count(F(row, "curves"))};
        if (curves == 0 || curves > flowmesh::BOOK_SNAPSHOT_MAX_CURVES || level.size == 0 || level.own > level.size) Fail("Invalid book level.");
        level.curves = static_cast<uint32_t>(curves);
        // Exactly its bucket's range, disjoint from and worse than the last.
        const auto range{flowmesh::LevelBucketRange(levels.grouping, bid, level.bucket, level.zero_price)};
        if (!range || range->first != level.low || range->second != level.high) Fail("Book level outside its bucket.");
        if (!side.levels.empty() && (bid ? level.high >= side.levels.back().low : level.low <= side.levels.back().high)) Fail("Book levels out of order.");
        running += static_cast<uint64_t>(level.size);
        if (running != static_cast<uint64_t>(level.cumulative)) Fail("Book level cumulative mismatch.");
        if (!F(row, "notional").isNull()) {
            const auto text{Text(F(row, "notional"))};
            const auto notional{text == QStringLiteral("0") ? std::optional<Wide>{0} : PositiveInteger(text)};
            // Every lot is priced within the level's range.
            if (!notional || *notional < Wide{static_cast<uint64_t>(level.size)} * static_cast<uint64_t>(level.low) ||
                *notional > Wide{static_cast<uint64_t>(level.size)} * static_cast<uint64_t>(level.high)) Fail("Book level notional outside its range.");
            level.notional = notional;
        }
        side.levels.push_back(std::move(level));
    }
    return side;
}
BookLevels BookLevelsOf(const UniValue& v, const Snapshot& s)
{
    BookLevels out;
    try {
        if (Flag(F(v, "certified")) || !Flag(F(v, "excludes_pending_actions")) || Text(F(v, "basis")) != QStringLiteral("verified_standing_curves")) {
            Fail("Book levels are never certified.");
        }
        out.available = Flag(F(v, "available"));
        out.head = Id(F(v, "head"), true);
        const auto depth{Count(F(v, "depth"))};
        if (depth == 0 || depth > flowmesh::MARKET_DATA_MAX_LEVELS) Fail("Invalid book level depth.");
        out.depth = depth;
        out.grouping = GroupingOf(F(v, "grouping"));
        if (!out.available) {
            out.reason = Text(F(v, "unavailable_reason")).left(160);
            return out;
        }
        // Levels of another head than this snapshot's are not shown.
        if (out.head != s.head) Fail("Book levels of another head.");
        const auto total{Count(F(v, "total_curves"))};
        if (total > flowmesh::BOOK_SNAPSHOT_MAX_CURVES) Fail("Invalid book level curve count.");
        out.total_curves = total;
        if (!F(v, "best_bid").isNull()) out.best_bid = Amount(F(v, "best_bid"));
        if (!F(v, "best_ask").isNull()) out.best_ask = Amount(F(v, "best_ask"));
        out.bids = LevelsSideOf(F(v, "bids"), true, out);
        out.asks = LevelsSideOf(F(v, "asks"), false, out);
        // The exact best price lies in the best level, and exists with it.
        for (const auto& [side, best] : {std::pair{&out.bids, out.best_bid}, std::pair{&out.asks, out.best_ask}}) {
            if (side->levels.empty()) continue;
            if (!best || *best < side->levels.front().low || *best > side->levels.front().high) Fail("Best price outside the best level.");
        }
        out.standing = IndicativeOf(F(v, "standing"), s);
        if (out.standing.has_draft) Fail("A standing clearing has no draft.");
    } catch (const std::exception&) {
        out = {};
        out.reason = QStringLiteral("malformed");
    }
    return out;
}
}

void MergeHeldTimes(std::map<uint64_t, flowmesh::ObservedStamp>& held, const std::deque<Tick>& ticks, const ObservedTimes* page)
{
    const auto tick_at = [&](const uint64_t sequence) -> const Tick* {
        if (ticks.empty() || sequence < ticks.front().sequence || sequence > ticks.back().sequence) return nullptr;
        return &ticks[sequence - ticks.front().sequence];
    };
    if (page && page->available) {
        for (const auto& row : page->rows) {
            const auto* tick{tick_at(row.sequence)};
            if (tick && tick->hash == QString::fromStdString(row.hash.GetHex())) held[row.sequence] = row;
        }
    }
    std::erase_if(held, [&](const auto& item) {
        const auto* tick{tick_at(item.first)};
        return !tick || tick->hash != QString::fromStdString(item.second.hash.GetHex());
    });
}

bool MergeSameHeadObserved(std::optional<ObservedTimes>& retained, std::optional<ObservedTimes> fresh)
{
    const bool same_section{retained && fresh && retained->available && fresh->available && retained->source == fresh->source &&
                            retained->clock_suspect == fresh->clock_suspect && retained->reason == fresh->reason};
    if (!same_section) {
        const bool changed{retained != fresh};
        retained = std::move(fresh);
        return changed;
    }
    bool changed{false};
    auto& rows{retained->rows};
    for (auto& row : fresh->rows) {
        const auto at{std::lower_bound(rows.begin(), rows.end(), row.sequence, [](const auto& held, uint64_t sequence) { return held.sequence < sequence; })};
        if (at != rows.end() && at->sequence == row.sequence) {
            if (*at == row) continue;
            *at = std::move(row);
        } else {
            rows.insert(at, std::move(row));
        }
        changed = true;
    }
    return changed;
}

CandlePage ParseCandles(const UniValue& value, const flowmesh::CandleInterval requested)
{
    CandlePage page;
    page.interval = requested;
    try {
        if (Text(F(value, "source_kind")) != QStringLiteral("operator_observed") || Flag(F(value, "certified"))) Fail("Candles are never certified.");
        page.available = Flag(F(value, "available"));
        page.source = Text(F(value, "source")).left(256);
        if (!F(value, "endpoint_reported").isNull()) page.remote = Flag(F(value, "endpoint_reported"));
        if (!page.available) {
            page.reason = Text(F(value, "unavailable_reason")).left(160);
            return page;
        }
        if (Text(F(value, "interval")) != QString::fromLatin1(flowmesh::CandleIntervalName(requested))) Fail("Another candle interval.");
        const int64_t step{flowmesh::CandleIntervalMs(requested)};
        if (Millis(F(value, "interval_ms")) != step) Fail("Another candle interval length.");
        page.from_ms = Millis(F(value, "from_ms")); page.before_ms = Millis(F(value, "before_ms"));
        if (page.before_ms < page.from_ms || (page.before_ms - page.from_ms) % step != 0 || page.before_ms - page.from_ms > 500 * step) Fail("Invalid candle window.");
        page.more = Flag(F(value, "more"));
        const auto& rows{F(value, "candles")};
        if (!rows.isArray() || rows.size() > 500) Fail("Oversized candle page.");
        for (const auto& row : rows.getValues()) {
            flowmesh::ObservedCandle c;
            c.open_ms = Millis(F(row, "open_ms"));
            if (c.open_ms % step != 0 || c.open_ms < page.from_ms || c.open_ms >= page.before_ms ||
                (!page.candles.empty() && c.open_ms <= page.candles.back().open_ms)) Fail("Misaligned or unordered candle.");
            c.first_sequence = Count(F(row, "first_sequence")); c.last_sequence = Count(F(row, "last_sequence"));
            if (c.last_sequence < c.first_sequence || (!page.candles.empty() && c.first_sequence <= page.candles.back().last_sequence)) Fail("Overlapping candle sequences.");
            const auto entries{Count(F(row, "entries"))}, clearings{Count(F(row, "clearings"))};
            if (entries == 0 || clearings > entries || entries - 1 > c.last_sequence - c.first_sequence) Fail("Invalid candle counts.");
            c.entries = static_cast<uint32_t>(entries); c.clearings = static_cast<uint32_t>(clearings);
            // Exact decimal sums; zero only as "0" (a clearing at price zero).
            const auto wide = [](const QString& text) { return text == QStringLiteral("0") ? std::optional<Wide>{0} : PositiveInteger(text); };
            const auto base{wide(Text(F(row, "base_volume")))}, quote{wide(Text(F(row, "quote_volume")))};
            if (c.clearings > 0) {
                c.open = Amount(F(row, "open")); c.high = Amount(F(row, "high")); c.low = Amount(F(row, "low")); c.close = Amount(F(row, "close"));
                if (!base || !quote || *base == 0 || c.low > c.high || c.open < c.low || c.open > c.high || c.close < c.low || c.close > c.high) Fail("Inconsistent candle prices.");
                c.base_volume = *base; c.quote_volume = *quote;
            } else if (Text(F(row, "base_volume")) != QStringLiteral("0") || Text(F(row, "quote_volume")) != QStringLiteral("0") || !F(row, "open").isNull()) {
                Fail("Candle values without a clearing.");
            }
            const auto& flags{F(row, "flags")};
            if (!flags.isArray() || flags.size() > 8) Fail("Invalid candle flags.");
            for (const auto& flag : flags.getValues()) {
                const auto name{Text(flag)};
                if (name == QStringLiteral("partial")) c.flags |= flowmesh::CANDLE_PARTIAL;
                else if (name == QStringLiteral("late")) c.flags |= flowmesh::CANDLE_LATE;
                else if (name == QStringLiteral("seam_before")) c.flags |= flowmesh::CANDLE_SEAM_BEFORE;
                else if (name == QStringLiteral("clock")) c.flags |= flowmesh::CANDLE_CLOCK;
            }
            page.candles.push_back(c);
        }
    } catch (const std::exception&) {
        const auto interval{page.interval};
        page = {};
        page.interval = interval;
        page.reason = QStringLiteral("malformed");
    }
    return page;
}

bool MergeHeldTicks(std::deque<Tick>& held, const TickPage& page)
{
    if (!page.available) return false;
    bool kept{!page.gap};
    for (const auto& tick : page.ticks) {
        if (!held.empty() && tick.sequence <= held.back().sequence && tick.sequence >= held.front().sequence) {
            if (held[tick.sequence - held.front().sequence] == tick) continue;
            held.clear();
            kept = false;
        } else if (!held.empty() && tick.sequence != held.back().sequence + 1) {
            held.clear();
            kept = false;
        }
        held.push_back(tick);
        if (held.size() > MAX_HELD_TICKS) held.pop_front();
    }
    return kept;
}

std::optional<flowmesh::ClearingTick> ToClearingTick(const Tick& tick)
{
    const auto hash{uint256::FromHex(tick.hash.toStdString())};
    if (!hash) return std::nullopt;
    flowmesh::ClearingTick out;
    out.sequence = tick.sequence;
    out.microblock_hash = *hash;
    out.kind = tick.kind == QStringLiteral("handoff") ? flowmesh::ClearingTick::Kind::HANDOFF
             : tick.kind == QStringLiteral("settlement") ? flowmesh::ClearingTick::Kind::SETTLEMENT
                                                         : flowmesh::ClearingTick::Kind::EXECUTION;
    out.cleared = tick.cleared;
    out.price = tick.price;
    out.volume = tick.volume;
    out.imbalance = tick.imbalance;
    return out;
}

std::vector<Trade> TicksAsTrades(const std::deque<Tick>& held)
{
    std::vector<Trade> out;
    for (const auto& tick : held) {
        Trade t; t.sequence = tick.sequence; t.hash = tick.hash; t.cleared = tick.cleared;
        t.price = tick.price; t.quantity = tick.volume;
        // Exact, as the engine's matched notional; an overflow is no candle.
        if (tick.cleared && (tick.volume == 0 || tick.price > MAX_MONEY / tick.volume)) continue;
        t.notional = tick.price * tick.volume;
        out.push_back(t);
    }
    return out;
}

QString ClearingTickText(const Tick& tick, const Units& units, bool inverse, bool endpoint_reported, QString* exact_prices, const QString& when)
{
    if (exact_prices) exact_prices->clear();
    QString text;
    const QString at{when.isEmpty() ? QString{} : QLatin1Char(' ') + when};
    if (tick.kind == QStringLiteral("handoff")) text = QObject::tr("Validator handoff%1 · no auction").arg(at);
    else if (tick.kind == QStringLiteral("settlement")) text = QObject::tr("Withdrawal settlement%1 · no auction").arg(at);
    else if (!tick.cleared) text = QObject::tr("Last auction%1 · no match").arg(at);
    else if (!ValidUnits(units)) text = QObject::tr("Last auction%1 · matched (asset precision unavailable)").arg(at);
    else {
        const QString price_units{inverse ? QObject::tr("%1 / B3").arg(units.ticker) : QObject::tr("B3 / %1").arg(units.ticker)};
        const auto price{DisplayPrice(tick.price, units, inverse)};
        if (exact_prices && price.approximate) *exact_prices = QObject::tr("Exact price: %1 %2").arg(price.exact, price_units);
        text = QObject::tr("Last auction%1 · %2 %3 at %4 %5").arg(at).arg(FormatAmount(tick.volume, units.decimals), units.ticker,
                                                                   inverse && tick.price == 0 ? QObject::tr("zero") : price.text, price_units);
        // In the B3 view a canonical bid (buying the token) sells B3.
        const bool bid{tick.side == QStringLiteral("bid")};
        if (tick.side == QStringLiteral("unknown")) text += QObject::tr(" · unfilled side not observed");
        else if (tick.side != QStringLiteral("none")) {
            const QString who{inverse ? (bid ? QObject::tr("B3 sellers") : QObject::tr("B3 buyers")) : (bid ? QObject::tr("buyers") : QObject::tr("sellers"))};
            text += QObject::tr(" · %1 left %2 %3 unfilled").arg(who, FormatAmount(tick.imbalance, units.decimals), units.ticker);
        }
    }
    if (endpoint_reported) text += QObject::tr(" · endpoint-reported");
    return text;
}

QString IndicativeText(const Indicative& view, const Units& units, bool inverse, QString* exact_prices)
{
    if (exact_prices) exact_prices->clear();
    if (!view.available) return view.reason.isEmpty() ? QString{} : QObject::tr("Indicative price unavailable (%1)").arg(view.reason);
    if (!ValidUnits(units)) return {};
    const QString price_units{inverse ? QObject::tr("%1 / B3").arg(units.ticker) : QObject::tr("B3 / %1").arg(units.ticker)};
    QStringList exact;
    const auto price = [&](CAmount raw) {
        if (inverse && raw == 0) return QObject::tr("zero");
        const auto shown{DisplayPrice(raw, units, inverse)};
        if (shown.approximate && !exact.contains(shown.exact)) exact << shown.exact;
        return shown.text;
    };
    const auto finish = [&](const QString& text) {
        if (exact_prices && !exact.isEmpty()) *exact_prices = QObject::tr("Exact prices: %1 %2").arg(exact.join(QStringLiteral(", ")), price_units);
        return text;
    };
    // In the B3 view the best bid (buying the token) is the lowest B3 price.
    QStringList touch;
    const auto& high{inverse ? view.best_ask : view.best_bid};
    const auto& low{inverse ? view.best_bid : view.best_ask};
    if (high) touch << QObject::tr("best bid %1").arg(price(*high));
    if (low) touch << QObject::tr("best ask %1").arg(price(*low));
    const QString book{touch.isEmpty() ? QString{} : QStringLiteral(" · ") + touch.join(QStringLiteral(" · "))};
    const QString label{QObject::tr("Indicative, not certified")};
    if (!view.has_draft) {
        if (!view.cleared) return finish(label + QObject::tr(": no cross") + book);
        return finish(label + QObject::tr(": standing orders would clear at %1 %2 for %3 %4").arg(price(view.price), price_units, FormatAmount(view.volume, units.decimals), units.ticker));
    }
    if (!view.cleared || !view.fill_max || *view.fill_max == 0) {
        return finish(label + QObject::tr(": this order would not match now") + book + QObject::tr(" · other pending orders excluded"));
    }
    const QString fill{*view.fill_min == *view.fill_max ? FormatAmount(*view.fill_max, units.decimals)
                                                        : QObject::tr("%1 to %2").arg(FormatAmount(*view.fill_min, units.decimals), FormatAmount(*view.fill_max, units.decimals))};
    return finish(label + QObject::tr(": would clear at %1 %2, filling %3 of %4 %5 if submitted now (other pending orders excluded%6)")
        .arg(price(view.price), price_units, fill, FormatAmount(view.draft_quantity, units.decimals), units.ticker,
             view.replaces ? QObject::tr("; replaces your standing order on this side") : QString{}));
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
PriceText DisplayPrice(CAmount raw, int decimals, bool inverse, CAmount step)
{
    PriceText out;
    // B3 per token: a whole number of B3 atoms per token atom always ends
    // within the asset precision, so it is shown exactly.
    out.exact = out.text = inverse ? ExactInversePrice(raw, decimals) : FormatPrice(raw, decimals);
    if (!inverse) return out;
    const auto rounded{RoundInverse(raw, decimals, step)};
    // Too small for 18 places, or not roundable: the exact form, never "≈0".
    if (!rounded || rounded->exact || rounded->text == QStringLiteral("0")) return out;
    out.text = QStringLiteral("≈") + rounded->text;
    out.approximate = true;
    return out;
}
PriceText DisplayPrice(CAmount raw, const Units& units, bool inverse)
{
    return DisplayPrice(raw, units.decimals, inverse, ValidUnits(units) ? units.price_step : 0);
}
QString FormatDisplayPrice(CAmount raw, int decimals, bool inverse) { return DisplayPrice(raw, decimals, inverse).text; }
QString FormatDisplayPrice(CAmount raw, const Units& units, bool inverse) { return DisplayPrice(raw, units, inverse).text; }
PriceText DisplaySpread(CAmount bid, CAmount ask, const Units& units, bool inverse)
{
    return LimitSpread(bid, ask, units.decimals, ValidUnits(units) ? units.price_step : 0, inverse);
}
QString ExactInversePrice(CAmount raw, int decimals)
{
    const auto ratio{InverseRatio(raw, decimals)};
    return ratio ? ExactRatioText(*ratio) : QStringLiteral("—");
}
QString FixedDisplayPrice(const CAmount raw, const int decimals, const bool inverse, const int places, const bool up, bool* exact)
{
    return RoundedPrice(raw, decimals, inverse, places, up ? Rounding::UP : Rounding::DOWN, exact);
}
QString NearestDisplayPrice(const CAmount raw, const int decimals, const bool inverse, const int places, bool* exact)
{
    return RoundedPrice(raw, decimals, inverse, places, Rounding::NEAREST, exact);
}
std::optional<int> ViewPricePlaces(const Units& units, const bool inverse, const std::vector<CAmount>& references)
{
    if (!ValidUnits(units) || units.price_step > MAX_MONEY) return std::nullopt;
    if (!inverse) {
        // Every on-grid price is exact at the tick's places.
        if (units.decimals > 9) return 0;
        int places{NeededDecimals(Wide{static_cast<uint64_t>(units.price_step)}, 9 - units.decimals)};
        for (const CAmount price : references) {
            if (price > 0 && MoneyRange(price)) places = std::max(places, NeededDecimals(Wide{static_cast<uint64_t>(price)}, 9 - units.decimals));
        }
        return places;
    }
    const CAmount step{units.price_step};
    // The fewest places at which each reference and its neighbouring ticks
    // have three different nearest labels, none of them zero. Checked for
    // all references at once: labels that differ at some places can meet at
    // more.
    const auto resolved = [&](const int places) {
        bool any{false};
        for (const CAmount price : references) {
            if (price <= 0 || !MoneyRange(price)) continue;
            any = true;
            const auto own{ScaledPrice(price, units.decimals, true, places, Rounding::NEAREST)};
            if (!own || *own == 0) return false;
            for (const CAmount neighbour : {price > step ? price - step : CAmount{0}, price <= MAX_MONEY - step ? price + step : CAmount{0}}) {
                if (neighbour <= 0) continue;
                const auto other{ScaledPrice(neighbour, units.decimals, true, places, Rounding::NEAREST)};
                if (!other || *other == *own) return false;
            }
        }
        return any;
    };
    for (int places{0}; places <= 18; ++places) {
        if (resolved(places)) return places;
    }
    return std::nullopt;
}
PriceText ViewPrice(const CAmount raw, const Units& units, const bool inverse, const std::optional<int> places)
{
    if (!places || !ValidUnits(units)) return DisplayPrice(raw, units, inverse);
    bool whole{true};
    const QString text{NearestDisplayPrice(raw, units.decimals, inverse, *places, &whole)};
    // An inverse zero, or a positive price too small for these places (never
    // "≈0"): the price's own significant digits, as DisplayPrice shows it.
    const bool zero{std::none_of(text.begin(), text.end(), [](const QChar c) { return c >= QLatin1Char('1') && c <= QLatin1Char('9'); })};
    if (text.isEmpty() || (!whole && zero)) return DisplayPrice(raw, units, inverse);
    PriceText out;
    out.exact = inverse ? ExactInversePrice(raw, units.decimals) : FormatPrice(raw, units.decimals);
    out.text = whole ? text : QStringLiteral("≈") + text;
    out.approximate = !whole;
    return out;
}
PriceText ViewSpread(const CAmount bid, const CAmount ask, const Units& units, const bool inverse, const std::optional<int> places)
{
    const auto exact{DisplaySpread(bid, ask, units, inverse)};
    if (!places || !ValidUnits(units) || exact.exact.isEmpty()) return exact;
    PriceText out;
    out.exact = exact.exact;
    if (!inverse) {
        // Both prices are exact: so is their difference, at the view's places.
        out.text = PadDecimals(FormatPrice(ask - bid, units.decimals), *places);
        return out;
    }
    // The difference of the two labels beside the spread. The B3 view's
    // higher price is the canonical best bid's.
    bool high_whole{true}, low_whole{true};
    const auto high{ScaledPrice(bid, units.decimals, true, *places, Rounding::NEAREST, &high_whole)};
    const auto low{ScaledPrice(ask, units.decimals, true, *places, Rounding::NEAREST, &low_whole)};
    // A label shown with its own digits instead (ViewPrice) is no column value.
    if (!high || !low || *high == 0 || *low == 0 || *high < *low) return exact;
    out.text = ScaledText(*high - *low, *places);
    if (!high_whole || !low_whole) {
        out.text.prepend(QStringLiteral("≈"));
        out.approximate = true;
    }
    return out;
}
QString RoundedDecimal(const QString& text, const int places)
{
    if (places < 0 || places > 18 || text.contains(QLatin1Char('/'))) return {};
    const auto ratio{PositiveRatio(text)};
    if (!ratio || ratio->numerator > ~Wide{0} / Pow10(places)) return {};
    const Wide scaled{ratio->numerator * Pow10(places)};
    Wide units{scaled / ratio->denominator};
    const Wide remainder{scaled % ratio->denominator};
    if (remainder != 0 && remainder >= ratio->denominator - remainder) ++units;
    return ScaledText(units, places);
}
QString PadDecimals(QString text, const int places)
{
    if (places <= 0) return text;
    const auto dot{text.indexOf(QLatin1Char('.'))};
    const int have{dot < 0 ? 0 : static_cast<int>(text.size() - dot - 1)};
    if (dot < 0) text += QLatin1Char('.');
    if (have < places) text += QString(places - have, QLatin1Char('0'));
    return text;
}
int NeededDecimals(Wide raw, const int decimals)
{
    int places{std::max(0, decimals)};
    while (places > 0 && raw % 10 == 0) { raw /= 10; --places; }
    return places;
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

LimitBook ProjectLimitBook(const Snapshot& snapshot, bool inverse)
{
    LimitBook out;
    out.units_known = ValidUnits(snapshot.units) && MoneyRange(snapshot.units.quantity_step) &&
        MoneyRange(snapshot.units.price_step) && !snapshot.base.isEmpty() && snapshot.units.asset == snapshot.base;
    // Parse() has the same page bound. A direct caller cannot turn an oversized
    // page into an apparently complete, silently truncated book.
    if (snapshot.curves.size() > 128) {
        out.invalid_curves = snapshot.curves.size();
        return out;
    }
    using AccountSide = std::pair<QString, QString>;
    std::map<AccountSide, size_t> accounts;
    for (const auto& curve : snapshot.curves) ++accounts[{curve.account.toLower(), curve.side}];
    struct Group { Wide remaining{0}, notional{0}; size_t curves{0}; };
    std::map<std::pair<bool, CAmount>, Group> groups;
    uint256 base; base.begin()[0] = 1;
    const flowmesh::ClearingEngine engine{base, modern::NativeAsset(), flowmesh::HARD_MAX_CURVE_POINTS};
    for (const auto& curve : snapshot.curves) {
        const QString account{curve.account.toLower()};
        const bool bid{curve.side == QStringLiteral("bid")};
        if ((!bid && curve.side != QStringLiteral("ask")) || account.size() != 64 ||
            !IsHex(account.toStdString()) || account == QString(64, QLatin1Char('0')) ||
            accounts[{account, curve.side}] != 1 || !MoneyRange(curve.filled) ||
            !MoneyRange(curve.remaining) || !MoneyRange(curve.reserved) ||
            curve.points.empty() || curve.points.size() > flowmesh::HARD_MAX_CURVE_POINTS) {
            ++out.invalid_curves;
            continue;
        }
        std::vector<flowmesh::ClearingEngine::Breakpoint> points;
        points.reserve(curve.points.size());
        for (const auto& point : curve.points) points.push_back({point.price, point.quantity});
        if (!engine.CurveIsValid(bid ? flowmesh::ClearingEngine::Side::BID : flowmesh::ClearingEngine::Side::ASK, points)) {
            ++out.invalid_curves;
            continue;
        }
        const CAmount maximum{bid ? curve.points.front().quantity : curve.points.back().quantity};
        if (curve.filled > maximum || curve.remaining != maximum - curve.filled) {
            ++out.invalid_curves;
            continue;
        }
        if (curve.remaining == 0) {
            ++out.exhausted_curves;
            continue;
        }
        // These are precisely the integer step shapes emitted by the native
        // limit and ladder helpers: every price is a limit order's. Sloped or
        // merely equivalent curves are not orders.
        const auto steps{LadderStepsOf(curve)};
        if (!steps) {
            ++out.general_curves;
            continue;
        }
        bool overflow{false}, zero{false};
        for (const auto& step : *steps) {
            overflow |= Wide{static_cast<uint64_t>(step.price)} * static_cast<uint64_t>(step.remaining) > static_cast<uint64_t>(MAX_MONEY);
            zero |= inverse && step.price == 0 && step.remaining > 0;
        }
        if (overflow) {
            ++out.invalid_curves;
            continue;
        }
        if (zero) {
            ++out.zero_inverse_curves;
            continue;
        }
        for (const auto& step : *steps) {
            if (step.remaining == 0) continue;
            auto& group{groups[{bid, step.price}]};
            group.remaining += static_cast<uint64_t>(step.remaining);
            group.notional += Wide{static_cast<uint64_t>(step.price)} * static_cast<uint64_t>(step.remaining);
            ++group.curves;
        }
    }
    for (const auto& [key, group] : groups) {
        // Omit the WHOLE group if aggregation exceeds the supported range;
        // displaying a prefix would make quantity depend on account ordering.
        if (group.remaining > static_cast<uint64_t>(MAX_MONEY) || group.notional > static_cast<uint64_t>(MAX_MONEY)) {
            out.invalid_curves += group.curves;
            continue;
        }
        const auto [bid, price]{key};
        LimitLevel level{price, static_cast<CAmount>(group.remaining), static_cast<CAmount>(group.notional), group.curves, {}, {}, {}};
        if (out.units_known) {
            level.price = inverse ? ExactInversePrice(price, snapshot.units.decimals) : FormatPrice(price, snapshot.units.decimals);
            level.amount = inverse ? FormatAmount(level.gross_notional, 9) : FormatAmount(level.remaining, snapshot.units.decimals);
            level.total = inverse ? FormatAmount(level.remaining, snapshot.units.decimals) : FormatAmount(level.gross_notional, 9);
        }
        if (bid) {
            if (!out.canonical_best_bid || price > *out.canonical_best_bid) out.canonical_best_bid = price;
        } else if (!out.canonical_best_ask || price < *out.canonical_best_ask) {
            out.canonical_best_ask = price;
        }
        (bid != inverse ? out.bids : out.asks).push_back(std::move(level));
        out.projected_curves += group.curves;
    }
    const auto descending = [inverse](const LimitLevel& a, const LimitLevel& b) {
        return inverse ? a.canonical_price < b.canonical_price : a.canonical_price > b.canonical_price;
    };
    std::sort(out.asks.begin(), out.asks.end(), descending);
    std::sort(out.bids.begin(), out.bids.end(), descending);
    out.complete = snapshot.curves_complete && out.general_curves == 0 && out.invalid_curves == 0 && out.zero_inverse_curves == 0;
    if (out.complete && out.units_known && out.canonical_best_bid && out.canonical_best_ask) {
        const auto spread{LimitSpread(*out.canonical_best_bid, *out.canonical_best_ask, snapshot.units.decimals, snapshot.units.price_step, inverse)};
        out.spread = spread.exact;
        out.spread_display = spread.text;
    }
    return out;
}

std::vector<flowmesh::LevelGrouping> BookGroupingChoices(const std::optional<CAmount> best_bid, const std::optional<CAmount> best_ask,
                                                         const Units& units, const bool inverse)
{
    std::vector<flowmesh::LevelGrouping> out{flowmesh::LevelGrouping{}};
    const auto reference{best_bid && *best_bid > 0 ? best_bid : best_ask};
    if (!ValidUnits(units) || !reference || *reference <= 0) return out;
    // floor(log10) of the displayed price: P x 10^(d - 9) B3 per token, or
    // 10^(9 - d) / P token per B3, from P's digit count alone.
    const QString digits{QString::number(*reference)};
    const bool power_of_ten{digits.count(QLatin1Char('0')) == digits.size() - 1 && digits.startsWith(QLatin1Char('1'))};
    const int magnitude{inverse ? 9 - units.decimals - static_cast<int>(digits.size()) + (power_of_ten ? 1 : 0)
                                : static_cast<int>(digits.size()) - 1 + units.decimals - 9};
    const auto add = [&](const uint8_t mantissa, const int exponent) {
        flowmesh::LevelGrouping grouping;
        grouping.exact = false; grouping.inverse = inverse; grouping.mantissa = mantissa;
        grouping.exponent = exponent; grouping.decimals = static_cast<uint8_t>(units.decimals);
        if (flowmesh::LevelGroupingError(grouping).empty()) out.push_back(grouping);
    };
    for (const uint8_t mantissa : {1, 2, 5}) add(mantissa, magnitude - 4);
    for (const int step : {3, 2, 1}) add(1, magnitude - step);
    return out;
}

QString GroupingLabel(const flowmesh::LevelGrouping& grouping)
{
    if (grouping.exact) return QObject::tr("Exact");
    const QString mantissa{QString::number(grouping.mantissa)};
    if (grouping.exponent >= 0) return mantissa + QString(grouping.exponent, QLatin1Char('0'));
    const int places{-grouping.exponent};
    QString text{mantissa.rightJustified(places + 1, QLatin1Char('0'))};
    text.insert(text.size() - places, QLatin1Char('.'));
    return text;
}

std::optional<std::vector<LadderStep>> LadderStepsOf(const QString& side, const std::vector<Point>& points, const CAmount filled)
{
    const bool bid{side == QStringLiteral("bid")};
    if ((!bid && side != QStringLiteral("ask")) || points.empty() || points.size() > flowmesh::HARD_MAX_CURVE_POINTS || filled < 0) return std::nullopt;
    std::vector<flowmesh::LadderStep> steps;
    if (!bid && points.front().quantity > 0) {
        if (points.front().price != 0) return std::nullopt;
        steps.push_back({0, points.front().quantity});
    }
    for (size_t i{1}; i < points.size(); ++i) {
        const auto& a{points[i - 1]};
        const auto& b{points[i]};
        if (a.quantity == b.quantity) continue;
        // Every change is a one-tick step.
        if (a.price >= MAX_MONEY || b.price != a.price + 1) return std::nullopt;
        if (bid ? b.quantity > a.quantity : b.quantity < a.quantity) return std::nullopt;
        steps.push_back(bid ? flowmesh::LadderStep{a.price, a.quantity - b.quantity} : flowmesh::LadderStep{b.price, b.quantity - a.quantity});
    }
    // Exactly the builder's canonical points, nothing merely equivalent.
    const auto rebuilt{bid ? flowmesh::MakeLadderBidCurve(steps) : flowmesh::MakeLadderAskCurve(steps)};
    if (!rebuilt || rebuilt->size() != points.size()) return std::nullopt;
    std::vector<flowmesh::ClearingEngine::Breakpoint> exact;
    for (size_t i{0}; i < points.size(); ++i) {
        if ((*rebuilt)[i].price != points[i].price || (*rebuilt)[i].qty != points[i].quantity) return std::nullopt;
        exact.push_back((*rebuilt)[i]);
    }
    const auto remaining = [&](const CAmount price) {
        return price < 0 ? CAmount{0} : std::max<CAmount>(0, flowmesh::ClearingEngine::EvaluateCurve(exact, price) - filled);
    };
    std::vector<LadderStep> out;
    std::sort(steps.begin(), steps.end(), [](const auto& a, const auto& b) { return a.price < b.price; });
    for (const auto& step : steps) {
        out.push_back({step.price, step.qty, bid ? remaining(step.price) - remaining(step.price + 1) : remaining(step.price) - remaining(step.price - 1)});
    }
    return out;
}

std::optional<std::vector<LadderStep>> LadderStepsOf(const Curve& curve)
{
    return LadderStepsOf(curve.side, curve.points, curve.filled);
}

std::optional<CAmount> LimitPriceOf(const Curve& curve)
{
    const auto& p{curve.points};
    if (curve.side == QStringLiteral("bid") && p.size() == 2) {
        if (p[0].price < MAX_MONEY && p[1].price == p[0].price + 1 && p[1].quantity == 0) return p[0].price;
    } else if (curve.side == QStringLiteral("ask") && p.size() == 1) {
        if (p[0].price == 0) return CAmount{0};
    } else if (curve.side == QStringLiteral("ask") && p.size() == 2) {
        if (p[1].price > 0 && p[0].price == p[1].price - 1 && p[0].quantity == 0) return p[1].price;
    }
    return std::nullopt;
}

std::optional<MarketableLimit> ComputeMarketableLimit(const Snapshot& s, bool display_buy, bool inverse, int bps,
                                                      std::optional<CAmount> quantity, QString* error)
{
    const auto fail = [&](const QString& text) -> std::optional<MarketableLimit> {
        if (error) *error = text;
        return std::nullopt;
    };
    if (bps < 1 || bps > MAX_SLIPPAGE_BPS) return fail(QStringLiteral("Maximum slippage must be from 0.01% to 10%."));
    if (!s.certified) return fail(QStringLiteral("A certified book is required; enter a limit price."));
    const bool bid{CanonicalSide(display_buy, inverse) == QStringLiteral("bid")};
    // Price only against other accounts: this account's own curves are neither
    // liquidity for it nor a reference price.
    Snapshot others{s};
    others.own_curves.clear();
    if (!s.account.isEmpty()) {
        others.curves.erase(std::remove_if(others.curves.begin(), others.curves.end(), [&](const Curve& c) {
            return c.account.compare(s.account, Qt::CaseInsensitive) == 0;
        }), others.curves.end());
    }
    const auto book{ProjectLimitBook(others, /*inverse=*/false)};
    if (!book.units_known) return fail(QStringLiteral("Verified asset precision is required; enter a limit price."));
    if (!book.complete) return fail(QStringLiteral("Only part of the certified book is visible, or it holds curves that are not simple limits; enter a limit price."));
    const auto reference{bid ? book.canonical_best_ask : book.canonical_best_bid};
    if (!reference) return fail(bid ? QStringLiteral("No certified sell orders to price against; enter a limit price.")
                                    : QStringLiteral("No certified buy orders to price against; enter a limit price."));
    if (*reference <= 0) return fail(bid ? QStringLiteral("The best sell order is a zero-price curve; enter a limit price.")
                                         : QStringLiteral("The best buy order has a zero price; enter a limit price."));
    // Never beyond the slippage bound: a bid floors, an ask ceils.
    const Wide scaled{Wide{static_cast<uint64_t>(*reference)} * static_cast<uint64_t>(bid ? 10'000 + bps : 10'000 - bps)};
    Wide limit{bid ? scaled / 10'000 : (scaled + 9'999) / 10'000};
    const uint64_t step{static_cast<uint64_t>(s.units.price_step)};
    limit = bid ? limit / step * step : (limit + step - 1) / step * step;
    if (bid && limit < static_cast<uint64_t>(*reference)) limit = static_cast<uint64_t>(*reference);
    if (!bid && limit > static_cast<uint64_t>(*reference)) limit = static_cast<uint64_t>(*reference);
    // A limit bid needs its (price + 1, 0) endpoint: the highest bid is
    // MAX_MONEY - 1, still at or above the reference or refused.
    const uint64_t highest{static_cast<uint64_t>(MAX_MONEY - (bid ? 1 : 0))};
    if (bid && limit > highest && highest >= static_cast<uint64_t>(*reference)) limit = highest;
    if (limit == 0 || limit > highest) return fail(QStringLiteral("The slippage limit is outside the supported price range; enter a limit price."));
    MarketableLimit out;
    out.reference = *reference; out.price = static_cast<CAmount>(limit); out.slippage_bps = bps;
    out.display = inverse ? ExactInversePrice(out.price, s.units.decimals) : FormatPrice(out.price, s.units.decimals);
    out.reference_display = inverse ? ExactInversePrice(out.reference, s.units.decimals) : FormatPrice(out.reference, s.units.decimals);
    // The ticket signs what its parser reads back: it must be exactly this.
    const auto parsed{ParseDisplayLimit(out.display, s.units, inverse, display_buy)};
    if (!parsed || parsed->price != out.price || parsed->adjusted) return fail(QStringLiteral("This limit cannot be entered exactly; enter a limit price."));
    Wide visible{0};
    for (const auto& level : bid ? book.asks : book.bids) {
        if (bid ? level.canonical_price <= out.price : level.canonical_price >= out.price) visible += static_cast<uint64_t>(level.remaining);
    }
    out.visible = visible > static_cast<uint64_t>(MAX_MONEY) ? MAX_MONEY : static_cast<CAmount>(visible);
    out.covers_quantity = quantity && *quantity > 0 && out.visible >= *quantity;
    // Never cross this account's own opposite order (any price of a ladder).
    for (const auto& own : s.own_curves) {
        if (own.side == (bid ? QStringLiteral("bid") : QStringLiteral("ask")) || own.remaining == 0) continue;
        const auto steps{LadderStepsOf(own)};
        if (!steps) return fail(QStringLiteral("Your own opposite order is not a limit or ladder order; cancel it or enter a limit price."));
        for (const auto& step : *steps) {
            if (step.remaining > 0 && (bid ? step.price <= out.price : step.price >= out.price)) {
                return fail(QStringLiteral("This limit would cross your own opposite order; cancel or edit it first, or enter a limit price."));
            }
        }
    }
    return out;
}

std::optional<int> ParseSlippageBps(const QString& text, QString* error)
{
    QString parse_error;
    const auto hundredths{B3AssetTransfer::ParseAmount(text.trimmed(), 2, &parse_error)};
    if (!hundredths || *hundredths < 1 || *hundredths > MAX_SLIPPAGE_BPS) {
        if (error) *error = QStringLiteral("Enter a maximum slippage from 0.01 to 10 percent.");
        return std::nullopt;
    }
    return static_cast<int>(*hundredths);
}

QString FormatSlippage(int bps)
{
    if (bps < 0) return QStringLiteral("—");
    return QStringLiteral("%1.%2").arg(bps / 100).arg(bps % 100, 2, 10, QLatin1Char('0'));
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
    // Optional display sections, read before the unchanged return and each
    // failing closed on its own.
    if (!F(value, "clearing_ticks").isNull()) s.ticks = Ticks(F(value, "clearing_ticks"), s);
    if (!F(value, "indicative").isNull()) s.indicative = IndicativeOf(F(value, "indicative"), s);
    if (!F(value, "observed_time").isNull()) s.observed = ObservedOf(F(value, "observed_time"), s);
    // Also on an unchanged reply that asked for them (after a grouping change).
    if (!F(value, "book_levels").isNull()) s.levels = BookLevelsOf(F(value, "book_levels"), s);
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
    if (!s.certified) return QStringLiteral("Awaiting the first certified auction · no price or fills yet.");
    if (s.remote && (!s.certificate_verified || !s.account_state_verified)) return QStringLiteral("Remote evidence not verified · new actions disabled.");
    if (s.pending_actions > 0 && queued_without_progress >= 30'000) return QStringLiteral("Queued requests have not certified for 30 seconds · new submissions paused locally. Existing requests are not canceled.");
    if (s.remote) return QStringLiteral("Remote client · certificate and whole-state verified locally; newest network head is not proven. %1%2")
        .arg(s.b3_checkpoint_confirmed ? QStringLiteral("Head checkpoint confirmed on B3; this is not proof of a payout.") : QStringLiteral("Quorum-attested head, not B3-checkpoint confirmed."),
             s.event_gap ? QStringLiteral(" Event history has a gap; missing action outcomes remain unknown.") : QString{});
    if (certificate_age < 0 || certificate_age > 10000) return QStringLiteral("Certified state · awaiting another auction; current quorum is not independently proven.");
    return QStringLiteral("Certified updates observed · runtime status alone is not proof of current quorum.");
}

LinkState ClassifyLink(const LinkInputs& in)
{
    if (!in.wallet) return LinkState::None;
    if (in.polling_ms < 0) return LinkState::Checking;
    if (!in.read_failed && in.response_age_ms >= 0 && in.response_age_ms <= LINK_FRESH_MS) return LinkState::Live;
    // A reply older than the current polling interval predates it: only the
    // time since polling resumed counts, never a hidden interval.
    const bool before_polling{in.response_age_ms < 0 || in.response_age_ms > in.polling_ms};
    const int64_t waited{before_polling ? in.polling_ms : in.response_age_ms};
    if (waited > LINK_STALE_MS) return LinkState::Stale;
    if (in.read_failed) return LinkState::Reconnecting;
    return before_polling && waited <= LINK_FRESH_MS ? LinkState::Checking : LinkState::Slow;
}
} // namespace B3FlowMeshMarketData

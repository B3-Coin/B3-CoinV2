// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHMARKETDATA_H
#define BITCOIN_QT_B3FLOWMESHMARKETDATA_H

#include <consensus/amount.h>
#include <flowmesh/market_data.h>
#include <QString>
#include <univalue.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace B3FlowMeshMarketData {
//! Verified token precision is required. No ticker-based decimal guesses.
struct Units {
    QString asset, ticker, name, source;
    int decimals{0};
    bool known{false}, test_only{false};
    CAmount quantity_step{1}, price_step{1};
    bool operator==(const Units&) const = default;
};
QString FormatAmount(CAmount raw, int decimals);
QString FormatPrice(CAmount raw_atoms_per_raw_unit, int base_decimals);
//! The one formatter for every FlowMesh price shown to a person. Display
//! only: order inputs, order reviews and signed actions keep the exact forms
//! (FormatPrice, ExactInversePrice, ParseDisplayLimit); `text` is never
//! parsed into an action. Orientation never changes canonical market data.
//!
//! `text` carries at most the market's meaningful precision, trailing zeros
//! trimmed. B3 per token is exact: a canonical price is a whole number of B3
//! atoms per token atom, so the asset precision bounds its decimals. Token
//! per B3 keeps the significant digits that tell a price from its
//! neighbouring canonical ticks (one more than the digits of price / price
//! step), at most MAX_PRICE_SIGNIFICANT_DIGITS, and at most
//! FALLBACK_PRICE_SIGNIFICANT_DIGITS when no verified price step is given.
//! It rounds half up, never into the integer part, and to at most 18
//! decimal places. A rounded value starts with "≈" and `exact` holds the
//! exact decimal or reduced fraction for a tooltip; otherwise both match.
inline constexpr int MAX_PRICE_SIGNIFICANT_DIGITS{8}, FALLBACK_PRICE_SIGNIFICANT_DIGITS{6};
struct PriceText {
    QString text, exact;
    bool approximate{false};
    bool operator==(const PriceText&) const = default;
};
//! price_step <= 0: no verified step (FALLBACK_PRICE_SIGNIFICANT_DIGITS).
PriceText DisplayPrice(CAmount raw_price, int base_decimals, bool inverse, CAmount price_step = 0);
//! Uses the verified price step of valid units.
PriceText DisplayPrice(CAmount raw_price, const Units& units, bool inverse);
//! DisplayPrice(...).text.
QString FormatDisplayPrice(CAmount raw_price, int base_decimals, bool inverse = false);
QString FormatDisplayPrice(CAmount raw_price, const Units& units, bool inverse);
QString ExactInversePrice(CAmount raw_price, int base_decimals);
//! The exact spread between two canonical best prices in the displayed
//! orientation, rounded to the places its prices show (with `exact`); empty
//! when ask < bid or, in the B3 view, a price is zero. Display only.
PriceText DisplaySpread(CAmount bid, CAmount ask, const Units& units, bool inverse);
//! Exact sampled gross B3 when inverted; not a fixed-B3 order quantity.
QString FormatDepthQuantity(CAmount raw_price, CAmount base_quantity, int base_decimals, bool inverse);
std::optional<CAmount> ParseDisplayPrice(const QString& text, const Units& units, bool inverse, QString* error = nullptr);
//! Executable canonical limit and its exact presentation. An adjusted inverse
//! limit is rounded only in the customer's favour; the UI must disclose it.
struct DisplayLimit {
    CAmount price{0};
    bool adjusted{false};
    QString executable_price;
};
std::optional<DisplayLimit> ParseDisplayLimit(const QString& text, const Units& units, bool inverse, bool display_buy, QString* error = nullptr);
QString CanonicalSide(bool buy, bool inverse);
bool DisplayBuy(const QString& canonical_side, bool inverse);
std::optional<CAmount> ReplacementBudget(CAmount available, CAmount reserved);
std::optional<CAmount> ParseQuantity(const QString& text, const Units& units, QString* error = nullptr);
std::optional<CAmount> ParsePrice(const QString& text, const Units& units, QString* error = nullptr);
std::optional<CAmount> Notional(CAmount raw_price, CAmount raw_quantity);
//! The published aggregate 100-ppm fee; seller allocation is not a future quote.
std::optional<CAmount> FeeExample(CAmount raw_notional);

struct Point { CAmount price{0}, quantity{0}; bool operator==(const Point&) const = default; };
struct Curve {
    QString account, side;
    std::vector<Point> points;
    CAmount filled{0}, remaining{0}, reserved{0};
    bool operator==(const Curve&) const = default;
};
struct Depth { CAmount price{0}, demand{0}, supply{0}; bool operator==(const Depth&) const = default; };
struct Trade {
    uint64_t sequence{0}, epoch{0};
    QString hash;
    int64_t anchor_height{0};
    bool cleared{false}, own_fills_known{false};
    CAmount price{0}, quantity{0}, notional{0}, fee{0}, own_buy{0}, own_sell{0};
    bool operator==(const Trade&) const = default;
};
//! One certified microblock as an auction tick (getflowmeshmarketdata
//! "clearing_ticks"): kind execution, settlement or handoff; side of the
//! unfilled interest bid, ask, none or unknown. Sequence only, no time.
struct Tick {
    uint64_t sequence{0};
    QString hash, kind, side;
    bool cleared{false};
    CAmount price{0}, volume{0}, imbalance{0};
    bool operator==(const Tick&) const = default;
};
//! A reply's tick page. A malformed section is only unavailable ("malformed");
//! it never fails the read or is shown as data.
struct TickPage {
    bool available{false}, remote{false}, gap{false}, more{false};
    QString reason;
    std::vector<Tick> ticks;
    bool operator==(const TickPage&) const = default;
};
//! Operator-observed times of a reply ("observed_time"): when the source
//! (this node, or the endpoint that reported them) applied each entry, by
//! its own clock. Never certified and never a trade time for a delayed or
//! untimed row. A malformed section is only unavailable ("malformed"); it
//! never fails the read or is shown as data.
struct ObservedTimes {
    bool available{false}, clock_suspect{false};
    QString reason, source;
    //! Ascending sequences.
    std::vector<flowmesh::ObservedStamp> rows;
    bool operator==(const ObservedTimes&) const = default;
};
//! One page of getflowmeshcandles, checked fail closed: a malformed page is
//! only unavailable ("malformed").
struct CandlePage {
    bool available{false}, remote{false}, more{false};
    QString reason, source;
    flowmesh::CandleInterval interval{flowmesh::CandleInterval::M1};
    int64_t from_ms{0}, before_ms{0};
    std::vector<flowmesh::ObservedCandle> candles;
    bool operator==(const CandlePage&) const = default;
};
//! `requested` is the interval asked for; any other is malformed.
CandlePage ParseCandles(const UniValue& value, flowmesh::CandleInterval requested);
//! Keeps the stamps of held ticks only: `page`'s rows whose tick is held with
//! the same hash are added, and every stamp whose tick left `ticks` goes.
void MergeHeldTimes(std::map<uint64_t, flowmesh::ObservedStamp>& held, const std::deque<Tick>& ticks,
                    const ObservedTimes* page);
//! The node's indicative next clearing ("indicative"): never certified.
struct Indicative {
    bool available{false}, cleared{false};
    QString reason, head, side;
    CAmount price{0}, volume{0}, imbalance{0};
    std::optional<CAmount> best_bid, best_ask, fill_min, fill_max;
    //! The draft it was computed for: canonical side, price and quantity.
    bool has_draft{false}, replaces{false};
    QString draft_side;
    CAmount draft_price{0}, draft_quantity{0};
    bool operator==(const Indicative&) const = default;
};
//! One price level of the node's "book_levels" (a canonical side), checked
//! against its bucket's exact canonical range. Display only.
struct BookLevel {
    util::Unsigned128 bucket{0};
    //! Canonical price zero in the inverse view (no finite token-per-B3 price).
    bool zero_price{false};
    CAmount low{0}, high{0}, size{0}, cumulative{0}, own{0};
    //! Exact B3 atoms of the level, when the node computed them.
    std::optional<util::Unsigned128> notional;
    uint32_t curves{0};
    bool operator==(const BookLevel&) const = default;
};
struct BookLevelsSide {
    bool available{false}, more{false};
    //! Why the side is unavailable, or why it stopped early (truncated).
    QString reason, truncated;
    //! Best first: bids by descending, asks by ascending canonical price.
    std::vector<BookLevel> levels;
    bool operator==(const BookLevelsSide&) const = default;
};
//! The node's price levels of every verified standing curve at the reply's
//! head (getflowmeshmarketdata "book_levels"): never certified, pending
//! actions excluded. A malformed section is only unavailable ("malformed");
//! it never fails the read or is shown as data.
struct BookLevels {
    bool available{false};
    QString reason, head;
    size_t depth{0}, total_curves{0};
    //! The grouping the node used, as asked for.
    flowmesh::LevelGrouping grouping;
    //! Exact best standing prices, whatever the grouping.
    std::optional<CAmount> best_bid, best_ask;
    BookLevelsSide bids, asks;
    //! The standing curves cleared as they are, with no draft.
    Indicative standing;
    bool operator==(const BookLevels&) const = default;
};
struct Snapshot {
    QString market, base, domain, config, head, state_root, account, halt, error;
    Units units;
    bool certified{false}, running{false}, paused{true}, handoff{false}, observer{true}, unchanged{false};
    bool chain_reconciling{false};
    // These are local-client verification results, not endpoint assertions.
    bool remote{false}, certificate_verified{false}, account_state_verified{false}, execution_result_verified{false};
    bool b3_checkpoint_confirmed{false}, event_gap{false};
    QString endpoint;
    bool curves_complete{false}, history_truncated{false}, history_available{false}, history_page_partial{false};
    uint64_t next_sequence{0}, epoch{0}, account_sequence{0}, active_seats{0}, quorum_required{0}, pending_actions{0};
    CAmount base_available{0}, base_reserved{0}, b3_available{0}, b3_reserved{0};
    std::vector<Curve> curves, own_curves;
    std::vector<Trade> history;
    std::vector<Depth> depth;
    //! Optional display sections, also on unchanged replies.
    std::optional<TickPage> ticks;
    std::optional<Indicative> indicative;
    std::optional<ObservedTimes> observed;
    //! Present when the reply carried them: a changed head, or an unchanged
    //! reply asked for them (see B3FlowMeshTradingPanel's levels request).
    std::optional<BookLevels> levels;
    bool operator==(const Snapshot&) const = default;
};
//! Ticks the panel holds for one market: consecutive, oldest first, at most
//! MAX_HELD_TICKS. An available page extends them; a page that skips ticks or
//! disagrees with one already held replaces them. Returns false when the held
//! ticks were replaced (or the page was unavailable).
inline constexpr size_t MAX_HELD_TICKS{256};
bool MergeHeldTicks(std::deque<Tick>& held, const TickPage& page);
//! A held tick as the engine's clearing tick (for the shared observed-time
//! bucketing); nullopt for a malformed hash.
std::optional<flowmesh::ClearingTick> ToClearingTick(const Tick& tick);
//! The held ticks as the chart's trades: cleared ticks with their exact
//! notional (price x volume; fees are not part of a tick), others as clearings
//! without a match.
std::vector<Trade> TicksAsTrades(const std::deque<Tick>& held);
//! "Last auction 12:34:56 · …" for the latest tick, in the display
//! orientation; `when` is its observed local time ("": none shown). The
//! auction's number belongs in the tooltip. `exact_prices`, when given,
//! receives the exact form of every price the text shows rounded (empty
//! when none was), for a tooltip.
QString ClearingTickText(const Tick& tick, const Units& units, bool inverse, bool endpoint_reported, QString* exact_prices = nullptr,
                         const QString& when = {});
//! The indicative line for the ticket; empty when there is nothing to say.
QString IndicativeText(const Indicative& view, const Units& units, bool inverse, QString* exact_prices = nullptr);
//! A price-grouped projection of exact limit-shaped curves, not FIFO orders.
//! Raw fields always retain canonical units: base raw quantity and B3 atoms.
//! Inverse amount is gross B3 at this limit, not a promised execution amount.
struct LimitLevel {
    CAmount canonical_price{0}, remaining{0}, gross_notional{0};
    size_t curve_count{0};
    QString price, amount, total;
    bool operator==(const LimitLevel&) const = default;
};
struct LimitBook {
    // Display price DESCENDING on both sides: best ask last, best bid first.
    std::vector<LimitLevel> asks, bids;
    // Best canonical prices among INCLUDED levels, independent of orientation.
    std::optional<CAmount> canonical_best_bid, canonical_best_ask;
    size_t projected_curves{0}, general_curves{0}, invalid_curves{0}, zero_inverse_curves{0}, exhausted_curves{0};
    // Complete means no active curves are missing from this projection.
    // Unknown/mismatched units retain raw levels but leave display text empty.
    bool complete{false}, units_known{false};
    // Exact decimal or rational; empty for incomplete, one-sided, crossed, or
    // unknown-unit books. Never present a partial-limit spread as market-wide.
    QString spread;
    // The spread as displayed: token per B3 rounds to the decimal places the
    // two best prices are displayed with (then marked "≈"); else `spread`.
    QString spread_display;
    bool operator==(const LimitBook&) const = default;
};
//! The order book's grouping choices: exact, then display steps m x 10^j
//! around the magnitude k of the reference price (the best bid, else the
//! best ask) in the displayed orientation - 1, 2 and 5 x 10^(k-4), then
//! 10^(k-3), 10^(k-2) and 10^(k-1) - each kept only when valid for the
//! orientation (LevelGroupingError: a whole number of canonical ticks, or
//! an inverse scale in range). Integer digit counts only. Exact alone
//! without verified units or a positive reference price.
std::vector<flowmesh::LevelGrouping> BookGroupingChoices(std::optional<CAmount> best_bid, std::optional<CAmount> best_ask,
                                                         const Units& units, bool inverse);
//! A grouping's display step ("0.01"), or "Exact".
QString GroupingLabel(const flowmesh::LevelGrouping& grouping);
//! Only limit orders and ladder orders (LadderStepsOf: each price is a limit)
//! are projected. Invalid curves and overflowing entire price groups are
//! omitted fail-closed; general curves remain in Aggregate(), not
//! misrepresented as limit orders.
LimitBook ProjectLimitBook(const Snapshot& snapshot, bool inverse);
//! The exact limit price of a native limit-shaped curve (the shapes
//! MakeLimitBidCurve / MakeLimitAskCurve emit); nullopt for any other curve.
//! It does not validate the curve itself.
std::optional<CAmount> LimitPriceOf(const Curve& curve);
//! One price of a ladder order: canonical price, original quantity and what
//! remains of it after fills (the best-priced lots fill first).
struct LadderStep {
    CAmount price{0}, quantity{0}, remaining{0};
    bool operator==(const LadderStep&) const = default;
};
//! The prices of a curve exactly as a ladder order builds it
//! (flowmesh::MakeLadderBidCurve / MakeLadderAskCurve: 1 to 4 prices, one
//! of them a limit order), ascending, with each price's remaining quantity
//! after `filled`; nullopt for any other curve.
std::optional<std::vector<LadderStep>> LadderStepsOf(const QString& side, const std::vector<Point>& points, CAmount filled = 0);
std::optional<std::vector<LadderStep>> LadderStepsOf(const Curve& curve);

//! A client-side "marketable limit": the best verified opposite price
//! moved by at most the given slippage. It is an ordinary limit order, not a
//! market or immediate-or-cancel order: any unfilled part rests.
inline constexpr int DEFAULT_SLIPPAGE_BPS{100}, MAX_SLIPPAGE_BPS{1000};
struct MarketableLimit {
    //! Canonical prices (B3 atoms per base atom).
    CAmount reference{0}, price{0};
    //! Other accounts' certified remaining quantity at or better than the
    //! limit, in base atoms (saturating): visible depth, not a fill estimate.
    CAmount visible{0};
    int slippage_bps{0};
    bool covers_quantity{false};
    //! Exact display prices in the requested orientation.
    QString display, reference_display;
};
//! Fails closed with a reason unless the certified book is complete, holds
//! only limit-shaped curves, has verified units and a positive opposite
//! best price. Prices only against other accounts' curves, applies the
//! slippage in exact integer arithmetic (a bid floors and never goes below
//! the reference, an ask ceils and never goes above it), must round-trip
//! exactly through ParseDisplayLimit, and refuses to cross this account's
//! own opposite order.
std::optional<MarketableLimit> ComputeMarketableLimit(const Snapshot& snapshot, bool display_buy, bool inverse, int slippage_bps,
                                                      std::optional<CAmount> quantity, QString* error = nullptr);
//! A maximum slippage in percent with at most two decimals ("1.00"), as
//! basis points from 1 (0.01%) to MAX_SLIPPAGE_BPS (10%).
std::optional<int> ParseSlippageBps(const QString& text, QString* error = nullptr);
//! Basis points as an exact percent with two decimals ("1.00").
QString FormatSlippage(int bps);
//! Fail closed; no JSON display values are financial sources of truth.
Snapshot Parse(const UniValue& value);
std::vector<Depth> Aggregate(const std::vector<Curve>& curves);
//! Poll health is not evidence that validators currently form a quorum.
bool AdmissionReady(const Snapshot& snapshot, int64_t response_age_ms, int64_t certificate_age_ms, int64_t queued_without_progress_ms = -1);
QString StatusText(const Snapshot& snapshot, int64_t response_age_ms, int64_t certificate_age_ms, int64_t queued_without_progress_ms = -1);

//! The trading panel's one-line connection state. Display only: every gate
//! keeps its own checks. Live is exactly the freshness part of the trading
//! gate: no failed read and a verified reply at most 3 s old. Time while the
//! panel was hidden (not polling) never counts as waiting.
enum class LinkState { None, Checking, Live, Slow, Reconnecting, Stale };
struct LinkInputs {
    bool wallet{false}, read_failed{false};
    //! Age of the last verified reply of the selected market (-1: none).
    int64_t response_age_ms{-1};
    //! How long the panel has been polling since it was last shown (-1: not polling).
    int64_t polling_ms{-1};
};
inline constexpr int64_t LINK_FRESH_MS{3000}, LINK_STALE_MS{30'000};
LinkState ClassifyLink(const LinkInputs& inputs);
} // namespace B3FlowMeshMarketData
#endif

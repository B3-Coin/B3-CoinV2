// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHMARKETDATA_H
#define BITCOIN_QT_B3FLOWMESHMARKETDATA_H

#include <consensus/amount.h>
#include <QString>
#include <univalue.h>
#include <cstddef>
#include <cstdint>
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
//! Orientation never changes canonical market data or the signed integer grid.
//! Approximate display prices are marked; use ExactInversePrice for order input.
QString FormatDisplayPrice(CAmount raw_price, int base_decimals, bool inverse = false);
QString ExactInversePrice(CAmount raw_price, int base_decimals);
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
    bool operator==(const Snapshot&) const = default;
};
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
    bool operator==(const LimitBook&) const = default;
};
//! Only exact MakeLimitBidCurve / MakeLimitAskCurve shapes are projected.
//! Invalid curves and overflowing entire price groups are omitted fail-closed;
//! general curves remain in Aggregate(), not misrepresented as limit orders.
LimitBook ProjectLimitBook(const Snapshot& snapshot, bool inverse);
//! Fail closed; no JSON display values are financial sources of truth.
Snapshot Parse(const UniValue& value);
std::vector<Depth> Aggregate(const std::vector<Curve>& curves);
//! Poll health is not evidence that validators currently form a quorum.
bool AdmissionReady(const Snapshot& snapshot, int64_t response_age_ms, int64_t certificate_age_ms, int64_t queued_without_progress_ms = -1);
QString StatusText(const Snapshot& snapshot, int64_t response_age_ms, int64_t certificate_age_ms, int64_t queued_without_progress_ms = -1);
} // namespace B3FlowMeshMarketData
#endif

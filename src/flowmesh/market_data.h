// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_DATA_H
#define B3COIN_FLOWMESH_MARKET_DATA_H

#include <flowmesh/clearing.h>
#include <flowmesh/market.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace flowmesh {

// Local read API bounds. These do not alter execution or serialization rules.
inline constexpr size_t MARKET_DATA_MAX_CURVES{128};
inline constexpr size_t MARKET_DATA_MAX_HISTORY{100};
inline constexpr size_t MARKET_DATA_HISTORY_CAPACITY{256};
inline constexpr size_t MARKET_DATA_MAX_FILL_ACCOUNTS{128};
//! A clearing-tick page holds at most every retained history entry.
inline constexpr size_t MARKET_DATA_MAX_TICKS{MARKET_DATA_HISTORY_CAPACITY};
inline constexpr size_t MARKET_DATA_DEFAULT_TICKS{64};

/** Which side a clearing left unfilled at its uniform price: BID when buy
 * interest remained, ASK when sell interest did. NONE for an exact match, an
 * execution without a clearing, a settlement or a handoff. UNKNOWN only for a
 * clearing whose post-execution book was not observed or disagreed with the
 * committed imbalance. A display observation, never certified or committed. */
enum class ImbalanceSide : uint8_t { NONE, BID, ASK, UNKNOWN };

inline const char* ImbalanceSideName(const ImbalanceSide side)
{
    switch (side) {
    case ImbalanceSide::NONE: return "none";
    case ImbalanceSide::BID: return "bid";
    case ImbalanceSide::ASK: return "ask";
    case ImbalanceSide::UNKNOWN: return "unknown";
    }
    return "unknown";
}

//! A bounded page of clearing ticks, oldest first.
struct ClearingTickQuery {
    //! Ticks after this microblock sequence; absent: the newest `limit`.
    std::optional<uint64_t> after_sequence;
    size_t limit{MARKET_DATA_DEFAULT_TICKS};
};

/** A prospective limit order exactly as the wallet would build it
 * (MakeLimitBidCurve / MakeLimitAskCurve). Submitted, it would replace the
 * signing account's standing curve on the same side. */
struct IndicativeDraft {
    ClearingEngine::Side side{ClearingEngine::Side::BID};
    CAmount price{0};
    CAmount quantity{0};
    //! The account that would sign it. Absent (no account yet): a new curve,
    //! and the draft's fill under rationing is reported as bounds.
    std::optional<AccountId> account;
    bool operator==(const IndicativeDraft&) const = default;
};

struct IndicativeQuery {
    std::optional<IndicativeDraft> draft;
};

/** What the next clearing would be if the verified standing curves (plus
 * the draft, if any) were cleared as they are. NOT certified: every action
 * not yet in the verified state is excluded, and the draft is assumed to be
 * admitted (its balance and reservation are not checked). */
struct IndicativeClearing {
    bool available{false};
    //! "work_budget", "malformed_curve", "invalid_draft", "overflow",
    //! "no_certified_state" or "head_changed".
    std::string unavailable_reason;
    //! Certified head of the curves, set by the caller.
    uint256 head;
    std::optional<IndicativeDraft> draft;
    //! The draft replaces its account's standing curve on that side.
    bool draft_replaces_curve{false};
    bool cleared{false};
    CAmount price{0};
    CAmount volume{0};
    //! |demand - supply| at the price, saturated at MAX_MONEY as committed.
    CAmount imbalance{0};
    ImbalanceSide imbalance_side{ImbalanceSide::NONE};
    //! Highest price with standing bid quantity and lowest with standing ask
    //! quantity, over every verified curve (never the draft).
    std::optional<CAmount> best_bid;
    std::optional<CAmount> best_ask;
    //! The draft's fill at the price: exact largest-remainder rationing when
    //! its account is known (min == max), otherwise its bounds.
    std::optional<CAmount> draft_fill_min;
    std::optional<CAmount> draft_fill_max;
};

struct MarketDataQuery {
    size_t limit{50};
    size_t curve_limit{128};
    std::optional<uint64_t> before_sequence;
    std::optional<ClearingEngine::CurveKey> curve_cursor;
    std::optional<uint256> expected_head;
    std::optional<uint256> known_head;
    //! Head and runtime status only: no curves, account or history are read.
    //! A local read option, never part of any codec or reply format.
    bool status_only{false};
    //! Clearing ticks, also read for a light known_head poll.
    std::optional<ClearingTickQuery> ticks;
    //! Indicative next clearing, also computed for a light known_head poll.
    std::optional<IndicativeQuery> indicative;
};

struct MarketAccountFill {
    AccountId account_id;
    CAmount bid_quantity{0};
    CAmount ask_quantity{0};
};

/** Derived only from fully verified execution results, after durable commit.
 * There is deliberately no fabricated execution timestamp or candle. */
struct MarketHistoryEntry {
    uint64_t sequence{0};
    uint256 microblock_hash;
    uint64_t epoch{0};
    int32_t anchor_height{-1};
    uint256 anchor_hash;
    bool handoff{false};
    bool cleared{false};
    CAmount price{0};
    CAmount quantity{0};
    CAmount notional_atoms{0};
    CAmount fee_atoms{0};
    std::vector<MarketAccountFill> account_fills;
    bool account_fills_complete{true};
    // Display observations of the same verified execution; never part of a
    // root, certificate, commitment or any reply that predates them.
    //! A dedicated withdrawal-settlement execution (it never clears).
    bool settlement{false};
    //! The committed |demand - supply| at the clearing price, saturated at
    //! MAX_MONEY exactly as ClearingResult::imbalance; zero without a clearing.
    CAmount imbalance{0};
    ImbalanceSide imbalance_side{ImbalanceSide::NONE};
};

/** One certified microblock of the bounded history as an auction tick: the
 * uniform price and volume of its clearing and the side left unfilled.
 * Sequence identity only: microblocks carry no execution time. */
struct ClearingTick {
    enum class Kind : uint8_t { EXECUTION, SETTLEMENT, HANDOFF };
    uint64_t sequence{0};
    uint256 microblock_hash;
    Kind kind{Kind::EXECUTION};
    bool cleared{false};
    CAmount price{0};
    CAmount volume{0};
    CAmount imbalance{0};
    ImbalanceSide imbalance_side{ImbalanceSide::NONE};
    bool operator==(const ClearingTick&) const = default;
};

inline const char* ClearingTickKindName(const ClearingTick::Kind kind)
{
    switch (kind) {
    case ClearingTick::Kind::EXECUTION: return "execution";
    case ClearingTick::Kind::SETTLEMENT: return "settlement";
    case ClearingTick::Kind::HANDOFF: return "handoff";
    }
    return "execution";
}

inline ClearingTick MakeClearingTick(const MarketHistoryEntry& entry)
{
    return {entry.sequence, entry.microblock_hash,
            entry.handoff ? ClearingTick::Kind::HANDOFF : entry.settlement ? ClearingTick::Kind::SETTLEMENT
                                                                           : ClearingTick::Kind::EXECUTION,
            entry.cleared, entry.price, entry.quantity, entry.imbalance, entry.imbalance_side};
}

struct ClearingTickPage {
    bool available{false};
    //! Why ticks are unavailable; empty when available.
    std::string unavailable_reason;
    //! Ascending, consecutive microblock sequences.
    std::vector<ClearingTick> ticks;
    std::optional<uint64_t> oldest_retained_sequence;
    //! Ticks after after_sequence were evicted before this page's first one:
    //! a gap in the record, never a period without trading.
    bool gap{false};
    //! Retained ticks follow this page: continue after its last sequence.
    bool more{false};
};

//! The standing curves of one certified head, copied once for display
//! views. Transient: never projected, serialized or read for signing.
struct BookCurves {
    uint256 head;
    std::vector<ClearingEngine::CurveView> curves;
    //! Set when the curves were not copied ("work_budget": more than
    //! MARKET_VIEW_MAX_BOOK_CURVES; "head_changed": the head moved during a
    //! paged copy): the views are then unavailable for this reason.
    std::string unavailable_reason;
};

struct MarketHistoryPage {
    bool available{false};
    std::vector<MarketHistoryEntry> entries;
    std::optional<uint64_t> oldest_retained_sequence;
    std::optional<uint64_t> next_before_sequence;
    bool truncated{false};
};

/** Local, in-memory observations for the snapshot's epoch/next sequence.
 * Counters saturate and reset on certified advancement or process restart.
 * They do not establish a remote tip, explain a stall, or prove quorum health. */
struct MarketDataRuntimeDiagnostics {
    uint32_t round{0};
    size_t candidate_count{0};
    size_t max_verified_attestations{0};
    size_t active_catchup_requests{0};
    uint64_t proposals_missing_evidence{0};
    // Legacy RPC field retained for compatibility; timer skew is not rejection.
    uint64_t proposals_rejected_round{0};
    // Fully checked candidate proposals received at a different local round.
    uint64_t proposals_verified_different_round{0};
    // Authenticated competing proposals ignored without changing our lock.
    uint64_t proposals_conflicting_lock{0};
    // Decoded seat-in-range attestations observed with no candidate to verify.
    uint64_t attestations_without_candidate{0};
    std::optional<int64_t> last_message_observed_at;
    // Cached public hash from a successful durable lock, never a journal read
    // during RPC and never a signature or secret. Current position only.
    std::optional<uint256> local_locked_candidate;
};

struct MarketDataSnapshot {
    bool certified{false};
    uint64_t next_microblock_sequence{0};
    uint256 last_microblock_hash;
    uint256 state_root;
    uint64_t epoch{0};
    int32_t anchor_height{-1};
    uint256 anchor_hash;
    size_t active_seats{0};
    size_t quorum_required{0};
    bool running{false};
    bool paused{true};
    // Local service overlay only; never part of a certificate or state root.
    bool chain_reconciling{false};
    bool observer_only{true};
    bool pending_handoff{false};
    std::string halt;
    std::string error;
    size_t pending_actions{0};
    // Local observation time, never a certified execution timestamp. Absent
    // after restart until this process commits another certified entry.
    std::optional<int64_t> local_observed_at;
    MarketDataRuntimeDiagnostics runtime;
};

struct MarketAccountData {
    AccountId account_id;
    uint64_t next_sequence{0};
    CAmount base_available{0};
    CAmount base_reserved{0};
    CAmount b3_available_atoms{0};
    CAmount b3_reserved_atoms{0};
    std::vector<ClearingEngine::CurveView> curves;
};

struct MarketData {
    uint256 domain;
    MarketId market_id;
    AssetId base_asset_id;
    uint256 execution_config_id;
    MarketDataSnapshot snapshot;
    bool unchanged{false};
    ClearingEngine::CurvePage liquidity;
    std::optional<MarketAccountData> account;
    MarketHistoryPage history;
    //! Present only when the query asked for clearing ticks.
    std::optional<ClearingTickPage> ticks;
    //! Present only when the query asked for it; never certified.
    std::optional<IndicativeClearing> indicative;
    //! Transient input of ComputeMarketViews; never projected or serialized.
    std::shared_ptr<const BookCurves> book;
    //! Public-client verification observations; never serialized into state roots.
    bool remote{false};
    bool certificate_verified{false};
    bool account_state_verified{false};
    bool execution_result_verified{false};
    bool b3_checkpoint_confirmed{false};
    bool event_gap{false};
    std::string endpoint;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_DATA_H

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_DATA_H
#define B3COIN_FLOWMESH_MARKET_DATA_H

#include <flowmesh/clearing.h>
#include <flowmesh/market.h>
#include <uint256.h>
#include <util/int128.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
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
    bool operator==(const IndicativeClearing&) const = default;
};

//! At most this many book levels per side in one reply.
inline constexpr size_t MARKET_DATA_MAX_LEVELS{50};
inline constexpr size_t MARKET_DATA_DEFAULT_LEVELS{20};

/** How book levels group canonical prices: into the buckets of a display
 * step h = mantissa x 10^exponent, in B3 per token (canonical) or in token
 * per B3 (inverse), for a base asset of `decimals` decimals. A displayed
 * buy bucket is labelled with its lower edge and a displayed sell bucket
 * with its upper edge, so every lot in a bucket accepts its label price.
 * Display only; never read by signing or any market decision. */
struct LevelGrouping {
    //! One level per canonical price; the other fields are unused.
    bool exact{true};
    bool inverse{false};
    //! 1, 2 or 5.
    uint8_t mantissa{1};
    int32_t exponent{0};
    //! The base asset's verified decimals (0-18), as the caller displays it.
    uint8_t decimals{0};
    bool operator==(const LevelGrouping&) const = default;
};

struct BookLevelsQuery {
    //! Levels per side, 1..MARKET_DATA_MAX_LEVELS.
    size_t depth{MARKET_DATA_DEFAULT_LEVELS};
    LevelGrouping grouping;
    //! Also compute them for an unchanged known_head reply (after a grouping
    //! change); otherwise only a reply of a changed head carries them.
    bool include_unchanged{false};
    //! The caller's own account, for each level's own share. Set by the
    //! local RPC from its wallet, never taken from a request.
    std::optional<AccountId> account;
    bool operator==(const BookLevelsQuery&) const = default;
};

/** One price level (bucket) of one side of the verified standing curves.
 * Exact: every quantity is the lots whose own limit lies in [low, high]
 * (a bid lot's highest price, an ask lot's lowest), after fills. */
struct BookLevel {
    //! The display bucket index: the canonical price (exact grouping),
    //! floor(P / g) of a bid or ceil(P / g) of an ask (canonical, g ticks),
    //! ceil(u) of a canonical bid or floor(u) of a canonical ask with u =
    //! 10^s / (mantissa x P) (inverse). Zero for the zero-price level of the
    //! inverse view.
    util::Unsigned128 bucket{0};
    //! Canonical price 0 in the inverse view: no finite token-per-B3 price
    //! (an ask that sells at any price, or a bid that buys only for free).
    bool zero_price{false};
    //! The bucket's inclusive range of canonical prices (B3 atoms per base
    //! atom); an unbounded end is MAX_MONEY.
    CAmount low{0}, high{0};
    //! Base units at this level, and from the best level through it.
    CAmount size{0}, cumulative{0};
    //! Exact B3 atoms of the level at each lot's own price (sum of
    //! quantity x price); absent when a curve had more distinct prices in
    //! the bucket than the level's bounded walk reads.
    std::optional<util::Unsigned128> notional;
    //! Standing curves with quantity here (one per account and side).
    uint32_t curves{0};
    //! The caller's account's part of `size`.
    CAmount own{0};
    bool operator==(const BookLevel&) const = default;
};

struct BookLevelsSide {
    bool available{false};
    //! "work_budget" or "overflow" before the first level; empty when available.
    std::string unavailable_reason;
    //! Best first: bids by descending, asks by ascending canonical price.
    std::vector<BookLevel> levels;
    //! More levels follow the last one returned.
    bool more{false};
    //! The levels stopped early ("work_budget" or "overflow"): those returned
    //! are an exact prefix and nothing deeper is guessed.
    std::string truncated_reason;
    bool operator==(const BookLevelsSide&) const = default;
};

/** Aggregated price levels of the verified standing curves of one certified
 * head. Never certified: computed by this node from the curves it verified
 * or holds, excluding every pending action. Display only. */
struct BookLevels {
    bool available{false};
    //! "no_certified_state", "head_changed", "work_budget" (also a book over
    //! MARKET_VIEW_MAX_BOOK_CURVES) or "malformed_curve".
    std::string unavailable_reason;
    //! Certified head of the curves, set by the caller.
    uint256 head;
    size_t depth{0};
    LevelGrouping grouping;
    size_t total_curves{0};
    //! Exact best standing prices, independent of the grouping.
    std::optional<CAmount> best_bid, best_ask;
    BookLevelsSide bids, asks;
    //! The standing curves cleared as they are, with no draft: what the next
    //! auction would do if nothing else arrived. Not certified.
    IndicativeClearing standing;
    bool operator==(const BookLevels&) const = default;
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
    //! Operator-observed times of the head and of the reply's history and
    //! tick entries, also for a light known_head poll; never with
    //! status_only. Display data only.
    bool observed_time{false};
    //! Price levels of the verified standing curves, for a changed head
    //! (and an unchanged one when asked); never with status_only. A local
    //! read option only: never part of a public request or reply.
    std::optional<BookLevelsQuery> levels;
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

/** Operator-observed time (doc/design/flowmesh-chart-time.md): when a node
 * durably applied a certified microblock, by that node's own wall clock.
 * Display data only, like a clearing tick: never certified, never part of a
 * root, certificate, commitment or signed object, and never read by signing,
 * admission, execution, verification or any market decision. V1 microblocks
 * carry no execution time; this is an observation, labelled with the node
 * that made it, and history without one stays without a time. */
enum class ObservedMode : uint8_t {
    //! Applied while the node was caught up with the network: its
    //! observation of the entry as it was certified.
    LIVE = 1,
    //! Applied late (catch-up, a deferral, a proven lag, a stale anchor, or
    //! the warm-up after start): `ms` is only when the node applied it, an
    //! upper bound, never a trade time.
    DELAYED = 2,
    //! Applied while the wall clock stood more than the slew tolerance
    //! behind an earlier stamp: no time at all.
    UNTIMED = 3,
};

inline const char* ObservedModeName(const ObservedMode mode)
{
    switch (mode) {
    case ObservedMode::LIVE: return "live";
    case ObservedMode::DELAYED: return "delayed";
    case ObservedMode::UNTIMED: return "untimed";
    }
    return "untimed";
}

//! Why an entry is not LIVE (NONE for a LIVE one). Local detail only: never
//! in a public reply. HINT is no longer recorded (an unverified peer hint
//! decides nothing); it names rows an earlier build of this branch stored.
enum class ObservedDelay : uint8_t { NONE, CATCHUP, DEFERRED, BEHIND, HINT, STALE_ANCHOR, WARMUP, CLOCK_BACK };

inline const char* ObservedDelayName(const ObservedDelay delay)
{
    switch (delay) {
    case ObservedDelay::NONE: return "none";
    case ObservedDelay::CATCHUP: return "catchup";
    case ObservedDelay::DEFERRED: return "deferred";
    case ObservedDelay::BEHIND: return "behind";
    case ObservedDelay::HINT: return "peer_hint";
    case ObservedDelay::STALE_ANCHOR: return "stale_anchor";
    case ObservedDelay::WARMUP: return "warmup";
    case ObservedDelay::CLOCK_BACK: return "clock_stepped_back";
    }
    return "none";
}

enum ObservedFlag : uint8_t {
    //! The wall clock stood up to the slew tolerance (1 s) behind the
    //! previous stamp; the stamp holds that previous value.
    OBSERVED_CLOCK_CLAMPED = 1 << 0,
    //! Wall and steady clocks disagreed by more than 1 s since the previous
    //! stamp: time continuity with it is not established.
    OBSERVED_CLOCK_STEP = 1 << 1,
    //! This node's clock is suspected wrong (it stood behind the B3 tip's
    //! block time by more than the block future bound).
    OBSERVED_CLOCK_SUSPECT = 1 << 2,
    //! The first stamp of this process: continuity with the previous stamp
    //! (an earlier process) is not established.
    OBSERVED_RESTART = 1 << 3,
    //! The entry's clearing tick was unavailable when it was recorded; it is
    //! counted without a price. Local detail only.
    OBSERVED_NO_CLEARING = 1 << 4,
    //! Set by a remote client only: a plausibility check of the endpoint's
    //! stamp failed. The stamp is shown flagged, never corrected.
    OBSERVED_IMPLAUSIBLE = 1 << 5,
};
//! The only stamp flags a public reply carries: none of them names a seat.
inline constexpr uint8_t OBSERVED_PUBLIC_FLAGS{OBSERVED_CLOCK_CLAMPED | OBSERVED_CLOCK_STEP | OBSERVED_CLOCK_SUSPECT |
                                               OBSERVED_RESTART};

struct ObservedStamp {
    uint64_t sequence{0};
    uint256 hash;
    ObservedMode mode{ObservedMode::UNTIMED};
    //! Unix milliseconds. LIVE: the observation; DELAYED: when the node
    //! applied the entry (an upper bound only); UNTIMED: zero.
    int64_t ms{0};
    uint8_t flags{0};
    ObservedDelay delay{ObservedDelay::NONE};
    bool operator==(const ObservedStamp&) const = default;
};

/** The observed times of one market-data reply, for exactly the sequences
 * of its history page and tick page whose hash they carry. Present only
 * when the query asked for them. */
struct ObservedTimes {
    bool available{false};
    //! Why they are unavailable; empty when available.
    std::string unavailable_reason;
    //! "this_node", "endpoint_operator" (in a public reply) or, at a remote
    //! client, the endpoint URL that reported them.
    std::string source;
    bool clock_suspect{false};
    //! The first sequence the source recorded (retained), if any.
    std::optional<uint64_t> recorded_since_sequence;
    //! Ascending sequences.
    std::vector<ObservedStamp> rows;
};

enum class CandleInterval : uint8_t { M1, M5, M15, H1, H4, D1 };
inline constexpr std::array<CandleInterval, 6> CANDLE_INTERVALS{
    CandleInterval::M1, CandleInterval::M5, CandleInterval::M15,
    CandleInterval::H1, CandleInterval::H4, CandleInterval::D1};

inline int64_t CandleIntervalMs(const CandleInterval interval)
{
    switch (interval) {
    case CandleInterval::M1: return 60'000;
    case CandleInterval::M5: return 300'000;
    case CandleInterval::M15: return 900'000;
    case CandleInterval::H1: return 3'600'000;
    case CandleInterval::H4: return 14'400'000;
    case CandleInterval::D1: return 86'400'000;
    }
    return 60'000;
}

inline const char* CandleIntervalName(const CandleInterval interval)
{
    switch (interval) {
    case CandleInterval::M1: return "1m";
    case CandleInterval::M5: return "5m";
    case CandleInterval::M15: return "15m";
    case CandleInterval::H1: return "1h";
    case CandleInterval::H4: return "4h";
    case CandleInterval::D1: return "1d";
    }
    return "1m";
}

inline std::optional<CandleInterval> ParseCandleInterval(const std::string& name)
{
    for (const auto interval : CANDLE_INTERVALS) {
        if (name == CandleIntervalName(interval)) return interval;
    }
    return std::nullopt;
}

enum ObservedCandleFlag : uint8_t {
    //! An entry whose time bounds overlap this bucket could not be placed in
    //! it (or in any bucket): its volume and prices are missing here.
    CANDLE_PARTIAL = 1 << 0,
    //! Holds a DELAYED entry whose whole time bound lies inside this bucket.
    CANDLE_LATE = 1 << 1,
    //! Time continuity with the previous candle is not established (a clock
    //! step or a restart at its first entry): never fill the space between.
    CANDLE_SEAM_BEFORE = 1 << 2,
    //! A member stamp carries a clock flag (clamped, step or suspect).
    CANDLE_CLOCK = 1 << 3,
};

/** One UTC-epoch-aligned bucket of operator-observed time: exactly the
 * entries placed in it, never an interpolation. Two candles whose sequences
 * are consecutive (and the later without CANDLE_SEAM_BEFORE) prove that no
 * microblock existed between them. */
struct ObservedCandle {
    //! floor(ms / interval) * interval, Unix milliseconds.
    int64_t open_ms{0};
    uint64_t first_sequence{0}, last_sequence{0};
    uint32_t entries{0}, clearings{0};
    //! Canonical integer prices; meaningful only when clearings > 0.
    CAmount open{0}, high{0}, low{0}, close{0};
    //! Exact sums: base units, and B3 atoms (price x volume per clearing).
    util::Unsigned128 base_volume{0}, quote_volume{0};
    uint8_t flags{0};
    bool operator==(const ObservedCandle&) const = default;
};

/** A time window of one interval's candles, oldest first: every candle
 * whose open is in [from_ms, before_ms). A window without candles is not
 * evidence of no trading unless the neighbours are contiguous. */
struct ObservedCandlePage {
    bool available{false};
    std::string unavailable_reason;
    //! As ObservedTimes::source.
    std::string source;
    //! The candles were reported by a remote endpoint: their prices,
    //! volumes and times are that endpoint's claims, not verified here.
    bool remote{false};
    CandleInterval interval{CandleInterval::M1};
    int64_t from_ms{0}, before_ms{0};
    std::vector<ObservedCandle> candles;
    //! Older candles may exist before from_ms: continue with before_ms =
    //! from_ms.
    bool more{false};
};

//! Book levels already computed from one copy of the curves, by query:
//! reused while that copy (and so its head) is current. Bounded.
struct BookViewMemo {
    static constexpr size_t MAX_ENTRIES{8};
    std::mutex mutex;
    std::vector<std::pair<BookLevelsQuery, std::shared_ptr<const BookLevels>>> levels;
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
    //! Views computed from exactly these curves (see ComputeMarketViews).
    std::shared_ptr<BookViewMemo> memo{std::make_shared<BookViewMemo>()};
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
    //! Present only when the query asked for observed_time; never certified.
    std::optional<ObservedTimes> observed;
    //! Present only when the query asked for levels and they were computed
    //! (a changed head, or include_unchanged); never certified.
    std::optional<BookLevels> levels;
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

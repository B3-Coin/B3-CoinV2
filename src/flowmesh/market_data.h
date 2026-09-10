// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_DATA_H
#define B3COIN_FLOWMESH_MARKET_DATA_H

#include <flowmesh/clearing.h>
#include <flowmesh/market.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flowmesh {

// Local read API bounds. These do not alter execution or serialization rules.
inline constexpr size_t MARKET_DATA_MAX_CURVES{128};
inline constexpr size_t MARKET_DATA_MAX_HISTORY{100};
inline constexpr size_t MARKET_DATA_HISTORY_CAPACITY{256};
inline constexpr size_t MARKET_DATA_MAX_FILL_ACCOUNTS{128};

struct MarketDataQuery {
    size_t limit{50};
    size_t curve_limit{128};
    std::optional<uint64_t> before_sequence;
    std::optional<ClearingEngine::CurveKey> curve_cursor;
    std::optional<uint256> expected_head;
    std::optional<uint256> known_head;
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
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_DATA_H

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_BURN_GATE_V2_H
#define B3COIN_BRIDGE_BURN_GATE_V2_H

#include <bridge/proof.h>
#include <bridge/state_observation.h>
#include <consensus/bridge_params.h>
#include <util/overflow.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

/**
 * Bridge V2 burn liveness gate (doc/design/b3-bridge-v2-node-changes.md
 * sections 8.4 and 8.6): rows C1-C4 and E1-E8 over the fresh lineage's
 * projected view and the latest kind 9 observations.
 *
 * Pure: no chain state, no I/O and no node types. The bridge index
 * (node/bridge_state.h) builds BurnGateInputsV2 from the fresh lineage
 * projected for the candidate height, its latest VERIFIER_E, TOKEN_FLAGS and
 * RESERVE_E observations and its sticky conflict. Mempool admission, block
 * connection, block assembly and wallet prevalidation then apply the one
 * decision. The reserve and wind-down bounds (section 7.2 checks 2-3) are
 * per burn and live in bridge/reserve_v2.h. Invariant R and the E5 verdict
 * are judged when an observation connects, and they reach this gate as the
 * sticky conflict (C4).
 *
 * Rows are evaluated in the order C1, C2, C3, C4, E1, ..., E8, and the first
 * failure is returned with a stable reason. Time rows compare against
 * `now_eff` (BurnGateNowEffV2); each limit is inclusive: an age exactly at
 * the limit passes, one second more fails.
 */
namespace bridge {

/** Sticky-conflict reasons (node changes section 8.5). */
inline constexpr uint8_t BRIDGE_V2_CONFLICT_E5{5};
inline constexpr uint8_t BRIDGE_V2_CONFLICT_E8{8};
//! Reserved for conflict kind 13 (FINALITY_CONFLICT), a later slice.
inline constexpr uint8_t BRIDGE_V2_CONFLICT_KIND13{13};

/** The gate row a decision names; NONE when the gate is READY. */
enum class BurnGateRowV2 : uint8_t {
    NONE = 0,
    C1,
    C2,
    C3,
    C4,
    E1,
    E2,
    E3,
    E4,
    E5,
    E6,
    E7,
    E8,
};

inline const char* BurnGateRowName(const BurnGateRowV2 row)
{
    switch (row) {
    case BurnGateRowV2::NONE: return "none";
    case BurnGateRowV2::C1: return "C1";
    case BurnGateRowV2::C2: return "C2";
    case BurnGateRowV2::C3: return "C3";
    case BurnGateRowV2::C4: return "C4";
    case BurnGateRowV2::E1: return "E1";
    case BurnGateRowV2::E2: return "E2";
    case BurnGateRowV2::E3: return "E3";
    case BurnGateRowV2::E4: return "E4";
    case BurnGateRowV2::E5: return "E5";
    case BurnGateRowV2::E6: return "E6";
    case BurnGateRowV2::E7: return "E7";
    case BurnGateRowV2::E8: return "E8";
    }
    return "unknown";
}

/**
 * One retained observation as the gate reads it: the anchor's execution
 * timestamp (`t_obs`, seconds) and the decoded state. `verdict` is the
 * VERIFIER_E verdict byte (bit 0 SETS_MISMATCH, bit 1 CERT_CONFLICT) and zero
 * for the other subjects.
 */
template <typename State>
struct BurnGateObservationV2 {
    int64_t t_obs{0};
    State state{};
    uint8_t verdict{0};
};

/** The fresh lineage projected for the candidate height (section 7.4). */
struct BurnGateLineageV2 {
    bool configured{false};
    bool bootstrapped{false};
    bool broken{false};
    //! e_B3, the projected epoch.
    uint64_t epoch{0};
    uint32_t current_count{0};
    uint64_t current_weight{0};
    uint32_t next_count{0};
    uint64_t next_weight{0};
    std::optional<int> finalized_height{};
    std::optional<uint64_t> finalized_epoch{};
};

/** The verifier's bridge-authorizing committee interval (C2). */
struct BurnGateBoundsV2 {
    uint32_t min_validators{0};
    uint32_t max_validators{0};
    uint64_t min_weight{0};

    bool Valid() const
    {
        return min_validators >= 1 && max_validators >= min_validators && min_weight > 0;
    }
};

inline BurnGateBoundsV2 BurnGateBoundsFromPins(const Consensus::BridgeV2VerifierPins& pins)
{
    return {pins.min_bridge_validators, pins.max_bridge_validators,
            pins.min_bridge_total_weight};
}

struct BurnGateInputsV2 {
    //! The candidate height, the block that would include the burn.
    int height{0};
    //! W2, the first height of bridge V2 burns (C1).
    int burn_height{0};
    //! now_eff (BurnGateNowEffV2).
    int64_t now_eff{0};
    BurnGateLineageV2 lineage{};
    BurnGateBoundsV2 bounds{};
    std::optional<BurnGateObservationV2<VerifierStateV2>> verifier{};
    std::optional<BurnGateObservationV2<TokenFlagsV2>> token{};
    std::optional<BurnGateObservationV2<ReserveStateV2>> reserve{};
    //! The sticky conflict's reason (BRIDGE_V2_CONFLICT_*), when one is set.
    std::optional<uint8_t> conflict_reason{};
};

struct BurnGateDecisionV2 {
    bool ready{false};
    BurnGateRowV2 row{BurnGateRowV2::NONE};
    std::string reason{};
    //! READY only: the first now_eff at which a time row (E1, E2, E7 or E8)
    //! fails if nothing new is observed. nullopt when no representable time
    //! closes it, and always when NOT_READY.
    std::optional<int64_t> closes_at{};

    //! "ready", or "<row>: <reason>".
    std::string Describe() const
    {
        if (ready) return "ready";
        return std::string{BurnGateRowName(row)} + ": " + reason;
    }
};

/**
 * now_eff = max(candidate block time, light-client finalized execution
 * timestamp) (section 8.4). PoS V1 block time is exact, and proven L1 time
 * covers a lagging B3 clock. A light-client time above INT64_MAX clamps to
 * INT64_MAX, which only makes observations stale: a dishonest sync
 * committee can close the gate early, never hold it open.
 */
inline int64_t BurnGateNowEffV2(const int64_t candidate_time,
                                const std::optional<uint64_t> light_client_time)
{
    if (!light_client_time) return candidate_time;
    constexpr uint64_t MAX_TIME{static_cast<uint64_t>(std::numeric_limits<int64_t>::max())};
    return std::max(candidate_time, static_cast<int64_t>(std::min(*light_client_time, MAX_TIME)));
}

namespace burn_gate_detail {

//! The first time `base + limit + 1` at which an age limit is exceeded, or
//! nullopt when that time is not representable (it never closes).
inline std::optional<int64_t> FirstPastLimit(const int64_t base, const int64_t limit)
{
    const auto at_limit{CheckedAdd(base, limit)};
    if (!at_limit) return std::nullopt;
    return CheckedAdd(*at_limit, int64_t{1});
}

} // namespace burn_gate_detail

/**
 * The gate (section 8.4 rows; the order and stable reasons below are part
 * of the interface the RPC reports):
 *
 * - C1: inputs in range (now_eff and every observation time non-negative),
 *   valid gate parameters, height >= W2, and the entry not impaired by its
 *   latest VERIFIER_E (status HALTED_CONFLICT or CANCELLED, or
 *   t_obs - lastRotationTime > set_expiry: the verifier had expired, I-4).
 *   Impairment is permanent on Ethereum, so it reads the latest observation
 *   at any age.
 * - C2: lineage configured, bootstrapped, not broken; current and next
 *   committee counts within [min, max] and weights >= min.
 * - C3: a finalized checkpoint at most max_finality_lag_blocks below the
 *   candidate height.
 * - C4: no sticky conflict (E5, E8 or later kind 13).
 * - E1: a VERIFIER_E observation, 0 <= now_eff - t_obs <= max age, status LIVE.
 * - E2: now_eff - lastRotationTime <= RotationAgeLimit().
 * - E3: 0 <= e_B3 - e_E <= max_epoch_gap. A new LIVE VERIFIER_E is judged
 *   only for e_E in {e_B3, e_B3 - 1} (node::VerifierVerdictV2: the fresh
 *   lineage retains only those sets), so a gap of 2 is reached only by an
 *   observation judged at gap 1 and aged by a later B3 rotation. A verifier
 *   kept two epochs behind therefore cannot refresh its observation, and the
 *   gate closes at E1 once the last one is stale (reported deviation from
 *   section 8.4's effective tolerance of 2; retaining Set'_{e-2} in the fresh
 *   lineage would restore it).
 * - E4: the verdict has no SETS_MISMATCH bit and no unknown bit.
 * - E5: the verdict has no CERT_CONFLICT bit. The index also turns that bit
 *   into the sticky conflict (C4).
 * - E6: if e_B3 > e_E, the handover into e_B3 is certified on B3, read as
 *   finalized_epoch >= e_B3 - 1: an epoch-(e_B3 - 1) certificate, the one
 *   that commits Set'_{e_B3} and lets the lagging verifier rotate toward
 *   e_B3, already exists. node::ProjectFreshLineage rotates into epoch e only
 *   after it applied such a certificate (handover_certified), and that
 *   certificate becomes the finalized checkpoint, so every lineage the chain
 *   can produce meets E6 whenever C2 and C3 pass. The row is kept as a
 *   defensive check against a future change of the rotation rule; it never
 *   fires on a reachable state.
 * - E7: a fresh TOKEN_FLAGS observation: not paused, not deprecated, vault
 *   not blacklisted. The code-hash pin is checked when it connects.
 * - E8: a fresh RESERVE_E observation. Invariant R is judged when it
 *   connects.
 *
 * An observation whose t_obs is later than now_eff is refused as "newer
 * than now_eff". It cannot occur when the caller follows section 8.4,
 * because every retained anchor is at or below the light client's
 * finalized execution header, whose timestamp bounds now_eff from below.
 */
inline BurnGateDecisionV2 EvaluateBurnGateV2(const BurnGateInputsV2& in,
                                             const Consensus::BridgeV2GateParams& gate)
{
    BurnGateDecisionV2 out;
    const auto fail{[&out](const BurnGateRowV2 row, std::string reason) {
        out.ready = false;
        out.row = row;
        out.reason = std::move(reason);
        out.closes_at.reset();
        return out;
    }};
    // Age of an observation: refuses observations newer than now_eff or
    // older than the maximum age. Both operands are non-negative (C1).
    const auto age_failure{[&](const int64_t t_obs, const char* subject) -> std::optional<std::string> {
        if (t_obs > in.now_eff) return std::string{subject} + " observation is newer than now_eff";
        if (in.now_eff - t_obs > gate.observation_max_age_seconds) {
            return std::string{subject} + " observation is stale";
        }
        return std::nullopt;
    }};

    // C1: inputs, parameters, W2 and impairment.
    if (in.now_eff < 0 || (in.verifier && (in.verifier->t_obs < 0 || in.verifier->state.last_rotation_time < 0)) ||
        (in.token && in.token->t_obs < 0) || (in.reserve && in.reserve->t_obs < 0)) {
        return fail(BurnGateRowV2::C1, "burn gate inputs are out of range");
    }
    if (!gate.Valid()) {
        return fail(BurnGateRowV2::C1, "bridge V2 gate parameters are invalid");
    }
    if (in.height < in.burn_height) {
        return fail(BurnGateRowV2::C1, "bridge V2 burns are not active at this height");
    }
    if (in.verifier) {
        const VerifierStateV2& v{in.verifier->state};
        if (v.status == VERIFIER_STATUS_HALTED_CONFLICT) {
            return fail(BurnGateRowV2::C1, "verifier is halted by a conflict");
        }
        if (v.status == VERIFIER_STATUS_CANCELLED) {
            return fail(BurnGateRowV2::C1, "verifier is cancelled");
        }
        if (in.verifier->t_obs - v.last_rotation_time > gate.set_expiry_seconds) {
            return fail(BurnGateRowV2::C1, "verifier signing set had expired at the observation");
        }
    }

    // C2: a lineage whose committees can authorize a Verifier V2 root.
    const BurnGateLineageV2& lineage{in.lineage};
    if (!lineage.configured) return fail(BurnGateRowV2::C2, "fresh finality lineage is not configured");
    if (!lineage.bootstrapped) return fail(BurnGateRowV2::C2, "fresh finality lineage is not bootstrapped");
    if (lineage.broken) return fail(BurnGateRowV2::C2, "fresh finality lineage is broken");
    if (!in.bounds.Valid()) return fail(BurnGateRowV2::C2, "bridge committee bounds are invalid");
    const auto count_ok{[&in](const uint32_t count) {
        return count >= in.bounds.min_validators && count <= in.bounds.max_validators;
    }};
    if (!count_ok(lineage.current_count)) {
        return fail(BurnGateRowV2::C2, "current committee member count is outside the bridge bounds");
    }
    if (lineage.current_weight < in.bounds.min_weight) {
        return fail(BurnGateRowV2::C2, "current committee weight is below the bridge minimum");
    }
    if (!count_ok(lineage.next_count)) {
        return fail(BurnGateRowV2::C2, "next committee member count is outside the bridge bounds");
    }
    if (lineage.next_weight < in.bounds.min_weight) {
        return fail(BurnGateRowV2::C2, "next committee weight is below the bridge minimum");
    }

    // C3: finality keeps up with the chain.
    if (!lineage.finalized_height) {
        return fail(BurnGateRowV2::C3, "fresh finality lineage has no finalized checkpoint");
    }
    const int64_t lag{int64_t{in.height} - int64_t{*lineage.finalized_height}};
    if (lag < 0) return fail(BurnGateRowV2::C3, "finalized checkpoint is above the candidate height");
    if (lag > gate.max_finality_lag_blocks) {
        return fail(BurnGateRowV2::C3, "finality lags the candidate height by more than the limit");
    }

    // C4: no sticky conflict.
    if (in.conflict_reason) {
        switch (*in.conflict_reason) {
        case BRIDGE_V2_CONFLICT_E5:
            return fail(BurnGateRowV2::C4, "sticky E5 conflict: the verifier accepted a non-canonical certificate");
        case BRIDGE_V2_CONFLICT_E8:
            return fail(BurnGateRowV2::C4, "sticky E8 conflict: the vault failed invariant R");
        case BRIDGE_V2_CONFLICT_KIND13:
            return fail(BurnGateRowV2::C4, "sticky conflict: finality conflict evidence");
        default:
            return fail(BurnGateRowV2::C4, "sticky conflict");
        }
    }

    // E1: a fresh VERIFIER_E showing a LIVE verifier.
    if (!in.verifier) return fail(BurnGateRowV2::E1, "no VERIFIER_E observation");
    if (const auto why{age_failure(in.verifier->t_obs, "VERIFIER_E")}) {
        return fail(BurnGateRowV2::E1, *why);
    }
    const VerifierStateV2& verifier{in.verifier->state};
    if (verifier.status != VERIFIER_STATUS_LIVE) {
        return fail(BurnGateRowV2::E1, "verifier status is not LIVE");
    }

    // E2: a covering root can still be relayed before the set expires.
    if (in.now_eff - verifier.last_rotation_time > gate.RotationAgeLimit()) {
        return fail(BurnGateRowV2::E2, "verifier rotation is older than the rotation age limit");
    }

    // E3: the verifier's epoch trails B3's by at most max_epoch_gap.
    const uint64_t e_b3{lineage.epoch};
    const uint64_t e_e{verifier.current_epoch};
    if (e_e > e_b3) return fail(BurnGateRowV2::E3, "verifier epoch is ahead of the lineage epoch");
    if (e_b3 - e_e > gate.max_epoch_gap) {
        return fail(BurnGateRowV2::E3, "verifier epoch trails the lineage epoch by more than the limit");
    }

    // E4, E5: the verdict B3 recorded for this VERIFIER_E.
    const uint8_t verdict{in.verifier->verdict};
    if ((verdict & ~ETH_OBSERVATION_VERDICT_MASK) != 0) {
        return fail(BurnGateRowV2::E4, "VERIFIER_E verdict has unknown bits");
    }
    if (verdict & ETH_OBSERVATION_VERDICT_SETS_MISMATCH) {
        return fail(BurnGateRowV2::E4, "observed verifier set hashes differ from the lineage's sets");
    }
    if (verdict & ETH_OBSERVATION_VERDICT_CERT_CONFLICT) {
        return fail(BurnGateRowV2::E5, "observed latestCertId differs from B3's certificate");
    }

    // E6: a lagging verifier can catch up from certificates B3 has formed.
    // Implied by the lineage's rotation rule (see above); defensive.
    if (e_b3 > e_e && (!lineage.finalized_epoch || *lineage.finalized_epoch < e_b3 - 1)) {
        return fail(BurnGateRowV2::E6, "the handover into the lineage epoch is not certified on B3");
    }

    // E7: the token can still pay.
    if (!in.token) return fail(BurnGateRowV2::E7, "no TOKEN_FLAGS observation");
    if (const auto why{age_failure(in.token->t_obs, "TOKEN_FLAGS")}) {
        return fail(BurnGateRowV2::E7, *why);
    }
    const TokenFlagsV2& token{in.token->state};
    if (token.paused) return fail(BurnGateRowV2::E7, "token is paused");
    if (token.deprecated) return fail(BurnGateRowV2::E7, "token is deprecated");
    if (token.vault_blacklisted) return fail(BurnGateRowV2::E7, "vault is blacklisted by the token");

    // E8: a fresh reserve observation.
    if (!in.reserve) return fail(BurnGateRowV2::E8, "no RESERVE_E observation");
    if (const auto why{age_failure(in.reserve->t_obs, "RESERVE_E")}) {
        return fail(BurnGateRowV2::E8, *why);
    }

    // READY. The gate next closes at the earliest time row.
    out.ready = true;
    out.row = BurnGateRowV2::NONE;
    out.reason.clear();
    out.closes_at.reset();
    const auto consider{[&out](const std::optional<int64_t> at) {
        if (at && (!out.closes_at || *at < *out.closes_at)) out.closes_at = at;
    }};
    using burn_gate_detail::FirstPastLimit;
    consider(FirstPastLimit(in.verifier->t_obs, gate.observation_max_age_seconds));
    consider(FirstPastLimit(in.token->t_obs, gate.observation_max_age_seconds));
    consider(FirstPastLimit(in.reserve->t_obs, gate.observation_max_age_seconds));
    consider(FirstPastLimit(verifier.last_rotation_time, gate.RotationAgeLimit()));
    return out;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_BURN_GATE_V2_H

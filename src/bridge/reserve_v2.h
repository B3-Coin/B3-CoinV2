// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_RESERVE_V2_H
#define B3COIN_BRIDGE_RESERVE_V2_H

#include <bridge/state_observation.h>
#include <consensus/amount.h>
#include <util/overflow.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>

/**
 * Bridge V2 per-vault reserve accounting (doc/design/b3-bridge-v2-node-changes.md):
 * - the reserve ledger (section 7.1);
 * - check 2 of a burn, the reserve bound, and check 3, the wind-down bound
 *   (section 7.2);
 * - invariant R (section 8.4);
 * - the amount of a B-23 migration or sweep leaf, its transit production,
 *   and transit crediting on proven arrival (the accounting part of
 *   section 7.6).
 *
 * Pure: no chain state and no I/O. The bridge index (node::BridgeStateIndex)
 * keeps the ledger and the proven-deposit index under undo. It applies each
 * update at the record it belongs to and supplies the latest RESERVE_E
 * observation (bridge/state_observation.h). Every amount is a raw bUSD V2
 * unit (six decimals, the USDT unit). Arithmetic is exact or refused: a
 * negative input, or a sum that would leave the CAmount range, refuses the
 * operation (nullopt or false). Nothing saturates or wraps.
 */
namespace bridge {

/**
 * One vault entry's ledger (section 7.1). At launch there is one entry; a
 * B-23 migration adds the successor v' with a ledger of its own. Supply <=
 * the sum over entries of reserve + transit. A plain burn lowers supply
 * only, and the ledger never sees it.
 */
struct ReserveLedgerV2 {
    //! Sum of the bUSD V2 minted against this entry's proven deposits.
    CAmount minted{0};
    //! burned_v: the sum of the leaf V2 amounts naming this entry.
    CAmount burned{0};
    //! C_v: migration amounts credited to this entry on proven arrival
    //! (section 7.6). It never counts transit.
    CAmount credited{0};
    //! transit_v: amounts paid toward this entry and not yet credited.
    CAmount transit{0};

    //! Every field is non-negative, minted + credited stays in range, and
    //! burned <= minted + credited (the reserve is never negative).
    bool Valid() const
    {
        if (minted < 0 || burned < 0 || credited < 0 || transit < 0) return false;
        const auto backing{CheckedAdd(minted, credited)};
        return backing && burned <= *backing;
    }

    //! reserve_v = minted + credited - burned. nullopt unless Valid().
    std::optional<CAmount> Reserve() const
    {
        if (!Valid()) return std::nullopt;
        return minted + credited - burned;
    }

    friend bool operator==(const ReserveLedgerV2&, const ReserveLedgerV2&) = default;
};

/**
 * Index of proven deposit amounts by Vault V2 deposit id (section 7.1). Each
 * vault entry keeps its own index, so within one index the id alone is the
 * nullifier's distinguishing part.
 */
using ProvenDepositMapV2 = std::map<uint64_t, CAmount>;

/**
 * Proven deposits split at an observation's cut, its `nextDepositId`
 * (section 7.2): P_pre sums ids below the cut and P_post ids at or above it.
 * `cut` records which observation the split belongs to.
 */
struct ProvenDepositSumsV2 {
    uint64_t cut{0};
    CAmount below_cut{0};
    CAmount from_cut{0};

    friend bool operator==(const ProvenDepositSumsV2&, const ProvenDepositSumsV2&) = default;
};

/**
 * Sums the proven deposits of `base` (the connected index) and `overlay`
 * (deposits proven by the block or preview being evaluated) at `cut`, with
 * CheckedAdd. The reference form: O(N) over both maps. Consensus uses
 * SplitProvenDepositsAtCut, which must equal it. nullopt when an amount is not positive (admission mints only
 * amounts above zero), when an id is in both maps (the nullifier set
 * forbids a second mint, so a duplicate means a corrupt index), or when a
 * sum leaves the CAmount range.
 */
inline std::optional<ProvenDepositSumsV2> SumProvenDeposits(
    const ProvenDepositMapV2& base, const ProvenDepositMapV2& overlay,
    const uint64_t cut)
{
    ProvenDepositSumsV2 sums;
    sums.cut = cut;
    const auto add{[&sums, cut](const uint64_t id, const CAmount amount) {
        if (amount <= 0) return false;
        CAmount& total{id < cut ? sums.below_cut : sums.from_cut};
        const auto next{CheckedAdd(total, amount)};
        if (!next) return false;
        total = *next;
        return true;
    }};
    for (const auto& [id, amount] : base) {
        if (!add(id, amount)) return std::nullopt;
    }
    for (const auto& [id, amount] : overlay) {
        if (base.contains(id) || !add(id, amount)) return std::nullopt;
    }
    return sums;
}

/**
 * SumProvenDeposits from running totals, the form consensus uses: walks only
 * the ids at or above `cut` (map lower_bound), so the cost is
 * O(log N + K) for K deposits at or above the cut instead of O(N). `cut` is
 * a recent RESERVE_E's nextDepositId, so K counts only deposits made after
 * that observation's anchor. `base_total` and `overlay_total` are the sums of
 * every amount of each map, which the bridge index keeps under undo
 * (node::BridgeStateIndex::ProvenDepositsTotalV2) and the block's scratch
 * state as it proves deposits. P_post sums the walked ids and
 * P_pre = base_total + overlay_total - P_post.
 *
 * Equal to SumProvenDeposits(base, overlay, cut) whenever each total is its
 * map's sum, every amount is positive and no id is in both maps (unit
 * tested). nullopt when a walked amount is not positive, a walked overlay id
 * is also in `base`, a total is negative, a sum leaves the CAmount range, or
 * P_post exceeds the totals (inconsistent totals). The nullifier set and
 * the mint path keep every amount positive and every id unique, so these
 * refusals are defensive.
 */
inline std::optional<ProvenDepositSumsV2> SplitProvenDepositsAtCut(
    const ProvenDepositMapV2& base, const CAmount base_total,
    const ProvenDepositMapV2& overlay, const CAmount overlay_total,
    const uint64_t cut)
{
    if (base_total < 0 || overlay_total < 0) return std::nullopt;
    const auto total{CheckedAdd(base_total, overlay_total)};
    if (!total) return std::nullopt;
    CAmount from_cut{0};
    const auto add{[&from_cut](const CAmount amount) {
        if (amount <= 0) return false;
        const auto next{CheckedAdd(from_cut, amount)};
        if (!next) return false;
        from_cut = *next;
        return true;
    }};
    for (auto it{base.lower_bound(cut)}; it != base.end(); ++it) {
        if (!add(it->second)) return std::nullopt;
    }
    for (auto it{overlay.lower_bound(cut)}; it != overlay.end(); ++it) {
        if (base.contains(it->first) || !add(it->second)) return std::nullopt;
    }
    if (from_cut > *total) return std::nullopt;
    return ProvenDepositSumsV2{cut, *total - from_cut, from_cut};
}

/** minted += amount, for a V2 mint the bridge index admitted. False, with
 * the ledger unchanged, for a non-positive amount, an invalid ledger or an
 * overflow. */
[[nodiscard]] inline bool RecordMintV2(ReserveLedgerV2& ledger, const CAmount amount)
{
    if (amount <= 0 || !ledger.Valid()) return false;
    ReserveLedgerV2 next{ledger};
    const auto minted{CheckedAdd(ledger.minted, amount)};
    if (!minted) return false;
    next.minted = *minted;
    if (!next.Valid()) return false;
    ledger = next;
    return true;
}

/**
 * Check 2 of section 7.2: 0 < amount <= reserve_v. The caller has already
 * added the block's earlier burns of this entry to `ledger.burned`, so the
 * bound is "after earlier burns in the block".
 */
inline bool BurnWithinReserve(const CAmount amount, const ReserveLedgerV2& ledger)
{
    const auto reserve{ledger.Reserve()};
    return amount > 0 && reserve && amount <= *reserve;
}

/** burned += amount for a burn that passed every check. False, with the
 * ledger unchanged, unless BurnWithinReserve(amount, ledger). */
[[nodiscard]] inline bool RecordBurnV2(ReserveLedgerV2& ledger, const CAmount amount)
{
    if (!BurnWithinReserve(amount, ledger)) return false;
    ledger.burned += amount; // <= minted + credited, checked above
    return true;
}

/**
 * transit += amount on the successor's ledger, for a migration or sweep leaf
 * (section 7.6) that the bridge index appended: the leaf names the retired
 * entry, whose burned_v counts it (RecordBurnV2), and pays the successor's
 * vault, where the amount counts in none of reserve_v', invariant R or R_v'
 * until CreditTransit moves it. False, with the ledger unchanged, for a
 * non-positive amount, an invalid ledger or an overflow of transit.
 */
[[nodiscard]] inline bool RecordTransitV2(ReserveLedgerV2& ledger, const CAmount amount)
{
    if (amount <= 0 || !ledger.Valid()) return false;
    const auto transit{CheckedAdd(ledger.transit, amount)};
    if (!transit) return false;
    ledger.transit = *transit; // transit does not enter Valid()'s backing sum
    return true;
}

/**
 * Credit on proven arrival (section 7.6): a RESERVE_E observation moves
 * min(transit_v, max(0, absorbedTotal - C_v)) from transit into credited.
 * So nothing is credited before Ethereum's absorb() counted it, and the
 * credit never exceeds what absorb() counted. Returns the amount moved,
 * which may be zero. nullopt, with the ledger unchanged, for an invalid
 * ledger or a negative `absorbed_total`.
 */
inline std::optional<CAmount> CreditTransit(ReserveLedgerV2& ledger, const CAmount absorbed_total)
{
    if (!ledger.Valid() || absorbed_total < 0) return std::nullopt;
    // Both are non-negative, so the difference cannot overflow, and
    // credited + moved <= absorbed_total stays in range.
    const CAmount uncredited{std::max<CAmount>(0, absorbed_total - ledger.credited)};
    const CAmount moved{std::min(ledger.transit, uncredited)};
    ReserveLedgerV2 next{ledger};
    next.transit -= moved;
    next.credited += moved;
    if (!next.Valid()) return std::nullopt;
    ledger = next;
    return moved;
}

/**
 * Invariant R (section 8.4): observed locked >= P + C_v - burned_v, where P
 * sums the proven deposits below the observation's nextDepositId (P_pre).
 * The comparison is the exact one, locked + burned >= P + C_v. Each side is
 * a sum of two non-negative int64 values, so it fits in uint64. False for
 * an invalid ledger or a negative input. The caller turns false into the
 * sticky E8 conflict, and an invalid input cannot reach it.
 */
inline bool InvariantRHolds(const CAmount locked, const CAmount p_below,
                            const ReserveLedgerV2& ledger)
{
    if (!ledger.Valid() || locked < 0 || p_below < 0) return false;
    const uint64_t have{static_cast<uint64_t>(locked) + static_cast<uint64_t>(ledger.burned)};
    const uint64_t owed{static_cast<uint64_t>(p_below) + static_cast<uint64_t>(ledger.credited)};
    return have >= owed;
}

/** locked - (P + C_v - burned_v), the slack InvariantRHolds tests is at
 * least zero (for getbridgev2info). nullopt for an invalid input or when
 * the margin leaves the CAmount range. */
inline std::optional<CAmount> InvariantRMarginV2(const CAmount locked, const CAmount p_below,
                                                 const ReserveLedgerV2& ledger)
{
    if (!ledger.Valid() || locked < 0 || p_below < 0) return std::nullopt;
    const auto owed{CheckedAdd(p_below, ledger.credited)};
    if (!owed) return std::nullopt;
    // burned and owed are non-negative, so their difference is in range.
    return CheckedAdd(locked, ledger.burned - *owed);
}

/**
 * margin_v of section 8.6: zero while the vault's USDT balance B covers
 * `locked` L, else ceil(B * ppm / 1,000,000), computed exactly as
 * (B / 1e6) * ppm + ceil((B % 1e6) * ppm / 1e6), so it never overflows.
 * nullopt for a negative amount or ppm above 1,000,000.
 */
inline std::optional<CAmount> WindDownMarginV2(const ReserveStateV2& reserve, const uint32_t ppm)
{
    constexpr CAmount PPM_DENOMINATOR{1'000'000};
    if (reserve.balance < 0 || reserve.locked < 0 || ppm > PPM_DENOMINATOR) return std::nullopt;
    if (reserve.balance >= reserve.locked) return CAmount{0};
    const CAmount quotient{reserve.balance / PPM_DENOMINATOR};
    const CAmount remainder{reserve.balance % PPM_DENOMINATOR};
    return quotient * ppm + (remainder * ppm + PPM_DENOMINATOR - 1) / PPM_DENOMINATOR;
}

/** The terms of the wind-down bound (section 7.2 check 3), for one entry. */
struct WindDownTermsV2 {
    //! observed_balance_v = B + P_post: fresh deposits redeem at once.
    CAmount observed{0};
    //! R_v = max(0, P_pre + C_v - L): a lower bound on the vault's releases
    //! up to the observation.
    CAmount released_lower_bound{0};
    //! unreleased_v = max(0, burned_v - R_v), the block's earlier burns
    //! included through the ledger.
    CAmount unreleased{0};
    //! margin_v (WindDownMarginV2).
    CAmount margin{0};
    //! observed - margin - unreleased: the largest burn the bound admits
    //! now. Negative when even the leaves already appended exceed it.
    CAmount headroom{0};

    friend bool operator==(const WindDownTermsV2&, const WindDownTermsV2&) = default;
};

/**
 * The wind-down terms from the latest RESERVE_E observation, the deposit
 * sums at its cut and the ledger (earlier burns of the block included).
 * nullopt for an invalid ledger, a negative amount, sums split at another
 * cut than the observation's nextDepositId, ppm above 1,000,000, or a term
 * that leaves the CAmount range.
 */
inline std::optional<WindDownTermsV2> ComputeWindDownTerms(
    const ReserveStateV2& reserve, const ProvenDepositSumsV2& sums,
    const ReserveLedgerV2& ledger, const uint32_t ppm)
{
    if (!ledger.Valid() || reserve.balance < 0 || reserve.locked < 0 ||
        sums.below_cut < 0 || sums.from_cut < 0 ||
        sums.cut != reserve.next_deposit_id) {
        return std::nullopt;
    }
    const auto margin{WindDownMarginV2(reserve, ppm)};
    const auto observed{CheckedAdd(reserve.balance, sums.from_cut)};
    const auto backed{CheckedAdd(sums.below_cut, ledger.credited)};
    if (!margin || !observed || !backed) return std::nullopt;
    WindDownTermsV2 terms;
    terms.observed = *observed;
    terms.margin = *margin;
    // Differences of non-negative values below stay in range, and
    // margin <= B <= observed.
    terms.released_lower_bound = std::max<CAmount>(0, *backed - reserve.locked);
    terms.unreleased = std::max<CAmount>(0, ledger.burned - terms.released_lower_bound);
    terms.headroom = terms.observed - terms.margin - terms.unreleased;
    return terms;
}

/**
 * Check 3 of section 7.2, the wind-down bound:
 * amount + unreleased_v <= observed_balance_v - margin_v, with the block's
 * earlier burns already in `ledger.burned`. False for a non-positive amount
 * or whenever ComputeWindDownTerms refuses.
 */
inline bool WindDownAllows(const CAmount amount, const ReserveStateV2& reserve,
                           const ProvenDepositSumsV2& sums,
                           const ReserveLedgerV2& ledger, const uint32_t ppm)
{
    const auto terms{ComputeWindDownTerms(reserve, sums, ledger, ppm)};
    return amount > 0 && terms && amount <= terms->headroom;
}

/**
 * The amount of a B-23 migration or sweep leaf (section 7.6): the retired
 * entry's reserve_v, capped by the wind-down headroom of its latest
 * RESERVE_E (check 3 of section 7.2), so the leaf never asks the vault for
 * more than the bound admits. With the vault holding what it locks the
 * headroom covers reserve_v and the leaf moves all of it. After a partial
 * seizure (balance below locked) the leaf moves what the vault can still
 * pay and leaves only the shortfall in reserve_v, which a later sweep
 * moves as far as a later observation allows. nullopt when nothing can be
 * swept: reserve_v or the headroom is not positive, or ComputeWindDownTerms
 * refuses. The result passes WindDownAllows and BurnWithinReserve.
 */
inline std::optional<CAmount> VaultSweepAmountV2(const ReserveLedgerV2& retired,
                                                 const ReserveStateV2& reserve,
                                                 const ProvenDepositSumsV2& sums,
                                                 const uint32_t ppm)
{
    const auto reserve_v{retired.Reserve()};
    const auto terms{ComputeWindDownTerms(reserve, sums, retired, ppm)};
    if (!reserve_v || !terms || *reserve_v <= 0 || terms->headroom <= 0) return std::nullopt;
    return std::min(*reserve_v, terms->headroom);
}

/**
 * The accounting of one migration or sweep leaf (section 7.6), on both
 * ledgers or on neither: the leaf names the retired entry, so `retired`'s
 * burned_v counts the amount (RecordBurnV2: 0 < amount <= reserve_v), and
 * it pays the successor's vault, so `successor`'s transit receives it
 * (RecordTransitV2). Nothing is burned: the sum over both entries of
 * reserve + transit is unchanged. False, with both ledgers unchanged, when
 * either update refuses or both arguments are the same ledger.
 */
[[nodiscard]] inline bool RecordSweepV2(ReserveLedgerV2& retired, ReserveLedgerV2& successor,
                                        const CAmount amount)
{
    if (&retired == &successor) return false;
    ReserveLedgerV2 next_retired{retired};
    ReserveLedgerV2 next_successor{successor};
    if (!RecordBurnV2(next_retired, amount) || !RecordTransitV2(next_successor, amount)) return false;
    retired = next_retired;
    successor = next_successor;
    return true;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_RESERVE_V2_H

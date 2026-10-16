// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_ENGINE_ENGINE_H
#define B3_FLOWMESH2_ENGINE_ENGINE_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** The slot engine of FlowMesh V2 (slice R7; ARCHITECTURE.md sections 3 and
 * 11; plan section 3.5). PROVISIONAL REGTEST (D4, D5, D6, D7).
 *
 * This part: the canonical order of a slot's originals and the phase each
 * action kind executes in.
 *
 * Canonical order (O-4, O-4b). The originals of a slot at height h whose
 * parent header hash is p are first sorted by
 *   OrderKey(p, h, id) = Tagged(ORDER, p || u64 LE h || ActionId),
 * ties (only possible for a SHA-256 collision) by ActionId. Then every
 * account's positions in that sequence are given to its own actions in
 * ascending nonce. So the order across accounts depends on the parent
 * header, which no client knows when it signs, while one account's actions
 * always execute in nonce order. Plan section 3.5 seeds with the parent
 * CERTIFICATE hash; a BLS certificate is not unique (every quorum subset
 * aggregates differently), so a leader could grind it. The outline seeds
 * with the parent HEADER hash and the height instead; the owner ratifies or
 * replaces this under D4 (M3-ENG-12).
 *
 * Determinism: tagged SHA-256, ordered containers, no clock, I/O or
 * randomness. Nothing here takes a lock.
 */
namespace flowmesh2::engine {

//! What the canonical order needs to know about one original.
struct OrderingEntry {
    ActionId id;
    AccountId account;
    uint64_t nonce{0};
    friend bool operator==(const OrderingEntry&, const OrderingEntry&) = default;
};

//! Tagged(ORDER, parent_header_hash || u64 LE height || ActionId).
uint256 OrderKey(const uint256& parent_header_hash, uint64_t height, const ActionId& id);

/** The canonical order of `originals` in the slot at `height` whose parent
 * header hash is `parent_header_hash` (see the file comment). The result has
 * one entry per original: result[r] is the index into `originals` of the
 * action at canonical rank r. It does not depend on the order of the span.
 * INVALID_INPUT for a repeated ActionId or a repeated (account, nonce); the
 * validity predicate (ValidateSlotInputs) refuses both first.
 */
// TODO(rebirth:M3-ENG-12) seed hardening: grinding tests; ratify the parent-header seed or replace it (VRF, unique threshold signature).
Result<std::vector<size_t>> CanonicalOrder(std::span<const OrderingEntry> originals, const uint256& parent_header_hash,
                                           uint64_t height);

//! CanonicalOrder over verified actions: {Id(), Account(), Action().nonce}.
Result<std::vector<size_t>> CanonicalOrder(std::span<const VerifiedAction> originals, const uint256& parent_header_hash,
                                           uint64_t height);

/** The phases of one slot (plan section 3.5). APPEND-ONLY registry; zero is
 * never a value. Engine::ApplySlot runs them in this order; inside a phase,
 * actions run in canonical order.
 */
enum class SlotPhase : uint8_t {
    DEPOSITS = 1,     //!< chain facts: deposits credit accounts
    ACCOUNT = 2,      //!< TRANSFER (later WITHDRAW, M2-LED-05)
    CANCELS = 3,      //!< CANCEL by OrderId (later CANCEL_ALL, M3-ENG-09)
    POST_ONLY = 4,    //!< PLACE POST_ONLY: rest, or REJ_WOULD_CROSS
    TAKERS = 5,       //!< PLACE GTC/IOC: reserve all, then one A2 sweep per market
    REST = 6,         //!< GTC remainders rest; IOC and refused remainders are released
    FUTURES = 7,      //!< futures sweep (M9-FUT-04); only with futures_enabled
    FEES = 8,         //!< one FEES_DISTRIBUTED outcome per quote asset
    CONSERVATION = 9, //!< conservation and slot-boundary invariants, then roots
};
bool IsKnown(SlotPhase phase);
//! Registry name; "UNKNOWN" for an unregistered value.
std::string_view ToString(SlotPhase phase);

/** The phase in which `action` executes: TRANSFER in ACCOUNT, CANCEL in
 * CANCELS, a POST_ONLY PLACE in POST_ONLY, a GTC or IOC PLACE in TAKERS. A
 * PLACE whose payload does not decode returns the decoder's error. The
 * other kinds are the engine branches the outline leaves as stubs; the
 * validity predicate refuses them first (KindSupported), so a validated
 * slot never reaches them:
 * - CANCEL_ALL: NotImplemented(M3-ENG-09) (phase CANCELS once done);
 * - WITHDRAW: NotImplemented(M2-LED-05) (phase ACCOUNT once done);
 * - SPOT_TO_FUTURES, FUTURES_TO_SPOT: NotImplemented(M2-LED-06).
 * INVALID_INPUT for an unregistered kind.
 */
Result<SlotPhase> PhaseOf(const ActionV2& action);

/** The arrival_rank (engine/book.h) of an order that starts resting in the
 * slot at `height`, in `phase` (POST_ONLY or REST), at canonical `rank`:
 *   (2 * height + (phase == REST ? 1 : 0)) * max_actions_per_slot + rank.
 * Ranks therefore grow strictly over the life of every market: by slot,
 * then post-only placements before resting remainders, then canonical rank
 * (O-35, O-39). A post-only order rests in phase 4, before a same-slot
 * remainder in phase 6 at the same level, so it must queue ahead of it even
 * with a higher canonical rank. INVALID_INPUT for another phase, a zero cap
 * or rank >= max_actions_per_slot; ARITHMETIC_OVERFLOW above 2^64-1.
 * PROVISIONAL: the stride assumes a fixed max_actions_per_slot; changing it
 * at a versioned height (M7-GOV-03) must keep the ranks increasing.
 */
Result<uint64_t> ArrivalRank(uint64_t height, SlotPhase phase, uint64_t rank, uint32_t max_actions_per_slot);

} // namespace flowmesh2::engine

#endif // B3_FLOWMESH2_ENGINE_ENGINE_H

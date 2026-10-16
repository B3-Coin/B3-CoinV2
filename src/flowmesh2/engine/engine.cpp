// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/engine.h>

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/hashes.h>
#include <hash.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <utility>

namespace flowmesh2::engine {

namespace {

constexpr Error Invalid(const char* detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

} // namespace

// ---- Canonical order -----------------------------------------------------------

uint256 OrderKey(const uint256& parent_header_hash, uint64_t height, const ActionId& id)
{
    HashWriter writer{TaggedWriter(HashTag::ORDER)};
    writer << parent_header_hash << height << id;
    return writer.GetSHA256();
}

Result<std::vector<size_t>> CanonicalOrder(std::span<const OrderingEntry> originals, const uint256& parent_header_hash,
                                           uint64_t height)
{
    std::set<ActionId> ids;
    std::set<std::pair<AccountId, uint64_t>> nonces;
    for (const OrderingEntry& entry : originals) {
        if (!ids.insert(entry.id).second) return Invalid("repeated ActionId in the canonical order");
        if (!nonces.emplace(entry.account, entry.nonce).second) {
            return Invalid("repeated (account, nonce) in the canonical order");
        }
    }

    // 1. Sort by the seeded key; ties (a SHA-256 collision) by ActionId, which is unique.
    std::vector<uint256> keys;
    keys.reserve(originals.size());
    for (const OrderingEntry& entry : originals) keys.push_back(OrderKey(parent_header_hash, height, entry.id));
    std::vector<size_t> seeded(originals.size());
    std::iota(seeded.begin(), seeded.end(), size_t{0});
    std::sort(seeded.begin(), seeded.end(), [&](size_t a, size_t b) {
        if (keys[a] != keys[b]) return keys[a] < keys[b];
        return originals[a].id < originals[b].id;
    });

    // 2. Each account keeps the positions it drew and fills them in ascending nonce.
    std::map<AccountId, std::vector<size_t>> positions;  // ascending ranks
    std::map<AccountId, std::vector<size_t>> by_nonce;   // indices, sorted below
    for (size_t rank{0}; rank < seeded.size(); ++rank) {
        const OrderingEntry& entry{originals[seeded[rank]]};
        positions[entry.account].push_back(rank);
        by_nonce[entry.account].push_back(seeded[rank]);
    }
    std::vector<size_t> order(originals.size());
    for (auto& [account, indices] : by_nonce) {
        std::sort(indices.begin(), indices.end(),
                  [&](size_t a, size_t b) { return originals[a].nonce < originals[b].nonce; });
        const std::vector<size_t>& ranks{positions.at(account)};
        for (size_t k{0}; k < indices.size(); ++k) order[ranks[k]] = indices[k];
    }
    return Ok(std::move(order));
}

Result<std::vector<size_t>> CanonicalOrder(std::span<const VerifiedAction> originals, const uint256& parent_header_hash,
                                           uint64_t height)
{
    std::vector<OrderingEntry> entries;
    entries.reserve(originals.size());
    for (const VerifiedAction& action : originals) {
        entries.push_back(OrderingEntry{action.Id(), action.Account(), action.Action().nonce});
    }
    return CanonicalOrder(std::span<const OrderingEntry>{entries}, parent_header_hash, height);
}

// ---- Phases ---------------------------------------------------------------------

bool IsKnown(SlotPhase phase)
{
    switch (phase) {
    case SlotPhase::DEPOSITS:
    case SlotPhase::ACCOUNT:
    case SlotPhase::CANCELS:
    case SlotPhase::POST_ONLY:
    case SlotPhase::TAKERS:
    case SlotPhase::REST:
    case SlotPhase::FUTURES:
    case SlotPhase::FEES:
    case SlotPhase::CONSERVATION:
        return true;
    }
    return false;
}

std::string_view ToString(SlotPhase phase)
{
    switch (phase) {
    case SlotPhase::DEPOSITS: return "DEPOSITS";
    case SlotPhase::ACCOUNT: return "ACCOUNT";
    case SlotPhase::CANCELS: return "CANCELS";
    case SlotPhase::POST_ONLY: return "POST_ONLY";
    case SlotPhase::TAKERS: return "TAKERS";
    case SlotPhase::REST: return "REST";
    case SlotPhase::FUTURES: return "FUTURES";
    case SlotPhase::FEES: return "FEES";
    case SlotPhase::CONSERVATION: return "CONSERVATION";
    }
    return "UNKNOWN";
}

Result<SlotPhase> PhaseOf(const ActionV2& action)
{
    switch (action.kind) {
    case ActionKind::PLACE: {
        const Result<PlacePayload> place{action.Place()};
        if (!place.has_value()) return place.error();
        return Ok(place->tif == TimeInForce::POST_ONLY ? SlotPhase::POST_ONLY : SlotPhase::TAKERS);
    }
    case ActionKind::CANCEL:
        return Ok(SlotPhase::CANCELS);
    case ActionKind::TRANSFER:
        return Ok(SlotPhase::ACCOUNT);
    case ActionKind::CANCEL_ALL:
        return NotImplemented("M3-ENG-09"); // TODO(rebirth:M3-ENG-09) CANCEL_ALL in phase 3: release every order of the account
    case ActionKind::WITHDRAW:
        return NotImplemented("M2-LED-05"); // TODO(rebirth:M2-LED-05) WITHDRAW in phase 2 through LedgerState::RequestWithdrawal
    case ActionKind::SPOT_TO_FUTURES:
    case ActionKind::FUTURES_TO_SPOT:
        return NotImplemented("M2-LED-06"); // TODO(rebirth:M2-LED-06) spot/futures cash transfers in phase 2
    }
    return Invalid("unregistered action kind");
}

Result<uint64_t> ArrivalRank(uint64_t height, SlotPhase phase, uint64_t rank, uint32_t max_actions_per_slot)
{
    if (phase != SlotPhase::POST_ONLY && phase != SlotPhase::REST) return Invalid("arrival rank outside phases 4 and 6");
    if (max_actions_per_slot == 0) return Invalid("zero max_actions_per_slot");
    if (rank >= max_actions_per_slot) return Invalid("canonical rank at or above max_actions_per_slot");
    const Result<uint64_t> doubled{MulChecked(height, 2)};
    if (!doubled.has_value()) return doubled.error();
    const uint64_t block_index{*doubled + (phase == SlotPhase::REST ? 1 : 0)}; // doubled is even, so +1 cannot wrap
    const Result<uint64_t> block_start{MulChecked(block_index, max_actions_per_slot)};
    if (!block_start.has_value()) return block_start.error();
    if (rank > UINT64_MAX - *block_start) return Error{ErrorCode::ARITHMETIC_OVERFLOW, "arrival rank above 2^64-1"};
    return Ok(*block_start + rank);
}

} // namespace flowmesh2::engine

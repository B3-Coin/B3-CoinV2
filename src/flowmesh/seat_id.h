// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_SEAT_ID_H
#define B3COIN_FLOWMESH_SEAT_ID_H

#include <crypto/bls.h>
#include <crypto/common.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>

namespace flowmesh {

//! Domain-bound identifier of an active FN seat. The chain-derived seat
//! tracker owns construction from an FN outpoint and supplies canonical order.
using SeatId = uint256;

inline constexpr const char* FLOWMESH_SEAT_ID_TAG{"B3/FLOWMESH/SEAT/V1"};

//! SeatId(O) = TaggedHash("B3/FLOWMESH/SEAT/V1", domain || O.outpoint).
inline SeatId ComputeFlowMeshSeatId(const uint256& domain, const COutPoint& outpoint)
{
    HashWriter writer{TaggedHash(FLOWMESH_SEAT_ID_TAG)};
    writer << std::span<const unsigned char>{domain.begin(), 32} << outpoint;
    return writer.GetSHA256();
}

/** Raw, canonically ordered member committed by an anchored FlowMesh set. */
struct FlowMeshSeatSetMember {
    SeatId seat_id;
    COutPoint outpoint;
    std::array<unsigned char, bls::PUBKEY_SIZE> public_key{};

    friend bool operator==(const FlowMeshSeatSetMember& a,
                           const FlowMeshSeatSetMember& b)
    {
        return a.seat_id == b.seat_id && a.outpoint == b.outpoint &&
               a.public_key == b.public_key;
    }
};

inline constexpr const char* FLOWMESH_SEAT_SET_TAG{"B3/FLOWMESH/SEATSET/V1"};

//! Frozen quorum committed into every seat-set hash: floor(2*k/3)+1.
inline constexpr uint32_t FlowMeshSeatSetThreshold(const uint32_t k)
{
    return static_cast<uint32_t>((2ULL * k) / 3ULL + 1ULL);
}

/**
 * Mandatory anchored seat-set commitment. Input must be strict
 * `(SeatId,outpoint)` order, every SeatId must be derived from `domain`, keys
 * must be unique, and at most 5,000 seats may be committed. Returning
 * nullopt rather than sorting is deliberate: bitmap position is consensus
 * identity and no caller may choose an alternate ordering.
 *
 * TaggedHash("B3/FLOWMESH/SEATSET/V1",
 *   domain || market || epoch_be || anchor_height_be || anchor_hash ||
 *   member_count_be || threshold_be ||
 *   each(seat_id || txid || vout_be || bls_pubkey))
 */
inline std::optional<uint256> ComputeFlowMeshSeatSetHash(
    const uint256& domain, const uint256& market_id, const uint64_t epoch,
    const uint64_t anchor_height, const uint256& anchor_hash,
    const std::span<const FlowMeshSeatSetMember> members)
{
    if (members.size() > 5000 || members.size() > UINT32_MAX) return std::nullopt;
    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> unique_keys;
    for (size_t i{0}; i < members.size(); ++i) {
        const FlowMeshSeatSetMember& member{members[i]};
        if (member.seat_id != ComputeFlowMeshSeatId(domain, member.outpoint)) {
            return std::nullopt;
        }
        if (i > 0) {
            const FlowMeshSeatSetMember& previous{members[i - 1]};
            if (!(previous.seat_id < member.seat_id ||
                  (previous.seat_id == member.seat_id &&
                   previous.outpoint < member.outpoint))) {
                return std::nullopt;
            }
        }
        if (!unique_keys.insert(member.public_key).second) return std::nullopt;
    }

    std::array<unsigned char, 24> numbers{};
    WriteBE64(numbers.data(), epoch);
    WriteBE64(numbers.data() + 8, anchor_height);
    const uint32_t count{static_cast<uint32_t>(members.size())};
    WriteBE32(numbers.data() + 16, count);
    WriteBE32(numbers.data() + 20, FlowMeshSeatSetThreshold(count));

    HashWriter writer{TaggedHash(FLOWMESH_SEAT_SET_TAG)};
    writer << std::span<const unsigned char>{domain.begin(), 32};
    writer << std::span<const unsigned char>{market_id.begin(), 32};
    writer << std::span<const unsigned char>{numbers.data(), 16};
    writer << std::span<const unsigned char>{anchor_hash.begin(), 32};
    writer << std::span<const unsigned char>{numbers.data() + 16, 8};
    for (const FlowMeshSeatSetMember& member : members) {
        writer << std::span<const unsigned char>{member.seat_id.begin(), 32};
        writer << member.outpoint.hash;
        std::array<unsigned char, 4> vout{};
        WriteBE32(vout.data(), member.outpoint.n);
        writer << std::span<const unsigned char>{vout};
        writer << std::span<const unsigned char>{member.public_key};
    }
    return writer.GetSHA256();
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_SEAT_ID_H

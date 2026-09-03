// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_PRODUCTION_COMMITMENT_H
#define B3COIN_FLOWMESH_PRODUCTION_COMMITMENT_H

#include <crypto/common.h>
#include <flowmesh/market.h>
#include <hash.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace flowmesh {

inline constexpr uint16_t FLOWMESH_PRODUCTION_ENTRY_VERSION_V1{1};
inline constexpr const char* FLOWMESH_PRODUCTION_ENTRY_TAG{
    "B3/FLOWMESH/ENTRY/V1"};

enum class ProductionEntryKind : uint8_t {
    EXECUTION = 1,
    EPOCH_HANDOFF = 2,
};

struct ProductionCommitmentAnchorV1 {
    uint64_t height{0};
    uint256 block_hash;

    friend bool operator==(const ProductionCommitmentAnchorV1& a,
                           const ProductionCommitmentAnchorV1& b) = default;
};

/**
 * The compact, reconstructible identity signed by one production
 * certificate. The full action vector is represented exactly once by
 * `actions_root`; deterministic execution is represented by result/state and
 * typed-effect commitments. No publication-only B3 checkpoint link appears
 * here.
 */
struct ProductionEntryCommitmentV1 {
    uint16_t version{FLOWMESH_PRODUCTION_ENTRY_VERSION_V1};
    uint8_t kind{static_cast<uint8_t>(ProductionEntryKind::EXECUTION)};
    uint256 domain;
    MarketId market_id;
    uint64_t epoch{0};
    uint256 seat_set_hash;
    uint64_t sequence{0};
    uint256 parent_hash;
    ProductionCommitmentAnchorV1 production_anchor;
    uint256 previous_state_root;
    uint256 actions_root;
    uint256 result_root;
    uint256 state_root;
    uint64_t effect_start{0};
    uint32_t effect_count{0};
    uint256 effect_root;
    uint64_t next_epoch{0};
    ProductionCommitmentAnchorV1 next_seat_anchor;
    uint256 next_seat_set_hash;

    friend bool operator==(const ProductionEntryCommitmentV1& a,
                           const ProductionEntryCommitmentV1& b) = default;
};

/**
 * Fixed-width, endian-stable signed identity. This is deliberately not the
 * serialized production entry: large action bodies are committed through
 * `actions_root`, allowing a compact type-8 record to reconstruct the exact
 * BLS-signed message.
 */
inline std::optional<uint256> ComputeProductionEntryIdentityV1(
    const ProductionEntryCommitmentV1& entry)
{
    const bool execution{
        entry.kind == static_cast<uint8_t>(ProductionEntryKind::EXECUTION)};
    const bool handoff{
        entry.kind == static_cast<uint8_t>(ProductionEntryKind::EPOCH_HANDOFF)};
    if (entry.version != FLOWMESH_PRODUCTION_ENTRY_VERSION_V1 ||
        (!execution && !handoff) || entry.domain.IsNull() ||
        entry.market_id.IsNull() || entry.seat_set_hash.IsNull() ||
        entry.production_anchor.block_hash.IsNull() ||
        entry.production_anchor.height >
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        entry.previous_state_root.IsNull() || entry.actions_root.IsNull() ||
        entry.result_root.IsNull() || entry.state_root.IsNull() ||
        entry.effect_root.IsNull() ||
        entry.effect_start >
            std::numeric_limits<uint64_t>::max() - entry.effect_count ||
        (entry.sequence == 0 ? !entry.parent_hash.IsNull()
                             : entry.parent_hash.IsNull())) {
        return std::nullopt;
    }
    if (execution) {
        if (entry.next_epoch != 0 ||
            !entry.next_seat_anchor.block_hash.IsNull() ||
            !entry.next_seat_set_hash.IsNull()) {
            return std::nullopt;
        }
    } else if (entry.epoch == std::numeric_limits<uint64_t>::max() ||
               entry.next_epoch != entry.epoch + 1 ||
               entry.next_seat_anchor.block_hash.IsNull() ||
               entry.next_seat_anchor.height >
                   static_cast<uint64_t>(
                       std::numeric_limits<int32_t>::max()) ||
               entry.next_seat_set_hash.IsNull()) {
        return std::nullopt;
    }

    std::array<unsigned char, 55> numbers{};
    WriteBE16(numbers.data(), entry.version);
    numbers[2] = entry.kind;
    WriteBE64(numbers.data() + 3, entry.epoch);
    WriteBE64(numbers.data() + 11, entry.sequence);
    WriteBE64(numbers.data() + 19, entry.production_anchor.height);
    WriteBE64(numbers.data() + 27, entry.effect_start);
    WriteBE32(numbers.data() + 35, entry.effect_count);
    WriteBE64(numbers.data() + 39, entry.next_epoch);
    WriteBE64(numbers.data() + 47, entry.next_seat_anchor.height);

    HashWriter writer{TaggedHash(FLOWMESH_PRODUCTION_ENTRY_TAG)};
    writer << std::span<const unsigned char>{numbers.data(), 3}
           << std::span<const unsigned char>{entry.domain.begin(), 32}
           << std::span<const unsigned char>{entry.market_id.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 3, 8}
           << std::span<const unsigned char>{entry.seat_set_hash.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 11, 8}
           << std::span<const unsigned char>{entry.parent_hash.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 19, 8}
           << std::span<const unsigned char>{
                  entry.production_anchor.block_hash.begin(), 32}
           << std::span<const unsigned char>{entry.previous_state_root.begin(),
                                              32}
           << std::span<const unsigned char>{entry.actions_root.begin(), 32}
           << std::span<const unsigned char>{entry.result_root.begin(), 32}
           << std::span<const unsigned char>{entry.state_root.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 27, 12}
           << std::span<const unsigned char>{entry.effect_root.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 39, 8}
           << std::span<const unsigned char>{numbers.data() + 47, 8}
           << std::span<const unsigned char>{
                  entry.next_seat_anchor.block_hash.begin(), 32}
           << std::span<const unsigned char>{entry.next_seat_set_hash.begin(),
                                              32};
    return writer.GetSHA256();
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_PRODUCTION_COMMITMENT_H

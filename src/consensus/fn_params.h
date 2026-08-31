// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_CONSENSUS_FN_PARAMS_H
#define B3COIN_CONSENSUS_FN_PARAMS_H

#include <uint256.h>

#include <array>
#include <cstdint>

namespace Consensus {

//! Consensus hard cap on FN units ever issued (historical genesis plus all
//! later PoD issuance). Extinguishment never reopens issuance capacity.
inline constexpr uint32_t MAX_FN_EVER_ISSUED{5'000};

//! Historical rights already established by the equivalence-gated scan. This
//! is the minimum proven population the sealed manifest is allowed to carry;
//! the final through-H scan may contain more rows, but never fewer.
inline constexpr uint32_t HISTORICAL_FN_PROVEN_FLOOR{3'500};
static_assert(HISTORICAL_FN_PROVEN_FLOOR <= MAX_FN_EVER_ISSUED);

/**
 * One historical FN right in the sealed FN Genesis manifest.
 *
 * `pod_id` is the raw 32-byte transaction id of the qualifying historical
 * PoD. `recipient_key_hash` is the exact HASH160 carried by that PoD's legacy
 * P2PKH designation. Keeping this aggregate in the neutral consensus layer
 * lets chain parameters carry the pinned manifest without depending on any
 * modern validation implementation.
 */
struct FnGenesisRight {
    uint256 pod_id{};
    std::array<unsigned char, 20> recipient_key_hash{};

    friend bool operator==(const FnGenesisRight&, const FnGenesisRight&) = default;
};

} // namespace Consensus

#endif // B3COIN_CONSENSUS_FN_PARAMS_H

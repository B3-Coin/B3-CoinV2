// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_TEST_UTIL_ASSET_H
#define B3COIN_TEST_UTIL_ASSET_H

#include <modern/asset.h>
#include <modern/chain_domain.h>
#include <primitives/transaction.h>
#include <uint256.h>

namespace modern {
namespace test_only {

//! A deterministic synthetic asset id for model-level tests and benches
//! that only need DISTINCT ids (FlowMesh markets, vault fixtures): the
//! real v1 derivation over a fixed synthetic chain domain and a fixed
//! genesis record, keyed by the caller's outpoint. Never a mainnet id.
inline AssetId SyntheticAssetId(const COutPoint& outpoint)
{
    static const uint256 domain{*ModernChainDomain(
        uint256{"0000000000000000000000000000000000000000000000000000000000000001"},
        uint256{"0000000000000000000000000000000000000000000000000000000000000002"})};
    static const uint256 genesis{AssetGenesisCommitment(
        AssetGenesisV1{.max_supply = 1'000'000'000, .decimals = 0})};
    return AssetIdV1(domain, outpoint, genesis);
}

} // namespace test_only
} // namespace modern

#endif // B3COIN_TEST_UTIL_ASSET_H

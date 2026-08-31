// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_FN_H
#define B3COIN_MODERN_FN_H

#include <hash.h>
#include <modern/policy.h>
#include <uint256.h>

namespace modern {

/**
 * The one chain-scoped FN Coin asset identity.
 *
 *     FN_ASSET_ID = TaggedHash("B3/FN/ASSET/V1", ModernChainDomain)
 *
 * All historical units created by the mandatory H+1 FN Genesis event and
 * all later proof-free PoD units use this same asset. Ownership and transfer
 * authorization live in the ordinary modern output carrier; no PoD identity,
 * historical proof, or claim data travels with an FN output.
 */
inline AssetId FnAssetId(const uint256& chain_domain)
{
    HashWriter writer{TaggedHash("B3/FN/ASSET/V1")};
    writer << chain_domain;
    return writer.GetSHA256();
}

} // namespace modern

#endif // B3COIN_MODERN_FN_H

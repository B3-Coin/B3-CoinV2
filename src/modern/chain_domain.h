// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_CHAIN_DOMAIN_H
#define B3COIN_MODERN_CHAIN_DOMAIN_H

#include <hash.h>
#include <uint256.h>

#include <optional>

namespace modern {

//! The contract's immutable anti-replay network identifier:
//! TaggedHash("B3/MODERN/CHAIN", genesis || X), both as their 32 raw
//! internal-order (header-serialization) bytes. Pure function of its
//! arguments — no defaults, no globals; fails closed (nullopt) when
//! either hash is null, so a call site with an unset X cannot obtain a
//! domain. Every chain-scoped modern identity (the FN asset id, every
//! colored-asset id, the modern-PoS seed/eligibility/signature tags)
//! binds this domain, so identities differ across chains and forks by
//! construction.
inline std::optional<uint256> ModernChainDomain(const uint256& genesis_hash,
                                                const uint256& final_legacy_hash)
{
    if (genesis_hash.IsNull() || final_legacy_hash.IsNull()) return std::nullopt;
    HashWriter writer{TaggedHash("B3/MODERN/CHAIN")};
    writer << genesis_hash << final_legacy_hash;
    return writer.GetSHA256();
}

} // namespace modern

#endif // B3COIN_MODERN_CHAIN_DOMAIN_H

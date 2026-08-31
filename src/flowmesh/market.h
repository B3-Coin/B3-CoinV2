// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_MARKET_H
#define B3COIN_FLOWMESH_MARKET_H

#include <hash.h>
#include <modern/policy.h>
#include <uint256.h>

#include <cstddef>
#include <optional>
#include <span>

namespace flowmesh {

using MarketId = uint256;
using VaultId = uint256;

inline constexpr const char* FLOWMESH_MARKET_TAG{"B3/FLOWMESH/MARKET/V1"};
inline constexpr const char* FLOWMESH_VAULT_TAG{"B3/FLOWMESH/VAULT/V1"};
inline constexpr size_t FLOWMESH_V1_MAX_CURVE_POINTS{8};
inline constexpr size_t FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS{1024};
inline constexpr size_t FLOWMESH_V1_MAX_MICROBLOCK_BYTES{2U * 1024U * 1024U};

/**
 * v1 is exactly one activated simple colored asset priced in native B3.
 * Asset activation/issuance is checked by the caller's chain index; this
 * helper freezes the identity bytes and rejects only intrinsically invalid
 * pairs.
 */
inline std::optional<MarketId> ComputeFlowMeshMarketId(
    const uint256& domain, const modern::AssetId& base,
    const modern::AssetId& quote = modern::NativeAsset())
{
    if (domain.IsNull() || base.IsNull() || base == quote ||
        quote != modern::NativeAsset()) {
        return std::nullopt;
    }
    HashWriter writer{TaggedHash(FLOWMESH_MARKET_TAG)};
    writer << std::span<const unsigned char>{domain.begin(), domain.size()}
           << std::span<const unsigned char>{base.begin(), base.size()}
           << std::span<const unsigned char>{quote.begin(), quote.size()};
    return writer.GetSHA256();
}

inline std::optional<VaultId> ComputeFlowMeshVaultId(const uint256& domain,
                                                     const MarketId& market_id)
{
    if (domain.IsNull() || market_id.IsNull()) return std::nullopt;
    HashWriter writer{TaggedHash(FLOWMESH_VAULT_TAG)};
    writer << std::span<const unsigned char>{domain.begin(), domain.size()}
           << std::span<const unsigned char>{market_id.begin(), market_id.size()};
    return writer.GetSHA256();
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_MARKET_H

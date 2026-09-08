// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_ASSET_METADATA_H
#define BITCOIN_WALLET_ASSET_METADATA_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>

namespace Consensus { struct Params; }

namespace wallet {

//! A compact preimage proving immutable simple-v1 precision, not chain inclusion.
//! Only fixed-genesis issuance with empty mode parameters is supported.
struct AssetMetadataProof {
    COutPoint issuance_prevout;
    uint64_t max_supply{0};
    uint8_t decimals{0};

    SERIALIZE_METHODS(AssetMetadataProof, obj)
    {
        READWRITE(obj.issuance_prevout, obj.max_supply, obj.decimals);
    }
};

//! Cosmetic, wallet-local labels. The database key also contains the chain domain.
struct LocalAssetMetadata {
    std::string name;
    std::string ticker;
    AssetMetadataProof proof;

    SERIALIZE_METHODS(LocalAssetMetadata, obj)
    {
        READWRITE(LIMITED_STRING(obj.name, 64), LIMITED_STRING(obj.ticker, 12), obj.proof);
    }
};

struct WalletAssetMetadata {
    std::string display_name;
    std::string ticker;
    std::optional<int> decimals;
    std::string source{"unknown"};
    bool test_only{false};
};

std::optional<uint256> AssetMetadataDomain(const Consensus::Params& params);
bool AssetMetadataProofMatches(const uint256& domain, const uint256& asset,
                               const AssetMetadataProof& proof);
//! Strictly decode one genesis record and derive its chain-bound id. This does
//! not assert signatures, current chain membership, or monetary backing.
bool DecodeAssetMetadataProof(const CTransaction& tx, const uint256& domain,
                              uint256& asset, AssetMetadataProof& proof, std::string& error);
bool ValidAssetMetadataLabels(const std::string& name, const std::string& ticker,
                              std::string& error);
std::optional<WalletAssetMetadata> ConfiguredAssetMetadata(const Consensus::Params& params,
                                                          const uint256& asset);

} // namespace wallet
#endif // BITCOIN_WALLET_ASSET_METADATA_H

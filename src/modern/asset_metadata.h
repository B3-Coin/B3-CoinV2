// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_MODERN_ASSET_METADATA_H
#define BITCOIN_MODERN_ASSET_METADATA_H

#include <modern/asset.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <string>

namespace modern {

//! Immutable simple-v1 genesis preimage. This proves precision, not current
//! chain inclusion, a display name, issuer identity, or monetary backing.
struct AssetMetadataProof {
    COutPoint issuance_prevout;
    uint64_t max_supply{0};
    uint8_t decimals{0};

    SERIALIZE_METHODS(AssetMetadataProof, obj)
    {
        READWRITE(obj.issuance_prevout, obj.max_supply, obj.decimals);
    }
};

//! Optional display data, never part of an asset commitment or signed action.
//! The proof authenticates precision; name/ticker are assertions by source.
struct AssetDisplayMetadata {
    std::string name;
    std::string ticker;
    AssetMetadataProof proof;
    std::string source;
};

inline bool VerifyAssetMetadataProof(const uint256& domain, const uint256& asset,
                                     const AssetMetadataProof& proof)
{
    const AssetGenesisV1 genesis{.max_supply = proof.max_supply, .decimals = proof.decimals};
    return !domain.IsNull() && !asset.IsNull() && !proof.issuance_prevout.IsNull() &&
           AssetGenesisValid(genesis) &&
           AssetIdV1(domain, proof.issuance_prevout, AssetGenesisCommitment(genesis)) == asset;
}

inline bool DecodeAssetPrecisionProof(const CTransaction& tx, const uint256& domain,
                                      uint256& asset, AssetMetadataProof& proof,
                                      std::string& error)
{
    if (domain.IsNull() || tx.vin.empty() || tx.vin[0].prevout.IsNull()) {
        error = "Issuance proof requires a pinned chain domain and a non-null first input";
        return false;
    }
    const CMpaRecord* issuance{nullptr};
    for (const auto& record : tx.mpa) {
        if (record.payload_type != CREATION_ACTION_ASSET_ISSUANCE) continue;
        if (issuance) {
            error = "Issuance proof has multiple genesis records";
            return false;
        }
        issuance = &record;
    }
    if (!issuance) {
        error = "Issuance proof has no genesis record";
        return false;
    }
    AssetGenesisV1 genesis;
    if (!DecodeAssetIssuanceAction(
            {issuance->payload_type, issuance->payload_version, issuance->payload}, genesis, error)) return false;
    if (!AssetGenesisValid(genesis)) {
        error = "Issuance proof has invalid or unsupported genesis rules";
        return false;
    }
    proof = {tx.vin[0].prevout, genesis.max_supply, genesis.decimals};
    asset = AssetIdV1(domain, proof.issuance_prevout, AssetGenesisCommitment(genesis));
    if (!VerifyAssetMetadataProof(domain, asset, proof)) {
        error = "Issuance proof does not identify a simple-v1 asset";
        return false;
    }
    return true;
}

} // namespace modern
#endif // BITCOIN_MODERN_ASSET_METADATA_H

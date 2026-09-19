// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_PUBLIC_ASSET_METADATA_H
#define BITCOIN_NODE_PUBLIC_ASSET_METADATA_H

#include <modern/asset_metadata.h>
#include <uint256.h>
#include <util/fs.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

class UniValue;

namespace node {

using PublicAssetMetadataCatalog = std::map<uint256, modern::AssetDisplayMetadata>;

/**
 * Load an explicitly configured PUBLIC display catalog. This never reads wallet
 * labels. The JSON array is limited to 256 records and 256 KiB; every record must
 * match the pinned domain and its immutable simple-v1 genesis preimage.
 *
 * On success, replace the entire catalog. An empty path produces an empty
 * catalog. On failure, leave the existing catalog unchanged and return a
 * diagnostic that contains neither file contents nor the configured path.
 * Labels are source assertions, not proof of issuer identity or backing.
 */
bool LoadPublicAssetMetadataCatalog(const fs::path& path, const uint256& domain,
                                    PublicAssetMetadataCatalog& catalog, std::string& error);

/**
 * Parse the same public JSON catalog in memory, enforcing the byte and record
 * bounds before populating a replacement catalog. Failure leaves catalog
 * unchanged. This validates record shape and precision, not source authority.
 */
bool ParsePublicAssetMetadataCatalog(std::string_view contents, const uint256& domain,
                                     PublicAssetMetadataCatalog& catalog, std::string& error);

//! Serialize a previously validated public display record with its precision proof.
UniValue PublicAssetMetadataJson(const uint256& domain, const uint256& asset,
                                 const modern::AssetDisplayMetadata& metadata);

/**
 * Parse a complete public display record, rejecting unknown/duplicate fields,
 * unsafe or reserved labels, and precision proofs for any other domain/asset.
 * A verified preimage proves decimals, not chain inclusion, labels, or backing.
 */
std::optional<modern::AssetDisplayMetadata> ParsePublicAssetMetadata(
    const UniValue& value, const uint256& domain, const uint256& asset, std::string& error);

} // namespace node

#endif // BITCOIN_NODE_PUBLIC_ASSET_METADATA_H

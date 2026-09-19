// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_asset_metadata.h>

#include <univalue.h>

namespace node {

bool LoadFlowMeshAssetMetadataCatalog(const fs::path& path, const uint256& domain,
                                     FlowMeshAssetMetadataCatalog& catalog, std::string& error)
{
    return LoadPublicAssetMetadataCatalog(path, domain, catalog, error);
}

UniValue FlowMeshAssetMetadataJson(const uint256& domain, const uint256& asset,
                                   const modern::AssetDisplayMetadata& metadata)
{
    return PublicAssetMetadataJson(domain, asset, metadata);
}

std::optional<modern::AssetDisplayMetadata> ParseFlowMeshAssetMetadata(
    const UniValue& value, const uint256& domain, const uint256& asset, std::string& error)
{
    return ParsePublicAssetMetadata(value, domain, asset, error);
}

} // namespace node

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_asset_metadata.h>
#include <node/public_asset_metadata.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>

namespace {

modern::AssetDisplayMetadata DisplayMetadata(uint32_t output = 0)
{
    return {"Example Asset", "EXA",
            {COutPoint{Txid::FromUint256(uint256{1}), output}, 1'000'000'000, 6}, {}};
}

uint256 MetadataAsset(const uint256& domain, const modern::AssetDisplayMetadata& metadata)
{
    return modern::AssetIdV1(domain, metadata.proof.issuance_prevout,
        modern::AssetGenesisCommitment(
            {.max_supply = metadata.proof.max_supply, .decimals = metadata.proof.decimals}));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(public_asset_metadata_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(record_roundtrip_and_flowmesh_compatibility)
{
    const uint256 domain{1};
    const auto metadata{DisplayMetadata()};
    const auto asset{MetadataAsset(domain, metadata)};
    const auto record{node::PublicAssetMetadataJson(domain, asset, metadata)};
    BOOST_CHECK_EQUAL(record.size(), 8U);
    BOOST_CHECK_EQUAL(record.write(), node::FlowMeshAssetMetadataJson(domain, asset, metadata).write());
    std::string error{"stale error"};
    const auto parsed{node::ParsePublicAssetMetadata(record, domain, asset, error)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(parsed->name, metadata.name);
    BOOST_CHECK_EQUAL(parsed->ticker, metadata.ticker);
    BOOST_CHECK_EQUAL(parsed->proof.decimals, metadata.proof.decimals);
    BOOST_CHECK_EQUAL(parsed->proof.max_supply, metadata.proof.max_supply);
    BOOST_CHECK(parsed->proof.issuance_prevout == metadata.proof.issuance_prevout);
    BOOST_CHECK(parsed->source.empty());
    BOOST_CHECK(node::ParseFlowMeshAssetMetadata(record, domain, asset, error));

    BOOST_CHECK(!node::ParsePublicAssetMetadata(record, uint256{2}, asset, error));
    BOOST_CHECK(!node::ParsePublicAssetMetadata(record, domain, uint256{2}, error));
    auto changed{record};
    changed.pushKV("decimals", 5);
    BOOST_CHECK(!node::ParsePublicAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKV("issuance_vout", 1);
    BOOST_CHECK(!node::ParsePublicAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKV("max_supply", metadata.proof.max_supply + 1);
    BOOST_CHECK(!node::ParsePublicAssetMetadata(changed, domain, asset, error));
}

BOOST_AUTO_TEST_CASE(catalog_replacement_is_atomic)
{
    const uint256 domain{1};
    const auto metadata{DisplayMetadata()};
    const auto asset{MetadataAsset(domain, metadata)};
    const auto record{node::PublicAssetMetadataJson(domain, asset, metadata)};
    // This existing entry must survive every rejected replacement unchanged.
    auto existing{DisplayMetadata(500)};
    existing.name = "Existing Local Value";
    existing.source = "private-source-not-published";
    const auto existing_asset{MetadataAsset(domain, existing)};
    node::PublicAssetMetadataCatalog catalog{{existing_asset, existing}};
    auto invalid{record};
    invalid.pushKV("decimals", 4);
    auto unknown{record};
    unknown.pushKV("private_data", "must-not-appear-in-diagnostics");
    auto duplicate_field{record};
    duplicate_field.pushKVEnd("ticker", "EXA");
    std::string error;
    for (const std::string& contents : {
             std::string{}, std::string{"{"}, std::string{"{}"}, std::string{"[1]"},
             "[" + record.write() + "," + invalid.write() + "]",
             "[" + record.write() + "," + record.write() + "]",
             "[" + unknown.write() + "]", "[" + duplicate_field.write() + "]"}) {
        BOOST_CHECK(!node::ParsePublicAssetMetadataCatalog(contents, domain, catalog, error));
        BOOST_REQUIRE_EQUAL(catalog.size(), 1U);
        BOOST_REQUIRE(catalog.contains(existing_asset));
        BOOST_CHECK_EQUAL(catalog.at(existing_asset).name, existing.name);
        BOOST_CHECK_EQUAL(catalog.at(existing_asset).source, existing.source);
        BOOST_CHECK(error.find("private_data") == std::string::npos);
        BOOST_CHECK(error.find("must-not-appear-in-diagnostics") == std::string::npos);
    }
    const auto contents{"[" + record.write() + "]"};
    BOOST_CHECK(!node::ParsePublicAssetMetadataCatalog(contents, uint256{}, catalog, error));
    BOOST_CHECK(!node::ParsePublicAssetMetadataCatalog(contents, uint256{2}, catalog, error));
    BOOST_CHECK(catalog.contains(existing_asset));

    BOOST_REQUIRE(node::ParsePublicAssetMetadataCatalog(contents, domain, catalog, error));
    BOOST_CHECK(error.empty());
    BOOST_REQUIRE_EQUAL(catalog.size(), 1U);
    BOOST_CHECK(catalog.contains(asset));
    BOOST_CHECK(!catalog.contains(existing_asset));
    BOOST_CHECK(catalog.at(asset).source.empty());
    BOOST_REQUIRE(node::ParsePublicAssetMetadataCatalog("[]", domain, catalog, error));
    BOOST_CHECK(catalog.empty());
}

BOOST_AUTO_TEST_CASE(genesis_proof_does_not_authenticate_labels)
{
    const uint256 domain{1};
    const auto original{DisplayMetadata()};
    auto relabelled{original};
    relabelled.name = "Other Safe Asset Name";
    relabelled.ticker = "OTHER";
    const auto asset{MetadataAsset(domain, original)};
    BOOST_CHECK(MetadataAsset(domain, relabelled) == asset);
    BOOST_CHECK(modern::VerifyAssetMetadataProof(domain, asset, original.proof));
    BOOST_CHECK(modern::VerifyAssetMetadataProof(domain, asset, relabelled.proof));

    std::string error;
    const auto first{node::ParsePublicAssetMetadata(
        node::PublicAssetMetadataJson(domain, asset, original), domain, asset, error)};
    const auto second{node::ParsePublicAssetMetadata(
        node::PublicAssetMetadataJson(domain, asset, relabelled), domain, asset, error)};
    BOOST_REQUIRE(first);
    BOOST_REQUIRE(second);
    BOOST_CHECK_EQUAL(first->name, original.name);
    BOOST_CHECK_EQUAL(second->name, relabelled.name);
    BOOST_CHECK_EQUAL(second->ticker, relabelled.ticker);
    BOOST_CHECK(first->proof.issuance_prevout == second->proof.issuance_prevout);
    BOOST_CHECK_EQUAL(first->proof.max_supply, second->proof.max_supply);
    BOOST_CHECK_EQUAL(first->proof.decimals, second->proof.decimals);
    // Label authority must be established outside the generic precision parser.
    BOOST_CHECK(first->source.empty());
    BOOST_CHECK(second->source.empty());
}

BOOST_AUTO_TEST_CASE(catalog_record_and_byte_boundaries)
{
    const uint256 domain{1};
    UniValue records{UniValue::VARR};
    for (uint32_t i{0}; i < 256; ++i) {
        const auto metadata{DisplayMetadata(i)};
        records.push_back(node::PublicAssetMetadataJson(domain, MetadataAsset(domain, metadata), metadata));
    }
    node::PublicAssetMetadataCatalog catalog;
    std::string error;
    BOOST_REQUIRE(node::ParsePublicAssetMetadataCatalog(records.write(), domain, catalog, error));
    BOOST_CHECK_EQUAL(catalog.size(), 256U);

    const auto metadata{DisplayMetadata(256)};
    records.push_back(node::PublicAssetMetadataJson(domain, MetadataAsset(domain, metadata), metadata));
    BOOST_CHECK(!node::ParsePublicAssetMetadataCatalog(records.write(), domain, catalog, error));
    BOOST_CHECK_EQUAL(catalog.size(), 256U);
    BOOST_CHECK(error.find("256-record") != std::string::npos);

    // Size is checked before JSON parsing, and rejection cannot clear the cache.
    const auto maximum{"[]" + std::string(256 * 1024 - 2, ' ')};
    BOOST_CHECK(!node::ParsePublicAssetMetadataCatalog(maximum + " ", domain, catalog, error));
    BOOST_CHECK_EQUAL(catalog.size(), 256U);
    BOOST_CHECK(error.find("256 KiB") != std::string::npos);
    BOOST_REQUIRE(node::ParsePublicAssetMetadataCatalog(maximum, domain, catalog, error));
    BOOST_CHECK(catalog.empty());
    BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_asset_metadata.h>
#include <test/util/setup_common.h>
#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <fstream>
#include <string>
#include <utility>

namespace {

modern::AssetDisplayMetadata DisplayMetadata(uint8_t tag = 1)
{
    return {"Example Asset", "EXA",
            {COutPoint{Txid::FromUint256(uint256{tag}), 3}, 1'000'000'000, 6}, {}};
}

uint256 MetadataAsset(const uint256& domain, const modern::AssetDisplayMetadata& metadata)
{
    return modern::AssetIdV1(domain, metadata.proof.issuance_prevout,
        modern::AssetGenesisCommitment(
            {.max_supply = metadata.proof.max_supply, .decimals = metadata.proof.decimals}));
}

void WriteCatalog(const fs::path& path, const std::string& contents)
{
    std::ofstream file{path.std_path(), std::ios::binary | std::ios::trunc};
    BOOST_REQUIRE(file.is_open());
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    BOOST_REQUIRE(!file.fail());
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_asset_metadata_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(proof_bound_labels_roundtrip)
{
    const uint256 domain{1};
    const auto metadata{DisplayMetadata()};
    const auto asset{MetadataAsset(domain, metadata)};
    const auto record{node::FlowMeshAssetMetadataJson(domain, asset, metadata)};
    std::string error{"stale error"};
    const auto parsed{node::ParseFlowMeshAssetMetadata(record, domain, asset, error)};
    BOOST_REQUIRE(parsed);
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(parsed->name, metadata.name);
    BOOST_CHECK_EQUAL(parsed->ticker, metadata.ticker);
    BOOST_CHECK_EQUAL(parsed->proof.decimals, 6);
    BOOST_CHECK(parsed->proof.issuance_prevout == metadata.proof.issuance_prevout);
    BOOST_CHECK(parsed->source.empty());
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(record, uint256{2}, asset, error));
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(record, domain, uint256{2}, error));

    auto changed{record};
    changed.pushKV("decimals", 5);
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKV("issuance_vout", 4);
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKV("issuance_txid", "1");
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
}

BOOST_AUTO_TEST_CASE(untrusted_fields_and_reserved_labels)
{
    const uint256 domain{1};
    const auto metadata{DisplayMetadata()};
    const auto asset{MetadataAsset(domain, metadata)};
    const auto record{node::FlowMeshAssetMetadataJson(domain, asset, metadata)};
    std::string error;
    auto changed{record};
    changed.pushKV("source", "consensus");
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKVEnd("decimals", 6);
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    for (const auto* label : {"B3", "FN", "FNcoin", "bUSD", "tUSD", "cUSD",
                              "Test USD", "Bridged USD", "cUSD Unbacked Test", "f.n-c_o i n"}) {
        changed = record;
        changed.pushKV("name", label);
        BOOST_CHECK_MESSAGE(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error), label);
    }
    for (const std::string name : {" leading", "trailing ", "<script>", "emoji\xf0\x9f\x92\xb0", "line\nbreak"}) {
        changed = record;
        changed.pushKV("name", name);
        BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    }
    changed = record;
    changed.pushKV("ticker", "TWO WORDS");
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKV("name", std::string(65, 'a'));
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    changed = record;
    changed.pushKV("ticker", std::string(13, 'a'));
    BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
}

BOOST_AUTO_TEST_CASE(integer_and_identifier_bounds)
{
    const uint256 domain{1};
    const auto metadata{DisplayMetadata()};
    const auto asset{MetadataAsset(domain, metadata)};
    const auto record{node::FlowMeshAssetMetadataJson(domain, asset, metadata)};
    std::string error;
    for (const auto* field : {"issuance_vout", "max_supply", "decimals"}) {
        for (const auto* number : {"-1", "1.0", "1e0", "18446744073709551616"}) {
            auto changed{record};
            changed.pushKV(field, UniValue{UniValue::VNUM, number});
            BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
        }
        auto changed{record};
        changed.pushKV(field, "6");
        BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
    }
    for (const auto* field : {"domain", "asset_id", "issuance_txid"}) {
        for (const std::string bad_id : {std::string(63, 'a'), std::string(64, 'g'), std::string(64, '0')}) {
            auto changed{record};
            changed.pushKV(field, bad_id);
            BOOST_CHECK(!node::ParseFlowMeshAssetMetadata(changed, domain, asset, error));
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_load_is_atomic_and_bounded)
{
    const uint256 domain{1};
    const auto metadata{DisplayMetadata()};
    const auto asset{MetadataAsset(domain, metadata)};
    const auto record{node::FlowMeshAssetMetadataJson(domain, asset, metadata)};
    const auto path{m_path_root / "public-catalog-private-filename.json"};
    node::FlowMeshAssetMetadataCatalog catalog;
    std::string error;
    WriteCatalog(path, "[" + record.write() + "]");
    BOOST_REQUIRE(node::LoadFlowMeshAssetMetadataCatalog(path, domain, catalog, error));
    BOOST_REQUIRE_EQUAL(catalog.size(), 1U);
    BOOST_CHECK(catalog.contains(asset));

    auto invalid{record};
    invalid.pushKV("ticker", "cUSD");
    for (const std::string contents : {
             "[" + record.write() + "," + invalid.write() + "]",
             "[" + record.write() + "," + record.write() + "]",
             std::string{"{\"private_data\":\"must not appear in errors\"}"},
             std::string(256 * 1024 + 1, ' ')}) {
        WriteCatalog(path, contents);
        BOOST_CHECK(!node::LoadFlowMeshAssetMetadataCatalog(path, domain, catalog, error));
        BOOST_CHECK_EQUAL(catalog.size(), 1U);
        BOOST_CHECK(catalog.contains(asset));
        BOOST_CHECK(error.find("public-catalog-private-filename") == std::string::npos);
        BOOST_CHECK(error.find("private_data") == std::string::npos);
    }

    UniValue too_many{UniValue::VARR};
    for (size_t i{0}; i < 257; ++i) too_many.push_back(record);
    WriteCatalog(path, too_many.write());
    BOOST_CHECK(!node::LoadFlowMeshAssetMetadataCatalog(path, domain, catalog, error));
    BOOST_CHECK_EQUAL(catalog.size(), 1U);
    BOOST_CHECK(error.find("256-record") != std::string::npos);

    // Whitespace is part of the byte limit; exactly 256 KiB remains valid.
    WriteCatalog(path, "[]" + std::string(256 * 1024 - 2, ' '));
    BOOST_CHECK(node::LoadFlowMeshAssetMetadataCatalog(path, domain, catalog, error));
    BOOST_CHECK(catalog.empty());
    catalog.emplace(asset, metadata);
    BOOST_CHECK(node::LoadFlowMeshAssetMetadataCatalog({}, {}, catalog, error));
    BOOST_CHECK(catalog.empty());
    BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_SUITE_END()

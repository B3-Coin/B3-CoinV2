// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/fn_params.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <modern/fn.h>
#include <modern/fn_genesis.h>
#include <modern/fn_genesis_validation.h>
#include <modern/chain_domain.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <span.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Right = Consensus::FnGenesisRight;

uint256 RawPodId(const uint32_t value)
{
    uint256 id;
    WriteBE32(id.begin(), value);
    return id;
}

Right Row(const uint32_t pod, const unsigned char recipient_fill)
{
    Right row;
    row.pod_id = RawPodId(pod);
    row.recipient_key_hash.fill(recipient_fill);
    return row;
}

uint256 Domain(const unsigned char first = 0x42)
{
    uint256 domain;
    domain.begin()[0] = first;
    return domain;
}

std::vector<Right> Manifest(const size_t count)
{
    std::vector<Right> out;
    out.reserve(count);
    for (size_t i{0}; i < count; ++i) {
        out.push_back(Row(static_cast<uint32_t>(i + 1),
                          static_cast<unsigned char>(i & 0xff)));
    }
    return out;
}

std::string RawHex(const uint256& value)
{
    return HexStr(std::span<const unsigned char>(value.begin(), value.size()));
}

Consensus::Params ConfiguredParams(const size_t count)
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.fn_genesis_required = true;
    params.hard_fork_height = 810'001;
    params.transition_pow_length = 1'000;
    params.hashGenesisBlock = Domain(0x11);
    params.legacy_final_hash = Domain(0x22);
    params.fn_genesis_manifest = Manifest(count);
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock,
                                                *params.legacy_final_hash)};
    if (!domain) throw std::runtime_error("test chain domain unavailable");
    const auto root{modern::ComputeFnGenesisManifestRootV1(
        *domain, static_cast<uint32_t>(*params.hard_fork_height),
        params.fn_genesis_manifest)};
    if (!root) throw std::runtime_error("test FN root unavailable");
    params.fn_genesis_rights_root = *root;
    return params;
}

CBlock CoinbaseBlock(const std::vector<CTxOut>& extra_outputs = {})
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << CScriptNum{810'001};
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    coinbase.vout.insert(coinbase.vout.end(), extra_outputs.begin(),
                         extra_outputs.end());
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    return block;
}

} // namespace

BOOST_AUTO_TEST_SUITE(fn_genesis_tests)

BOOST_AUTO_TEST_CASE(fn_asset_identity)
{
    const uint256 genesis{
        "1111111111111111111111111111111111111111111111111111111111111111"};
    const auto domain{modern::ModernChainDomain(genesis, uint256::ONE)};
    BOOST_REQUIRE(domain);
    BOOST_CHECK_EQUAL(
        domain->GetHex(),
        "531336c7b81523168c60b8ddadae01d32e6f9733a9ca9de82534ca647f91ae14");

    const modern::AssetId asset{modern::FnAssetId(*domain)};
    BOOST_CHECK_EQUAL(
        asset.GetHex(),
        "ce7761a4dd646c10aacaaeba341006b9298824efca3547b4bcd79c6ee41e5326");
    BOOST_CHECK(asset != modern::NativeAsset());
    BOOST_CHECK(modern::FnAssetId(uint256::ONE) != asset);
    BOOST_CHECK(modern::FnAssetId(*domain) == asset);
}

BOOST_AUTO_TEST_CASE(canonical_order_duplicates_and_recipient_script)
{
    std::string error;
    const std::vector<Right> empty;
    BOOST_CHECK(!modern::ValidateFnGenesisManifest(empty, error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-empty");

    std::vector<Right> rows{Row(1, 0x11), Row(2, 0x22), Row(3, 0x33)};
    BOOST_CHECK(modern::ValidateFnGenesisManifest(rows, error));
    BOOST_CHECK(error.empty());

    // The order is the lexicographic order of the 32 serialized bytes. It is
    // intentionally independent of uint256's reversed display convention.
    std::swap(rows[0], rows[1]);
    BOOST_CHECK(!modern::ValidateFnGenesisManifest(rows, error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-not-raw-sorted");

    rows = {Row(1, 0x11), Row(1, 0x99)};
    BOOST_CHECK(!modern::ValidateFnGenesisManifest(rows, error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-duplicate-pod");

    // Different PoDs may designate the same address.
    rows = {Row(1, 0x55), Row(2, 0x55)};
    BOOST_CHECK(modern::ValidateFnGenesisManifest(rows, error));

    const CScript actual{modern::FnGenesisRecipientScript(rows[0])};
    const CScript expected{CScript() << OP_DUP << OP_HASH160
                                     << std::vector<unsigned char>(20, 0x55)
                                     << OP_EQUALVERIFY << OP_CHECKSIG};
    BOOST_CHECK(actual == expected);
    BOOST_CHECK_EQUAL(actual.size(), 25U);
    BOOST_CHECK_EQUAL(HexStr(actual),
                      "76a914555555555555555555555555555555555555555588ac");
}

BOOST_AUTO_TEST_CASE(every_row_field_and_position_mutates_the_root)
{
    const uint256 domain{Domain()};
    constexpr uint32_t fn_genesis_height{810'001};
    const std::vector<Right> rows{Row(10, 0x10), Row(20, 0x20), Row(30, 0x30)};
    const auto root{modern::ComputeFnGenesisManifestRootV1(
        domain, fn_genesis_height, rows)};
    BOOST_REQUIRE(root.has_value());

    auto changed{rows};
    changed[1].recipient_key_hash[7] ^= 0x01;
    const auto recipient_root{modern::ComputeFnGenesisManifestRootV1(
        domain, fn_genesis_height, changed)};
    BOOST_REQUIRE(recipient_root.has_value());
    BOOST_CHECK(*recipient_root != *root);

    changed = rows;
    // Mutate a low-significance raw byte without disturbing the leading bytes
    // that establish canonical order.
    changed[1].pod_id.begin()[31] ^= 0x01;
    const auto pod_root{modern::ComputeFnGenesisManifestRootV1(
        domain, fn_genesis_height, changed)};
    BOOST_REQUIRE(pod_root.has_value());
    BOOST_CHECK(*pod_root != *root);

    // Position and count are direct inputs to every leaf, independent of the
    // row bytes themselves.
    const uint256 leaf{modern::FnGenesisManifestLeaf(
        domain, fn_genesis_height, modern::FN_GENESIS_MANIFEST_VERSION_V1,
        3, 0, rows[0])};
    BOOST_CHECK(leaf != modern::FnGenesisManifestLeaf(
                            domain, fn_genesis_height,
                            modern::FN_GENESIS_MANIFEST_VERSION_V1,
                            3, 1, rows[0]));
    BOOST_CHECK(leaf != modern::FnGenesisManifestLeaf(
                            domain, fn_genesis_height,
                            modern::FN_GENESIS_MANIFEST_VERSION_V1,
                            4, 0, rows[0]));
}

BOOST_AUTO_TEST_CASE(chain_height_version_and_count_are_bound)
{
    constexpr uint32_t fn_genesis_height{0x01020304};
    const uint256 domain{Domain(0x42)};
    const std::vector<Right> rows{Row(1, 0x11), Row(2, 0x22)};

    const auto root{modern::ComputeFnGenesisManifestRoot(
        domain, fn_genesis_height, 1, rows)};
    const auto other_chain{modern::ComputeFnGenesisManifestRoot(
        Domain(0x43), fn_genesis_height, 1, rows)};
    const auto other_height{modern::ComputeFnGenesisManifestRoot(
        domain, fn_genesis_height + 1, 1, rows)};
    const auto other_version{modern::ComputeFnGenesisManifestRoot(
        domain, fn_genesis_height, 2, rows)};
    const auto other_count{modern::ComputeFnGenesisManifestRoot(
        domain, fn_genesis_height, 1,
        std::vector<Right>{Row(1, 0x11), Row(2, 0x22), Row(3, 0x33)})};

    BOOST_REQUIRE(root && other_chain && other_height && other_version && other_count);
    BOOST_CHECK(*root != *other_chain);
    BOOST_CHECK(*root != *other_height);
    BOOST_CHECK(*root != *other_version);
    BOOST_CHECK(*root != *other_count);

    std::string error;
    BOOST_CHECK(!modern::ComputeFnGenesisManifestRootV1(
                     uint256{}, fn_genesis_height, rows, &error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-null-chain-domain");
}

BOOST_AUTO_TEST_CASE(synthetic_commitment_vector_is_pinned)
{
    uint256 domain;
    std::iota(domain.begin(), domain.end(), 0);

    Right first;
    first.pod_id.begin()[0] = 0x10;
    std::iota(first.pod_id.begin() + 1, first.pod_id.end(), 1);
    std::iota(first.recipient_key_hash.begin(), first.recipient_key_hash.end(), 0xa0);

    Right second;
    second.pod_id.begin()[0] = 0x20;
    std::iota(second.pod_id.begin() + 1, second.pod_id.end(), 1);
    std::reverse(second.pod_id.begin() + 1, second.pod_id.end());
    std::iota(second.recipient_key_hash.begin(), second.recipient_key_hash.end(), 0xc0);

    const std::vector<Right> rows{first, second};
    std::string error;
    const auto root{modern::ComputeFnGenesisManifestRootV1(
        domain, 0x01020304, rows, &error)};
    BOOST_REQUIRE_MESSAGE(root.has_value(), error);

    BOOST_CHECK_EQUAL(
        RawHex(modern::FnGenesisManifestLeaf(domain, 0x01020304, 1, 2, 0, first)),
        "9296212e40ea3efc098df59b18ccd80fa80cd29cd37d2ffb5713ea8fb45bd496");
    BOOST_CHECK_EQUAL(
        RawHex(*root),
        "06050873af4eb9cf98454e0eed45f4664f970ab00f5db17afe794e07020f94d5");

    const auto file{modern::EncodeFnGenesisManifestFileV1(
        domain, 0x01020304, modern::FN_GENESIS_MANIFEST_VERSION_V1,
        rows, *root, &error)};
    BOOST_REQUIRE_MESSAGE(file.has_value(), error);
    BOOST_CHECK_EQUAL(file->size(), 195U);
    BOOST_CHECK_EQUAL(
        HexStr(*file),
        "62332d666e2d67656e657369732f76310a"
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "01020304000100000002"
        "06050873af4eb9cf98454e0eed45f4664f970ab00f5db17afe794e07020f94d5"
        "100102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3"
        "201f1e1d1c1b1a191817161514131211100f0e0d0c0b0a090807060504030201"
        "c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3");
    BOOST_CHECK_EQUAL(
        HexStr(modern::FnGenesisManifestFileSha256(*file)),
        "8458ee2717f9f61fbf70f51839006a85d38f68441637d315071a67ffceda158e");

    BOOST_CHECK(!modern::EncodeFnGenesisManifestFileV1(
        domain, 0x01020304, 2, rows, *root, &error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-file-unsupported-version");
    BOOST_CHECK(!modern::EncodeFnGenesisManifestFileV1(
        domain, 0x01020304, 1, rows, uint256::ONE, &error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-file-root-mismatch");
}

BOOST_AUTO_TEST_CASE(historical_and_hard_cap_bounds)
{
    static_assert(Consensus::MAX_FN_EVER_ISSUED == 5'000);
    static_assert(Consensus::HISTORICAL_FN_PROVEN_FLOOR == 3'500);
    std::string error;
    const std::vector<Right> historical{Manifest(3'500)};
    BOOST_CHECK(modern::ValidateFnGenesisManifest(historical, error));
    BOOST_CHECK(modern::ComputeFnGenesisManifestRootV1(
                    Domain(), 810'001, historical, &error)
                    .has_value());

    const std::vector<Right> at_cap{Manifest(Consensus::MAX_FN_EVER_ISSUED)};
    BOOST_CHECK_EQUAL(at_cap.size(), 5'000U);
    BOOST_CHECK(modern::ValidateFnGenesisManifest(at_cap, error));
    BOOST_CHECK(modern::ComputeFnGenesisManifestRootV1(
                    Domain(), 810'001, at_cap, &error)
                    .has_value());

    const std::vector<Right> over_cap{Manifest(Consensus::MAX_FN_EVER_ISSUED + 1)};
    BOOST_CHECK(!modern::ValidateFnGenesisManifest(over_cap, error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-too-large");
    BOOST_CHECK(!modern::ComputeFnGenesisManifestRootV1(
                     Domain(), 810'001, over_cap, &error));
    BOOST_CHECK_EQUAL(error, "fn-genesis-manifest-too-large");
}

BOOST_AUTO_TEST_CASE(mandatory_configuration_fails_closed_and_enforces_bounds)
{
    std::string error;
    Consensus::Params valid{
        ConfiguredParams(Consensus::HISTORICAL_FN_PROVEN_FLOOR)};
    BOOST_REQUIRE_MESSAGE(modern::CheckFnGenesisConfiguration(valid, error), error);
    const auto expected{modern::ExpectedFnGenesisOutputs(valid, error)};
    BOOST_REQUIRE_MESSAGE(expected.has_value(), error);
    BOOST_REQUIRE_EQUAL(expected->size(),
                        Consensus::HISTORICAL_FN_PROVEN_FLOOR);
    BOOST_CHECK(modern::CheckFnGenesisBlock(
        CoinbaseBlock(*expected), *valid.hard_fork_height, valid, error));

    // Production marks the event mandatory independently of whether the seal
    // constants have been inserted. An X-only release therefore cannot enter
    // H+1 with an empty allocation.
    Consensus::Params missing{valid};
    missing.fn_genesis_manifest.clear();
    missing.fn_genesis_rights_root.reset();
    BOOST_CHECK(!modern::CheckFnGenesisBlock(
        CoinbaseBlock(), *missing.hard_fork_height, missing, error));
    BOOST_CHECK_EQUAL(error, "FN Genesis is not configured");
    // The mandate is height-exact and does not retroactively affect H.
    BOOST_CHECK(modern::CheckFnGenesisBlock(
        CoinbaseBlock(), *missing.hard_fork_height - 1, missing, error));

    Consensus::Params incomplete{valid};
    incomplete.fn_genesis_rights_root.reset();
    BOOST_CHECK(!modern::CheckFnGenesisConfiguration(incomplete, error));
    BOOST_CHECK_EQUAL(error, "FN Genesis configuration is incomplete");
    BOOST_CHECK(!modern::CheckFnGenesisBlock(
        CoinbaseBlock(), *incomplete.hard_fork_height, incomplete, error));

    Consensus::Params below{
        ConfiguredParams(Consensus::HISTORICAL_FN_PROVEN_FLOOR - 1)};
    BOOST_CHECK(!modern::CheckFnGenesisConfiguration(below, error));
    BOOST_CHECK_EQUAL(error,
                      "FN Genesis manifest is below the proven historical floor");

    Consensus::Params over{valid};
    over.fn_genesis_manifest.resize(Consensus::MAX_FN_EVER_ISSUED + 1);
    BOOST_CHECK(!modern::CheckFnGenesisConfiguration(over, error));
    BOOST_CHECK_EQUAL(error, "FN Genesis manifest exceeds the lifetime cap");

    Consensus::Params mismatched{valid};
    mismatched.fn_genesis_rights_root = uint256::ONE;
    BOOST_CHECK(!modern::CheckFnGenesisConfiguration(mismatched, error));
    BOOST_CHECK_EQUAL(
        error, "FN Genesis manifest does not match the pinned rights root");

    // Synthetic B3 chains retain an explicit opt-out; mainnet pins the
    // required bit in chainparams and is covered by the shipped-network test.
    missing.fn_genesis_required = false;
    BOOST_CHECK(modern::CheckFnGenesisBlock(
        CoinbaseBlock(), *missing.hard_fork_height, missing, error));
}

BOOST_AUTO_TEST_SUITE_END()

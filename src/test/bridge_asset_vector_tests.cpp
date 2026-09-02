// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/bridge_params.h>
#include <modern/bridge_asset.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

BOOST_AUTO_TEST_SUITE(bridge_asset_vector_tests)

BOOST_AUTO_TEST_CASE(cpp_solidity_asset_id_byte_order_vector)
{
    std::array<unsigned char, 32> domain_bytes{};
    for (size_t i{0}; i < domain_bytes.size(); ++i) {
        domain_bytes[i] = static_cast<unsigned char>(i);
    }
    const uint256 chain_domain{std::span<const unsigned char>{domain_bytes}};

    Consensus::BridgeAssetIdentityV1 identity;
    identity.version = 1;
    identity.origin_chain_id = 1;
    const auto vault{ParseHex(
        "11223344556677889900aabbccddeeff00112233")};
    const auto token{ParseHex(
        "dac17f958d2ee523a2206206994597c13d831ec7")};
    BOOST_REQUIRE_EQUAL(vault.size(), identity.vault_address.size());
    BOOST_REQUIRE_EQUAL(token.size(), identity.token_address.size());
    std::copy(vault.begin(), vault.end(), identity.vault_address.begin());
    std::copy(token.begin(), token.end(), identity.token_address.begin());
    identity.origin_decimals = 6;
    identity.asset_decimals = 6;

    const modern::AssetId asset{
        modern::BridgeAssetIdV1(chain_domain, identity)};

    // Raw uint256 bytes are the Solidity bytes32 / policy-wire order.
    BOOST_CHECK_EQUAL(
        HexStr(std::span<const unsigned char>{asset.begin(), asset.size()}),
        "72eca63ac9f140eee09f06cf86faed49cbe961b8b07e0dd291f76af30dc826d3");
    // B3's ordinary uint256 display reverses those bytes. Keeping both values
    // explicit prevents an operator from pasting the display form into an EVM
    // deployment manifest.
    BOOST_CHECK_EQUAL(
        asset.GetHex(),
        "d326c80df36af791d20d7eb0b861e9cb49edfa86cf069fe0ee40f1c93aa6ec72");
}

BOOST_AUTO_TEST_SUITE_END()

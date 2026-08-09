// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/consensus.h>
#include <legacy/primitives.h>

#include <consensus/params.h>
#include <consensus/merkle.h>
#include <kernel/chainparams.h>
#include <netaddress.h>
#include <protocol.h>
#include <streams.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <utility>
#include <vector>

BOOST_AUTO_TEST_SUITE(legacy_genesis_tests)

BOOST_AUTO_TEST_CASE(matches_existing_b3coin_chain)
{
    const legacy::Block genesis{legacy::CreateGenesisBlock()};

    BOOST_CHECK_EQUAL(genesis.bits, 0x1e0fffffU);
    BOOST_CHECK_EQUAL(genesis.merkle_root.GetHex(),
        "4243fd570d4cb2e2930767f5bf18b2f65f1b7c4e16a392552d1efadeec00753d");
    BOOST_CHECK_EQUAL(genesis.GetHash().GetHex(),
        "4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a");

    DataStream encoded_header;
    encoded_header << static_cast<const legacy::BlockHeader&>(genesis);
    BOOST_CHECK_EQUAL(encoded_header.size(), 80U);
}

BOOST_AUTO_TEST_CASE(active_core_primitives_match_legacy_wire_format)
{
    const legacy::Block legacy_genesis{legacy::CreateGenesisBlock()};
    const CBlock core_genesis{legacy::CreateCoreGenesisBlock()};

    BOOST_CHECK_EQUAL(core_genesis.hashMerkleRoot.GetHex(), legacy_genesis.merkle_root.GetHex());
    BOOST_CHECK_EQUAL(core_genesis.GetLegacyB3Hash().GetHex(), legacy_genesis.GetHash().GetHex());
    BOOST_CHECK_EQUAL(BlockMerkleRoot(core_genesis).GetHex(), legacy_genesis.merkle_root.GetHex());

    DataStream legacy_serialized;
    legacy_serialized << legacy_genesis;
    DataStream core_serialized;
    core_serialized << TX_WITH_WITNESS(core_genesis);
    BOOST_REQUIRE_EQUAL(core_serialized.size(), legacy_serialized.size());
    BOOST_CHECK(std::equal(core_serialized.begin(), core_serialized.end(),
                           legacy_serialized.begin(), legacy_serialized.end()));
}

BOOST_AUTO_TEST_CASE(accepts_the_configured_historical_genesis_exception)
{
    const CBlock genesis{legacy::CreateCoreGenesisBlock()};
    Consensus::Params params;
    params.legacy_b3coin = true;
    params.powLimit = uint256{"00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    params.hashGenesisBlock = genesis.GetLegacyB3Hash();

    BlockValidationState state;
    BOOST_CHECK(CheckBlock(genesis, state, params));
}

BOOST_AUTO_TEST_CASE(legacy_bootstrap_addresses_are_fixed_seeds)
{
    const auto params{CChainParams::Main()};
    BOOST_CHECK(params->DNSSeeds().empty());

    ParamsStream stream{SpanReader{params->FixedSeeds()}, CAddress::V2_NETWORK};
    std::vector<CService> endpoints;
    while (!stream.empty()) {
        CService endpoint;
        stream >> endpoint;
        endpoints.push_back(std::move(endpoint));
    }

    BOOST_REQUIRE_EQUAL(endpoints.size(), 32U);
    BOOST_CHECK_EQUAL(endpoints.front().ToStringAddrPort(), "101.111.89.85:5647");
    BOOST_CHECK_EQUAL(endpoints.back().ToStringAddrPort(), "98.97.143.14:5647");
}

BOOST_AUTO_TEST_CASE(live_legacy_client_wire_identity)
{
    const auto params{CChainParams::Main()};
    const auto& message_start{params->MessageStart()};

    BOOST_CHECK_EQUAL(message_start[0], 0xb3U);
    BOOST_CHECK_EQUAL(message_start[1], 0x2eU);
    BOOST_CHECK_EQUAL(message_start[2], 0x1eU);
    BOOST_CHECK_EQUAL(message_start[3], 0xe6U);
    BOOST_CHECK_EQUAL(params->GetDefaultPort(), 5647U);
    BOOST_CHECK_EQUAL(legacy::P2P_PROTOCOL_VERSION, 80'008);
    BOOST_CHECK_EQUAL(legacy::P2P_COMPATIBILITY_VERSION, 70'011);
}

BOOST_AUTO_TEST_SUITE_END()

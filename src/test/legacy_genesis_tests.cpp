// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <legacy/primitives.h>

#include <consensus/params.h>
#include <consensus/merkle.h>
#include <kernel/chainparams.h>
#include <netaddress.h>
#include <protocol.h>
#include <streams.h>
#include <util/strencodings.h>
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
    // The explicit legacy codec reproduces the historical block bytes; the
    // modern default encoding intentionally does not.
    DataStream core_serialized;
    core_serialized << legacy::TX_LEGACY(core_genesis);
    BOOST_REQUIRE_EQUAL(core_serialized.size(), legacy_serialized.size());
    BOOST_CHECK(std::equal(core_serialized.begin(), core_serialized.end(),
                           legacy_serialized.begin(), legacy_serialized.end()));

    DataStream modern_serialized;
    modern_serialized << TX_WITH_WITNESS(core_genesis);
    BOOST_CHECK(modern_serialized.size() < legacy_serialized.size());
}

BOOST_AUTO_TEST_CASE(frozen_legacy_transaction_golden_vector)
{
    const CBlock genesis{legacy::CreateCoreGenesisBlock()};
    const CTransaction& coinbase{*genesis.vtx[0]};

    BOOST_CHECK(coinbase.IsLegacyEncoded());
    BOOST_CHECK_EQUAL(coinbase.nTime, 1481667355U);

    DataStream encoded;
    encoded << legacy::TX_LEGACY(coinbase);
    // Frozen historical bytes: version 1, nTime 1481667355 (1b735058 LE),
    // one null-prevout input carrying the timestamp string, one empty
    // zero-value output, lock time 0.
    BOOST_CHECK_EQUAL(HexStr(encoded),
        "010000001b735058010000000000000000000000000000000000000000000000000000"
        "000000000000ffffffff5300012a4c4e4368696e61206c61756e636865732047616f66"
        "656e2d3320537461656c6c69746520746f2067657420616363757261746520696d6167"
        "6573206f66206561727468206f6e2031312d617567757374ffffffff01"
        "00000000000000000000000000");
    // The historical single-transaction merkle root IS the coinbase txid.
    BOOST_CHECK_EQUAL(coinbase.GetHash().GetHex(),
        "4243fd570d4cb2e2930767f5bf18b2f65f1b7c4e16a392552d1efadeec00753d");
    BOOST_CHECK_EQUAL(legacy::TxId(coinbase).GetHex(), coinbase.GetHash().GetHex());

    // Round-trip through the explicit legacy codec preserves the identity.
    CTransactionRef decoded;
    DataStream copy{encoded};
    copy >> legacy::TX_LEGACY(decoded);
    BOOST_CHECK(decoded->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(decoded->nTime, coinbase.nTime);
    BOOST_CHECK_EQUAL(decoded->GetHash().GetHex(), coinbase.GetHash().GetHex());

    // The modern default encoding omits nTime and yields a different txid.
    DataStream modern;
    modern << TX_NO_WITNESS(coinbase);
    BOOST_CHECK_EQUAL(modern.size() + 4, encoded.size());
    const CMutableTransaction modern_copy{[&] {
        CMutableTransaction m{coinbase};
        m.m_legacy_encoding = false;
        return m;
    }()};
    BOOST_CHECK(modern_copy.GetHash().GetHex() != coinbase.GetHash().GetHex());
}

BOOST_AUTO_TEST_CASE(legacy_codec_rejects_truncated_bytes)
{
    const CBlock genesis{legacy::CreateCoreGenesisBlock()};
    DataStream encoded;
    encoded << legacy::TX_LEGACY(*genesis.vtx[0]);

    for (const size_t len : {size_t{0}, size_t{3}, encoded.size() / 2, encoded.size() - 1}) {
        DataStream truncated{std::span{encoded}.first(len)};
        CTransactionRef decoded;
        BOOST_CHECK_THROW(truncated >> legacy::TX_LEGACY(decoded), std::ios_base::failure);
    }
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

    const auto decode_endpoints = [](const std::vector<uint8_t>& serialized) {
        ParamsStream stream{SpanReader{serialized}, CAddress::V2_NETWORK};
        std::vector<CService> endpoints;
        while (!stream.empty()) {
            CService endpoint;
            stream >> endpoint;
            endpoints.push_back(std::move(endpoint));
        }
        return endpoints;
    };

    const std::vector<CService> endpoints{decode_endpoints(params->FixedSeeds())};

    // 32 historical bootstrap peers, three transition-capable peers available
    // to ordinary fresh nodes, and the owner-supplied release-v1 seed last.
    BOOST_REQUIRE_EQUAL(endpoints.size(), 36U);
    BOOST_CHECK_EQUAL(endpoints.front().ToStringAddrPort(), "101.111.89.85:5647");
    BOOST_CHECK_EQUAL(endpoints[31].ToStringAddrPort(), "98.97.143.14:5647");
    BOOST_CHECK_EQUAL(endpoints[32].ToStringAddrPort(), "38.191.246.166:5647");
    BOOST_CHECK_EQUAL(endpoints[33].ToStringAddrPort(), "46.151.140.5:5647");
    BOOST_CHECK_EQUAL(endpoints[34].ToStringAddrPort(), "77.74.83.147:5647");
    BOOST_CHECK_EQUAL(endpoints.back().ToStringAddrPort(), "176.31.13.198:5647");

    // The rescue list is deliberately modern-only so a stale peers.dat cannot
    // cause historical-only endpoints to be re-advertised as capable peers.
    const std::vector<CService> recovery_endpoints{
        decode_endpoints(params->ModernRecoverySeeds())};
    BOOST_REQUIRE_EQUAL(recovery_endpoints.size(), 3U);
    BOOST_CHECK_EQUAL(recovery_endpoints[0].ToStringAddrPort(), "38.191.246.166:5647");
    BOOST_CHECK_EQUAL(recovery_endpoints[1].ToStringAddrPort(), "46.151.140.5:5647");
    BOOST_CHECK_EQUAL(recovery_endpoints[2].ToStringAddrPort(), "77.74.83.147:5647");
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

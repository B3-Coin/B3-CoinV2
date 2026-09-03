// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <core_io.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/hardfork.h>
#include <consensus/params.h>
#include <legacy/codec.h>
#include <legacy/replay.h>
#include <modern/validation.h>
#include <primitives/block.h>
#include <streams.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>

BOOST_AUTO_TEST_SUITE(hardfork_tests)

namespace {

//! Synthetic legacy boundary for era-selection tests. H is the final legacy
//! height, so Params::hard_fork_height carries the first MODERN height H + 1.
constexpr int SYNTHETIC_H{1000};

Consensus::Params B3Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    return params;
}

} // namespace

BOOST_AUTO_TEST_CASE(era_boundary_is_inclusive_of_final_legacy_height)
{
    const Consensus::Params params{B3Params()};

    BOOST_CHECK(Consensus::GetB3Era(SYNTHETIC_H - 1, params) == Consensus::B3Era::LEGACY);
    BOOST_CHECK(Consensus::GetB3Era(SYNTHETIC_H, params) == Consensus::B3Era::LEGACY);
    BOOST_CHECK(Consensus::GetB3Era(SYNTHETIC_H + 1, params) == Consensus::B3Era::MODERN);

    BOOST_CHECK(!Consensus::IsHardForkActive(params, SYNTHETIC_H));
    BOOST_CHECK(Consensus::IsHardForkActive(params, SYNTHETIC_H + 1));
}

BOOST_AUTO_TEST_CASE(unset_boundary_keeps_b3_chain_legacy)
{
    Consensus::Params params{};
    params.legacy_b3coin = true;

    BOOST_CHECK(Consensus::GetB3Era(-1, params) == Consensus::B3Era::LEGACY);
    BOOST_CHECK(Consensus::GetB3Era(0, params) == Consensus::B3Era::LEGACY);
    BOOST_CHECK(Consensus::GetB3Era(std::numeric_limits<int>::max(), params) ==
                Consensus::B3Era::LEGACY);
    BOOST_CHECK(!Consensus::IsHardForkActive(params, 0));
}

BOOST_AUTO_TEST_CASE(non_b3_chains_are_modern_at_every_height)
{
    const Consensus::Params params{};

    BOOST_CHECK(Consensus::GetB3Era(-1, params) == Consensus::B3Era::MODERN);
    BOOST_CHECK(Consensus::GetB3Era(0, params) == Consensus::B3Era::MODERN);
    BOOST_CHECK(Consensus::GetB3Era(std::numeric_limits<int>::max(), params) ==
                Consensus::B3Era::MODERN);
}

BOOST_AUTO_TEST_CASE(block_header_hash_switches_at_activation_height)
{
    CBlockHeader header;
    header.nVersion = 4;
    header.hashPrevBlock = uint256{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    header.hashMerkleRoot = uint256{"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"};
    header.nTime = 1'481'667'355;
    header.nBits = 0x1e0fffff;
    header.nNonce = 499'515;

    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = 100;

    BOOST_CHECK(header.GetLegacyB3Hash() != header.GetHash());
    BOOST_CHECK_EQUAL(header.GetHash(params, 99).GetHex(), header.GetLegacyB3Hash().GetHex());
    BOOST_CHECK_EQUAL(header.GetHash(params, 100).GetHex(), header.GetHash().GetHex());
}

BOOST_AUTO_TEST_CASE(b3_block_codec_marker_is_height_bound_and_versionbits_compatible)
{
    const Consensus::Params params{B3Params()};
    constexpr int32_t legacy_version{4};
    constexpr int32_t modern_version{static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION)};

    // The marker is the BIP9 top pattern plus reserved B3 bit 27. It must
    // leave lower versionbits usable for future upgrades.
    BOOST_CHECK_EQUAL(modern_version, 0x28000000);
    BOOST_CHECK(Consensus::HasB3BlockCodecV2(modern_version));
    BOOST_CHECK(Consensus::HasB3BlockCodecV2(modern_version | 0x00000004));
    BOOST_CHECK(!Consensus::HasB3BlockCodecV2(legacy_version));
    BOOST_CHECK(!Consensus::HasB3BlockCodecV2(0x30000000));

    DataStream encoded;
    encoded << modern_version;
    BOOST_CHECK_EQUAL(HexStr(encoded), "00000028");

    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(legacy_version, SYNTHETIC_H, params));
    BOOST_CHECK(!Consensus::HasExpectedB3BlockCodec(modern_version, SYNTHETIC_H, params));
    BOOST_CHECK(!Consensus::HasExpectedB3BlockCodec(legacy_version, SYNTHETIC_H + 1, params));
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(modern_version, SYNTHETIC_H + 1, params));

    Consensus::Params core_params{};
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(legacy_version, SYNTHETIC_H + 1, core_params));
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(modern_version, SYNTHETIC_H + 1, core_params));
}

BOOST_AUTO_TEST_CASE(legacy_chain_block_codec_is_marker_aware)
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{1}), 0};
    mtx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    mtx.mpa.push_back(CMpaRecord{/*payload_type=*/6,
                                 /*payload_version=*/1,
                                 /*payload=*/{0x01, 0x02, 0x03}});

    CBlock block;
    block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
    block.vtx.push_back(MakeTransactionRef(mtx));

    // A marker-modern block keeps the unmodified Core body through the
    // legacy-chain codec, plus the modern trailing signature vector (one
    // byte here: the empty vector), per the frozen Modern PoS V1 spec §5.
    DataStream via_legacy;
    via_legacy << legacy::TX_LEGACY(block);
    DataStream via_modern;
    via_modern << TX_MODERN(block);
    BOOST_REQUIRE_EQUAL(via_legacy.size(), via_modern.size() + 1);
    BOOST_CHECK(std::equal(via_modern.begin(), via_modern.end(), via_legacy.begin()));
    BOOST_CHECK_EQUAL(std::to_integer<int>(*(via_legacy.end() - 1)), 0); // empty sig vector

    // This is the decoder used by submitblock/getblocktemplate proposal mode.
    // Its B3-chain context is explicit; the in-header marker then selects
    // TX_MODERN for the body rather than guessing from height or payload bytes.
    CBlock decoded;
    BOOST_REQUIRE(DecodeHexBlk(decoded, HexStr(via_legacy), B3Params()));
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(
        decoded.nVersion, SYNTHETIC_H + 1, B3Params()));
    BOOST_REQUIRE_EQUAL(decoded.vtx.size(), 1U);
    BOOST_CHECK(!decoded.vtx[0]->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(decoded.vtx[0]->GetHash().GetHex(), block.vtx[0]->GetHash().GetHex());
    BOOST_CHECK(decoded.vtx[0]->mpa == block.vtx[0]->mpa);

    DataStream modern_round_trip;
    modern_round_trip << legacy::TX_LEGACY(decoded);
    BOOST_CHECK(std::equal(via_legacy.begin(), via_legacy.end(),
                           modern_round_trip.begin(), modern_round_trip.end()));

    // The context-free Core decoder must not reinterpret B3 MPA bytes. RPC
    // submission always has chain context and therefore never uses this form.
    CBlock contextless;
    BOOST_CHECK(!DecodeHexBlk(contextless, HexStr(via_legacy)));

    // Without the marker the same chain codec produces the historical body:
    // nTime transactions plus the trailing block signature.
    mtx.mpa.clear();
    mtx.nTime = 1'481'667'355;
    mtx.m_legacy_encoding = true;
    block.vtx = {MakeTransactionRef(mtx)};
    block.nVersion = 4;
    block.vchBlockSig = {0x01};
    DataStream legacy_bytes;
    legacy_bytes << legacy::TX_LEGACY(block);
    BOOST_CHECK(!std::equal(legacy_bytes.begin(), legacy_bytes.end(),
                            via_legacy.begin(), via_legacy.end()));

    CBlock legacy_decoded;
    BOOST_REQUIRE(DecodeHexBlk(legacy_decoded, HexStr(legacy_bytes), B3Params()));
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(
        legacy_decoded.nVersion, SYNTHETIC_H, B3Params()));
    BOOST_REQUIRE_EQUAL(legacy_decoded.vtx.size(), 1U);
    BOOST_CHECK(legacy_decoded.vtx[0]->IsLegacyEncoded());
    BOOST_CHECK_EQUAL(legacy_decoded.vtx[0]->nTime, mtx.nTime);
    BOOST_CHECK_EQUAL(legacy_decoded.vchBlockSig.size(), 1U);

    DataStream legacy_round_trip;
    legacy_round_trip << legacy::TX_LEGACY(legacy_decoded);
    BOOST_CHECK(std::equal(legacy_bytes.begin(), legacy_bytes.end(),
                           legacy_round_trip.begin(), legacy_round_trip.end()));
}

BOOST_AUTO_TEST_SUITE_END()

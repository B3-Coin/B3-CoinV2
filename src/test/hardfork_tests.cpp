// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

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

BOOST_AUTO_TEST_SUITE_END()

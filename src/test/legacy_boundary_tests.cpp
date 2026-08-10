// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Synthetic legacy-final-boundary enforcement. All values here are
//! test-only; no mainnet H or X is configured anywhere.

#include <consensus/block_codec.h>
#include <consensus/boundary.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <primitives/block.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(legacy_boundary_tests)

namespace {

//! Synthetic boundary: H is the final legacy height, X its exact hash.
constexpr int SYNTHETIC_H{1000};

CBlockHeader FinalLegacyHeader()
{
    CBlockHeader header;
    header.nVersion = 4;
    header.hashPrevBlock = uint256{"00000000000000000000000000000000000000000000000000000000000000aa"};
    header.hashMerkleRoot = uint256{"00000000000000000000000000000000000000000000000000000000000000bb"};
    header.nTime = 1'700'000'000;
    header.nBits = 0x1e0fffff;
    header.nNonce = 7;
    return header;
}

Consensus::Params BoundaryParams()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    params.legacy_final_hash = FinalLegacyHeader().GetLegacyB3Hash(); // X
    return params;
}

CBlockHeader ModernChildHeader(const uint256& prev)
{
    CBlockHeader header;
    header.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
    header.hashPrevBlock = prev;
    header.hashMerkleRoot = uint256{"00000000000000000000000000000000000000000000000000000000000000cc"};
    header.nTime = 1'700'000'360;
    header.nBits = 0x1d00ffff;
    header.nNonce = 1;
    return header;
}

} // namespace

BOOST_AUTO_TEST_CASE(correct_boundary_identities_pass)
{
    const Consensus::Params params{BoundaryParams()};
    const uint256 X{*params.legacy_final_hash};

    BOOST_REQUIRE(Consensus::LegacyFinalHeight(params).has_value());
    BOOST_CHECK_EQUAL(*Consensus::LegacyFinalHeight(params), SYNTHETIC_H);

    // The exact legacy hash X at H, and a modern child referencing X at H+1.
    const CBlockHeader final_header{FinalLegacyHeader()};
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(final_header, SYNTHETIC_H, params) ==
                Consensus::BoundaryCheck::OK);
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(ModernChildHeader(X), SYNTHETIC_H + 1, params) ==
                Consensus::BoundaryCheck::OK);

    // Codec legality across the boundary: legacy through H, modern from H+1.
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(final_header.nVersion, SYNTHETIC_H, params));
    BOOST_CHECK(Consensus::HasExpectedB3BlockCodec(ModernChildHeader(X).nVersion, SYNTHETIC_H + 1, params));

    // Heights away from the boundary carry no identity constraint here.
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(final_header, SYNTHETIC_H - 1, params) ==
                Consensus::BoundaryCheck::OK);
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(ModernChildHeader(X), SYNTHETIC_H + 2, params) ==
                Consensus::BoundaryCheck::OK);
}

BOOST_AUTO_TEST_CASE(wrong_block_hash_at_H_is_rejected)
{
    const Consensus::Params params{BoundaryParams()};

    CBlockHeader impostor{FinalLegacyHeader()};
    impostor.nNonce += 1; // different legacy hash
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(impostor, SYNTHETIC_H, params) ==
                Consensus::BoundaryCheck::WRONG_FINAL_HASH);
}

BOOST_AUTO_TEST_CASE(wrong_parent_at_H_plus_1_is_rejected)
{
    const Consensus::Params params{BoundaryParams()};

    const CBlockHeader stray{ModernChildHeader(
        uint256{"00000000000000000000000000000000000000000000000000000000000000ff"})};
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(stray, SYNTHETIC_H + 1, params) ==
                Consensus::BoundaryCheck::WRONG_FINAL_PARENT);
}

BOOST_AUTO_TEST_CASE(legacy_block_at_H_plus_1_is_rejected)
{
    const Consensus::Params params{BoundaryParams()};

    // A legacy-codec block above the boundary fails codec legality even
    // when it references X correctly.
    CBlockHeader late_legacy{FinalLegacyHeader()};
    late_legacy.hashPrevBlock = *params.legacy_final_hash;
    BOOST_CHECK(!Consensus::HasExpectedB3BlockCodec(late_legacy.nVersion, SYNTHETIC_H + 1, params));
}

BOOST_AUTO_TEST_CASE(modern_block_at_H_is_rejected)
{
    const Consensus::Params params{BoundaryParams()};

    // Codec legality refuses the modern marker at H...
    const CBlockHeader early_modern{ModernChildHeader(FinalLegacyHeader().hashPrevBlock)};
    BOOST_CHECK(!Consensus::HasExpectedB3BlockCodec(early_modern.nVersion, SYNTHETIC_H, params));
    // ...and independently, its marker-domain hash can never equal the
    // legacy X, so the boundary identity check rejects it too.
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(early_modern, SYNTHETIC_H, params) ==
                Consensus::BoundaryCheck::WRONG_FINAL_HASH);
}

BOOST_AUTO_TEST_CASE(reorg_crossing_H_is_prohibited)
{
    const Consensus::Params params{BoundaryParams()};

    // Disconnecting the block at H — or anything below it — crosses the
    // boundary and is permanently refused (DisconnectTip consults this).
    BOOST_CHECK(Consensus::DisconnectCrossesLegacyBoundary(params, SYNTHETIC_H));
    BOOST_CHECK(Consensus::DisconnectCrossesLegacyBoundary(params, SYNTHETIC_H - 5));
    BOOST_CHECK(Consensus::DisconnectCrossesLegacyBoundary(params, 0));
}

BOOST_AUTO_TEST_CASE(modern_reorg_entirely_above_H_is_permitted)
{
    const Consensus::Params params{BoundaryParams()};

    // A fork whose disconnects all sit above H never touches the boundary.
    BOOST_CHECK(!Consensus::DisconnectCrossesLegacyBoundary(params, SYNTHETIC_H + 1));
    BOOST_CHECK(!Consensus::DisconnectCrossesLegacyBoundary(params, SYNTHETIC_H + 42));

    // Without a configured boundary nothing is restricted.
    Consensus::Params unconfigured{};
    unconfigured.legacy_b3coin = true;
    BOOST_CHECK(!Consensus::DisconnectCrossesLegacyBoundary(unconfigured, 1));
    BOOST_CHECK(!Consensus::LegacyFinalHeight(unconfigured).has_value());

    // A boundary height without a finalized X constrains codecs and reorgs
    // but pins no identity yet.
    Consensus::Params no_hash{BoundaryParams()};
    no_hash.legacy_final_hash.reset();
    BOOST_CHECK(Consensus::CheckLegacyBoundaryHeader(FinalLegacyHeader(), SYNTHETIC_H, no_hash) ==
                Consensus::BoundaryCheck::OK);
    BOOST_CHECK(Consensus::DisconnectCrossesLegacyBoundary(no_hash, SYNTHETIC_H));
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Unit tests for the ported historical checkpoint rules: the hardened
//! checkpoint map (Checkpoints::CheckHardened) and the rolling deep-reorg
//! bound (Checkpoints::CheckSync / nCheckpointSpan). Pure functions, no chain.

#include <consensus/params.h>
#include <legacy/consensus.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(legacy_checkpoint_tests)

BOOST_AUTO_TEST_CASE(mainnet_checkpoint_data_is_verbatim)
{
    const auto& cp{legacy::MainnetCheckpoints()};
    // The exact historical set: 13 heights, genesis first, 95350 last.
    BOOST_CHECK_EQUAL(cp.size(), 13U);
    BOOST_CHECK_EQUAL(cp.begin()->first, 0);
    BOOST_CHECK_EQUAL(cp.begin()->second.GetHex(),
                      "4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a");
    BOOST_CHECK_EQUAL(cp.rbegin()->first, 95350);
    BOOST_CHECK_EQUAL(cp.rbegin()->second.GetHex(),
                      "095f1cb3cf1f1300ad99f891c2c0bb13cc374d9215781ad988e82cc0086a8e45");
    BOOST_CHECK_EQUAL(legacy::LEGACY_CHECKPOINT_SPAN, 500);
}

BOOST_AUTO_TEST_CASE(hardened_checkpoint_matches_pinned_height_only)
{
    Consensus::Params params;
    params.legacy_checkpoints = legacy::MainnetCheckpoints();

    const uint256 good{"907be67958dcd6d9d06c2c896f3b65aad687867ff342db2c7cb0ff5d717c5255"}; // height 77900
    const uint256 wrong{"0000000000000000000000000000000000000000000000000000000000000001"};

    // A pinned height must match exactly.
    BOOST_CHECK(legacy::CheckpointAllows(params, 77900, good));
    BOOST_CHECK(!legacy::CheckpointAllows(params, 77900, wrong));
    BOOST_CHECK(legacy::CheckpointAllows(params, 0,
        uint256{"4b0d7f133c5267d715d4d8992635a5490d1edd6b7072cce3f8fe116aba983b6a"}));
    BOOST_CHECK(!legacy::CheckpointAllows(params, 0, wrong));

    // Heights with no checkpoint always pass, whatever the hash.
    BOOST_CHECK(legacy::CheckpointAllows(params, 1, wrong));
    BOOST_CHECK(legacy::CheckpointAllows(params, 77901, wrong));
    BOOST_CHECK(legacy::CheckpointAllows(params, 200000, wrong));

    // No checkpoints configured: everything passes.
    Consensus::Params none;
    BOOST_CHECK(none.legacy_checkpoints.empty());
    BOOST_CHECK(legacy::CheckpointAllows(none, 77900, wrong));
}

BOOST_AUTO_TEST_CASE(reorg_depth_bound_matches_the_historical_span)
{
    Consensus::Params params;
    params.legacy_checkpoint_span = 500;

    // A block exactly span below the tip is refused; one deeper is refused;
    // one shallower is allowed. Boundary is `height <= tip - span`.
    BOOST_CHECK(legacy::ReorgDepthExceeded(params, /*block_height=*/500, /*tip=*/1000));  // == boundary
    BOOST_CHECK(legacy::ReorgDepthExceeded(params, 499, 1000));                            // deeper
    BOOST_CHECK(!legacy::ReorgDepthExceeded(params, 501, 1000));                           // shallower
    BOOST_CHECK(!legacy::ReorgDepthExceeded(params, 1000, 1000));                          // the tip itself
    BOOST_CHECK(!legacy::ReorgDepthExceeded(params, 1001, 1000));                          // ahead of tip

    // A chain shorter than the span rejects nothing (tip - span is negative,
    // and real heights are >= 0).
    BOOST_CHECK(!legacy::ReorgDepthExceeded(params, 1, 100));
    BOOST_CHECK(!legacy::ReorgDepthExceeded(params, 0, 100));

    // Span zero disables the rule entirely.
    Consensus::Params off;
    BOOST_CHECK_EQUAL(off.legacy_checkpoint_span, 0);
    BOOST_CHECK(!legacy::ReorgDepthExceeded(off, 1, 1000000));
}

BOOST_AUTO_TEST_SUITE_END()

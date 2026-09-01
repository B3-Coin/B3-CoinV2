// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/modern_pos_params.h>
#include <consensus/params.h>
#include <key.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/fn.h>
#include <modern/pos_v1.h>
#include <modern/stake.h>
#include <node/miner.h>
#include <node/stake_registry.h>
#include <node/staking.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <vector>

#include <test/util/modern_pos_setup.h>

using namespace b3test;

BOOST_FIXTURE_TEST_SUITE(modern_pos_tests, BasicTestingSetup)

//! Mainnet's sealed transition pins every modern-PoS value explicitly. Other
//! shipped networks remain unconfigured, and no production chain may carry a
//! test-only injection point.
BOOST_AUTO_TEST_CASE(mainnet_release_parameters_are_exact)
{
    const auto params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const Consensus::Params& consensus{params->GetConsensus()};

    BOOST_CHECK_EQUAL(consensus.hard_fork_height.value_or(0), 810'001);
    BOOST_CHECK(consensus.legacy_final_hash == uint256{
        "2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6"});
    BOOST_CHECK_EQUAL(consensus.transition_pow_length, 1'000);
    BOOST_CHECK_EQUAL(consensus.transition_pow_min_spacing, 60);
    BOOST_CHECK_EQUAL(consensus.transition_pow_max_future, 120);
    BOOST_CHECK_EQUAL(consensus.transition_pow_bits.value_or(0), 0x1f008000U);
    BOOST_CHECK_EQUAL(consensus.transition_pow_reward.value_or(-1), 0);
    BOOST_CHECK_EQUAL(consensus.min_stake_amount.value_or(-1),
                      333 * CAmount{1'000'000'000});
    BOOST_CHECK(consensus.fn_genesis_required);
    BOOST_CHECK_EQUAL(consensus.fn_genesis_manifest_version, 1);
    BOOST_CHECK_EQUAL(consensus.fn_genesis_manifest.size(), 3'592U);
    BOOST_CHECK(consensus.fn_genesis_rights_root == uint256{
        "e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec"});

    BOOST_REQUIRE(consensus.modern_pos.has_value());
    const Consensus::ModernPosParams& pos{*consensus.modern_pos};
    BOOST_CHECK(pos.Valid());
    BOOST_CHECK_EQUAL(pos.block_interval_seconds, 60);
    BOOST_CHECK_EQUAL(pos.round_seconds, 30);
    BOOST_CHECK_EQUAL(pos.f0_num, 1U);
    BOOST_CHECK_EQUAL(pos.f0_den, 1U);
    BOOST_CHECK_EQUAL(pos.sentinel_bits, 0x207fffffU);
    BOOST_CHECK_EQUAL(pos.max_future_seconds, 120);
    BOOST_CHECK_EQUAL(pos.reward, 19'836'712'254);
    BOOST_CHECK_EQUAL(pos.halving_interval, 525'600);
    BOOST_CHECK_EQUAL(pos.treasury_percent, 10U);
    BOOST_CHECK(pos.treasury_script == ParseHex(
        "76a91412602418ffc74640e37f1a73d0cdc255d2a07c3588ac"));
    BOOST_REQUIRE(pos.reorg_horizon.has_value());
    BOOST_CHECK_EQUAL(*pos.reorg_horizon, 1'440);
    BOOST_CHECK_EQUAL(pos.finality_epoch_blocks, 1'440);
    BOOST_CHECK_EQUAL(pos.checkpoint_interval, 10);
    BOOST_CHECK_EQUAL(pos.checkpoint_depth, 12);
    BOOST_CHECK_EQUAL(pos.max_epoch_extension, 10'080);
    BOOST_CHECK_EQUAL(pos.min_finality_set, 4);

    BOOST_CHECK(consensus.test_only_modern_pos_validator == nullptr);
    BOOST_CHECK(!consensus.test_only_asset_policies_active);
    BOOST_REQUIRE(consensus.fn_pod_activation_height.has_value());
    BOOST_REQUIRE(consensus.asset_activation_height.has_value());
    BOOST_REQUIRE(consensus.flowmesh_activation_height.has_value());
    BOOST_CHECK_EQUAL(*consensus.fn_pod_activation_height, 812'000);
    BOOST_CHECK_EQUAL(*consensus.asset_activation_height, 813'000);
    BOOST_CHECK_EQUAL(*consensus.flowmesh_activation_height, 815'000);
    BOOST_CHECK(Consensus::FnAssetActivationScheduleConfigured(consensus));
    BOOST_CHECK(Consensus::FlowMeshSeatBindingScheduleConfigured(consensus));
    BOOST_CHECK_EQUAL(*consensus.flowmesh_activation_height -
                          *consensus.asset_activation_height,
                      2'000);
    BOOST_CHECK(!Consensus::FlowMeshRulesActive(814'999, consensus));
    BOOST_CHECK(Consensus::FlowMeshRulesActive(815'000, consensus));
    BOOST_REQUIRE(consensus.busd_bridge.has_value());
    BOOST_CHECK(!consensus.busd_bridge->activation_height.has_value());
}

BOOST_AUTO_TEST_CASE(other_shipped_networks_have_no_transition_parameters)
{
    for (const ChainType chain : {ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto params{CreateChainParams(*m_node.args, chain)};
        const Consensus::Params& consensus{params->GetConsensus()};
        BOOST_CHECK(!consensus.modern_pos.has_value());
        BOOST_CHECK(consensus.test_only_modern_pos_validator == nullptr);
        BOOST_CHECK(!consensus.test_only_asset_policies_active);
        BOOST_CHECK(!consensus.min_stake_amount.has_value());
        BOOST_CHECK(!consensus.transition_pow_bits.has_value());
        BOOST_CHECK(!consensus.transition_pow_reward.has_value());
        BOOST_CHECK(!consensus.hard_fork_height.has_value());
        BOOST_CHECK(!consensus.legacy_final_hash.has_value());
        BOOST_CHECK(!consensus.fn_genesis_required);
    }
}

//! Structural sanity of the parameter type's safe fixture defaults.
BOOST_AUTO_TEST_CASE(parameter_block_is_structurally_valid)
{
    Consensus::ModernPosParams pos{};
    BOOST_CHECK(pos.Valid());
    BOOST_CHECK_EQUAL(pos.reorg_horizon.value_or(-1), 1440); // D RATIFIED 2026-08-21: one day at 60 s.

    pos.round_seconds = 0;
    BOOST_CHECK(!pos.Valid());
    pos = Consensus::ModernPosParams{};
    pos.f0_den = 0;
    BOOST_CHECK(!pos.Valid());
    pos = Consensus::ModernPosParams{};
    pos.reorg_horizon = 0;
    BOOST_CHECK(!pos.Valid());
}

//! The RULED mainnet corridor constant (owner, 2026-08-23) is the canonical
//! compact spelling of the 2^239 target. 0x20000080 encodes the SAME target
//! non-canonically; the consensus constant compared byte-for-byte on every
//! corridor header must be the canonical one, and a non-canonical configured
//! value fails closed like an unset one (the round-trip test the ruling asked
//! for).
BOOST_AUTO_TEST_CASE(corridor_bits_ruled_value_is_canonical_compact)
{
    constexpr uint32_t RULED_CORRIDOR_BITS{0x1f008000};
    constexpr uint32_t NONCANONICAL_SAME_TARGET{0x20000080};
    BOOST_CHECK(IsCanonicalCompactBits(RULED_CORRIDOR_BITS));
    BOOST_CHECK(!IsCanonicalCompactBits(NONCANONICAL_SAME_TARGET));
    const arith_uint256 ruled{arith_uint256().SetCompact(RULED_CORRIDOR_BITS)};
    const arith_uint256 noncanonical{arith_uint256().SetCompact(NONCANONICAL_SAME_TARGET)};
    BOOST_CHECK(ruled == noncanonical);
    BOOST_CHECK(ruled == (arith_uint256{1} << 239)); // 2^17 expected hashes per block
    BOOST_CHECK_EQUAL(ruled.GetCompact(), RULED_CORRIDOR_BITS);
    BOOST_CHECK_EQUAL(noncanonical.GetCompact(), RULED_CORRIDOR_BITS); // round-trips to the canonical form
    // The scaffolding constants used by the fixtures are canonical too.
    BOOST_CHECK(IsCanonicalCompactBits(EASY_BITS));
    BOOST_CHECK(!IsCanonicalCompactBits(0)); // zero target is never a difficulty
}

//! X-distribution PAUSE (owner ruling 2026-08-23): H configured, X unset.
//! The node accepts the chain through H and refuses EVERY block above H --
//! corridor blocks and legacy blocks alike -- without penalty and without
//! marking anything invalid; production refuses too. Pinning X resumes the
//! corridor from exactly that X.
BOOST_FIXTURE_TEST_CASE(h_without_x_fails_closed, ModernPosSetup)
{
    AdvanceLegacyToH();
    const uint256 real_x{Tip()->GetBlockHash()};
    ConfigureCorridor(/*x=*/std::nullopt);
    BOOST_CHECK(Consensus::LegacyBoundaryHeightOnly(m_node.chainman->GetConsensus()));
    BOOST_CHECK(!Consensus::LegacyBoundaryPinned(m_node.chainman->GetConsensus()));

    // A corridor block at H+1 is refused.
    const CBlock corridor{BuildCorridor(Tip(), {})};
    BOOST_CHECK(!Submit(corridor));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    {
        LOCK(cs_main);
        // Refused without an index entry: nothing was marked invalid, so the
        // follow-up release that pins X can still accept the real H+1.
        BOOST_CHECK(m_node.chainman->m_blockman.LookupBlockIndex(corridor.GetHash()) == nullptr);
    }
    // A legacy block at H+1 (an old client extending the dead legacy chain)
    // is refused the same way.
    BOOST_CHECK(!Submit(BuildLegacy(Tip(), {})));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    // Production refuses to enter the corridor with a blank X.
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    BOOST_CHECK_THROW(node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock(),
                      std::runtime_error);

    // The follow-up pins X: the very same corridor block is now accepted.
    ConfigureCorridor(real_x);
    BOOST_CHECK(Consensus::LegacyBoundaryPinned(m_node.chainman->GetConsensus()));
    BOOST_REQUIRE(Submit(corridor));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H + 1);
}

//! A configured non-canonical corridor constant fails closed (treated as
//! unconfigured); the canonical spelling of the same target works.
BOOST_FIXTURE_TEST_CASE(corridor_bits_must_be_canonical, ModernPosSetup)
{
    constexpr uint32_t CANONICAL{0x20008000};    // 2^247
    constexpr uint32_t NONCANONICAL{0x21000080}; // the same 2^247, non-canonical
    BOOST_REQUIRE(IsCanonicalCompactBits(CANONICAL));
    BOOST_REQUIRE(!IsCanonicalCompactBits(NONCANONICAL));
    BOOST_REQUIRE(arith_uint256().SetCompact(CANONICAL) == arith_uint256().SetCompact(NONCANONICAL));

    AdvanceLegacyToH();
    ConfigureCorridor(Tip()->GetBlockHash(), NONCANONICAL);
    BOOST_CHECK(!Submit(BuildCorridor(Tip(), {}, 60, NONCANONICAL)));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    ConfigureCorridor(Tip()->GetBlockHash(), CANONICAL);
    BOOST_REQUIRE(Submit(BuildCorridor(Tip(), {}, 60, CANONICAL)));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H + 1);
}

//! Corridor PACING (owner ruling 2026-08-23, after the verification that a
//! fixed difficulty alone lets hashpower compress the corridor): a corridor
//! block must be at least `transition_pow_min_spacing` (60 s) after its
//! parent and at most `transition_pow_max_future` (120 s) ahead of the
//! node's clock. So the corridor takes at least ~length * 60 s of real time
//! no matter how much hashpower shows up, while a lone CPU still proceeds at
//! its natural pace.
BOOST_FIXTURE_TEST_CASE(corridor_pacing_enforced, ModernPosSetup)
{
    AdvanceLegacyToH();
    ConfigureCorridor(Tip()->GetBlockHash());
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    BOOST_CHECK_EQUAL(consensus.transition_pow_min_spacing, 60);
    BOOST_CHECK_EQUAL(consensus.transition_pow_max_future, 120);

    // One-second and 59-second spacing are refused; exactly 60 s is accepted.
    BOOST_CHECK(!Submit(BuildCorridor(Tip(), {}, /*time_delta=*/1)));
    BOOST_CHECK(!Submit(BuildCorridor(Tip(), {}, /*time_delta=*/59)));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    BOOST_REQUIRE(Submit(BuildCorridor(Tip(), {}, /*time_delta=*/60)));
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H + 1);

    // A burst of blocks advances chain time by at least 60 s each: the
    // corridor cannot be compressed below length * spacing of chain time.
    const int64_t burst_start{Tip()->GetBlockTime()};
    constexpr int BURST{10};
    for (int i{0}; i < BURST; ++i) BOOST_REQUIRE(Submit(BuildCorridor(Tip(), {}, /*time_delta=*/60)));
    BOOST_CHECK_EQUAL(Tip()->GetBlockTime() - burst_start, BURST * 60);

    // The future bound paces acceptance in real time: a block dated more than
    // 120 s past the clock is refused (held, not marked invalid); within the
    // bound it is accepted.
    SetMockTime(Tip()->GetBlockTime() + 60);
    BOOST_CHECK(!Submit(BuildCorridor(Tip(), {}, /*time_delta=*/60 + 121)));
    {
        LOCK(cs_main);
        BOOST_CHECK(m_node.chainman->m_blockman.LookupBlockIndex(BuildCorridor(Tip(), {}, 60 + 121).GetHash()) == nullptr);
    }
    BOOST_REQUIRE(Submit(BuildCorridor(Tip(), {}, /*time_delta=*/60 + 120)));

    // Production respects the spacing: the template is never earlier than
    // parent + 60 s.
    SetMockTime(Tip()->GetBlockTime() + 1);
    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    const auto tmpl{node::BlockAssembler(m_node.chainman->ActiveChainstate(), nullptr, options).CreateNewBlock()};
    BOOST_REQUIRE(tmpl);
    BOOST_CHECK_GE(tmpl->block.GetBlockTime(), Tip()->GetBlockTime() + 60);
    SetMockTime(MOCK_NOW);
}

//! The automatic staking loop (release-v1 validator UX): started with
//! validator A's key it produces signed modern-PoS blocks on its own -- the
//! fixture's mock clock is far past every forced round time, so the pacing
//! wait is satisfied at once -- reports its status and the validator's
//! weights, and stops cleanly.
BOOST_FIXTURE_TEST_CASE(staking_loop_produces_blocks, ModernPosStakingSetup)
{
    AdvanceToModernPos();
    const int start_height{Tip()->nHeight};

    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr);
    {
        const auto idle{loop.Status(m_val_a)};
        BOOST_CHECK(idle.available);
        BOOST_CHECK(!idle.running);
        BOOST_CHECK(idle.modern_pos_active);
        BOOST_CHECK_EQUAL(idle.next_block_phase, "modern_pos");
        // One stake universe: weights are the snapshot weights in whole modern B3.
        BOOST_CHECK_EQUAL(idle.active_weight, STAKE_A / modern::FINALITY_WEIGHT_UNIT);
        BOOST_CHECK_EQUAL(idle.total_active_weight, (STAKE_A + STAKE_B) / modern::FINALITY_WEIGHT_UNIT);
        BOOST_CHECK_EQUAL(idle.stake_activation_depth, modern::STAKE_ACTIVATION_DEPTH);
    }
    // A node judges "initial block download" by tip age against the clock
    // and the loop idles during IBD, so bring the mock clock to the tip and
    // advance it a round per poll: each produced block's forced timestamp is
    // one interval past its parent, and the loop waits for the clock to
    // reach it.
    SetMockTime(Tip()->GetBlockTime() + 1);
    // The IBD flag is latched only at tip updates; every fixture block was
    // connected while the clock sat far ahead, so re-evaluate it now that
    // the tip is "recent" (a real node does this on its next tip update).
    WITH_LOCK(cs_main, m_node.chainman->UpdateIBDStatus());
    BOOST_REQUIRE(!m_node.chainman->IsInitialBlockDownload());
    std::string error;
    BOOST_REQUIRE_MESSAGE(loop.Start(m_key_a, CScript() << OP_TRUE, error), error);
    BOOST_CHECK(!loop.Start(m_key_a, CScript() << OP_TRUE, error)); // already running

    for (int i{0}; i < 600 && Tip()->nHeight < start_height + 3; ++i) {
        UninterruptibleSleep(std::chrono::milliseconds{50});
        SetMockTime(GetTime() + 60);
    }
    const auto running{loop.Status(std::nullopt)};
    loop.Stop();
    BOOST_CHECK_MESSAGE(Tip()->nHeight >= start_height + 3,
                        "loop state: " << running.state << " / last error: " << running.last_error);
    BOOST_CHECK(running.running);
    BOOST_CHECK(running.validator_key.has_value());
    BOOST_CHECK(running.validator_key && *running.validator_key == m_val_a);
    BOOST_CHECK_GE(running.blocks_produced, 3);
    BOOST_CHECK(!running.last_block_hash.IsNull());
    const auto stopped{loop.Status(std::nullopt)};
    BOOST_CHECK(!stopped.running);
    BOOST_CHECK_EQUAL(stopped.state, "stopped");
    // Every produced block is a signed modern-PoS block by validator A.
    {
        LOCK(cs_main);
        const CBlockIndex* tip{m_node.chainman->ActiveChain().Tip()};
        BOOST_CHECK(!tip->m_modern_pos_digest.IsNull());
    }
}

//! Scenario 1 — normal operation: deterministic production through the
//! assembler, signature by the validator key, seed chain verified block by
//! block, exact timestamps, sentinel bits, fees-only reward cap.
BOOST_FIXTURE_TEST_CASE(v1_normal_operation, ModernPosSetup)
{
    AdvanceToModernPos();
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    const CAmount W{STAKE_A + STAKE_B};

    node::BlockAssembler::Options options;
    options.coinbase_output_script = CScript() << OP_TRUE;
    options.include_dummy_extranonce = true;
    options.modern_pos_validator_key = m_val_a;

    for (int i{0}; i < 5; ++i) {
        const CBlockIndex* prev{Tip()};
        const int64_t expected_round{FindRound(prev, m_val_a, STAKE_A, W)};
        const uint256 expected_seed{SeedFor(prev)};

        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(), nullptr, options).CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        CBlock block{tmpl->block};
        BOOST_CHECK(Consensus::HasB3BlockCodecV2(block.nVersion));
        BOOST_CHECK_EQUAL(block.nBits, consensus.modern_pos->sentinel_bits);
        BOOST_CHECK_EQUAL(block.nNonce, 0U);
        BOOST_CHECK_EQUAL(block.vtx[0]->GetValueOut(), 0); // fees only under the cap
        BOOST_CHECK_EQUAL(block.GetBlockTime(),
                          modern::ModernPosBlockTime(prev->GetBlockTime(), expected_round,
                                                     *consensus.modern_pos));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        BOOST_REQUIRE_MESSAGE(Submit(block), "modern-PoS block at height "
                                                 << prev->nHeight + 1 << " rejected");
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev->nHeight + 1);

        // The connected index caches the block's eligibility digest — the
        // next height's seed — and it recomputes exactly.
        const uint256 expected_digest{modern::ModernPosEligibilityDigest(
            Domain(), expected_seed, static_cast<uint32_t>(Tip()->nHeight),
            static_cast<uint32_t>(expected_round), m_val_a)};
        BOOST_CHECK_EQUAL(
            WITH_LOCK(cs_main, return Tip()->m_modern_pos_digest).GetHex(),
            expected_digest.GetHex());
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H + SYN_CORRIDOR + 5);
}

//! Scenario 2 — low online stake: only the small validator (B, 0.5% of
//! stake) is online. Recovery rounds relax eligibility deterministically
//! until B qualifies; the block carries the exact round timestamp. A claim
//! of any earlier round is refused as ineligible. The PoS-native fork
//! choice prefers the lower-round block at equal height, and the horizon
//! refuses a deep fork.
BOOST_FIXTURE_TEST_CASE(v1_low_online_stake_recovery, ModernPosSetup)
{
    AdvanceToModernPos();
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    const CAmount W{STAKE_A + STAKE_B};

    const CBlockIndex* prev{Tip()};
    const int64_t round_b{FindRound(prev, m_val_b, STAKE_B, W)};
    BOOST_TEST_MESSAGE("validator B first eligible at round " << round_b);

    // An earlier round than B's first eligible round is refused at connect
    // (header-valid: the timestamp is exact for the claimed round).
    if (round_b > 0) {
        CBlock early{BuildPos(prev, m_val_b, round_b - 1, 0)};
        Sign(early, m_key_b);
        SubmitExpectConnectFailure(early);
    }

    // B's genuine recovery-round block connects.
    CBlock recovery{BuildPos(prev, m_val_b, round_b, 0)};
    Sign(recovery, m_key_b);
    BOOST_REQUIRE(Submit(recovery));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev->nHeight + 1);
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), recovery.GetHash().GetHex());
    BOOST_CHECK_EQUAL(recovery.GetBlockTime(),
                      modern::ModernPosBlockTime(prev->GetBlockTime(), round_b,
                                                 *consensus.modern_pos));

    // Fork choice, rule 2: A now produces at the SAME height in an earlier
    // round; equal height and equal accumulated work, but the lower round
    // wins deterministically and the chain reorganizes onto it.
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};
    if (round_a < round_b) {
        CBlock better{BuildPos(prev, m_val_a, round_a, 0)};
        Sign(better, m_key_a);
        BOOST_REQUIRE(Submit(better));
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, prev->nHeight + 1);
        BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), better.GetHash().GetHex());
    }

    // The modern reorganization horizon: a block forking deeper than D
    // below the tip is refused without ever entering the index. The horizon
    // governs modern-PoS heights only, so first extend the PoS span until
    // the fork point itself lies in the modern-PoS phase.
    {
        const int first_pos_height{SYN_H + SYN_CORRIDOR + 1};
        while (Tip()->nHeight < first_pos_height + *consensus.modern_pos->reorg_horizon) {
            const CBlockIndex* p{Tip()};
            CBlock ext{BuildPos(p, m_val_a, FindRound(p, m_val_a, STAKE_A, W), 0)};
            Sign(ext, m_key_a);
            BOOST_REQUIRE(Submit(ext));
        }
        const int deep_parent_height{Tip()->nHeight - *consensus.modern_pos->reorg_horizon - 1};
        const CBlockIndex* deep_parent{
            WITH_LOCK(cs_main, return chainman.ActiveChain()[deep_parent_height])};
        BOOST_REQUIRE(deep_parent != nullptr);
        CBlock deep{BuildPos(deep_parent, m_val_a,
                             FindRound(deep_parent, m_val_a, STAKE_A, W), 0, /*extra=*/9)};
        Sign(deep, m_key_a);
        BOOST_CHECK(!Submit(deep));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(deep.GetHash())) ==
                    nullptr);
    }
}

//! Scenario 3 — invalid validator signatures: corrupted, missing, and
//! wrong-key signatures are all refused and never move the tip. Each
//! variant carries a distinct identity (coinbase extra data), because the
//! signature itself lives outside the block hash.
BOOST_FIXTURE_TEST_CASE(v1_invalid_signature, ModernPosSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* prev{Tip()};
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};

    { // Corrupted signature.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/1)};
        Sign(block, m_key_a);
        block.vchBlockSig[0] ^= 0x01;
        SubmitExpectConnectFailure(block);
    }
    { // Missing signature (fails the contextual size rule).
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/2)};
        block.vchBlockSig.clear();
        BOOST_CHECK(!Submit(block));
    }
    { // Signed by a different key than the coinbase declares.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/3)};
        Sign(block, m_key_b);
        SubmitExpectConnectFailure(block);
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, prev->nHeight); // tip never moved

    // The honest block still connects afterwards.
    CBlock good{BuildPos(prev, m_val_a, round_a, 0)};
    Sign(good, m_key_a);
    BOOST_REQUIRE(Submit(good));
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), good.GetHash().GetHex());
}

//! Scenario 4 — invalid eligibility proofs: a validator with no stake, a
//! coinbase without a key declaration, a non-exact timestamp, wrong
//! sentinel bits, and a non-zero nonce are all refused.
BOOST_FIXTURE_TEST_CASE(v1_invalid_eligibility, ModernPosSetup)
{
    AdvanceToModernPos();
    ChainstateManager& chainman{*m_node.chainman};
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* prev{Tip()};

    { // A key with no active stake is never eligible, whatever the round.
        const modern::PosValidatorKey stranger{XOnly(MakeValidatorKey(0x33))};
        CBlock block{BuildPos(prev, stranger, /*round=*/0, 0)};
        Sign(block, MakeValidatorKey(0x33));
        SubmitExpectConnectFailure(block);
    }
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};
    { // Coinbase without the validator declaration.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/4)};
        CMutableTransaction cb{*block.vtx[0]};
        cb.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1}; // no key push
        block.vtx[0] = MakeTransactionRef(std::move(cb));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        SubmitExpectConnectFailure(block);
    }
    { // Non-exact timestamp: refused at the header, never stored.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/5)};
        ++block.nTime;
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        BOOST_CHECK(!Submit(block));
        BOOST_CHECK(WITH_LOCK(cs_main, return chainman.m_blockman.LookupBlockIndex(block.GetHash())) ==
                    nullptr);
    }
    { // Wrong nBits (not the sentinel): refused at the header.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/6)};
        block.nBits = EASY_BITS - 1;
        Sign(block, m_key_a);
        BOOST_CHECK(!Submit(block));
    }
    { // Non-zero nonce: refused at the header.
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/7)};
        block.nNonce = 1;
        Sign(block, m_key_a);
        BOOST_CHECK(!Submit(block));
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, prev->nHeight);
}

//! Scenario 5 — invalid rewards: the unconditional cap refuses a coinbase
//! above fees plus the configured reward, and ruling M6 refuses a reward
//! paid directly into a STAKE output even when it fits under the cap.
BOOST_FIXTURE_TEST_CASE(v1_invalid_reward, ModernPosSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* prev{Tip()};
    const int64_t round_a{FindRound(prev, m_val_a, STAKE_A, W)};

    { // Over the fees-only cap by a single unit.
        CBlock block{BuildPos(prev, m_val_a, round_a, /*coinbase_value=*/1)};
        Sign(block, m_key_a);
        SubmitExpectConnectFailure(block);
    }
    { // M6: reward into a STAKE output. Raise the provisional reward so the
      // amount fits under the cap and only the STAKE prohibition can fire.
        Consensus::Params& mutable_consensus{MutableConsensus()};
        mutable_consensus.modern_pos->reward = 2'000;
        CBlock block{BuildPos(prev, m_val_a, round_a, 0, /*extra=*/8)};
        CMutableTransaction cb{*block.vtx[0]};
        cb.vout[0].nValue = 1'500;
        cb.vout[0].scriptPubKey = modern::MakeStakeScript(m_val_a, CScript() << OP_TRUE);
        block.vtx[0] = MakeTransactionRef(std::move(cb));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Sign(block, m_key_a);
        SubmitExpectConnectFailure(block);
        mutable_consensus.modern_pos->reward = 0;
    }
    BOOST_CHECK_EQUAL(Tip()->nHeight, prev->nHeight);

    // The compliant block connects.
    CBlock good{BuildPos(prev, m_val_a, round_a, 0)};
    Sign(good, m_key_a);
    BOOST_REQUIRE(Submit(good));
}

//! Scenario 6 — restart and reindex: the persisted eligibility digests
//! reload with the block index (production continues without recomputing
//! seeds from block bodies), and a chainstate reindex reconnects the whole
//! legacy + corridor + modern-PoS history to the same tip.
BOOST_FIXTURE_TEST_CASE(v1_restart_and_reindex, ModernPosDiskSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};

    const auto produce{[&] {
        const CBlockIndex* prev{Tip()};
        CBlock block{BuildPos(prev, m_val_a, FindRound(prev, m_val_a, STAKE_A, W), 0)};
        Sign(block, m_key_a);
        BOOST_REQUIRE(Submit(block));
        return block.GetHash();
    }};
    for (int i{0}; i < 3; ++i) produce();
    const int pre_restart_height{Tip()->nHeight};
    const uint256 pre_restart_hash{Tip()->GetBlockHash()};
    const uint256 pre_restart_digest{WITH_LOCK(cs_main, return Tip()->m_modern_pos_digest)};
    BOOST_REQUIRE(!pre_restart_digest.IsNull());

    // ---- Simulated shutdown + restart over the persisted databases.
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_make_chainman();
    LoadVerifyActivateChainstate();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height);
    BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(), pre_restart_hash.GetHex());
    // The cached digest — the next block's seed — survived on disk.
    BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return Tip()->m_modern_pos_digest).GetHex(),
                      pre_restart_digest.GetHex());

    // Production continues seamlessly from the reloaded seed.
    produce();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height + 1);

    // ---- Chainstate reindex: rebuild by reconnecting the entire history
    // (legacy replay-scoped admission, corridor scrypt, modern-PoS
    // eligibility and signatures) from the block files.
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_args.ForceSetArg("-reindex-chainstate", "1");
    m_make_chainman();
    LoadVerifyActivateChainstate();
    m_args.ForceSetArg("-reindex-chainstate", "0");
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height + 1);

    // And the chain still extends after the reindex.
    produce();
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, pre_restart_height + 2);
}

//! The restored header-spam pre-filter: marker-modern headers are
//! header-only checkable once a corridor or modern-PoS policy is
//! configured; legacy headers stay body-judged; an unconfigured chain
//! keeps the filter open (status quo ante).
BOOST_AUTO_TEST_CASE(header_prefilter_is_marker_and_policy_aware)
{
    Consensus::Params params{};
    params.legacy_b3coin = true;

    CBlockHeader legacy_header;
    legacy_header.nVersion = 4;
    legacy_header.nBits = 0x207fffff;
    legacy_header.nTime = 1'900'000'000;

    CBlockHeader modern_header{legacy_header};
    modern_header.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);

    // Nothing configured: nothing checkable, filter open for both.
    BOOST_CHECK(HasValidProofOfWork({&legacy_header, 1}, params));
    BOOST_CHECK(HasValidProofOfWork({&modern_header, 1}, params));

    // Modern-PoS policy configured: the sentinel is required of
    // marker-modern headers; legacy headers stay unfiltered.
    params.modern_pos = Consensus::ModernPosParams{};
    modern_header.nBits = params.modern_pos->sentinel_bits;
    BOOST_CHECK(HasValidProofOfWork({&modern_header, 1}, params));
    modern_header.nBits = 0x207ffffe;
    BOOST_CHECK(!HasValidProofOfWork({&modern_header, 1}, params));
    BOOST_CHECK(HasValidProofOfWork({&legacy_header, 1}, params));

    // Corridor policy configured as well: corridor-ground scrypt at the
    // corridor target also satisfies the pre-filter (phase is unknowable
    // header-only, so either policy admits the header).
    params.transition_pow_bits = 0x207fffff;
    modern_header.nBits = 0x207fffff;
    modern_header.nNonce = 0;
    while (!CheckTransitionPowEligibility(modern_header)) ++modern_header.nNonce;
    BOOST_CHECK(HasValidProofOfWork({&modern_header, 1}, params));

    // Corridor bits without a passing scrypt hash and without the sentinel:
    // refused as spam.
    while (CheckTransitionPowEligibility(modern_header)) ++modern_header.nNonce;
    params.modern_pos->sentinel_bits = 0x1d00ffff; // sentinel no longer matches
    BOOST_CHECK(!HasValidProofOfWork({&modern_header, 1}, params));
}

BOOST_AUTO_TEST_SUITE_END()

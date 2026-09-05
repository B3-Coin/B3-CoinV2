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
#include <crypto/bls.h>
#include <key.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/fn.h>
#include <modern/pos_v1.h>
#include <modern/stake.h>
#include <node/finality_binding_index.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/stake_registry.h>
#include <node/staking.h>
#include <node/validator_set.h>
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

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <test/util/modern_pos_setup.h>

using namespace b3test;

namespace {

using CoordinationMember =
    std::pair<modern::ValidatorKeyBytes, uint64_t>;

modern::ValidatorKeyBytes CoordinationValidator(const unsigned char id)
{
    modern::ValidatorKeyBytes key{};
    key[0] = id;
    key[31] = 0xa5;
    return key;
}

std::optional<bls::SecretKey> CoordinationBlsKey(const unsigned char id)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = id;
    ikm[31] = 0x6c;
    return bls::SecretKey::FromIKM(ikm);
}

std::optional<node::ValidatorSetSnapshot> BuildCoordinationSet(
    const std::vector<CoordinationMember>& members)
{
    node::FinalityBindingIndex bindings;
    std::map<node::ValidatorKey, CAmount> weights;
    std::vector<node::FinalityBindingIndex::Transition> transitions;
    for (const auto& [validator, weight] : members) {
        const auto bls_key{CoordinationBlsKey(validator[0])};
        if (!bls_key) return std::nullopt;
        transitions.push_back(
            {validator, {bls_key->GetPublicKey().Compressed(), 0, 1}});
        weights[validator] = static_cast<CAmount>(weight) *
                             modern::FINALITY_WEIGHT_UNIT;
    }
    bindings.ConnectBlock(/*height=*/1, transitions);
    return node::ValidatorSetSnapshot::Build(/*epoch=*/7, weights, bindings);
}

uint256 CoordinationHash(const unsigned char first,
                         const unsigned char last)
{
    uint256 hash{};
    hash.begin()[0] = first;
    hash.begin()[31] = last;
    return hash;
}

const uint256 COORDINATION_DOMAIN{CoordinationHash(0xd1, 0x01)};
const uint256 COORDINATION_SEED{CoordinationHash(0x5e, 0xed)};
constexpr int COORDINATION_HEIGHT{1'234};
constexpr int64_t SATURATED_ROUND{128};

} // namespace

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
    BOOST_CHECK_EQUAL(pos.min_finality_set, 2);

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
    BOOST_REQUIRE(consensus.busd_bridge->activation_height.has_value());
    BOOST_CHECK_EQUAL(*consensus.busd_bridge->activation_height, 811'001);
    BOOST_REQUIRE(consensus.bridge_withdrawal_activation_height.has_value());
    BOOST_CHECK_EQUAL(*consensus.bridge_withdrawal_activation_height, 811'001);
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

//! The advisory proposer order is a deterministic permutation when every
//! eligible validator fits into a distinct mainnet recovery-round window.
//! Rebuilding the same immutable snapshot from the opposite insertion order
//! must give every validator the same rank and delay.
BOOST_AUTO_TEST_CASE(preferred_proposer_ranks_and_delays_are_deterministic)
{
    Consensus::ModernPosParams pos{};
    BOOST_REQUIRE_EQUAL(pos.round_seconds, 30);

    std::vector<CoordinationMember> members;
    for (unsigned char id{1}; id <= 5; ++id) {
        members.emplace_back(CoordinationValidator(id), 1);
    }
    const auto set{BuildCoordinationSet(members)};
    BOOST_REQUIRE(set.has_value());

    std::vector<CoordinationMember> reversed{members};
    std::reverse(reversed.begin(), reversed.end());
    const auto rebuilt{BuildCoordinationSet(reversed)};
    BOOST_REQUIRE(rebuilt.has_value());
    BOOST_REQUIRE(set->SetHash() == rebuilt->SetHash());

    std::array<bool, 5> ranks{};
    std::array<bool, 5> delay_slots{};
    for (const auto& [validator, weight] : members) {
        const auto plan{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            SATURATED_ROUND, *set, validator, pos)};
        const auto repeat{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            SATURATED_ROUND, *set, validator, pos)};
        const auto from_rebuilt{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            SATURATED_ROUND, *rebuilt, validator, pos)};

        BOOST_CHECK(plan.action == node::PreferredProposerAction::SCHEDULE);
        BOOST_REQUIRE_LT(plan.rank, ranks.size());
        BOOST_CHECK(!ranks[plan.rank]);
        ranks[plan.rank] = true;
        BOOST_CHECK_EQUAL(plan.eligible_count, members.size());
        BOOST_CHECK(plan.delay == std::chrono::seconds{5} * plan.rank);
        BOOST_CHECK(plan.window == std::chrono::seconds{5});
        const size_t delay_slot{
            static_cast<size_t>(plan.delay / std::chrono::seconds{5})};
        BOOST_REQUIRE_LT(delay_slot, delay_slots.size());
        BOOST_CHECK(!delay_slots[delay_slot]);
        delay_slots[delay_slot] = true;

        BOOST_CHECK(repeat.action == plan.action);
        BOOST_CHECK_EQUAL(repeat.rank, plan.rank);
        BOOST_CHECK_EQUAL(repeat.eligible_count, plan.eligible_count);
        BOOST_CHECK(repeat.delay == plan.delay);
        BOOST_CHECK(repeat.window == plan.window);
        BOOST_CHECK(from_rebuilt.action == plan.action);
        BOOST_CHECK_EQUAL(from_rebuilt.rank, plan.rank);
        BOOST_CHECK_EQUAL(from_rebuilt.eligible_count,
                          plan.eligible_count);
        BOOST_CHECK(from_rebuilt.delay == plan.delay);
        BOOST_CHECK(from_rebuilt.window == plan.window);
        (void)weight;
    }
    BOOST_CHECK(std::ranges::all_of(ranks, [](const bool used) { return used; }));
    BOOST_CHECK(std::ranges::all_of(delay_slots,
                                    [](const bool used) { return used; }));
}

//! Selection is stake-weighted, not merely a hash-sort of validator keys.
//! At saturated recovery rounds both members are consensus-eligible; across
//! this fixed deterministic sample the member holding 90% of the stake must
//! receive a strong majority of the first-proposer positions.
BOOST_AUTO_TEST_CASE(preferred_proposer_first_rank_is_stake_weighted)
{
    Consensus::ModernPosParams pos{};
    const auto light{CoordinationValidator(1)};
    const auto heavy{CoordinationValidator(2)};
    const auto set{BuildCoordinationSet({{light, 1}, {heavy, 9}})};
    BOOST_REQUIRE(set.has_value());

    unsigned int heavy_first{0};
    constexpr unsigned int SAMPLES{256};
    for (unsigned int offset{0}; offset < SAMPLES; ++offset) {
        const int64_t round{SATURATED_ROUND + offset};
        const auto light_plan{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            round, *set, light, pos)};
        const auto heavy_plan{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            round, *set, heavy, pos)};
        BOOST_REQUIRE(light_plan.action ==
                      node::PreferredProposerAction::SCHEDULE);
        BOOST_REQUIRE(heavy_plan.action ==
                      node::PreferredProposerAction::SCHEDULE);
        BOOST_REQUIRE_EQUAL(light_plan.eligible_count, 2U);
        BOOST_REQUIRE_EQUAL(heavy_plan.eligible_count, 2U);
        BOOST_REQUIRE_NE(light_plan.rank, heavy_plan.rank);
        if (heavy_plan.rank == 0) ++heavy_first;
    }
    BOOST_CHECK_GT(heavy_first, SAMPLES * 3 / 4);
}

//! Candidate membership is exactly the existing consensus eligibility result
//! for this recovery round. A snapshot member that is ineligible, and a key
//! outside the snapshot, wait for another round instead of receiving a slot.
BOOST_AUTO_TEST_CASE(preferred_proposer_filters_consensus_ineligible_keys)
{
    Consensus::ModernPosParams pos{};
    constexpr int64_t round{2};

    const auto heavy_a{CoordinationValidator(1)};
    const auto heavy_b{CoordinationValidator(2)};
    std::optional<modern::ValidatorKeyBytes> light;
    for (unsigned int id{3}; id <= 255; ++id) {
        const auto candidate{
            CoordinationValidator(static_cast<unsigned char>(id))};
        const uint256 digest{modern::ModernPosEligibilityDigest(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            static_cast<uint32_t>(round), candidate)};
        if (!modern::ModernPosEligible(digest, /*w=*/1, /*W=*/9, round,
                                       pos)) {
            light = candidate;
            break;
        }
    }
    BOOST_REQUIRE(light.has_value());

    const auto set{BuildCoordinationSet(
        {{heavy_a, 4}, {heavy_b, 4}, {*light, 1}})};
    BOOST_REQUIRE(set.has_value());
    BOOST_REQUIRE(set->IndexOf(*light).has_value());

    for (const auto& heavy : {heavy_a, heavy_b}) {
        const uint256 digest{modern::ModernPosEligibilityDigest(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            static_cast<uint32_t>(round), heavy)};
        BOOST_REQUIRE(modern::ModernPosEligible(digest, /*w=*/4, /*W=*/9,
                                                round, pos));
        const auto plan{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            round, *set, heavy, pos)};
        BOOST_CHECK(plan.action == node::PreferredProposerAction::SCHEDULE);
        BOOST_CHECK_EQUAL(plan.eligible_count, 2U);
        BOOST_CHECK_LT(plan.rank, 2U);
    }

    const auto ineligible{node::ComputePreferredProposerPlan(
        COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT, round,
        *set, *light, pos)};
    BOOST_CHECK(ineligible.action ==
                node::PreferredProposerAction::WAIT_NEXT_ROUND);
    BOOST_CHECK_EQUAL(ineligible.eligible_count, 2U);
    BOOST_CHECK_EQUAL(ineligible.delay.count(), 0);
    BOOST_CHECK_EQUAL(ineligible.window.count(), 0);

    const auto outsider{node::ComputePreferredProposerPlan(
        COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT, round,
        *set, CoordinationValidator(0xfe), pos)};
    BOOST_CHECK(outsider.action ==
                node::PreferredProposerAction::WAIT_NEXT_ROUND);
    BOOST_CHECK_EQUAL(outsider.eligible_count, 2U);
}

//! Five unique five-second windows fit safely in the 30-second mainnet round.
//! With six eligible validators exactly one defers to the next reshuffle; it
//! must never share a fallback timestamp with another honest validator.
BOOST_AUTO_TEST_CASE(preferred_proposer_overflow_defers_without_shared_slot)
{
    Consensus::ModernPosParams pos{};
    std::vector<CoordinationMember> members;
    for (unsigned char id{1}; id <= 6; ++id) {
        members.emplace_back(CoordinationValidator(id), 1);
    }
    const auto set{BuildCoordinationSet(members)};
    BOOST_REQUIRE(set.has_value());

    std::array<bool, 5> delay_slots{};
    unsigned int scheduled_count{0};
    unsigned int deferred_count{0};
    for (const auto& [validator, weight] : members) {
        const auto plan{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            SATURATED_ROUND, *set, validator, pos)};
        BOOST_CHECK_EQUAL(plan.eligible_count, members.size());
        if (plan.action == node::PreferredProposerAction::SCHEDULE) {
            ++scheduled_count;
            BOOST_REQUIRE_LT(plan.rank, delay_slots.size());
            BOOST_CHECK(!delay_slots[plan.rank]);
            delay_slots[plan.rank] = true;
            BOOST_CHECK(plan.delay == std::chrono::seconds{5} * plan.rank);
            BOOST_CHECK(plan.window == std::chrono::seconds{5});
        } else {
            BOOST_CHECK(plan.action ==
                        node::PreferredProposerAction::WAIT_NEXT_ROUND);
            ++deferred_count;
            BOOST_CHECK_EQUAL(plan.rank, 5U);
        }
        (void)weight;
    }
    BOOST_CHECK_EQUAL(scheduled_count, 5U);
    BOOST_CHECK_EQUAL(deferred_count, 1U);
    BOOST_CHECK(std::ranges::all_of(delay_slots,
                                    [](const bool used) { return used; }));
    const auto outsider{node::ComputePreferredProposerPlan(
        COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
        SATURATED_ROUND, *set, CoordinationValidator(0xfe), pos)};
    BOOST_CHECK(outsider.action ==
                node::PreferredProposerAction::WAIT_NEXT_ROUND);
    BOOST_CHECK_EQUAL(outsider.eligible_count, members.size());
}

//! Accelerated regtest uses one-second recovery rounds. One eligible proposer
//! owns that full round; every other eligible proposer waits for the next
//! deterministic reshuffle instead of using subsecond slots.
BOOST_AUTO_TEST_CASE(preferred_proposer_one_second_round_defers_backups)
{
    Consensus::ModernPosParams pos{};
    pos.round_seconds = 1;
    BOOST_REQUIRE(pos.Valid());

    const auto sole{CoordinationValidator(1)};
    const auto one{BuildCoordinationSet({{sole, 1}})};
    BOOST_REQUIRE(one.has_value());
    const auto scheduled{node::ComputePreferredProposerPlan(
        COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
        SATURATED_ROUND, *one, sole, pos)};
    BOOST_CHECK(scheduled.action == node::PreferredProposerAction::SCHEDULE);
    BOOST_CHECK_EQUAL(scheduled.rank, 0U);
    BOOST_CHECK_EQUAL(scheduled.eligible_count, 1U);
    BOOST_CHECK_EQUAL(scheduled.delay.count(), 0);
    BOOST_CHECK(scheduled.window == std::chrono::seconds{1});

    const auto second{CoordinationValidator(2)};
    const auto two{BuildCoordinationSet({{sole, 1}, {second, 1}})};
    BOOST_REQUIRE(two.has_value());
    unsigned int scheduled_count{0};
    unsigned int deferred_count{0};
    for (const auto& validator : {sole, second}) {
        const auto plan{node::ComputePreferredProposerPlan(
            COORDINATION_DOMAIN, COORDINATION_SEED, COORDINATION_HEIGHT,
            SATURATED_ROUND, *two, validator, pos)};
        BOOST_CHECK_EQUAL(plan.eligible_count, 2U);
        if (plan.action == node::PreferredProposerAction::SCHEDULE) {
            ++scheduled_count;
            BOOST_CHECK_EQUAL(plan.rank, 0U);
            BOOST_CHECK_EQUAL(plan.delay.count(), 0);
            BOOST_CHECK(plan.window == std::chrono::seconds{1});
        } else {
            ++deferred_count;
            BOOST_CHECK(plan.action ==
                        node::PreferredProposerAction::WAIT_NEXT_ROUND);
            BOOST_CHECK_EQUAL(plan.rank, 1U);
        }
    }
    BOOST_CHECK_EQUAL(scheduled_count, 1U);
    BOOST_CHECK_EQUAL(deferred_count, 1U);
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

    node::StakingLoop loop(*m_node.chainman, /*mempool=*/nullptr,
                           m_path_root / "finality_signer");
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

    // Producer policy may request an exact existing recovery round, but the
    // assembler still refuses it when this validator is not consensus-
    // eligible. The low-weight validator's first eligible round gives us an
    // exact negative case by definition.
    {
        const CBlockIndex* prev{Tip()};
        const int64_t round_b{FindRound(prev, m_val_b, STAKE_B, W)};
        BOOST_REQUIRE_GT(round_b, 0);
        node::BlockAssembler::Options exact_bad{options};
        exact_bad.modern_pos_validator_key = m_val_b;
        exact_bad.modern_pos_round = static_cast<uint32_t>(round_b - 1);
        BOOST_CHECK_THROW(
            node::BlockAssembler(chainman.ActiveChainstate(), nullptr,
                                 exact_bad)
                .CreateNewBlock(),
            std::runtime_error);
    }

    // The preferred-proposer schedule is deliberately not a validity rule.
    // At a saturated round both validators are eligible; build and accept the
    // rank-one backup directly to prove old/non-coordinating producers remain
    // consensus-compatible.
    {
        const CBlockIndex* prev{Tip()};
        constexpr int64_t round{128};
        std::shared_ptr<const node::ValidatorSetSnapshot> set;
        {
            LOCK(cs_main);
            Chainstate& chainstate{chainman.ActiveChainstate()};
            BOOST_REQUIRE(chainstate.ModernEligibilityWeights(m_val_a,
                                                               *prev));
            set = chainstate.ModernFinality().SetInForceAt(
                prev->nHeight + 1, consensus);
        }
        BOOST_REQUIRE(set != nullptr);
        const auto plan_a{node::ComputePreferredProposerPlan(
            Domain(), SeedFor(prev), prev->nHeight + 1, round, *set, m_val_a,
            *consensus.modern_pos)};
        const auto plan_b{node::ComputePreferredProposerPlan(
            Domain(), SeedFor(prev), prev->nHeight + 1, round, *set, m_val_b,
            *consensus.modern_pos)};
        BOOST_REQUIRE(plan_a.action ==
                      node::PreferredProposerAction::SCHEDULE);
        BOOST_REQUIRE(plan_b.action ==
                      node::PreferredProposerAction::SCHEDULE);
        BOOST_REQUIRE_NE(plan_a.rank, plan_b.rank);
        const bool backup_is_a{plan_a.rank > plan_b.rank};

        node::BlockAssembler::Options exact{options};
        exact.modern_pos_validator_key = backup_is_a ? m_val_a : m_val_b;
        exact.modern_pos_round = static_cast<uint32_t>(round);
        const auto tmpl{node::BlockAssembler(chainman.ActiveChainstate(),
                                             nullptr, exact)
                            .CreateNewBlock()};
        BOOST_REQUIRE(tmpl);
        CBlock backup{tmpl->block};
        backup.hashMerkleRoot = BlockMerkleRoot(backup);
        Sign(backup, backup_is_a ? m_key_a : m_key_b);
        BOOST_REQUIRE(Submit(backup));
        BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(),
                          backup.GetHash().GetHex());
    }

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
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H + SYN_CORRIDOR + 6);
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

//! Scenario 3 — a malicious peer must not poison a valid block identity by
//! sending corrupted, missing, or wrong-key trailing signature bytes first.
//! Those bytes live outside the block hash: each malformed copy is rejected
//! as mutated without storing data or marking the header failed, then the
//! authentic same-hash body is accepted.
BOOST_FIXTURE_TEST_CASE(v1_invalid_signature, ModernPosSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};
    unsigned int extra{1};
    const auto reject_mutation_then_accept =
        [&](const auto& mutate_signature) {
            const CBlockIndex* prev{Tip()};
            const int64_t round_a{
                FindRound(prev, m_val_a, STAKE_A, W)};
            CBlock good{BuildPos(prev, m_val_a, round_a, 0, extra++)};
            Sign(good, m_key_a);
            CBlock bad{good};
            mutate_signature(bad);
            BOOST_REQUIRE(bad.GetHash() == good.GetHash());

            BOOST_CHECK(!Submit(bad));
            BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(),
                              prev->GetBlockHash().GetHex());
            {
                LOCK(cs_main);
                const CBlockIndex* index{
                    m_node.chainman->m_blockman.LookupBlockIndex(
                        good.GetHash())};
                BOOST_REQUIRE(index != nullptr);
                BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
                BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
            }

            BOOST_REQUIRE(Submit(good));
            BOOST_CHECK_EQUAL(Tip()->GetBlockHash().GetHex(),
                              good.GetHash().GetHex());
        };

    reject_mutation_then_accept([](CBlock& block) {
        block.vchBlockSig[0] ^= 0x01;
    });
    reject_mutation_then_accept([](CBlock& block) {
        block.vchBlockSig.clear();
    });
    reject_mutation_then_accept([&](CBlock& block) {
        Sign(block, m_key_b);
    });
}

//! A malformed duplicate must also be unable to replace the valid bytes of an
//! already-stored side-chain block when that branch is next activated. The
//! duplicate has the same header/hash, so activation must reload the body
//! accepted on disk rather than trusting the duplicate caller's trailing
//! signature.
BOOST_FIXTURE_TEST_CASE(v1_stored_body_wins_over_bad_signature_duplicate,
                        ModernPosSetup)
{
    AdvanceToModernPos();
    const CAmount W{STAKE_A + STAKE_B};
    const CBlockIndex* parent{Tip()};

    CBlock active{BuildPos(parent, m_val_a,
                           FindRound(parent, m_val_a, STAKE_A, W), 0,
                           /*extra=*/41)};
    Sign(active, m_key_a);
    BOOST_REQUIRE(Submit(active));
    BOOST_REQUIRE(Tip()->GetBlockHash() == active.GetHash());

    CBlock stored_side{BuildPos(parent, m_val_b,
                                FindRound(parent, m_val_b, STAKE_B, W), 0,
                                /*extra=*/42)};
    Sign(stored_side, m_key_b);
    BOOST_REQUIRE(Submit(stored_side));
    BOOST_REQUIRE(Tip()->GetBlockHash() == active.GetHash());

    CBlockIndex* active_index{nullptr};
    CBlockIndex* side_index{nullptr};
    {
        LOCK(cs_main);
        active_index = m_node.chainman->m_blockman.LookupBlockIndex(
            active.GetHash());
        side_index = m_node.chainman->m_blockman.LookupBlockIndex(
            stored_side.GetHash());
        BOOST_REQUIRE(active_index != nullptr);
        BOOST_REQUIRE(side_index != nullptr);
        BOOST_REQUIRE(side_index->nStatus & BLOCK_HAVE_DATA);
        BOOST_REQUIRE(!(side_index->nStatus & BLOCK_FAILED_VALID));
    }

    BlockValidationState invalidate_state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(
        invalidate_state, active_index));
    BOOST_REQUIRE(Tip()->GetBlockHash() == parent->GetBlockHash());

    CBlock bad_duplicate{stored_side};
    bad_duplicate.vchBlockSig[0] ^= 0x01;
    BOOST_REQUIRE(bad_duplicate.GetHash() == stored_side.GetHash());
    BOOST_REQUIRE(Submit(bad_duplicate));
    BOOST_CHECK(Tip()->GetBlockHash() == stored_side.GetHash());
    {
        LOCK(cs_main);
        BOOST_CHECK(!(side_index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(side_index->nStatus & BLOCK_HAVE_DATA);
    }
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

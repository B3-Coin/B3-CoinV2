// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 9 of the Modern PoS V1 finality plan: deterministic validator-set
// snapshots from the single stake/binding universe — ordering, weights, BLS
// keys, members_root, total/quorum weight, set hash; immutability;
// reproducibility after rebuild; chain-level enumeration from the trackers.

#include <consensus/amount.h>
#include <crypto/bls.h>
#include <modern/finality_types.h>
#include <modern/stake.h>
#include <node/finality_binding_index.h>
#include <node/stake_tracker.h>
#include <node/validator_set.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <vector>

using namespace b3test;
using node::FinalityBindingIndex;
using node::ValidatorSetSnapshot;

namespace {

modern::ValidatorKeyBytes VK(const unsigned char fill) { modern::ValidatorKeyBytes k; k.fill(fill); return k; }

bls::SecretKey BlsK(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[1] = static_cast<unsigned char>(i >> 8);
    ikm[31] = 0x5C;
    return *bls::SecretKey::FromIKM(ikm);
}

std::string Hex(const uint256& u) { return HexStr(std::span<const unsigned char>(u.begin(), 32)); }

constexpr CAmount UNIT{modern::FINALITY_WEIGHT_UNIT};

} // namespace

BOOST_FIXTURE_TEST_SUITE(validator_set_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(unit_is_whole_modern_b3)
{
    BOOST_CHECK_EQUAL(modern::FINALITY_WEIGHT_UNIT, 1'000'000'000);
    BOOST_CHECK_EQUAL(modern::FINALITY_WEIGHT_UNIT, 1000 * COIN); // 1 modern B3 = 1 kB3 = 1,000 legacy B3
}

BOOST_AUTO_TEST_CASE(build_ordering_weights_exclusions_and_header)
{
    FinalityBindingIndex bindings;
    const auto kA{BlsK(1)}, kB{BlsK(2)}, kC{BlsK(3)}, kD{BlsK(4)};
    // bindings: A, B, C bound; D revoked; E (0x05) unbound
    bindings.ConnectBlock(10, {{VK(0x03), {kC.GetPublicKey().Compressed(), 0, 10}},
                               {VK(0x01), {kA.GetPublicKey().Compressed(), 0, 10}},
                               {VK(0x02), {kB.GetPublicKey().Compressed(), 0, 10}},
                               {VK(0x04), {kD.GetPublicKey().Compressed(), 0, 10}}});
    bindings.ConnectBlock(11, {{VK(0x04), {modern::BlsPubkeyBytes{}, 1, 11}}}); // D revokes
    std::map<node::ValidatorKey, CAmount> weights{
        {VK(0x03), 7 * UNIT + 999'999'999}, // floor -> 7
        {VK(0x01), 3 * UNIT},
        {VK(0x02), UNIT - 1},               // < 1 whole B3 -> dropped
        {VK(0x04), 5 * UNIT},               // revoked -> dropped
        {VK(0x05), 9 * UNIT},               // unbound -> dropped
    };
    const auto snap{ValidatorSetSnapshot::Build(7, weights, bindings)};
    BOOST_REQUIRE(snap.has_value());
    BOOST_REQUIRE_EQUAL(snap->Size(), 2u);
    // order: ascending validator_key -> 0x01 then 0x03
    BOOST_CHECK(snap->Members()[0].validator_key == VK(0x01));
    BOOST_CHECK(snap->Members()[1].validator_key == VK(0x03));
    BOOST_CHECK(snap->Members()[0].bls_pubkey == kA.GetPublicKey().Compressed());
    BOOST_CHECK(snap->Members()[1].bls_pubkey == kC.GetPublicKey().Compressed());
    BOOST_CHECK_EQUAL(snap->Members()[0].weight, 3u);
    BOOST_CHECK_EQUAL(snap->Members()[1].weight, 7u);
    BOOST_CHECK(snap->IndexOf(VK(0x03)) == std::optional<uint32_t>{1});
    BOOST_CHECK(!snap->IndexOf(VK(0x02)).has_value());
    // header
    const auto& h{snap->Header()};
    BOOST_CHECK_EQUAL(h.epoch, 7u);
    BOOST_CHECK_EQUAL(h.ruleset_version, 1);
    BOOST_CHECK_EQUAL(h.validator_count, 2u);
    BOOST_CHECK_EQUAL(h.total_weight, 10u);
    BOOST_CHECK_EQUAL(h.quorum_weight, modern::QuorumWeightV1(10)); // 7
    BOOST_CHECK_EQUAL(h.quorum_weight, 7u);
    // members_root and leaves exactly as the frozen construction
    const std::vector<uint256> leaves{modern::ValidatorSetLeaf(0, kA.GetPublicKey().Compressed(), 3),
                                      modern::ValidatorSetLeaf(1, kC.GetPublicKey().Compressed(), 7)};
    BOOST_CHECK(snap->Leaves() == leaves);
    BOOST_CHECK(h.members_root == *modern::ValidatorSetMembersRoot(leaves));
    // aggregate pubkey = sum of the member keys (independently aggregated)
    const std::vector<bls::VerifiedPublicKey> vk{bls::VerifiedPublicKey::FromPoP(kA.GetPublicKey(), kA.SignPoP()).value(),
                                                 bls::VerifiedPublicKey::FromPoP(kC.GetPublicKey(), kC.SignPoP()).value()};
    BOOST_CHECK(h.aggregate_pubkey == bls::AggregatePublicKeys(vk)->Compressed());
    // set hash = keccak(header)
    BOOST_CHECK(snap->SetHash() == modern::ValidatorSetHash(h));
    BOOST_CHECK(snap->SetHash() == modern::Keccak(h.Encode()));
    BOOST_CHECK_EQUAL(Hex(snap->SetHash()), "8d78c2bed64e48f0d9c49d151de41fc9d009fc6d67bc1d695bab35155c57d56d");
    // Deterministic: same inputs -> identical snapshot; epoch re-stamp changes hash only
    const auto again{ValidatorSetSnapshot::Build(7, weights, bindings)};
    BOOST_CHECK(*again == *snap);
    const auto e8{ValidatorSetSnapshot::Build(8, weights, bindings)};
    BOOST_CHECK(e8->Members() == snap->Members());
    BOOST_CHECK(e8->SetHash() != snap->SetHash());
}

BOOST_AUTO_TEST_CASE(fail_closed_and_immutability)
{
    FinalityBindingIndex bindings;
    std::map<node::ValidatorKey, CAmount> weights;
    // empty universe -> no snapshot
    BOOST_CHECK(!ValidatorSetSnapshot::Build(0, weights, bindings).has_value());
    // bound but zero weight -> no snapshot
    const auto k{BlsK(1)};
    bindings.ConnectBlock(1, {{VK(0x01), {k.GetPublicKey().Compressed(), 0, 1}}});
    weights[VK(0x01)] = UNIT - 1;
    BOOST_CHECK(!ValidatorSetSnapshot::Build(0, weights, bindings).has_value());
    weights[VK(0x01)] = UNIT;
    const auto snap{ValidatorSetSnapshot::Build(0, weights, bindings)};
    BOOST_REQUIRE(snap.has_value());
    const ValidatorSetSnapshot frozen{*snap};
    // Mutating the sources afterwards does not touch the snapshot value.
    bindings.ConnectBlock(2, {{VK(0x01), {modern::BlsPubkeyBytes{}, 1, 2}}}); // revoke
    weights[VK(0x01)] = 5 * UNIT;
    BOOST_CHECK(frozen == *snap);
    BOOST_CHECK_EQUAL(frozen.Members()[0].weight, 1u);
    BOOST_CHECK(!ValidatorSetSnapshot::Build(0, weights, bindings).has_value()); // now revoked
    // > MAX_FINALITY_SET members -> no snapshot (no truncation rule is invented)
    {
        FinalityBindingIndex big;
        std::map<node::ValidatorKey, CAmount> w;
        std::vector<FinalityBindingIndex::Transition> ts;
        for (unsigned i = 0; i < modern::MAX_FINALITY_SET + 1; ++i) {
            modern::ValidatorKeyBytes vk{};
            vk[0] = static_cast<unsigned char>(i >> 8);
            vk[1] = static_cast<unsigned char>(i);
            vk[31] = 0x01;
            // one BLS key per validator (distinct), cheap derivation
            const auto key{BlsK(100 + i)};
            ts.push_back({vk, {key.GetPublicKey().Compressed(), 0, 1}});
            w[vk] = UNIT;
        }
        big.ConnectBlock(1, ts);
        BOOST_CHECK(!ValidatorSetSnapshot::Build(0, w, big).has_value());
        // exactly MAX_FINALITY_SET is fine
        const auto last_it{w.rbegin()};
        modern::ValidatorKeyBytes drop{last_it->first};
        w.erase(drop);
        BOOST_CHECK(ValidatorSetSnapshot::Build(0, w, big).has_value());
    }
}

//! Chain level: stake + bindings in the corridor; the snapshot enumerates from
//! the stake tracker and binding index of the node, equals a snapshot built
//! from freshly rebuilt trackers (restart/reindex), and later bindings change
//! only later snapshots.
BOOST_FIXTURE_TEST_CASE(chain_level_enumeration_and_reproducibility, BindingFixture)
{
    Prepare();
    const auto kA{Bls(11)}, kB{Bls(22)};
    // Corridor block 1: two STAKE outputs (3 and 5 whole modern B3) for validators a
    // and b, and their bindings.
    CMutableTransaction stake;
    stake.version = 2;
    stake.vin.resize(1);
    stake.vin[0].prevout = COutPoint{m_fund_txid, 0};
    stake.vout.emplace_back(3 * UNIT, modern::MakeStakeScript(m_vk_a, CScript() << OP_TRUE));
    stake.vout.emplace_back(5 * UNIT, modern::MakeStakeScript(m_vk_b, CScript() << OP_TRUE));
    stake.vout.emplace_back(m_fund_value / 16 - 8 * UNIT - 100, CScript() << OP_TRUE);
    const auto ba{MakeBinding(m_validator_a, m_vk_a, &kA, 0)};
    const auto bb{MakeBinding(m_validator_b, m_vk_b, &kB, 0)};
    BOOST_REQUIRE(SubmitCorridor({stake, MakeTx(1, {ba.cell, bb.cell}, {ba.record, bb.record})}));
    const int stake_height{Tip()->nHeight};
    // Not yet mature: no members.
    {
        LOCK(cs_main);
        auto& st{m_node.chainman->ActiveChainstate().ModernStakeTracker()};
        BOOST_REQUIRE(st.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman, m_node.chainman->GetConsensus(), *Tip()));
        BOOST_CHECK(!ValidatorSetSnapshot::BuildAt(0, st, Tip()->nHeight, Index()).has_value());
    }
    // Mature the stake (STAKE_ACTIVATION_DEPTH = 20 corridor blocks).
    for (int i = 0; i < 20; ++i) BOOST_REQUIRE(SubmitCorridor({}));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, stake_height + 20);
    std::optional<ValidatorSetSnapshot> snap;
    {
        LOCK(cs_main);
        auto& st{m_node.chainman->ActiveChainstate().ModernStakeTracker()};
        BOOST_REQUIRE(st.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman, m_node.chainman->GetConsensus(), *Tip()));
        snap = ValidatorSetSnapshot::BuildAt(0, st, Tip()->nHeight, Index());
    }
    BOOST_REQUIRE(snap.has_value());
    BOOST_REQUIRE_EQUAL(snap->Size(), 2u);
    const bool a_first{m_vk_a < m_vk_b};
    const auto& first{snap->Members()[0]};
    const auto& second{snap->Members()[1]};
    BOOST_CHECK((a_first ? first : second).validator_key == m_vk_a);
    BOOST_CHECK_EQUAL((a_first ? first : second).weight, 3u);
    BOOST_CHECK_EQUAL((a_first ? second : first).weight, 5u);
    BOOST_CHECK((a_first ? first : second).bls_pubkey == kA.GetPublicKey().Compressed());
    BOOST_CHECK_EQUAL(snap->TotalWeight(), 8u);
    BOOST_CHECK_EQUAL(snap->QuorumWeight(), 6u); // floor(16/3)+1
    // Reproducible from freshly rebuilt trackers (restart/reindex).
    {
        LOCK(cs_main);
        node::StakeTracker fresh_stakes;
        node::FinalityBindingTracker fresh_bindings;
        BOOST_REQUIRE(fresh_stakes.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman, m_node.chainman->GetConsensus(), *Tip()));
        BOOST_REQUIRE(fresh_bindings.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman, m_node.chainman->GetConsensus(), *Tip()));
        const auto rebuilt{ValidatorSetSnapshot::BuildAt(0, fresh_stakes, Tip()->nHeight, fresh_bindings.Index())};
        BOOST_REQUIRE(rebuilt.has_value());
        BOOST_CHECK(*rebuilt == *snap);
        BOOST_CHECK(rebuilt->SetHash() == snap->SetHash());
    }
    // A later revocation changes only later snapshots; the old value is intact.
    const ValidatorSetSnapshot frozen{*snap};
    const auto rev{MakeBinding(m_validator_b, m_vk_b, nullptr, 1)};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(2, {rev.cell}, {rev.record})}));
    {
        LOCK(cs_main);
        auto& st{m_node.chainman->ActiveChainstate().ModernStakeTracker()};
        BOOST_REQUIRE(st.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman, m_node.chainman->GetConsensus(), *Tip()));
        const auto later{ValidatorSetSnapshot::BuildAt(1, st, Tip()->nHeight, Index())};
        BOOST_REQUIRE(later.has_value());
        BOOST_CHECK_EQUAL(later->Size(), 1u);
        BOOST_CHECK(later->Members()[0].validator_key == m_vk_a);
        BOOST_CHECK_EQUAL(frozen.Size(), 2u);
        BOOST_CHECK(frozen == *snap);
    }
}

BOOST_AUTO_TEST_SUITE_END()

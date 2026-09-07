// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <modern/chain_domain.h>
#include <node/finality_binding_index.h>
#include <node/finality_transport.h>
#include <node/validator_set.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <vector>

using namespace std::chrono_literals;

namespace {
using Queue = node::PendingFinalitySignatures;

node::FinalitySig Candidate(uint32_t index = 0, uint64_t height = 101)
{
    node::FinalitySig sig;
    sig.height = height;
    sig.index = index;
    sig.signature[0] = 1;
    return sig;
}

bls::SecretKey TestKey()
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = 0x65;
    ikm[31] = 0x91;
    return *bls::SecretKey::FromIKM(ikm);
}

/** Only indexed headers and a constructed immutable signing snapshot: no
 * mining, network, wallet, production files or signer-journal modifications. */
struct TransportChain : BasicTestingSetup {
    Consensus::Params params;
    node::FinalityTracker::State state;
    std::array<uint256, 180> hashes;
    std::array<CBlockIndex, 180> blocks;
    CChain chain;
    bls::SecretKey key{TestKey()};

    TransportChain() : BasicTestingSetup(ChainType::REGTEST, TestOpts{.setup_net = false})
    {
        params.legacy_b3coin = true;
        params.hard_fork_height = 1;
        params.transition_pow_length = 0;
        params.legacy_final_hash = uint256{2};
        params.hashGenesisBlock = uint256{1};
        params.modern_pos.emplace();
        params.modern_pos->checkpoint_interval = 10;
        params.modern_pos->checkpoint_depth = 12;
        for (size_t i{0}; i < blocks.size(); ++i) {
            hashes[i] = uint256{static_cast<uint8_t>(i + 1)};
            blocks[i].nHeight = static_cast<int>(i);
            blocks[i].phashBlock = &hashes[i];
            blocks[i].pprev = i == 0 ? nullptr : &blocks[i - 1];
        }
        chain.SetTip(blocks[100]);
        modern::ValidatorKeyBytes validator{};
        validator[0] = 1;
        node::FinalityBindingIndex bindings;
        bindings.ConnectBlock(0, {{validator, {key.GetPublicKey().Compressed(), 0, 0}}});
        const auto snapshot{node::ValidatorSetSnapshot::Build(
            0, {{validator, modern::FINALITY_WEIGHT_UNIT}}, bindings)};
        BOOST_REQUIRE(snapshot);
        state.bootstrapped = true;
        state.epoch_starts = {1};
        state.current = std::make_shared<const node::ValidatorSetSnapshot>(*snapshot);
        state.next = std::make_shared<const node::ValidatorSetSnapshot>(snapshot->WithEpoch(1));
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(finality_transport_recovery_tests)

BOOST_FIXTURE_TEST_CASE(future_admission_is_bounded_and_does_not_guess_epochs, TransportChain)
{
    const auto sig{Candidate()}; // checkpoint 101, local tip 100
    const auto future{node::InspectFinalityTransport(sig, state, chain, params)};
    BOOST_REQUIRE(future);
    BOOST_CHECK(!future->digest);
    BOOST_CHECK(!node::FinalityTransportDigest(sig, state, chain, params));

    auto invalid{sig};
    invalid.height = std::numeric_limits<uint64_t>::max();
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));
    invalid = sig;
    invalid.height = 171; // scheduled, but 71 blocks ahead
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));
    invalid.height = 102; // not a checkpoint
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));
    invalid = sig;
    invalid.index = 1; // snapshot has one validator
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));
    invalid = sig;
    invalid.epoch = 1; // a committed next set is not a started epoch
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));
    invalid.epoch = std::numeric_limits<uint64_t>::max();
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));

    chain.SetTip(blocks[97]);
    invalid = Candidate(0, 161);
    BOOST_CHECK_EQUAL(invalid.height - chain.Height(), node::MAX_FINALITY_TRANSPORT_AHEAD_BLOCKS);
    BOOST_REQUIRE(node::InspectFinalityTransport(invalid, state, chain, params));
    chain.SetTip(blocks[96]);
    BOOST_CHECK(!node::InspectFinalityTransport(invalid, state, chain, params));

    chain.SetTip(blocks[101]);
    const auto known{node::InspectFinalityTransport(sig, state, chain, params)};
    BOOST_REQUIRE(known);
    BOOST_CHECK(known->digest == node::FinalityTransportDigest(sig, state, chain, params));
    BOOST_CHECK(known->digest.has_value());
    state.lineage_broken = true;
    BOOST_CHECK(!node::InspectFinalityTransport(sig, state, chain, params));
}

BOOST_FIXTURE_TEST_CASE(valid_signature_before_block_waits_then_verifies_exact_bytes, TransportChain)
{
    auto good{Candidate()};
    chain.SetTip(blocks[113]);
    const auto digest{node::FinalityTransportDigest(good, state, chain, params)};
    BOOST_REQUIRE(digest);
    good.signature = key.Sign(std::span<const unsigned char>(digest->begin(), 32)).Compressed();
    auto bad{good};
    bad.signature.fill(0);
    chain.SetTip(blocks[100]);
    const auto admission{node::InspectFinalityTransport(good, state, chain, params)};
    BOOST_REQUIRE(admission);
    BOOST_CHECK(!admission->digest);
    Queue queue;
    BOOST_CHECK(queue.Add(bad, 1, admission->digest, 0us));
    BOOST_CHECK(queue.Add(good, 1, admission->digest, 0us));
    size_t charged{0};
    const auto resolver = [&](const Queue::Entry& entry) {
        return node::FinalityTransportDigest(entry.sig, state, chain, params);
    };
    const auto available = [](int64_t) { return true; };
    const auto budget = [&](int64_t) { ++charged; return true; };
    BOOST_CHECK(queue.TakeReadyWithSources(88, 1s, resolver, available, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 2U);
    chain.SetTip(blocks[101]); // hash known, but still twelve blocks too shallow
    BOOST_CHECK(queue.TakeReadyWithSources(89, 2s, resolver, available, budget).empty());
    chain.SetTip(blocks[112]);
    BOOST_CHECK(queue.TakeReadyWithSources(100, 3s, resolver, available, budget).empty());
    BOOST_CHECK_EQUAL(charged, 0U);
    chain.SetTip(blocks[113]);
    const auto ready{queue.TakeReadyWithSources(101, 4s, resolver, available, budget)};
    BOOST_REQUIRE_EQUAL(ready.size(), 2U);
    BOOST_CHECK_EQUAL(charged, 2U);
    size_t verified{0};
    for (const auto& entry : ready) {
        BOOST_CHECK(entry.digest_bound);
        BOOST_CHECK(entry.digest == *digest);
        const auto decoded{bls::Signature::Decode(entry.sig.signature)};
        const bool valid{decoded && bls::Verify(key.GetPublicKey(),
            std::span<const unsigned char>(entry.digest.begin(), 32), *decoded)};
        BOOST_CHECK_EQUAL(valid, entry.sig == good);
        verified += valid;
    }
    // Queue readiness alone never grants validity to the poisoned variant.
    BOOST_CHECK_EQUAL(verified, 1U);
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(alternate_source_survives_disconnect_and_pays_retry_budget)
{
    Queue queue;
    const auto sig{Candidate()};
    const uint256 digest{1};
    BOOST_CHECK(queue.Add(sig, 11, digest, 0us));
    BOOST_CHECK(!queue.Add(sig, 22, digest, 1s));
    BOOST_CHECK_EQUAL(queue.PeerSize(11), 1U);
    BOOST_CHECK_EQUAL(queue.PeerSize(22), 1U);
    const auto resolver = [&](const Queue::Entry&) { return std::optional<uint256>{digest}; };
    const auto connected = [](int64_t peer) { return peer == 22; };
    std::vector<int64_t> charged;
    BOOST_CHECK(queue.TakeReadyWithSources(101, 2s, resolver, connected,
        [&](int64_t peer) { charged.push_back(peer); return false; }).empty());
    BOOST_REQUIRE_EQUAL(charged.size(), 1U);
    BOOST_CHECK_EQUAL(charged[0], 22);
    BOOST_CHECK_EQUAL(queue.PeerSize(11), 0U);
    BOOST_CHECK_EQUAL(queue.Size(), 1U);
    const auto ready{queue.TakeReadyWithSources(101, 3s, resolver, connected,
        [&](int64_t peer) { charged.push_back(peer); return true; })};
    BOOST_REQUIRE_EQUAL(ready.size(), 1U);
    BOOST_CHECK_EQUAL(ready[0].peer, 22);
    BOOST_CHECK(ready[0].sig == sig);
    BOOST_CHECK_EQUAL(queue.PeerSize(22), 0U);
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(source_caps_duplicates_and_expiry_do_not_refresh_lifetime)
{
    Queue queue;
    const auto sig{Candidate()};
    BOOST_CHECK(queue.Add(sig, 1, std::nullopt, 0us));
    for (int64_t peer{2}; peer <= static_cast<int64_t>(Queue::MAX_SOURCES + 1); ++peer) {
        BOOST_CHECK(!queue.Add(sig, peer, std::nullopt, 299s));
        BOOST_CHECK_EQUAL(queue.PeerSize(peer), peer <= static_cast<int64_t>(Queue::MAX_SOURCES) ? 1U : 0U);
    }
    BOOST_CHECK_EQUAL(queue.Size(), 1U);
    queue.Expire(300s);
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
    for (int64_t peer{1}; peer <= static_cast<int64_t>(Queue::MAX_SOURCES); ++peer) {
        BOOST_CHECK_EQUAL(queue.PeerSize(peer), 0U);
    }
    BOOST_CHECK(queue.Add(sig, 1, std::nullopt, 301s));
    const auto no_digest = [](const Queue::Entry&) -> std::optional<uint256> { return std::nullopt; };
    BOOST_CHECK(queue.TakeReadyWithSources(100, 302s, no_digest,
        [](int64_t) { return false; }, [](int64_t) { return true; }).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 0U); // no surviving source; quotas reclaimed
}

BOOST_AUTO_TEST_CASE(future_binding_is_one_way_and_reorg_is_rejected_at_depth)
{
    Queue queue;
    const auto sig{Candidate()};
    const uint256 branch_a{1}, branch_b{2};
    const auto available = [](int64_t) { return true; };
    const auto budget = [](int64_t) { return true; };
    const auto missing = [](const Queue::Entry&) -> std::optional<uint256> { return std::nullopt; };
    BOOST_CHECK(queue.Add(sig, 1, std::nullopt, 0us));
    BOOST_CHECK(queue.TakeReadyWithSources(100, 1s, missing, available, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 1U);
    BOOST_CHECK(queue.TakeReadyWithSources(100, 2s,
        [&](const Queue::Entry&) { return std::optional<uint256>{branch_a}; }, available, budget).empty());
    BOOST_CHECK(queue.TakeReadyWithSources(100, 3s, missing, available, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 1U); // missing derived state before depth is not invalidity
    BOOST_CHECK(!queue.Add(sig, 2, branch_b, 4s)); // cannot rebind through a duplicate
    BOOST_CHECK_EQUAL(queue.PeerSize(2), 0U);
    BOOST_CHECK(queue.TakeReadyWithSources(101, 5s,
        [&](const Queue::Entry&) { return std::optional<uint256>{branch_b}; }, available, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
    BOOST_CHECK_EQUAL(queue.PeerSize(1), 0U);
    BOOST_CHECK(queue.Add(sig, 1, std::nullopt, 6s));
    BOOST_CHECK(queue.TakeReadyWithSources(101, 7s, missing, available, budget).empty());
    BOOST_CHECK_EQUAL(queue.Size(), 0U); // ready but invalid/unresolvable checkpoint
}

BOOST_AUTO_TEST_CASE(poisoned_variants_do_not_suppress_honest_late_arrival)
{
    Queue queue;
    const uint256 digest{1};
    for (unsigned char i{0}; i < 4; ++i) {
        auto poison{Candidate()};
        poison.signature.fill(0);
        poison.signature[1] = i;
        BOOST_REQUIRE(queue.Add(poison, 1, digest, 0us));
    }
    const auto key{TestKey()};
    auto honest{Candidate()};
    honest.signature = key.Sign(std::span<const unsigned char>(digest.begin(), 32)).Compressed();
    BOOST_REQUIRE(queue.Add(honest, 2, digest, 0us)); // fifth variant is not suppressed
    BOOST_CHECK_EQUAL(queue.PeerSize(2), 1U);
    BOOST_CHECK(queue.Add(Candidate(1), 1, digest, 0us));
    auto ready{queue.TakeReady(101, 1s,
        [](const Queue::Entry&) { return true; }, [](int64_t) { return true; })};
    BOOST_CHECK_EQUAL(ready.size(), Queue::MAX_RETRY_PER_COORDINATE + 1);
    BOOST_CHECK_EQUAL(std::count_if(ready.begin(), ready.end(), [](const auto& entry) { return entry.sig.index == 1; }), 1);
    for (int pass{2}; pass <= 3; ++pass) {
        const auto batch{queue.TakeReady(101, std::chrono::seconds{pass},
            [](const Queue::Entry&) { return true; }, [](int64_t) { return true; })};
        BOOST_CHECK_LE(batch.size(), Queue::MAX_RETRY_PER_COORDINATE);
        ready.insert(ready.end(), batch.begin(), batch.end());
    }
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
    BOOST_CHECK_EQUAL(std::count_if(ready.begin(), ready.end(), [&](const auto& entry) { return entry.sig == honest; }), 1);
    const auto signature{bls::Signature::Decode(honest.signature)};
    BOOST_REQUIRE(signature);
    BOOST_CHECK(bls::Verify(key.GetPublicKey(), std::span<const unsigned char>(digest.begin(), 32), *signature));
}

BOOST_AUTO_TEST_CASE(global_and_source_reference_caps_are_reclaimed)
{
    Queue queue;
    const uint256 digest{1};
    for (size_t i{0}; i < Queue::MAX_ENTRIES; ++i) {
        const int64_t peer{i < Queue::MAX_PEER_ENTRIES ? 1 : 2};
        const auto sig{Candidate(static_cast<uint32_t>(i % Queue::MAX_PEER_ENTRIES),
                                 i < Queue::MAX_PEER_ENTRIES ? 101 : 111)};
        BOOST_REQUIRE(queue.Add(sig, peer, digest, 0us));
    }
    BOOST_CHECK_EQUAL(queue.Size(), Queue::MAX_ENTRIES);
    BOOST_CHECK(!queue.Add(Candidate(0, 121), 3, digest, 1s));
    BOOST_CHECK(!queue.Add(Candidate(0, 111), 1, digest, 1s)); // source 1 already at its cap
    BOOST_CHECK(!queue.Add(Candidate(), 3, digest, 1s)); // existing payload may gain a source when full
    BOOST_CHECK_EQUAL(queue.PeerSize(3), 1U);
    BOOST_CHECK_EQUAL(queue.Size(), Queue::MAX_ENTRIES);
    queue.Expire(300s);
    BOOST_CHECK_EQUAL(queue.Size(), 0U);
    BOOST_CHECK_EQUAL(queue.PeerSize(1), 0U);
    BOOST_CHECK_EQUAL(queue.PeerSize(2), 0U);
    BOOST_CHECK_EQUAL(queue.PeerSize(3), 0U);
    BOOST_CHECK(queue.Add(Candidate(), 1, std::nullopt, 301s));
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/licenses/mit/.

#include <chain.h>
#include <interfaces/chain.h>
#include <modern/chain_domain.h>
#include <modern/finality_schedule.h>
#include <node/finality_binding_index.h>
#include <node/finality_signature.h>
#include <node/finality_signing_policy.h>
#include <node/validator_set.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>

namespace {
using node::FinalitySigner;
using node::FinalitySigningPolicy;
constexpr FinalitySigningPolicy MAINNET_POLICY{
    FinalitySigningPolicy::ForNetwork(ChainType::MAIN)};

bls::SecretKey SigningPolicyTestKey()
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = 0x37;
    ikm[31] = 0x91;
    return *bls::SecretKey::FromIKM(ikm);
}

/** Synthetic indexed headers and a real BLS snapshot, NOT a test of tracker
 * derivation or block consensus. No mining/network/live-wallet activity.
 * The non-const tracker is populated through its public State representation
 * solely to exercise production MaybeSign without a production test hook. */
struct SigningPolicyChain : BasicTestingSetup {
    Consensus::Params params;
    node::FinalityTracker tracker;
    std::array<uint256, 120> hashes;
    std::array<CBlockIndex, 120> blocks;
    CChain chain;
    bls::SecretKey key{SigningPolicyTestKey()};
    modern::ValidatorKeyBytes validator{};
    node::FinalitySignaturePool pool;

    SigningPolicyChain() : BasicTestingSetup(ChainType::REGTEST, TestOpts{.setup_net = false})
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
        chain.SetTip(blocks[0]);
        validator[0] = 1;
        node::FinalityBindingIndex bindings;
        bindings.ConnectBlock(0, {{validator, {key.GetPublicKey().Compressed(), 0, 0}}});
        const auto snapshot{node::ValidatorSetSnapshot::Build(
            0, {{validator, modern::FINALITY_WEIGHT_UNIT}}, bindings)};
        BOOST_REQUIRE(snapshot);
        auto& state{State()};
        state.bootstrapped = true;
        state.epoch_starts = {1};
        state.current = std::make_shared<const node::ValidatorSetSnapshot>(*snapshot);
        state.next = std::make_shared<const node::ValidatorSetSnapshot>(snapshot->WithEpoch(1));
    }

    node::FinalityTracker::State& State()
    {
        // The underlying tracker is genuinely non-const; no production
        // access-control or consensus-validation bypass is added.
        return const_cast<node::FinalityTracker::State&>(tracker.Current());
    }

    uint256 Domain() const
    {
        const auto domain{modern::ModernChainDomain(
            params.hashGenesisBlock, *params.legacy_final_hash)};
        BOOST_REQUIRE(domain);
        return *domain;
    }

    std::vector<node::FinalitySig> Sign(FinalitySigner& signer, const int tip)
    {
        chain.SetTip(blocks.at(tip));
        return signer.MaybeSign(tracker, chain, params, pool);
    }

    void Arm(FinalitySigner& signer, const fs::path& directory)
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(signer.SetKeyPersistent(
            key, validator, Domain(), directory, error), error);
    }

    node::FinalitySignerState Journal(const fs::path& directory)
    {
        node::FinalitySignerStore store;
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.Open(directory, Domain(), validator, error), error);
        BOOST_REQUIRE(store.State());
        return *store.State();
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(finality_signing_policy_tests)

BOOST_AUTO_TEST_CASE(mainnet_only_default_and_consensus_floor)
{
    BOOST_CHECK_EQUAL(MAINNET_POLICY.minimum_depth, 20);
    BOOST_CHECK_EQUAL(MAINNET_POLICY.EffectiveDepth(12), 20);
    BOOST_CHECK_EQUAL(MAINNET_POLICY.EffectiveDepth(25), 25);
    BOOST_CHECK_EQUAL(FinalitySigningPolicy{}.EffectiveDepth(3), 3);
    for (const auto network : {ChainType::TESTNET, ChainType::TESTNET4,
                              ChainType::SIGNET, ChainType::REGTEST}) {
        const auto policy{FinalitySigningPolicy::ForNetwork(network)};
        BOOST_CHECK_EQUAL(policy.minimum_depth, 0);
        BOOST_CHECK_EQUAL(policy.EffectiveDepth(3), 3);
    }
}

BOOST_AUTO_TEST_CASE(boundaries_spacing_and_upgrade_watermark)
{
    // Checkpoint 101 is 19/20/21 blocks deep at tips 120/121/122.
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(120, 12, 101, 10, -1));
    for (const int tip : {121, 122, 130}) {
        const auto window{MAINNET_POLICY.Checkpoints(tip, 12, 101, 10, -1)};
        BOOST_REQUIRE(window);
        BOOST_CHECK_EQUAL(window->first_checkpoint, 101);
        BOOST_CHECK_EQUAL(window->last_checkpoint, 101);
    }
    const auto two{MAINNET_POLICY.Checkpoints(131, 12, 101, 10, -1)};
    BOOST_REQUIRE(two);
    BOOST_CHECK_EQUAL(two->first_checkpoint, 101);
    BOOST_CHECK_EQUAL(two->last_checkpoint, 111);

    // Existing votes/locks/finalized checkpoints are never backfilled when
    // the new delay initially places the signable height below the watermark.
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(130, 12, 101, 10, 101));
    const auto next{MAINNET_POLICY.Checkpoints(131, 12, 101, 10, 101)};
    BOOST_REQUIRE(next);
    BOOST_CHECK_EQUAL(next->first_checkpoint, 111);
    BOOST_CHECK_EQUAL(next->last_checkpoint, 111);
    const auto unaligned{MAINNET_POLICY.Checkpoints(151, 12, 101, 10, 115)};
    BOOST_REQUIRE(unaligned);
    BOOST_CHECK_EQUAL(unaligned->first_checkpoint, 121);
    BOOST_CHECK_EQUAL(unaligned->last_checkpoint, 131);
}

BOOST_AUTO_TEST_CASE(stronger_consensus_invalid_inputs_and_height_limits)
{
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(125, 25, 101, 10, -1));
    const auto stronger{MAINNET_POLICY.Checkpoints(126, 25, 101, 10, -1)};
    BOOST_REQUIRE(stronger);
    BOOST_CHECK_EQUAL(stronger->first_checkpoint, 101);
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(120, 12, 101, 0, -1));
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(-1, 12, 101, 10, -1));
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(120, -1, 101, 10, -1));
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(120, 12, -1, 10, -1));
    BOOST_CHECK(!FinalitySigningPolicy{-1}.Checkpoints(120, 12, 101, 10, -1));
    constexpr int limit{std::numeric_limits<int>::max()};
    BOOST_CHECK(!MAINNET_POLICY.Checkpoints(limit, 12, 1, 10, limit));
    const auto edge{MAINNET_POLICY.Checkpoints(limit, 12, limit - 20, 10, -1)};
    BOOST_REQUIRE(edge);
    BOOST_CHECK_EQUAL(edge->first_checkpoint, limit - 20);
    BOOST_CHECK_EQUAL(edge->last_checkpoint, limit - 20);
}

BOOST_FIXTURE_TEST_CASE(actual_signer_waits_twenty_blocks_and_never_repeats, SigningPolicyChain)
{
    FinalitySigner signer{MAINNET_POLICY};
    signer.SetKey(key, validator);
    BOOST_CHECK(Sign(signer, 20).empty()); // checkpoint 1 is 19 deep
    BOOST_CHECK_EQUAL(signer.LastSignedHeight(), -1);
    const auto at_twenty{Sign(signer, 21)};
    BOOST_REQUIRE_EQUAL(at_twenty.size(), 1U);
    BOOST_CHECK_EQUAL(at_twenty[0].height, 1U);
    BOOST_CHECK(Sign(signer, 22).empty()); // 21 deep, already signed
    BOOST_CHECK(Sign(signer, 30).empty()); // checkpoint 11 is only 19 deep
    const auto next{Sign(signer, 31)};
    BOOST_REQUIRE_EQUAL(next.size(), 1U);
    BOOST_CHECK_EQUAL(next[0].height, 11U);
    BOOST_CHECK(Sign(signer, 31).empty());

    FinalitySigner fresh{MAINNET_POLICY};
    fresh.SetKey(key, validator);
    const auto at_twenty_one{Sign(fresh, 22)};
    BOOST_REQUIRE_EQUAL(at_twenty_one.size(), 1U);
    BOOST_CHECK_EQUAL(at_twenty_one[0].height, 1U);
}

BOOST_FIXTURE_TEST_CASE(default_signer_and_received_votes_keep_twelve_block_minimum, SigningPolicyChain)
{
    FinalitySigner delayed{MAINNET_POLICY};
    delayed.SetKey(key, validator);
    BOOST_CHECK(Sign(delayed, 13).empty());

    const auto fb{node::FinalitySignaturePool::ExpectedFinalizedBlock(
        0, 1, tracker.Current(), chain, params)};
    BOOST_REQUIRE(fb);
    const auto digest{modern::FinalityDigest(Domain(), *fb)};
    node::FinalitySig sig;
    sig.height = 1;
    sig.signature = key.Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
    chain.SetTip(blocks[12]);
    BOOST_CHECK(pool.Submit(sig, tracker, chain, params) ==
                node::FinalitySignaturePool::Accept::TOO_SHALLOW);
    chain.SetTip(blocks[13]);
    BOOST_CHECK(pool.Submit(sig, tracker, chain, params) ==
                node::FinalitySignaturePool::Accept::ACCEPTED);
    const auto hash_at = [&](const int height) -> std::optional<uint256> {
        return chain[height] ? std::optional<uint256>{chain[height]->GetBlockHash()} : std::nullopt;
    };
    BOOST_CHECK(modern::CheckCertificatePlacement(
        *fb, 12, tracker.Current().View(), *params.modern_pos, hash_at) ==
        modern::CertificatePlacement::INSUFFICIENT_DEPTH);
    BOOST_CHECK(modern::CheckCertificatePlacement(
        *fb, 13, tracker.Current().View(), *params.modern_pos, hash_at) ==
        modern::CertificatePlacement::OK);
    FinalitySigner original_timing;
    original_timing.SetKey(key, validator);
    const auto original_vote{Sign(original_timing, 13)};
    BOOST_REQUIRE_EQUAL(original_vote.size(), 1U);
    BOOST_CHECK_EQUAL(original_vote[0].height, 1U);
    BOOST_CHECK_EQUAL(params.modern_pos->checkpoint_depth, 12);
}

BOOST_FIXTURE_TEST_CASE(actual_signer_obeys_stronger_consensus_and_finalized_watermark, SigningPolicyChain)
{
    params.modern_pos->checkpoint_depth = 25;
    FinalitySigner signer{MAINNET_POLICY};
    signer.SetKey(key, validator);
    BOOST_CHECK(Sign(signer, 25).empty());
    const auto first{Sign(signer, 26)};
    BOOST_REQUIRE_EQUAL(first.size(), 1U);
    BOOST_CHECK_EQUAL(first[0].height, 1U);
    State().finalized = node::FinalizedCheckpoint{11, hashes[11], 0, 36};
    BOOST_CHECK(Sign(signer, 36).empty()); // 11 is finalized, not ours to sign
    const auto above_finalized{Sign(signer, 46)};
    BOOST_REQUIRE_EQUAL(above_finalized.size(), 1U);
    BOOST_CHECK_EQUAL(above_finalized[0].height, 21U);
}

BOOST_FIXTURE_TEST_CASE(upgrade_keeps_durable_vote_and_waits_for_next_checkpoint, SigningPolicyChain)
{
    const fs::path directory{m_path_root / "signing-policy-upgrade"};
    {
        FinalitySigner old;
        Arm(old, directory);
        BOOST_CHECK(Sign(old, 0).empty()); // safe fresh-journal initialization
        BOOST_REQUIRE_EQUAL(Sign(old, 13).size(), 1U);
    }
    const auto before{Journal(directory)};
    BOOST_CHECK_EQUAL(before.last_signed_height, 1);
    FinalitySigner upgraded{MAINNET_POLICY};
    Arm(upgraded, directory);
    for (const int tip : {13, 20, 21, 30}) BOOST_CHECK(Sign(upgraded, tip).empty());
    BOOST_CHECK(Journal(directory) == before);
    const auto next{Sign(upgraded, 31)};
    BOOST_REQUIRE_EQUAL(next.size(), 1U);
    BOOST_CHECK_EQUAL(next[0].height, 11U);
    BOOST_CHECK_EQUAL(Journal(directory).last_signed_height, 11);
}

BOOST_FIXTURE_TEST_CASE(extra_delay_does_not_relax_missing_journal_guard, SigningPolicyChain)
{
    const fs::path directory{m_path_root / "signing-policy-missing"};
    FinalitySigner delayed{MAINNET_POLICY};
    Arm(delayed, directory);
    for (const int tip : {13, 20}) {
        BOOST_CHECK(Sign(delayed, tip).empty()); // consensus 12 through 19 deep
        BOOST_CHECK(delayed.LastError().find("possibly deleted anti-equivocation record") !=
                    std::string::npos);
        node::FinalitySignerStore probe;
        std::string error;
        BOOST_REQUIRE_MESSAGE(probe.Open(directory, Domain(), validator, error), error);
        BOOST_CHECK(probe.IsAbsent());
        BOOST_CHECK(!probe.State());
    }
}

BOOST_FIXTURE_TEST_CASE(extra_delay_does_not_release_orphaned_vote, SigningPolicyChain)
{
    const fs::path directory{m_path_root / "signing-policy-orphan"};
    {
        FinalitySigner old;
        Arm(old, directory);
        BOOST_CHECK(Sign(old, 0).empty());
        BOOST_REQUIRE_EQUAL(Sign(old, 13).size(), 1U);
    }
    const auto before{Journal(directory)};
    hashes[1] = uint256{250}; // synthetic active branch no longer contains old vote
    FinalitySigner delayed{MAINNET_POLICY};
    Arm(delayed, directory);
    BOOST_CHECK(Sign(delayed, 31).empty());
    BOOST_CHECK(delayed.LastError().find("does not descend from signed checkpoint") !=
                std::string::npos);
    BOOST_CHECK(Journal(directory) == before);
}

BOOST_FIXTURE_TEST_CASE(pool_verification_budget_follows_exact_dedup_and_cheap_checks, SigningPolicyChain)
{
    using Pool = node::FinalitySignaturePool;
    using Accept = Pool::Accept;
    std::array<unsigned char, 32> ikm{};
    ikm.fill(0x84);
    const auto second_key{*bls::SecretKey::FromIKM(ikm)};
    modern::ValidatorKeyBytes second_validator{};
    second_validator[0] = 2;
    node::FinalityBindingIndex bindings;
    bindings.ConnectBlock(0, {
        {validator, {key.GetPublicKey().Compressed(), 0, 0}},
        {second_validator, {second_key.GetPublicKey().Compressed(), 0, 0}},
    });
    const auto snapshot{node::ValidatorSetSnapshot::Build(
        0, {{validator, modern::FINALITY_WEIGHT_UNIT},
            {second_validator, modern::FINALITY_WEIGHT_UNIT}}, bindings)};
    BOOST_REQUIRE(snapshot);
    State().current = std::make_shared<const node::ValidatorSetSnapshot>(*snapshot);
    State().next = std::make_shared<const node::ValidatorSetSnapshot>(snapshot->WithEpoch(1));
    chain.SetTip(blocks[113]);
    const auto idx_a{*snapshot->IndexOf(validator)};
    const auto idx_b{*snapshot->IndexOf(second_validator)};
    const auto make_sig = [&](const int height, const uint32_t index, const bls::SecretKey& secret) {
        const auto fb{Pool::ExpectedFinalizedBlock(0, height, tracker.Current(), chain, params)};
        BOOST_REQUIRE(fb);
        const auto digest{modern::FinalityDigest(Domain(), *fb)};
        node::FinalitySig result;
        result.height = height;
        result.index = index;
        result.signature = secret.Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        return result;
    };
    const auto a{make_sig(11, idx_a, key)};
    const auto b{make_sig(11, idx_b, second_key)};
    unsigned budget_calls{0};
    bool allow_verification{false};
    const auto budget = [&] { ++budget_calls; return allow_verification; };
    const auto submit = [&](const node::FinalitySig& sig) {
        return pool.Submit(sig, tracker, chain, params, nullptr, budget);
    };
    BOOST_CHECK_EQUAL(Pool::AcceptName(Accept::VERIFICATION_DEFERRED), "verification-deferred");
    BOOST_CHECK(submit(a) == Accept::VERIFICATION_DEFERRED);
    BOOST_CHECK_EQUAL(budget_calls, 1U);
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);
    allow_verification = true;
    BOOST_REQUIRE(submit(a) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(budget_calls, 2U);
    allow_verification = false;
    BOOST_CHECK(submit(a) == Accept::DUPLICATE);
    auto poisoned{a};
    poisoned.signature.fill(0);
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    poisoned.signature = b.signature;
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    BOOST_CHECK_EQUAL(budget_calls, 2U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, 11), 1U);
    BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == std::vector<node::FinalitySig>{a});
    BOOST_CHECK(submit(b) == Accept::VERIFICATION_DEFERRED);
    BOOST_CHECK_EQUAL(budget_calls, 3U);
    allow_verification = true;
    poisoned = b;
    poisoned.signature.fill(0);
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    BOOST_CHECK_EQUAL(budget_calls, 4U);
    BOOST_REQUIRE(submit(b) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(budget_calls, 5U);
    BOOST_CHECK(pool.BestCertificate(tracker, chain, params));

    auto bad{a};
    bad.index = snapshot->Size();
    BOOST_CHECK(submit(bad) == Accept::BAD_INDEX);
    bad = a;
    bad.epoch = 99;
    BOOST_CHECK(submit(bad) == Accept::UNKNOWN_EPOCH);
    bad = a;
    bad.height = 12;
    BOOST_CHECK(submit(bad) == Accept::NOT_CHECKPOINT);
    bad.height = 121;
    BOOST_CHECK(submit(bad) == Accept::NOT_CHECKPOINT);
    auto shallow{params};
    shallow.modern_pos->checkpoint_depth = 103;
    BOOST_CHECK(pool.Submit(a, tracker, chain, shallow, nullptr, budget) == Accept::TOO_SHALLOW);
    BOOST_CHECK(pool.Submit(a, tracker, chain, Consensus::Params{}, nullptr, budget) == Accept::STALE);
    BOOST_CHECK_EQUAL(budget_calls, 5U);

    for (int height{21}; height <= 81; height += 10) {
        BOOST_REQUIRE(pool.Submit(make_sig(height, idx_a, key), tracker, chain, params) == Accept::ACCEPTED);
    }
    BOOST_REQUIRE_EQUAL(pool.TrackedCheckpoints(), Pool::MAX_TRACKED_CHECKPOINTS);
    BOOST_CHECK(submit(make_sig(1, idx_a, key)) == Accept::POOL_FULL);
    BOOST_CHECK_EQUAL(budget_calls, 5U);
    const auto retained{pool.RelayableSignatures(tracker, chain, params)};
    const auto newest{make_sig(91, idx_a, key)};
    allow_verification = false;
    BOOST_CHECK(submit(newest) == Accept::VERIFICATION_DEFERRED);
    BOOST_CHECK_EQUAL(budget_calls, 6U);
    BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == retained);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, 91), 0U);
    allow_verification = true;
    poisoned = newest;
    poisoned.signature.fill(0);
    BOOST_CHECK(submit(poisoned) == Accept::BAD_SIGNATURE);
    BOOST_CHECK_EQUAL(budget_calls, 7U);
    BOOST_CHECK(pool.RelayableSignatures(tracker, chain, params) == retained);
    BOOST_REQUIRE(submit(newest) == Accept::ACCEPTED);
    BOOST_CHECK_EQUAL(budget_calls, 8U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, 11), 0U);
    BOOST_CHECK_EQUAL(pool.SignatureCount(0, 91), 1U);
    BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), Pool::MAX_TRACKED_CHECKPOINTS);
}

BOOST_FIXTURE_TEST_CASE(recovery_status_is_read_only_before_and_after_certificate_rebase, SigningPolicyChain)
{
    // The fixture supplies tracker state directly. This tests observation and
    // production signer/store behavior, not consensus validation of a carrier.
    const fs::path directory{m_path_root / "recovery-status"};
    FinalitySigner signer;
    const auto observe = [&] { return signer.RecoveryStatus(tracker, chain, params); };
    BOOST_CHECK_EQUAL(observe().state, "journal_unavailable");
    Arm(signer, directory);
    BOOST_CHECK_EQUAL(observe().state, "journal_missing");
    BOOST_CHECK(Sign(signer, 0).empty());
    const auto empty{observe()};
    BOOST_CHECK_EQUAL(empty.state, "no_ancestry_lock");
    BOOST_CHECK(empty.journal_present);
    BOOST_CHECK(!empty.lock_height);
    BOOST_REQUIRE_EQUAL(Sign(signer, 13).size(), 1U);
    const auto original{Journal(directory)};
    BOOST_CHECK_EQUAL(observe().state, "lock_matches_chain");

    // A shorter local tip is not evidence of an orphaned vote.
    chain.SetTip(blocks[0]);
    BOOST_CHECK_EQUAL(observe().state, "waiting_for_locked_height");
    BOOST_CHECK(!observe().blocked_on_orphan_vote);
    chain.SetTip(blocks[31]);
    hashes[1] = uint256{250};
    BOOST_CHECK(Sign(signer, 31).empty());
    for (int i{0}; i < 3; ++i) {
        const auto blocked{observe()};
        BOOST_CHECK_EQUAL(blocked.state, "blocked_on_orphan_vote");
        BOOST_CHECK(blocked.blocked_on_orphan_vote);
        BOOST_CHECK_EQUAL(blocked.observed_tip_height, 31);
        BOOST_CHECK(blocked.observed_tip_hash == hashes[31]);
        BOOST_CHECK(blocked.journal_open);
        BOOST_CHECK(blocked.journal_present);
        BOOST_CHECK(!blocked.permanent_error);
        BOOST_REQUIRE(blocked.lock_height);
        BOOST_CHECK_EQUAL(*blocked.lock_height, 1);
        BOOST_CHECK_EQUAL(blocked.last_signed_height, 1);
        BOOST_CHECK(blocked.last_signed_hash == original.last_signed_block_hash);
        BOOST_CHECK(blocked.lock_hash == original.lock_block_hash);
        BOOST_CHECK(blocked.lock_digest == original.last_signed_digest);
        BOOST_REQUIRE(blocked.current_chain_hash);
        BOOST_CHECK(*blocked.current_chain_hash == hashes[1]);
        BOOST_CHECK(blocked.lock_signing_set_hash == State().current->SetHash());
        BOOST_CHECK(blocked.lock_successor_set_hash == State().next->SetHash());
        BOOST_CHECK(!blocked.certificate_strictly_newer);
        BOOST_CHECK(Journal(directory) == original);
    }

    State().finalized = node::FinalizedCheckpoint{11, hashes[11], 0, 23};
    const auto before{observe()};
    BOOST_CHECK(before.blocked_on_orphan_vote);
    BOOST_CHECK(before.certificate_included);
    BOOST_CHECK(before.certificate_strictly_newer);
    BOOST_CHECK(before.certificate_same_epoch);
    BOOST_CHECK(before.certificate_same_set);
    BOOST_CHECK(before.certificate_reconstructible);
    BOOST_CHECK(Journal(directory) == original);
    BOOST_CHECK_EQUAL(signer.LastSignedHeight(), 1);

    // The observation changes only diagnostics. MaybeSign performs the actual
    // journal rebase using the supplied included-certificate state, then signs
    // only the next scheduled checkpoint above that certified anchor.
    const auto signed_new{Sign(signer, 33)};
    BOOST_REQUIRE_EQUAL(signed_new.size(), 1U);
    BOOST_CHECK_EQUAL(signed_new.front().height, 21U);
    const auto after{observe()};
    BOOST_CHECK_EQUAL(after.state, "lock_matches_chain");
    BOOST_CHECK(!after.blocked_on_orphan_vote);
    BOOST_REQUIRE(after.lock_height);
    BOOST_CHECK_EQUAL(*after.lock_height, 21);
    BOOST_CHECK(after.lock_hash == hashes[21]);
    BOOST_CHECK(!after.certificate_strictly_newer);
    BOOST_CHECK(signer.LastError().empty());
}

BOOST_AUTO_TEST_SUITE_END()

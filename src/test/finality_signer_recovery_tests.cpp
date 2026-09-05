// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// The chain-pinned one-time recovery of a finality signer journal whose
// ancestry lock points at an orphaned checkpoint (Consensus::
// FinalitySignerRecovery). The mainnet incident: validators signed checkpoint
// 811631 on a branch that was later discarded, no newer quorum certificate can
// form without their weight, and the protocol's only unlock proof therefore
// cannot exist. The recovery moves ONLY the ancestry lock to an agreed anchor
// on the current chain, keeps the recorded vote, never deletes or recreates a
// journal, and fails closed on every mismatch.

#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/era.h>
#include <consensus/finality_signer_recovery.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/finality_schedule.h>
#include <modern/finality_types.h>
#include <node/finality_signature.h>
#include <node/finality_signer_store.h>
#include <node/finality_tracker.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <string>

using b3test::FinalityChainFixture;
using node::FinalitySignaturePool;

namespace {

//! Scaled reorg horizon for these tests: the manual fork below is four blocks
//! deep, and the anchor must be buried this deep before the pin applies.
constexpr int SCALED_HORIZON{5};

//! The incident as the fixture builds it: A's vote on M+10 (old branch) that
//! the replacement branch discards, plus the agreed anchor M+15.
struct Incident {
    uint256 hash;
    uint256 digest;
    uint256 set0_hash;
    uint256 set1_hash;
};

struct RecoveryFixture : public FinalityChainFixture {
    fs::path m_store_dir;
    std::string m_error;

    //! Arm A durably, finalize M (carrier M+4, below the future fork point),
    //! let A vote on M+5 and M+10, then replace M+10.. with a longer branch
    //! from M+9 up to M + `branch_tip_offset`. Returns the incident facts.
    Incident BuildIncident(const int branch_tip_offset)
    {
        PrepareFinalityChain(/*min_finality_set=*/1,
                             /*reorg_horizon=*/SCALED_HORIZON);
        const int M{m_M};
        const int branch_tip{M + branch_tip_offset};
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        m_store_dir = m_path_root / "recovery-signer";

        node::FinalitySigner original;
        BOOST_REQUIRE_MESSAGE(original.SetKeyPersistent(
                                  m_bls_a, m_vk_a, m_domain, m_store_dir,
                                  m_error),
                              m_error);
        {
            LOCK(cs_main);
            FinalitySignaturePool pool;
            BOOST_CHECK(original.MaybeSign(Finality(),
                                           m_node.chainman->ActiveChain(),
                                           params, pool)
                            .empty());
            BOOST_CHECK(original.LastError().empty());
        }

        // Finalize M below the future fork point, exactly as mainnet's
        // finalized checkpoint lies below the incident.
        ProduceTo(M + 3, m_vk_a);
        const auto set0{*FinalityState().current};
        Incident incident;
        incident.set0_hash = set0.SetHash();
        incident.set1_hash = FinalityState().next->SetHash();
        Produce(m_vk_a, {MakeCertificate({M, 0, incident.set1_hash}, set0)});
        BOOST_REQUIRE(FinalityState().finalized.has_value());
        BOOST_CHECK_EQUAL(FinalityState().finalized->height, M);

        // A votes on M+5 and M+10; M+10 is the incident.
        ProduceTo(M + 13, m_vk_a);
        {
            LOCK(cs_main);
            FinalitySignaturePool pool;
            const auto sigs{original.MaybeSign(
                Finality(), m_node.chainman->ActiveChain(), params, pool)};
            BOOST_REQUIRE_EQUAL(sigs.size(), 2U);
            BOOST_CHECK_EQUAL(sigs.back().height,
                              static_cast<uint64_t>(M + 10));
            BOOST_CHECK_EQUAL(original.LastSignedHeight(), M + 10);
        }
        incident.hash = ChainHashAt(M + 10);
        {
            node::FinalitySignerStore probe;
            std::string e;
            BOOST_REQUIRE(probe.Open(m_store_dir, m_domain, m_vk_a, e));
            BOOST_REQUIRE(probe.State().has_value());
            incident.digest = probe.State()->lock_digest;
        }

        // While the incident block IS the active block at its height the
        // signer's lock is intact: the ordinary ancestry check passes, the
        // recovery is never consulted, and a pin (whatever it says) changes
        // nothing. A node still on the old fork therefore keeps its journal
        // exactly as it was.
        {
            MutableConsensus().finality_signer_recovery =
                MakePin(incident, m_rng.rand256());
            LOCK(cs_main);
            FinalitySignaturePool pool;
            BOOST_CHECK(original.MaybeSign(
                            Finality(), m_node.chainman->ActiveChain(),
                            params, pool)
                            .empty());
            BOOST_CHECK(original.LastError().empty());
            node::FinalitySignerStore probe;
            std::string e;
            BOOST_REQUIRE(probe.Open(m_store_dir, m_domain, m_vk_a, e));
            BOOST_CHECK_EQUAL(probe.State()->lock_height, M + 10);
            BOOST_CHECK(probe.State()->lock_block_hash == incident.hash);
            MutableConsensus().finality_signer_recovery.reset();
        }

        // The incident: M+10 is replaced by a longer branch from M+9 (four
        // blocks deep at tip M+13, within the scaled horizon). The pinned
        // checkpoint M stays final (its carrier M+4 is below the fork point),
        // so no newer certificate can unlock the orphaned vote.
        {
            const CBlockIndex* parent{IndexAt(M + 9)};
            uint256 seed{SeedFor(parent)};
            for (int height{M + 10}; height <= branch_tip; ++height) {
                auto [block, digest]{BuildPosBlockOnSeed(
                    parent, seed, m_vk_a, {}, {},
                    /*extra=*/30'000 + height)};
                if (block.GetBlockTime() > GetTime()) {
                    SetMockTime(block.GetBlockTime());
                }
                BOOST_REQUIRE(Submit(block));
                parent = WITH_LOCK(
                    cs_main,
                    return m_node.chainman->m_blockman.LookupBlockIndex(
                        block.GetHash()));
                BOOST_REQUIRE(parent != nullptr);
                seed = digest;
            }
        }
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, branch_tip);
        BOOST_REQUIRE(ChainHashAt(M + 10) != incident.hash);
        BOOST_REQUIRE_EQUAL(FinalityState().finalized->height, M);
        return incident;
    }

    //! The pin for the fixture's incident; the anchor is the active block at
    //! M+15 unless `anchor` is given (the chain may not reach M+15 yet).
    Consensus::FinalitySignerRecovery MakePin(
        const Incident& incident, std::optional<uint256> anchor = std::nullopt)
    {
        Consensus::FinalitySignerRecovery pin;
        pin.chain_domain = m_domain;
        pin.incident_height = m_M + 10;
        pin.incident_block_hash = incident.hash;
        pin.incident_epoch = 0;
        pin.incident_signing_set_hash = incident.set0_hash;
        pin.incident_successor_set_hash = incident.set1_hash;
        pin.anchor_height = m_M + 15;
        pin.anchor_block_hash = anchor ? *anchor : ChainHashAt(m_M + 15);
        return pin;
    }

    //! MaybeSign for a signer armed on `dir`; returns the produced heights.
    std::vector<int> Sign(node::FinalitySigner& signer,
                          FinalitySignaturePool& pool)
    {
        LOCK(cs_main);
        std::vector<int> heights;
        for (const auto& sig : signer.MaybeSign(
                 Finality(), m_node.chainman->ActiveChain(),
                 m_node.chainman->GetConsensus(), pool)) {
            heights.push_back(static_cast<int>(sig.height));
        }
        return heights;
    }

    node::FinalitySignerState Journal(const fs::path& dir)
    {
        node::FinalitySignerStore probe;
        std::string e;
        BOOST_REQUIRE_MESSAGE(probe.Open(dir, m_domain, m_vk_a, e), e);
        BOOST_REQUIRE(probe.State().has_value());
        return *probe.State();
    }

    //! A byte-for-byte copy of A's journal in `dir`: a second node holding
    //! the same validator identity in the same durable state (a validator
    //! that upgrades later than the others). Test scaffolding only.
    void CopyJournal(const fs::path& dir)
    {
        fs::create_directories(dir);
        fs::copy_file(
            node::FinalitySignerStore::StatePath(m_store_dir, m_domain, m_vk_a),
            node::FinalitySignerStore::StatePath(dir, m_domain, m_vk_a),
            fs::copy_options::none);
    }

    //! The deadlock: MaybeSign signs nothing, names the protocol refusal
    //! (`refusal`), and the journal still locks the incident.
    void ExpectDeadlocked(node::FinalitySigner& signer, const fs::path& dir,
                          const std::string& why,
                          const std::string& expected_note = "",
                          const std::string& refusal =
                              "does not descend from signed checkpoint")
    {
        FinalitySignaturePool pool;
        BOOST_CHECK_MESSAGE(Sign(signer, pool).empty(), why);
        BOOST_CHECK_MESSAGE(signer.LastError().find(refusal) !=
                                std::string::npos,
                            why << ": " << signer.LastError());
        if (!expected_note.empty()) {
            BOOST_CHECK_MESSAGE(signer.LastError().find(expected_note) !=
                                    std::string::npos,
                                why << ": " << signer.LastError());
        }
        const auto journal{Journal(dir)};
        BOOST_CHECK_EQUAL(journal.lock_height, m_M + 10);
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(finality_signer_recovery_tests)

BOOST_FIXTURE_TEST_CASE(store_recovery_moves_only_the_lock_and_fails_closed_otherwise,
                        BasicTestingSetup)
{
    const fs::path dir{m_path_root / "recovery-store"};
    const uint256 domain{m_rng.rand256()};
    modern::ValidatorKeyBytes validator{};
    validator.fill(0x51);
    const uint256 incident_block{m_rng.rand256()};
    const uint256 incident_digest{m_rng.rand256()};
    const uint256 set0{m_rng.rand256()};
    const uint256 set1{m_rng.rand256()};
    const uint256 anchor_block{m_rng.rand256()};
    const uint256 anchor_digest{m_rng.rand256()};
    std::string error;

    node::FinalitySignerStore store;
    BOOST_REQUIRE_MESSAGE(store.Open(dir, domain, validator, error), error);
    BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(
        store.CommitSignedCheckpoint(10, incident_block, incident_digest, 0,
                                     set0, set1, error),
        error);

    Consensus::FinalitySignerRecovery pin;
    pin.chain_domain = domain;
    pin.incident_height = 10;
    pin.incident_block_hash = incident_block;
    pin.incident_epoch = 0;
    pin.incident_signing_set_hash = set0;
    pin.incident_successor_set_hash = set1;
    pin.anchor_height = 20;
    pin.anchor_block_hash = anchor_block;
    BOOST_REQUIRE(pin.Valid());

    // Every single deviating fact fails closed and leaves the journal as it
    // was: another chain, another incident coordinate, another epoch or set,
    // a null digest, or a structurally invalid pin (anchor not above the
    // incident, rejected by Valid()).
    const auto refused{[&](const auto& mutate) {
        Consensus::FinalitySignerRecovery bad{pin};
        mutate(bad);
        std::string e;
        BOOST_CHECK(!store.CommitPinnedRecoveryAnchor(bad, anchor_digest, e));
        BOOST_CHECK(!e.empty());
        BOOST_REQUIRE(store.State().has_value());
        BOOST_CHECK_EQUAL(store.State()->lock_height, 10);
        BOOST_CHECK_EQUAL(store.State()->last_signed_height, 10);
    }};
    refused([&](Consensus::FinalitySignerRecovery& p) { p.chain_domain = m_rng.rand256(); });
    refused([&](Consensus::FinalitySignerRecovery& p) { p.incident_height = 5; });
    refused([&](Consensus::FinalitySignerRecovery& p) { p.incident_block_hash = m_rng.rand256(); });
    refused([&](Consensus::FinalitySignerRecovery& p) { p.incident_epoch = 1; });
    refused([&](Consensus::FinalitySignerRecovery& p) { p.incident_signing_set_hash = m_rng.rand256(); });
    refused([&](Consensus::FinalitySignerRecovery& p) { p.incident_successor_set_hash = m_rng.rand256(); });
    refused([&](Consensus::FinalitySignerRecovery& p) { p.anchor_height = 10; });
    {
        std::string e;
        BOOST_CHECK(!store.CommitPinnedRecoveryAnchor(pin, uint256{}, e));
        BOOST_CHECK(!e.empty());
    }

    // A journal with a newer signing record is not the incident.
    {
        node::FinalitySignerStore newer;
        std::string e;
        BOOST_REQUIRE(newer.Open(m_path_root / "recovery-store-newer", domain,
                                 validator, e));
        BOOST_REQUIRE(newer.InitializeEmpty(e));
        BOOST_REQUIRE(newer.CommitSignedCheckpoint(
            10, incident_block, incident_digest, 0, set0, set1, e));
        BOOST_REQUIRE(newer.CommitSignedCheckpoint(
            15, m_rng.rand256(), m_rng.rand256(), 0, set0, set1, e));
        BOOST_CHECK(!newer.CommitPinnedRecoveryAnchor(pin, anchor_digest, e));
        BOOST_CHECK_EQUAL(newer.State()->lock_height, 15);
    }
    // A journal whose lock already moved past the vote is not the incident.
    {
        node::FinalitySignerStore moved;
        std::string e;
        BOOST_REQUIRE(moved.Open(m_path_root / "recovery-store-moved", domain,
                                 validator, e));
        BOOST_REQUIRE(moved.InitializeEmpty(e));
        BOOST_REQUIRE(moved.CommitSignedCheckpoint(
            10, incident_block, incident_digest, 0, set0, set1, e));
        BOOST_REQUIRE(moved.CommitCertifiedAnchor(
            15, m_rng.rand256(), m_rng.rand256(), 0, set0, set1, e));
        BOOST_CHECK(!moved.CommitPinnedRecoveryAnchor(pin, anchor_digest, e));
        BOOST_CHECK_EQUAL(moved.State()->lock_height, 15);
        BOOST_CHECK_EQUAL(moved.State()->last_signed_height, 10);
    }

    // The exact incident: only the lock moves; the recorded vote is kept.
    BOOST_REQUIRE_MESSAGE(
        store.CommitPinnedRecoveryAnchor(pin, anchor_digest, error), error);
    BOOST_REQUIRE(store.State().has_value());
    BOOST_CHECK_EQUAL(store.State()->last_signed_height, 10);
    BOOST_CHECK(store.State()->last_signed_block_hash == incident_block);
    BOOST_CHECK(store.State()->last_signed_digest == incident_digest);
    BOOST_CHECK_EQUAL(store.State()->lock_height, 20);
    BOOST_CHECK(store.State()->lock_block_hash == anchor_block);
    BOOST_CHECK(store.State()->lock_digest == anchor_digest);
    BOOST_CHECK_EQUAL(store.State()->lock_epoch, 0U);
    BOOST_CHECK(store.State()->lock_signing_set_hash == set0);
    BOOST_CHECK(store.State()->lock_successor_set_hash == set1);

    // One time only: the journal no longer matches the incident.
    BOOST_CHECK(!store.CommitPinnedRecoveryAnchor(pin, anchor_digest, error));

    // Restart reloads the moved lock and the retained vote.
    {
        node::FinalitySignerStore restarted;
        std::string e;
        BOOST_REQUIRE_MESSAGE(restarted.Open(dir, domain, validator, e), e);
        BOOST_REQUIRE(restarted.State().has_value());
        BOOST_CHECK_EQUAL(restarted.State()->last_signed_height, 10);
        BOOST_CHECK(restarted.State()->last_signed_block_hash == incident_block);
        BOOST_CHECK_EQUAL(restarted.State()->lock_height, 20);
        BOOST_CHECK(restarted.State()->lock_block_hash == anchor_block);
    }

    // The next vote must be strictly above both the old vote and the anchor;
    // the anchor itself and anything between are refused.
    BOOST_CHECK(!store.CommitSignedCheckpoint(15, m_rng.rand256(),
                                              m_rng.rand256(), 0, set0, set1,
                                              error));
    BOOST_CHECK(!store.CommitSignedCheckpoint(20, anchor_block, anchor_digest,
                                              0, set0, set1, error));
    BOOST_REQUIRE_MESSAGE(
        store.CommitSignedCheckpoint(30, m_rng.rand256(), m_rng.rand256(), 0,
                                     set0, set1, error),
        error);
    BOOST_CHECK_EQUAL(store.State()->last_signed_height, 30);
    BOOST_CHECK_EQUAL(store.State()->lock_height, 30);
}

BOOST_FIXTURE_TEST_CASE(pinned_recovery_unlocks_the_exact_incident_and_resumes_certificates,
                        RecoveryFixture)
{
    const Incident incident{BuildIncident(/*branch_tip_offset=*/17)};
    const int M{m_M};
    Consensus::Params& params{MutableConsensus()};
    const uint256 anchor_hash{ChainHashAt(M + 15)};
    // A validator that will upgrade only after the finality lineage breaks.
    const fs::path late_dir{m_path_root / "recovery-late-broken"};
    CopyJournal(late_dir);
    const Consensus::FinalitySignerRecovery pin{MakePin(incident)};
    BOOST_REQUIRE(pin.Valid());
    BOOST_CHECK(pin.anchor_block_hash == anchor_hash);

    // Restart into the deadlock: the journal reloads the orphaned vote and
    // the signer refuses this branch (the protocol behaviour).
    node::FinalitySigner restarted;
    BOOST_REQUIRE_MESSAGE(restarted.SetKeyPersistent(
                              m_bls_a, m_vk_a, m_domain, m_store_dir, m_error),
                          m_error);
    BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 10);
    ExpectDeadlocked(restarted, m_store_dir, "no pin");

    // The exact pin, but the anchor (M+15) is only two blocks deep at tip
    // M+17: not yet buried beyond the scaled reorg horizon. The refusal is
    // named as pending.
    const auto rejects{[&](const std::string& why, const auto& mutate,
                           const std::string& note) {
        Consensus::FinalitySignerRecovery bad{pin};
        mutate(bad);
        params.finality_signer_recovery = bad;
        ExpectDeadlocked(restarted, m_store_dir, why, note);
        params.finality_signer_recovery.reset();
    }};
    rejects("anchor not yet buried beyond the reorg horizon",
            [](Consensus::FinalitySignerRecovery&) {},
            "pinned recovery pending: the anchor is not yet buried");
    ProduceTo(M + 19, m_vk_a);
    rejects("anchor one block short of the reorg horizon",
            [](Consensus::FinalitySignerRecovery&) {},
            "pinned recovery pending: the anchor is not yet buried");
    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, M + 20);

    // Wrong chain / wrong facts at a settled anchor: every deviation leaves
    // the deadlock intact. Pin-vs-journal mismatches are silent (the journal
    // is simply not the incident); pin-vs-chain mismatches are named.
    rejects("wrong anchor hash", [&](Consensus::FinalitySignerRecovery& p) {
        p.anchor_block_hash = ChainHashAt(M + 14);
    }, "does not carry the pinned anchor block");
    rejects("wrong chain domain", [&](Consensus::FinalitySignerRecovery& p) {
        p.chain_domain = m_rng.rand256();
    }, "");
    rejects("wrong incident hash", [&](Consensus::FinalitySignerRecovery& p) {
        p.incident_block_hash = m_rng.rand256();
    }, "");
    rejects("wrong epoch", [&](Consensus::FinalitySignerRecovery& p) {
        p.incident_epoch = 1;
    }, "");
    rejects("wrong signing set", [&](Consensus::FinalitySignerRecovery& p) {
        p.incident_signing_set_hash = m_rng.rand256();
    }, "");
    rejects("wrong successor set", [&](Consensus::FinalitySignerRecovery& p) {
        p.incident_successor_set_hash = m_rng.rand256();
    }, "");
    rejects("anchor is not a scheduled checkpoint",
            [&](Consensus::FinalitySignerRecovery& p) {
                p.anchor_height = M + 14;
                p.anchor_block_hash = ChainHashAt(M + 14);
            },
            "not a scheduled checkpoint");

    // Journals that agree with a pin but not with this chain, or that carry
    // a newer record than the incident, fail closed at the signer level too.
    const auto journal_rejects{
        [&](const std::string& why, const std::string& subdir,
            const auto& write, const auto& mutate_pin,
            const std::string& note) {
            const fs::path dir{m_path_root / fs::PathFromString(subdir)};
            {
                node::FinalitySignerStore j;
                std::string e;
                BOOST_REQUIRE_MESSAGE(j.Open(dir, m_domain, m_vk_a, e), e);
                BOOST_REQUIRE(j.InitializeEmpty(e));
                write(j, e);
            }
            Consensus::FinalitySignerRecovery variant{pin};
            mutate_pin(variant);
            params.finality_signer_recovery = variant;
            node::FinalitySigner signer;
            std::string e;
            BOOST_REQUIRE_MESSAGE(signer.SetKeyPersistent(
                                      m_bls_a, m_vk_a, m_domain, dir, e),
                                  e);
            FinalitySignaturePool pool;
            BOOST_CHECK_MESSAGE(Sign(signer, pool).empty(), why);
            BOOST_CHECK_MESSAGE(signer.LastError().find(
                                    "does not descend from signed checkpoint") !=
                                    std::string::npos,
                                why << ": " << signer.LastError());
            if (!note.empty()) {
                BOOST_CHECK_MESSAGE(signer.LastError().find(note) !=
                                        std::string::npos,
                                    why << ": " << signer.LastError());
            }
            params.finality_signer_recovery.reset();
            return Journal(dir);
        }};
    {
        // Journal and pin agree on random validator sets that this chain
        // never had: the chain-state lineage check refuses.
        const uint256 foreign0{m_rng.rand256()};
        const uint256 foreign1{m_rng.rand256()};
        const auto j{journal_rejects(
            "journal and pin agree on sets this chain never had",
            "recovery-foreign-sets",
            [&](node::FinalitySignerStore& s, std::string& e) {
                BOOST_REQUIRE_MESSAGE(s.CommitSignedCheckpoint(
                                          M + 10, incident.hash,
                                          incident.digest, 0, foreign0,
                                          foreign1, e),
                                      e);
            },
            [&](Consensus::FinalitySignerRecovery& p) {
                p.incident_signing_set_hash = foreign0;
                p.incident_successor_set_hash = foreign1;
            },
            "epoch state does not match the incident lineage")};
        BOOST_CHECK_EQUAL(j.lock_height, M + 10);
        BOOST_CHECK(j.lock_signing_set_hash == foreign0);
    }
    {
        // Journal and pin agree on an epoch this chain is not in.
        const auto j{journal_rejects(
            "journal and pin agree on a foreign epoch",
            "recovery-foreign-epoch",
            [&](node::FinalitySignerStore& s, std::string& e) {
                BOOST_REQUIRE_MESSAGE(s.CommitSignedCheckpoint(
                                          M + 10, incident.hash,
                                          incident.digest, 1,
                                          incident.set0_hash,
                                          incident.set1_hash, e),
                                      e);
            },
            [&](Consensus::FinalitySignerRecovery& p) { p.incident_epoch = 1; },
            "epoch state does not match the incident lineage")};
        BOOST_CHECK_EQUAL(j.lock_epoch, 1U);
        BOOST_CHECK_EQUAL(j.lock_height, M + 10);
        BOOST_CHECK(j.lock_block_hash == incident.hash);
    }
    {
        // A newer vote after the incident: the journal is not the incident.
        // The newer record sits at the pin's anchor height on purpose: only
        // its (foreign) hash proves the lock was left alone.
        const uint256 newer_hash{m_rng.rand256()};
        const auto j{journal_rejects(
            "journal with a newer vote", "recovery-newer-vote",
            [&](node::FinalitySignerStore& s, std::string& e) {
                BOOST_REQUIRE(s.CommitSignedCheckpoint(
                    M + 10, incident.hash, incident.digest, 0,
                    incident.set0_hash, incident.set1_hash, e));
                BOOST_REQUIRE(s.CommitSignedCheckpoint(
                    M + 15, newer_hash, m_rng.rand256(), 0,
                    incident.set0_hash, incident.set1_hash, e));
            },
            [](Consensus::FinalitySignerRecovery&) {}, "")};
        BOOST_CHECK_EQUAL(j.lock_height, M + 15);
        BOOST_CHECK(j.lock_block_hash == newer_hash);
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 15);
        BOOST_CHECK(j.last_signed_block_hash == newer_hash);
    }
    {
        // A lock already moved past the vote: the journal is not the incident.
        const uint256 moved_hash{m_rng.rand256()};
        const auto j{journal_rejects(
            "journal with a moved lock", "recovery-moved-lock",
            [&](node::FinalitySignerStore& s, std::string& e) {
                BOOST_REQUIRE(s.CommitSignedCheckpoint(
                    M + 10, incident.hash, incident.digest, 0,
                    incident.set0_hash, incident.set1_hash, e));
                BOOST_REQUIRE(s.CommitCertifiedAnchor(
                    M + 15, moved_hash, m_rng.rand256(), 0,
                    incident.set0_hash, incident.set1_hash, e));
            },
            [](Consensus::FinalitySignerRecovery&) {}, "")};
        BOOST_CHECK_EQUAL(j.lock_height, M + 15);
        BOOST_CHECK(j.lock_block_hash == moved_hash);
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 10);
        BOOST_CHECK(j.last_signed_block_hash == incident.hash);
    }

    // The exact pin at a settled anchor: the lock moves, the recorded vote is
    // retained, and nothing at or below the anchor is signed. M+15 is
    // signable by depth but is the anchor itself; M+20 is not yet signable.
    params.finality_signer_recovery = pin;
    {
        FinalitySignaturePool pool;
        BOOST_CHECK(Sign(restarted, pool).empty());
        BOOST_CHECK_MESSAGE(restarted.LastError().empty(),
                            restarted.LastError());
        BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 10);
        BOOST_CHECK_EQUAL(pool.TrackedCheckpoints(), 0U);
    }
    const auto expected_anchor_digest{[&] {
        LOCK(cs_main);
        const auto fb{FinalitySignaturePool::ExpectedFinalizedBlock(
            0, static_cast<uint64_t>(M + 15), Finality().Current(),
            m_node.chainman->ActiveChain(), params)};
        BOOST_REQUIRE(fb.has_value());
        return modern::FinalityDigest(m_domain, *fb);
    }()};
    {
        const auto j{Journal(m_store_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 10);
        BOOST_CHECK(j.last_signed_block_hash == incident.hash);
        BOOST_CHECK(j.last_signed_digest == incident.digest);
        BOOST_CHECK_EQUAL(j.lock_height, M + 15);
        BOOST_CHECK(j.lock_block_hash == anchor_hash);
        BOOST_CHECK(j.lock_digest == expected_anchor_digest);
        BOOST_CHECK_EQUAL(j.lock_epoch, 0U);
        BOOST_CHECK(j.lock_signing_set_hash == incident.set0_hash);
        BOOST_CHECK(j.lock_successor_set_hash == incident.set1_hash);
    }

    // Restart persistence: with the pin removed, a fresh process stands on
    // the reloaded journal alone, signs nothing at or below the anchor, and
    // leaves the recovered lock exactly as persisted.
    params.finality_signer_recovery.reset();
    node::FinalitySigner reloaded;
    BOOST_REQUIRE_MESSAGE(reloaded.SetKeyPersistent(
                              m_bls_a, m_vk_a, m_domain, m_store_dir, m_error),
                          m_error);
    BOOST_CHECK_EQUAL(reloaded.LastSignedHeight(), M + 10);
    {
        FinalitySignaturePool pool;
        BOOST_CHECK(Sign(reloaded, pool).empty());
        BOOST_CHECK_MESSAGE(reloaded.LastError().empty(), reloaded.LastError());
        const auto j{Journal(m_store_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 10);
        BOOST_CHECK_EQUAL(j.lock_height, M + 15);
        BOOST_CHECK(j.lock_digest == expected_anchor_digest);
    }

    // Resumed certificate production: the first new vote is the first
    // scheduled checkpoint strictly above the anchor (M+20), B signs it too,
    // any node aggregates, and consensus accepts the certificate.
    ProduceTo(M + 23, m_vk_a);
    std::optional<std::pair<modern::FinalizedBlock,
                            modern::FinalityCertificate>> best;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        FinalitySignaturePool pool;
        const auto signed_new{reloaded.MaybeSign(tracker, chain, params, pool)};
        BOOST_REQUIRE_EQUAL(signed_new.size(), 1U);
        BOOST_CHECK_EQUAL(signed_new.front().height,
                          static_cast<uint64_t>(M + 20));
        BOOST_CHECK(reloaded.LastError().empty());
        BOOST_CHECK_EQUAL(reloaded.LastSignedHeight(), M + 20);
        node::FinalitySigner signer_b;
        signer_b.SetKey(m_bls_b, m_vk_b);
        BOOST_CHECK(!signer_b.MaybeSign(tracker, chain, params, pool).empty());
        best = pool.BestCertificate(tracker, chain, params);
    }
    BOOST_REQUIRE(best.has_value());
    BOOST_CHECK_EQUAL(best->first.height, static_cast<uint64_t>(M + 20));
    BOOST_CHECK(best->first.block_hash == ChainHashAt(M + 20));
    const auto [payload, cell]{
        modern::BuildFinalityCertificate(best->first, best->second)};
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    rec.payload_version = modern::MPA_VERSION_V1;
    rec.payload = payload;
    Produce(m_vk_a, {{cell, rec}});
    BOOST_REQUIRE(FinalityState().finalized.has_value());
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 20);

    // The journal advanced normally with the new vote.
    {
        const auto j{Journal(m_store_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 20);
        BOOST_CHECK_EQUAL(j.lock_height, M + 20);
        BOOST_CHECK(j.lock_block_hash == ChainHashAt(M + 20));
    }

    // The pin cannot outlive the finality lineage. The M+20 certificate
    // certified the epoch-0 handover, so epoch 1 starts at M+30; with no
    // epoch-1 certificate the lineage breaks at M+30 + E + MAX_EXTENSION.
    // A validator that upgrades only then is refused: the incident sets no
    // longer resolve, the journal stays locked, and nothing is recreated.
    ProduceTo(M + 2 * SCALED_E + SCALED_MAX_EXTENSION, m_vk_a);
    BOOST_REQUIRE(FinalityState().lineage_broken);
    params.finality_signer_recovery = pin;
    {
        node::FinalitySigner late;
        BOOST_REQUIRE_MESSAGE(late.SetKeyPersistent(
                                  m_bls_a, m_vk_a, m_domain, late_dir, m_error),
                              m_error);
        BOOST_CHECK_EQUAL(late.LastSignedHeight(), M + 10);
        ExpectDeadlocked(late, late_dir, "lineage broken",
                         "epoch state does not match the incident lineage",
                         "cannot reconstruct the signing set");
        const auto j{Journal(late_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 10);
        BOOST_CHECK(j.lock_block_hash == incident.hash);
    }
    params.finality_signer_recovery.reset();
}

BOOST_FIXTURE_TEST_CASE(pinned_recovery_applies_after_the_epoch_rotated_and_resumes_in_the_next_epoch,
                        RecoveryFixture)
{
    // Mainnet's situation once the deadlock outlives the epoch boundary: the
    // certified handover rotates the tracker on schedule, the incident epoch
    // becomes "previous", and the pin must still apply.
    const Incident incident{BuildIncident(/*branch_tip_offset=*/17)};
    const int M{m_M};
    Consensus::Params& params{MutableConsensus()};
    // Two validators that upgrade later than the others: one after the
    // first post-incident certificate, one after the next epoch rotation.
    const fs::path late_dir{m_path_root / "recovery-late"};
    const fs::path too_late_dir{m_path_root / "recovery-too-late"};
    CopyJournal(late_dir);
    CopyJournal(too_late_dir);
    ProduceTo(M + SCALED_E + 3, m_vk_a); // past the rotation at M+30
    {
        const auto state{FinalityState()};
        BOOST_REQUIRE_EQUAL(state.epoch, 1U);
        BOOST_REQUIRE(state.previous && state.current && state.next);
        BOOST_CHECK(state.previous->SetHash() == incident.set0_hash);
        BOOST_CHECK(state.current->SetHash() == incident.set1_hash);
        BOOST_REQUIRE_EQUAL(state.finalized->height, M);
    }
    const Consensus::FinalitySignerRecovery pin{MakePin(incident)};

    node::FinalitySigner restarted;
    BOOST_REQUIRE_MESSAGE(restarted.SetKeyPersistent(
                              m_bls_a, m_vk_a, m_domain, m_store_dir, m_error),
                          m_error);
    ExpectDeadlocked(restarted, m_store_dir, "no pin, rotated");

    // The pin applies through the previous-epoch window, and the signer then
    // votes on every signable checkpoint strictly above the anchor: the
    // remaining epoch-0 checkpoints and the first epoch-1 checkpoint.
    params.finality_signer_recovery = pin;
    std::optional<std::pair<modern::FinalizedBlock,
                            modern::FinalityCertificate>> best;
    {
        LOCK(cs_main);
        const CChain& chain{m_node.chainman->ActiveChain()};
        node::FinalityTracker& tracker{Finality()};
        FinalitySignaturePool pool;
        const auto sigs{restarted.MaybeSign(tracker, chain, params, pool)};
        BOOST_CHECK_MESSAGE(restarted.LastError().empty(),
                            restarted.LastError());
        BOOST_REQUIRE_EQUAL(sigs.size(), 3U);
        BOOST_CHECK_EQUAL(sigs[0].height, static_cast<uint64_t>(M + 20));
        BOOST_CHECK_EQUAL(sigs[0].epoch, 0U);
        BOOST_CHECK_EQUAL(sigs[1].height, static_cast<uint64_t>(M + 25));
        BOOST_CHECK_EQUAL(sigs[1].epoch, 0U);
        BOOST_CHECK_EQUAL(sigs[2].height, static_cast<uint64_t>(M + 30));
        BOOST_CHECK_EQUAL(sigs[2].epoch, 1U);
        BOOST_CHECK_EQUAL(restarted.LastSignedHeight(), M + 30);
        node::FinalitySigner signer_b;
        signer_b.SetKey(m_bls_b, m_vk_b);
        BOOST_CHECK(!signer_b.MaybeSign(tracker, chain, params, pool).empty());
        best = pool.BestCertificate(tracker, chain, params);
    }
    {
        const auto j{Journal(m_store_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 30);
        BOOST_CHECK_EQUAL(j.lock_height, M + 30);
        BOOST_CHECK_EQUAL(j.lock_epoch, 1U);
        BOOST_CHECK(j.lock_signing_set_hash == incident.set1_hash);
    }
    BOOST_REQUIRE(best.has_value());
    BOOST_CHECK_EQUAL(best->first.height, static_cast<uint64_t>(M + 30));
    BOOST_CHECK_EQUAL(best->first.epoch, 1U);
    const auto [payload, cell]{
        modern::BuildFinalityCertificate(best->first, best->second)};
    CMpaRecord rec;
    rec.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
    rec.payload_version = modern::MPA_VERSION_V1;
    rec.payload = payload;
    Produce(m_vk_a, {{cell, rec}});
    BOOST_REQUIRE(FinalityState().finalized.has_value());
    BOOST_CHECK_EQUAL(FinalityState().finalized->height, M + 30);
    BOOST_CHECK_EQUAL(FinalityState().finalized->epoch, 1U);
    BOOST_CHECK(FinalityState().handover_certified);

    // A validator that upgrades only now, after the first post-incident
    // certificate: that epoch-1 certificate is not a protocol unlock proof
    // for an epoch-0 lock, so without the pin it would stay shut out.
    // The pin still applies; the next vote is the first checkpoint strictly
    // above the anchor AND the finalized checkpoint (M+35), in epoch 1.
    {
        node::FinalitySigner late;
        BOOST_REQUIRE_MESSAGE(late.SetKeyPersistent(
                                  m_bls_a, m_vk_a, m_domain, late_dir, m_error),
                              m_error);
        BOOST_CHECK_EQUAL(late.LastSignedHeight(), M + 10);
        params.finality_signer_recovery.reset();
        ExpectDeadlocked(late, late_dir, "late, no pin", "",
                         "included certificate does not use the exact epoch");
        params.finality_signer_recovery = pin;
        FinalitySignaturePool pool;
        BOOST_CHECK(Sign(late, pool).empty());
        BOOST_CHECK_MESSAGE(late.LastError().empty(), late.LastError());
        {
            const auto j{Journal(late_dir)};
            BOOST_CHECK_EQUAL(j.last_signed_height, M + 10);
            BOOST_CHECK(j.last_signed_block_hash == incident.hash);
            BOOST_CHECK_EQUAL(j.lock_height, M + 15);
            BOOST_CHECK(j.lock_block_hash == ChainHashAt(M + 15));
            BOOST_CHECK_EQUAL(j.lock_epoch, 0U);
        }
        ProduceTo(M + 38, m_vk_a);
        const auto heights{Sign(late, pool)};
        BOOST_CHECK_MESSAGE(late.LastError().empty(), late.LastError());
        BOOST_REQUIRE_EQUAL(heights.size(), 1U);
        BOOST_CHECK_EQUAL(heights.front(), M + 35);
        const auto j{Journal(late_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 35);
        BOOST_CHECK_EQUAL(j.lock_height, M + 35);
        BOOST_CHECK_EQUAL(j.lock_epoch, 1U);
        BOOST_CHECK(j.lock_signing_set_hash == incident.set1_hash);
    }

    // The window closes with the next rotation: the M+30 certificate
    // certified the epoch-1 handover, so epoch 2 starts at M+60 and Set_0
    // leaves the {current, current-1} window. A validator that upgrades only
    // then is refused and keeps its journal exactly as it was.
    ProduceTo(M + 2 * SCALED_E + 3, m_vk_a);
    BOOST_REQUIRE_EQUAL(FinalityState().epoch, 2U);
    {
        node::FinalitySigner too_late;
        BOOST_REQUIRE_MESSAGE(too_late.SetKeyPersistent(m_bls_a, m_vk_a,
                                                        m_domain, too_late_dir,
                                                        m_error),
                              m_error);
        ExpectDeadlocked(too_late, too_late_dir, "after the next rotation",
                         "epoch state does not match the incident lineage",
                         "included certificate does not use the exact epoch");
        const auto j{Journal(too_late_dir)};
        BOOST_CHECK_EQUAL(j.last_signed_height, M + 10);
        BOOST_CHECK(j.lock_block_hash == incident.hash);
    }
    params.finality_signer_recovery.reset();
}

BOOST_AUTO_TEST_CASE(mainnet_pins_the_811631_incident_and_the_811641_anchor)
{
    const auto params{CreateChainParams(ArgsManager{}, ChainType::MAIN)};
    const Consensus::Params& consensus{params->GetConsensus()};
    const auto& pin{consensus.finality_signer_recovery};
    BOOST_REQUIRE(pin.has_value());
    BOOST_CHECK(pin->Valid());
    BOOST_REQUIRE(consensus.legacy_final_hash.has_value());
    const auto domain{modern::ModernChainDomain(consensus.hashGenesisBlock,
                                                *consensus.legacy_final_hash)};
    BOOST_REQUIRE(domain.has_value());
    BOOST_CHECK(pin->chain_domain == *domain);
    BOOST_CHECK_EQUAL(pin->incident_height, 811'631);
    BOOST_CHECK_EQUAL(
        pin->incident_block_hash.GetHex(),
        "86297c1075392fa614a6b0733eeb178de0eb8dc11602226b2b10344453426be0");
    BOOST_CHECK_EQUAL(pin->incident_epoch, 0U);
    BOOST_CHECK_EQUAL(
        pin->incident_signing_set_hash.GetHex(),
        "ff7c306f539eec01c793cd7fd389672c53a955d10f00758a2807ef0e9d22514e");
    BOOST_CHECK_EQUAL(
        pin->incident_successor_set_hash.GetHex(),
        "6dd7d4575e9f1d74036c7c86175e4fd2e6cf9dc621cddac5b91831b85361d63a");
    BOOST_CHECK_EQUAL(pin->anchor_height, 811'641);
    BOOST_CHECK_EQUAL(
        pin->anchor_block_hash.GetHex(),
        "5dbb0e582be41444933d43c9dda576f15a2922a870c3fb9d1c47b84b473b1f75");
    BOOST_CHECK(pin->anchor_block_hash != pin->incident_block_hash);

    // Both heights are scheduled epoch-0 checkpoints of the mainnet schedule
    // (M = 811,001, interval 10), the anchor is one interval above the
    // incident, so the first permitted new vote is 811,651; the anchor is
    // settled from tip 813,081 (reorg horizon 1,440). Epoch 1 started at
    // 812,441; the pin applies while epoch 0 is inside the
    // {current, current-1} window, i.e. until epoch 2 starts at the first
    // height >= 813,881 once an epoch-1 certificate is included, or until
    // the lineage breaks at 823,961 if none ever is.
    BOOST_REQUIRE(consensus.modern_pos.has_value());
    const Consensus::ModernPosParams& pos{*consensus.modern_pos};
    const auto modern_start{Consensus::ModernPosStartHeight(consensus)};
    BOOST_REQUIRE(modern_start.has_value());
    BOOST_CHECK_EQUAL(*modern_start, 811'001);
    BOOST_CHECK(modern::IsCheckpointHeight(pin->incident_height, *modern_start, pos.checkpoint_interval));
    BOOST_CHECK(modern::IsCheckpointHeight(pin->anchor_height, *modern_start, pos.checkpoint_interval));
    BOOST_CHECK_EQUAL(pin->anchor_height - pin->incident_height, pos.checkpoint_interval);
    BOOST_CHECK_LT(pin->anchor_height, *modern_start + pos.finality_epoch_blocks);
    BOOST_REQUIRE(pos.reorg_horizon.has_value());
    BOOST_CHECK_EQUAL(pin->anchor_height + *pos.reorg_horizon, 813'081);
    BOOST_CHECK_EQUAL(*modern_start + pos.finality_epoch_blocks, 812'441);
    BOOST_CHECK_EQUAL(*modern_start + 2 * pos.finality_epoch_blocks, 813'881);
    BOOST_CHECK_EQUAL(*modern_start + 2 * pos.finality_epoch_blocks +
                          pos.max_epoch_extension,
                      823'961);

    // No other shipped network pins a recovery.
    for (const ChainType chain : {ChainType::TESTNET, ChainType::TESTNET4,
                                  ChainType::SIGNET, ChainType::REGTEST}) {
        const auto other{CreateChainParams(ArgsManager{}, chain)};
        BOOST_CHECK(!other->GetConsensus().finality_signer_recovery.has_value());
    }
}

BOOST_AUTO_TEST_SUITE_END()

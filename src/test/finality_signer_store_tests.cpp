// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_signer_store.h>

#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <limits>

BOOST_AUTO_TEST_SUITE(finality_signer_store_tests)

BOOST_FIXTURE_TEST_CASE(exact_vote_is_monotone_idempotent_and_restored,
                        BasicTestingSetup)
{
    const fs::path dir{m_path_root / "signer-store"};
    const uint256 domain{m_rng.rand256()};
    modern::ValidatorKeyBytes validator{};
    validator.fill(0x31);
    const uint256 block_a{m_rng.rand256()};
    const uint256 digest_a{m_rng.rand256()};
    const uint256 block_b{m_rng.rand256()};
    const uint256 digest_b{m_rng.rand256()};
    const uint256 set0{m_rng.rand256()};
    const uint256 set1{m_rng.rand256()};
    const uint256 set2{m_rng.rand256()};
    const uint256 foreign_set{m_rng.rand256()};

    node::FinalitySignerStore store;
    std::string error;
    BOOST_REQUIRE_MESSAGE(store.Open(dir, domain, validator, error), error);
    BOOST_CHECK(store.IsAbsent());
    BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(
        store.CommitSignedCheckpoint(10, block_a, digest_a, 0, set0, set1,
                                     error),
        error);

    // Exact retry is harmless. Same height/different object and every lower
    // height are refused, even when the digest alone or block alone matches.
    BOOST_CHECK(store.CommitSignedCheckpoint(10, block_a, digest_a, 0,
                                             set0, set1, error));
    BOOST_CHECK(!store.CommitSignedCheckpoint(10, block_b, digest_a, 0,
                                              set0, set1, error));
    BOOST_CHECK(!store.CommitSignedCheckpoint(10, block_a, digest_b, 0,
                                              set0, set1, error));
    BOOST_CHECK(!store.CommitSignedCheckpoint(5, block_a, digest_a, 0,
                                              set0, set1, error));
    BOOST_CHECK(!store.CommitSignedCheckpoint(15, block_b, digest_b, 0,
                                              foreign_set, set1, error));
    BOOST_REQUIRE_MESSAGE(
        store.CommitSignedCheckpoint(20, block_b, digest_b, 0, set0, set1,
                                     error),
        error);
    // A certificate from a disjoint Set0 cannot unlock a competing corridor
    // fork.
    BOOST_CHECK(!store.CommitCertifiedAnchor(30, m_rng.rand256(),
                                             m_rng.rand256(), 1,
                                             foreign_set, set2, error));
    // Even the committed successor set cannot unlock the old set's vote: the
    // two sets may have no quorum intersection.
    BOOST_CHECK(!store.CommitCertifiedAnchor(30, m_rng.rand256(),
                                             m_rng.rand256(), 1,
                                             set1, set2, error));
    const uint256 certified_block{m_rng.rand256()};
    const uint256 certified_digest{m_rng.rand256()};
    const uint256 certified_successor{m_rng.rand256()};
    BOOST_REQUIRE_MESSAGE(store.CommitCertifiedAnchor(
                              30, certified_block, certified_digest, 0,
                              set0, certified_successor, error),
                          error);

    node::FinalitySignerStore restarted;
    error.clear();
    BOOST_REQUIRE_MESSAGE(restarted.Open(dir, domain, validator, error), error);
    BOOST_REQUIRE(restarted.State().has_value());
    BOOST_CHECK_EQUAL(restarted.State()->last_signed_height, 20);
    BOOST_CHECK(restarted.State()->last_signed_block_hash == block_b);
    BOOST_CHECK(restarted.State()->last_signed_digest == digest_b);
    BOOST_CHECK_EQUAL(restarted.State()->lock_height, 30);
    BOOST_CHECK_EQUAL(restarted.State()->lock_epoch, 0U);
    BOOST_CHECK(restarted.State()->lock_signing_set_hash == set0);
    BOOST_CHECK(restarted.State()->lock_successor_set_hash ==
                certified_successor);
}

BOOST_FIXTURE_TEST_CASE(corrupt_foreign_and_unwritable_state_fail_closed,
                        BasicTestingSetup)
{
    uint256 domain{m_rng.rand256()};
    // Offset 17 is the ninth chain-domain byte after the 9-byte file prefix.
    // Force the value which made the old fixed-0xa5 "corruption" a no-op.
    domain.begin()[8] = 0xa5;
    modern::ValidatorKeyBytes validator_a{};
    modern::ValidatorKeyBytes validator_b{};
    validator_a.fill(0x41);
    validator_b.fill(0x42);
    const uint256 block_hash{m_rng.rand256()};
    const uint256 digest{m_rng.rand256()};
    const uint256 set0{m_rng.rand256()};
    const uint256 set1{m_rng.rand256()};
    std::string error;

    const fs::path corrupt_dir{m_path_root / "corrupt-store"};
    node::FinalitySignerStore original;
    BOOST_REQUIRE(original.Open(corrupt_dir, domain, validator_a, error));
    BOOST_REQUIRE(original.InitializeEmpty(error));
    BOOST_REQUIRE(
        original.CommitSignedCheckpoint(10, block_hash, digest, 0, set0,
                                        set1, error));
    const fs::path original_path{original.Path()};

    // Put A's checksummed record at B's identity-derived path. The content,
    // not merely the filename, is bound to chain domain + validator identity.
    const fs::path foreign_path{node::FinalitySignerStore::StatePath(
        corrupt_dir, domain, validator_b)};
    BOOST_REQUIRE(fs::copy_file(original_path, foreign_path,
                                fs::copy_options::overwrite_existing));
    node::FinalitySignerStore foreign;
    error.clear();
    BOOST_CHECK(!foreign.Open(corrupt_dir, domain, validator_b, error));
    BOOST_CHECK(!error.empty());

    // Corruption cannot be treated as absence and cannot be overwritten by a
    // fresh signer instance.
    {
        FILE* file{fsbridge::fopen(original_path, "r+b")};
        BOOST_REQUIRE(file != nullptr);
        BOOST_REQUIRE(std::fseek(file, 17, SEEK_SET) == 0);
        unsigned char original_byte{};
        BOOST_REQUIRE(std::fread(&original_byte, 1, 1, file) == 1);
        BOOST_REQUIRE_EQUAL(original_byte, 0xa5);
        // Always change a bit: overwriting with a constant could be a no-op.
        const unsigned char byte{static_cast<unsigned char>(original_byte ^ 0x01)};
        BOOST_REQUIRE(std::fseek(file, 17, SEEK_SET) == 0);
        BOOST_REQUIRE(std::fwrite(&byte, 1, 1, file) == 1);
        BOOST_REQUIRE(std::fclose(file) == 0);
    }
    node::FinalitySignerStore corrupt;
    error.clear();
    BOOST_CHECK(!corrupt.Open(corrupt_dir, domain, validator_a, error));
    BOOST_CHECK(!error.empty());

    // A write failure leaves the in-memory and durable watermark unchanged;
    // no caller may relay the proposed vote.
    const fs::path write_dir{m_path_root / "write-failure-store"};
    node::FinalitySignerStore unwritable;
    error.clear();
    BOOST_REQUIRE(unwritable.Open(write_dir, domain, validator_a, error));
    BOOST_REQUIRE(unwritable.InitializeEmpty(error));
    fs::remove_all(write_dir);
    {
        FILE* file{fsbridge::fopen(write_dir, "wb")};
        BOOST_REQUIRE(file != nullptr);
        BOOST_REQUIRE(std::fclose(file) == 0);
    }
    error.clear();
    BOOST_CHECK(!unwritable.CommitSignedCheckpoint(
        10, block_hash, digest, 0, set0, set1, error));
    BOOST_CHECK(!error.empty());
    BOOST_REQUIRE(unwritable.State().has_value());
    BOOST_CHECK_EQUAL(unwritable.State()->last_signed_height, -1);

    // Keep the predecessor intact but make creation of the fsynced temporary
    // file fail. This exercises the actual write path, not only predecessor
    // tamper detection.
    const fs::path readonly_dir{m_path_root / "readonly-store"};
    node::FinalitySignerStore readonly;
    error.clear();
    BOOST_REQUIRE(readonly.Open(readonly_dir, domain, validator_a, error));
    BOOST_REQUIRE(readonly.InitializeEmpty(error));
    fs::permissions(readonly_dir,
                    fs::perms::owner_read | fs::perms::owner_exec);
    error.clear();
    const bool wrote{readonly.CommitSignedCheckpoint(
        10, block_hash, digest, 0, set0, set1, error)};
    fs::permissions(readonly_dir, fs::perms::owner_all); // restore for teardown
    BOOST_CHECK(!wrote);
    BOOST_CHECK(!error.empty());
    BOOST_REQUIRE(readonly.State().has_value());
    BOOST_CHECK_EQUAL(readonly.State()->last_signed_height, -1);
}

BOOST_FIXTURE_TEST_CASE(lineage_catchup_is_sequential_durable_and_preserves_the_actual_vote,
                        BasicTestingSetup)
{
    const fs::path dir{m_path_root / "fresh-sequential-store"};
    const uint256 lineage{m_rng.rand256()};
    modern::ValidatorKeyBytes validator{};
    validator.fill(0x61);
    const uint256 set0{m_rng.rand256()};
    const uint256 set1{m_rng.rand256()};
    const uint256 set2{m_rng.rand256()};
    const uint256 set3{m_rng.rand256()};
    std::string error;

    node::FinalitySignerStore store;
    BOOST_REQUIRE_MESSAGE(store.Open(dir, lineage, validator, error,
                                    node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(store.CommitSignedCheckpoint(10, m_rng.rand256(), m_rng.rand256(),
                                                       0, set0, set1, error), error);
    const node::FinalitySignerState actual_vote{*store.State()};
    node::FinalitySignerState expected{actual_vote};
    expected.lock_height = 20;
    expected.lock_block_hash = m_rng.rand256();
    expected.lock_digest = m_rng.rand256();
    expected.lock_epoch = 1;
    expected.lock_signing_set_hash = set1;
    expected.lock_successor_set_hash = set2;
    BOOST_REQUIRE_MESSAGE(store.CommitLineageCatchUp(expected.lock_height, expected.lock_block_hash,
                                                    expected.lock_digest, expected.lock_epoch,
                                                    set1, set2, error), error);
    BOOST_CHECK(*store.State() == expected);

    // A restart between steps resumes from the durable successor, not the
    // epoch of the last vote. None of that vote's fields are rewritten.
    node::FinalitySignerStore restarted;
    BOOST_REQUIRE_MESSAGE(restarted.Open(dir, lineage, validator, error,
                                        node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE(restarted.State().has_value());
    BOOST_CHECK(*restarted.State() == expected);
    expected.lock_height = 30;
    expected.lock_block_hash = m_rng.rand256();
    expected.lock_digest = m_rng.rand256();
    expected.lock_epoch = 2;
    expected.lock_signing_set_hash = set2;
    expected.lock_successor_set_hash = set3;
    BOOST_REQUIRE_MESSAGE(restarted.CommitLineageCatchUp(expected.lock_height, expected.lock_block_hash,
                                                        expected.lock_digest, expected.lock_epoch,
                                                        set2, set3, error), error);
    BOOST_CHECK(*restarted.State() == expected);

    node::FinalitySignerStore reopened;
    BOOST_REQUIRE_MESSAGE(reopened.Open(dir, lineage, validator, error,
                                       node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE(reopened.State().has_value());
    BOOST_CHECK(*reopened.State() == expected);
    BOOST_CHECK_EQUAL(reopened.State()->last_signed_height, actual_vote.last_signed_height);
    BOOST_CHECK(reopened.State()->last_signed_block_hash == actual_vote.last_signed_block_hash);
    BOOST_CHECK(reopened.State()->last_signed_digest == actual_vote.last_signed_digest);

    // An exact catch-up retry is not a second transition. The caller must
    // resume at the next epoch rather than treating duplicate input as new.
    error.clear();
    BOOST_CHECK(!reopened.CommitLineageCatchUp(expected.lock_height, expected.lock_block_hash,
                                              expected.lock_digest, expected.lock_epoch,
                                              set2, set3, error));
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(*reopened.State() == expected);
}

BOOST_FIXTURE_TEST_CASE(lineage_catchup_rejects_skipped_epochs_wrong_successors_and_invalid_targets,
                        BasicTestingSetup)
{
    const fs::path dir{m_path_root / "fresh-refusal-store"};
    const uint256 lineage{m_rng.rand256()};
    modern::ValidatorKeyBytes validator{};
    validator.fill(0x62);
    const uint256 set0{m_rng.rand256()};
    const uint256 set1{m_rng.rand256()};
    const uint256 set2{m_rng.rand256()};
    const uint256 foreign_set{m_rng.rand256()};
    const uint256 block{m_rng.rand256()};
    const uint256 digest{m_rng.rand256()};
    std::string error;
    node::FinalitySignerStore store;
    BOOST_REQUIRE_MESSAGE(store.Open(dir, lineage, validator, error,
                                    node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    BOOST_CHECK(!store.CommitLineageCatchUp(20, block, digest, 1, set1, set2, error));
    BOOST_REQUIRE_MESSAGE(store.CommitSignedCheckpoint(10, m_rng.rand256(), m_rng.rand256(),
                                                       0, set0, set1, error), error);
    const node::FinalitySignerState before{*store.State()};
    const auto refuses = [&](const int height, const uint64_t epoch,
                             const uint256& signing, const uint256& successor,
                             const uint256& target_block, const uint256& target_digest) {
        error.clear();
        BOOST_CHECK(!store.CommitLineageCatchUp(height, target_block, target_digest,
                                                epoch, signing, successor, error));
        BOOST_CHECK(!error.empty());
        BOOST_CHECK(*store.State() == before);
        node::FinalitySignerStore disk;
        BOOST_REQUIRE_MESSAGE(disk.Open(dir, lineage, validator, error,
                                       node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
        BOOST_REQUIRE(disk.State().has_value());
        BOOST_CHECK(*disk.State() == before);
    };
    refuses(20, 2, set1, set2, block, digest); // skipped an epoch
    refuses(20, 0, set1, set2, block, digest); // same epoch
    refuses(20, 1, foreign_set, set2, block, digest); // wrong predecessor's successor
    refuses(20, 1, set0, set2, block, digest); // predecessor's current set is not enough
    refuses(10, 1, set1, set2, block, digest); // height did not advance
    refuses(9, 1, set1, set2, block, digest);
    refuses(-1, 1, set1, set2, block, digest);
    refuses(20, 1, set1, set2, uint256{}, digest);
    refuses(20, 1, set1, set2, block, uint256{});
    refuses(20, 1, uint256{}, set2, block, digest);
    refuses(20, 1, set1, uint256{}, block, digest);

    // The helper never relaxes the V1 journal's existing lineage predicate.
    node::FinalitySignerStore v1;
    BOOST_REQUIRE_MESSAGE(v1.Open(dir, lineage, validator, error), error);
    BOOST_REQUIRE_MESSAGE(v1.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(v1.CommitSignedCheckpoint(10, block, digest, 0, set0, set1, error), error);
    const node::FinalitySignerState v1_before{*v1.State()};
    BOOST_CHECK(!v1.CommitLineageCatchUp(20, block, digest, 1, set1, set2, error));
    BOOST_CHECK(*v1.State() == v1_before);
}

BOOST_FIXTURE_TEST_CASE(lineage_catchup_refuses_epoch_overflow_and_a_changed_disk_predecessor,
                        BasicTestingSetup)
{
    const fs::path dir{m_path_root / "fresh-stale-store"};
    const uint256 lineage{m_rng.rand256()};
    modern::ValidatorKeyBytes validator{};
    validator.fill(0x63);
    const uint256 set0{m_rng.rand256()};
    const uint256 set1{m_rng.rand256()};
    const uint256 set2{m_rng.rand256()};
    std::string error;
    node::FinalitySignerStore store;
    BOOST_REQUIRE_MESSAGE(store.Open(dir, lineage, validator, error,
                                    node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE_MESSAGE(store.InitializeEmpty(error), error);
    BOOST_REQUIRE_MESSAGE(store.CommitSignedCheckpoint(10, m_rng.rand256(), m_rng.rand256(),
                                                       0, set0, set1, error), error);
    const node::FinalitySignerState before{*store.State()};
    node::FinalitySignerStore stale;
    BOOST_REQUIRE_MESSAGE(stale.Open(dir, lineage, validator, error,
                                    node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE_MESSAGE(store.CommitLineageCatchUp(20, m_rng.rand256(), m_rng.rand256(),
                                                    1, set1, set2, error), error);
    const node::FinalitySignerState advanced{*store.State()};
    error.clear();
    // Valid against stale's in-memory predecessor, but that exact predecessor
    // is no longer on disk. The normal compare-before-write must still refuse.
    BOOST_CHECK(!stale.CommitLineageCatchUp(25, m_rng.rand256(), m_rng.rand256(),
                                           1, set1, set2, error));
    BOOST_CHECK(error.find("predecessor changed") != std::string::npos);
    BOOST_CHECK(*stale.State() == before);
    node::FinalitySignerStore disk;
    BOOST_REQUIRE_MESSAGE(disk.Open(dir, lineage, validator, error,
                                   node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE(disk.State().has_value());
    BOOST_CHECK(*disk.State() == advanced);

    const fs::path overflow_dir{m_path_root / "fresh-overflow-store"};
    node::FinalitySignerStore overflow;
    BOOST_REQUIRE_MESSAGE(overflow.Open(overflow_dir, lineage, validator, error,
                                       node::FinalitySignerNamespace::FRESH_LINEAGE_V2), error);
    BOOST_REQUIRE_MESSAGE(overflow.InitializeEmpty(error), error);
    const uint64_t last_epoch{std::numeric_limits<uint64_t>::max() - 1};
    BOOST_REQUIRE_MESSAGE(overflow.CommitSignedCheckpoint(10, m_rng.rand256(), m_rng.rand256(),
                                                          last_epoch, set0, set1, error), error);
    const node::FinalitySignerState overflow_before{*overflow.State()};
    BOOST_CHECK(!overflow.CommitLineageCatchUp(20, m_rng.rand256(), m_rng.rand256(),
                                              0, set1, set2, error));
    BOOST_CHECK(!overflow.CommitLineageCatchUp(20, m_rng.rand256(), m_rng.rand256(),
                                              std::numeric_limits<uint64_t>::max(), set1, set2, error));
    BOOST_CHECK(*overflow.State() == overflow_before);
}

BOOST_AUTO_TEST_SUITE_END()

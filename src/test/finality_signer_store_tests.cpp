// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_signer_store.h>

#include <test/util/setup_common.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <cstdio>

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
    const uint256 domain{m_rng.rand256()};
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
        const unsigned char byte{0xa5};
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

BOOST_AUTO_TEST_SUITE_END()

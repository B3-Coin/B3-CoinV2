// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <crypto/bls.h>
#include <flowmesh2/agreement/outbound.h>
#include <flowmesh2/storage/memory_journal.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {

// Sign takes a DurableIntent and nothing else (the compile-fail control
// test/compile_fail/sign_without_intent.cpp tries a PendingIntent for real).
template <class Arg>
concept CanSign = requires(const SeatSigner& signer, Arg arg) { signer.Sign(arg); };
static_assert(CanSign<const DurableIntent&>);
static_assert(!CanSign<const PendingIntent&>);
static_assert(!CanSign<const uint256&>);
static_assert(!CanSign<const DurableSignedVote&>);
static_assert(!CanSign<const DurableDecision&>);

// An OutboundVote comes only from a DurableSignedVote.
static_assert(!std::is_default_constructible_v<agreement::OutboundVote>);
static_assert(!std::is_constructible_v<agreement::OutboundVote, const VoteKey&, const uint256&, const VoteSignature&>);
static_assert(!std::is_constructible_v<agreement::OutboundVote, const PendingSignedVote&>);

const uint256 SEAT_IKM{LabelHash("fm2/test/storage/seat0")};

std::span<const unsigned char> Ikm() { return std::span<const unsigned char>{SEAT_IKM.data(), SEAT_IKM.size()}; }

struct SigningFixture {
    std::unique_ptr<MemoryJournal> journal;
    SignerLog log;
    SeatSigner signer;

    explicit SigningFixture(uint64_t instance, MemoryJournalFaults faults = {})
        : journal{NewMemoryJournal(JournalRole::SIGNER, instance, faults)},
          log{SignerLog::Create(*journal).value()},
          signer{SeatSigner::FromIKM(Ikm(), log).value()}
    {
    }

    //! Proposal and intent of `slot` in view 0, appended (not yet durable).
    PendingIntent Intend(const StorageSlot& slot)
    {
        const PendingProposal proposal{log.AppendAcceptedProposal(ProposalKey{slot.header.authority_epoch, slot.header.height, 0},
                                                                  slot.header, slot.body)
                                           .value()};
        return log.AppendIntent(CommitVoteKey(slot.header, 0), slot.header.CommitDigest(), proposal).value();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(seat_signer_tests)

BOOST_AUTO_TEST_CASE(signs_the_durable_intent_digest)
{
    SigningFixture f{1};
    const StorageSlot slot{MakeStorageSlot(3, 0, "sign")};
    const PendingIntent pending{f.Intend(slot)};
    BOOST_CHECK_EQUAL(f.log.Barrier().value(), 2U);
    const DurableIntent intent{f.log.Confirm(pending).value()};
    const Result<VoteSignature> sig{f.signer.Sign(intent)};
    RequireStorageOk(sig, "Sign");
    // A real BLS signature over the header's CommitDigest by the seat's key.
    const std::optional<bls::Signature> parsed{bls::Signature::Decode(*sig)};
    BOOST_REQUIRE(parsed.has_value());
    const uint256 digest{slot.header.CommitDigest()};
    BOOST_CHECK(bls::Verify(f.signer.PublicKey(), std::span<const unsigned char>{digest.data(), digest.size()}, *parsed));
    BOOST_CHECK(f.signer.PublicKey() == BlsKeyFromLabel("fm2/test/storage/seat0").GetPublicKey());
    // Deterministic: signing the same intent again gives the same bytes.
    BOOST_CHECK(f.signer.Sign(intent).value() == *sig);
    // Not a signature over another digest.
    const uint256 other{slot.header.Hash()};
    BOOST_CHECK(!bls::Verify(f.signer.PublicKey(), std::span<const unsigned char>{other.data(), other.size()}, *parsed));
    // The PoP verifies, so the key can join a seat set.
    const std::optional<bls::Signature> pop{bls::Signature::Decode(f.signer.ProofOfPossession())};
    BOOST_REQUIRE(pop.has_value());
    BOOST_CHECK(bls::VerifyPoP(f.signer.PublicKey(), *pop));
}

BOOST_AUTO_TEST_CASE(full_strict_path_to_an_outbound_vote)
{
    SigningFixture f{2};
    const StorageSlot slot{MakeStorageSlot(5, 1, "outbound")};
    const PendingIntent pending{f.Intend(slot)};
    RequireStorageOk(f.log.Barrier(), "B-INTENT");
    const DurableIntent intent{f.log.Confirm(pending).value()};
    const VoteSignature sig{f.signer.Sign(intent).value()};
    const PendingSignedVote signed_pending{f.log.AppendSignedVote(intent, sig).value()};
    RequireStorageOk(f.log.Barrier(), "B-VOTE");
    const DurableSignedVote vote{f.log.Confirm(signed_pending).value()};
    const agreement::OutboundVote out{agreement::OutboundVote::From(vote)};
    BOOST_CHECK(out.Key() == CommitVoteKey(slot.header, 0));
    BOOST_CHECK(out.Digest() == slot.header.CommitDigest());
    BOOST_CHECK(out.Signature() == sig);
}

BOOST_AUTO_TEST_CASE(sign_refuses_fenced_and_foreign_intents)
{
    // An intent of another signer log is not signed.
    SigningFixture a{3};
    SigningFixture b{4};
    const StorageSlot slot{MakeStorageSlot(2, 0, "foreign")};
    const PendingIntent pending{a.Intend(slot)};
    RequireStorageOk(a.log.Barrier(), "B-INTENT");
    const DurableIntent intent{a.log.Confirm(pending).value()};
    // b holds the very same intent (same key, digest and sequence number) in
    // its own journal: only the journal instance tells them apart.
    BOOST_CHECK_EQUAL(b.Intend(slot).Seq(), intent.Seq());
    RequireRefusal(b.signer.Sign(intent), ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN, "intent of another log");
    // same_id has a's instance id and the same key and digest, at another
    // sequence number: only the sequence number tells them apart.
    SigningFixture same_id{3};
    RequireStorageOk(same_id.log.AppendAcceptedProposal(ProposalKey{0, 1, 0}, MakeStorageSlot(1, 0, "pad").header,
                                                       MakeStorageSlot(1, 0, "pad").body),
                     "padding record");
    BOOST_CHECK(same_id.Intend(slot).Seq() != intent.Seq());
    RequireRefusal(same_id.signer.Sign(intent), ErrorCode::VERIFY_FAILED, INTENT_UNKNOWN, "same instance id, other log");
    RequireStorageOk(a.signer.Sign(intent), "own intent");

    // After the signer journal turns fatal, nothing is signed, not even an
    // intent that was durable before.
    SigningFixture c{5, MemoryJournalFaults{.fail_barrier = 2}};
    const PendingIntent first{c.Intend(slot)};
    RequireStorageOk(c.log.Barrier(), "B-INTENT");
    const DurableIntent durable{c.log.Confirm(first).value()};
    RequireStorageOk(c.signer.Sign(durable), "sign before the failure");
    RequireCode(c.log.Barrier(), ErrorCode::STORAGE_FATAL, "failed barrier");
    RequireRefusal(c.signer.Sign(durable), ErrorCode::FENCED, JOURNAL_FENCED, "sign after the failure");
}

BOOST_AUTO_TEST_CASE(from_ikm_refuses_short_material)
{
    auto journal{NewMemoryJournal(JournalRole::SIGNER, 6)};
    const SignerLog log{SignerLog::Create(*journal).value()};
    RequireCode(SeatSigner::FromIKM(Ikm().first(31), log), ErrorCode::INVALID_INPUT, "31-byte IKM");
    RequireStorageOk(SeatSigner::FromIKM(Ikm(), log), "32-byte IKM");
}

BOOST_AUTO_TEST_SUITE_END()

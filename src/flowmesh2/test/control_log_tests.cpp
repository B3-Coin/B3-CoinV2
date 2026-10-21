// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <boost/test/unit_test.hpp>
#include <flowmesh2/storage/control_log.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <util/chaintype.h>

using namespace flowmesh2;
using namespace flowmesh2::storage;
using namespace flowmesh2::test;

namespace {
struct ControlFixture {
    SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    std::unique_ptr<MemoryJournal> vote_journal{NewMemoryJournal(JournalRole::SIGNER, 701)};
    SignerLog vote_log{SignerLog::Create(*vote_journal).value()};
    std::unique_ptr<MemoryJournal> journal;
    ControlConfig config{TestDomain(), ProfileHash(StableQViewRegtestProfile(ChainType::REGTEST).value()), seats.Root(), 0, 0, 4, seats.Keys()[0].Key()};
    ControlLog log;
    SeatSigner signer;
    explicit ControlFixture(MemoryJournalFaults faults = {})
        : journal{NewMemoryJournal(JournalRole::SIGNER, 702, faults)}, log{ControlLog::Create(*journal, config).value()},
          signer{SeatSigner::FromIKM(AgreementRig::SeatIkm("s0"), vote_log, log).value()} {}
    ControlKey Key(uint64_t view = 1, ControlPhase phase = ControlPhase::VIEW_CHANGE, uint32_t seat = 0) const
    {
        return ControlKey{config.domain, config.profile_hash, config.seat_set_root, 0, 1, LabelHash("control-parent"), seat, view, phase};
    }
    DurableControlIntent Intent(uint64_t view = 1)
    {
        auto pending{log.AppendIntent({Key(view), Bytes("exact unsigned report " + std::to_string(view))})};
        RequireOk(pending, "append control intent");
        RequireOk(log.Barrier(), "intent barrier");
        auto durable{log.Confirm(*pending)};
        RequireOk(durable, "durable control intent");
        return std::move(*durable);
    }
};
}

BOOST_AUTO_TEST_SUITE(control_log_tests)

BOOST_AUTO_TEST_CASE(real_signature_requires_both_durable_barriers)
{
    ControlFixture f;
    const ControlIntentRecord record{f.Key(), Bytes("exact highest prepared report")};
    auto pending{f.log.AppendIntent(record).value()};
    RequireCode(f.log.Confirm(pending), ErrorCode::ORDER_VIOLATION, "no intent token before barrier");
    RequireOk(f.log.Barrier(), "intent barrier");
    auto intent{f.log.Confirm(pending).value()};
    const auto signature{f.signer.SignControl(intent).value()};
    const auto digest{record.Digest()};
    BOOST_CHECK(bls::Verify(f.signer.PublicKey(), std::span{digest.data(), digest.size()}, *bls::Signature::Decode(signature)));
    auto signed_pending{f.log.AppendSigned(intent, signature).value()};
    RequireCode(f.log.Confirm(signed_pending), ErrorCode::ORDER_VIOLATION, "no publication token before barrier");
    RequireOk(f.log.Barrier(), "signature barrier");
    auto durable{f.log.Confirm(signed_pending).value()};
    BOOST_CHECK(durable.Binding().key == record.key);
    BOOST_CHECK(durable.Binding().signature == signature);
    BOOST_CHECK(f.log.ViewAt(1)->changing);
    auto recovered{ControlLog::Create(*f.journal, f.config)};
    RequireOk(recovered, "control signature recovery");
    BOOST_CHECK(recovered->SignedAt(record.key).has_value());
}

BOOST_AUTO_TEST_CASE(restart_preserves_atomic_report_and_refuses_abandoned_first_signature)
{
    ControlFixture f;
    auto old_intent{f.Intent(1)};
    const auto old_record{*f.log.IntentAt(f.Key(1))};
    auto restored{ControlLog::Create(*f.journal, f.config).value()};
    auto signer{SeatSigner::FromIKM(AgreementRig::SeatIkm("s0"), f.vote_log, restored).value()};
    BOOST_CHECK(restored.ViewAt(1)->changing);
    BOOST_CHECK_EQUAL(restored.ViewAt(1)->view, 1U);
    auto mutated{old_record};
    mutated.message.push_back('x');
    RequireCode(restored.AppendIntent(mutated), ErrorCode::EQUIVOCATION_REFUSED, "report is immutable");
    RequireOk(restored.AppendIntent({f.Key(2), Bytes("later exact report")}), "atomic next view and report");
    RequireOk(restored.Barrier(), "next view durable");
    RequireCode(signer.SignControl(old_intent), ErrorCode::ORDER_VIOLATION, "cannot first sign abandoned report");
    auto again{ControlLog::Create(*f.journal, f.config).value()};
    BOOST_CHECK_EQUAL(again.ViewAt(1)->view, 2U);
    BOOST_CHECK(again.IntentAt(f.Key(1))->message == old_record.message);
}

BOOST_AUTO_TEST_CASE(active_view_needs_exact_accepted_bundle_and_barrier)
{
    ControlFixture f;
    auto intent{f.Intent(1)};
    RequireOk(f.signer.SignControlAndPersist(intent, f.log), "strict signed report path");
    RequireCode(f.log.AllowsVote(f.config.domain, 0, 1, 0), ErrorCode::ORDER_VIOLATION, "view zero abandoned");
    const AcceptedControlView accepted{f.Key(1, ControlPhase::NEW_VIEW, 2), Bytes("exact previously verified q3 bundle")};
    RequireOk(f.log.AcceptNewView(accepted), "accepted new view append");
    RequireCode(f.log.AllowsVote(f.config.domain, 0, 1, 1), ErrorCode::ORDER_VIOLATION, "accepted bundle not durable yet");
    RequireOk(f.log.Barrier(), "accepted new view barrier");
    RequireOk(f.log.AllowsVote(f.config.domain, 0, 1, 1), "active exact new view");
    auto replacement{accepted};
    replacement.message.push_back('x');
    RequireCode(f.log.AcceptNewView(replacement), ErrorCode::EQUIVOCATION_REFUSED, "same view bundle substitution");
    auto restored{ControlLog::Create(*f.journal, f.config).value()};
    RequireOk(restored.AllowsVote(f.config.domain, 0, 1, 1), "active view restored");
    RequireOk(restored.AppendIntent({f.Key(2), Bytes("next report")}), "enter later view");
    RequireCode(restored.AllowsVote(f.config.domain, 0, 1, 1), ErrorCode::ORDER_VIOLATION, "pending later transition already stops old votes");
}

BOOST_AUTO_TEST_CASE(foreign_token_wrong_key_and_storage_failure_cannot_publish)
{
    ControlFixture f;
    auto intent{f.Intent()};
    auto other_journal{NewMemoryJournal(JournalRole::SIGNER, 703)};
    auto other_log{ControlLog::Create(*other_journal, f.config).value()};
    auto other_signer{SeatSigner::FromIKM(AgreementRig::SeatIkm("s0"), f.vote_log, other_log).value()};
    RequireCode(other_signer.SignControl(intent), ErrorCode::VERIFY_FAILED, "foreign durable token");
    RequireCode(SeatSigner::FromIKM(AgreementRig::SeatIkm("s1"), f.vote_log, f.log), ErrorCode::INVALID_INPUT, "wrong configured signer key");
    MemoryJournalFaults faults;
    faults.fail_barrier = 2;
    ControlFixture failing{faults};
    auto durable{failing.Intent()};
    RequireCode(failing.signer.SignControlAndPersist(durable, failing.log), ErrorCode::STORAGE_FATAL, "signature barrier failed");
    BOOST_CHECK(failing.log.IsFatal());
    RequireCode(failing.signer.SignControl(durable), ErrorCode::FENCED, "no signing after storage failure");
    BOOST_CHECK_EQUAL(failing.journal->DurableSeq(), 1U);
    std::vector<unsigned char> too_big(MAX_CONTROL_MESSAGE_BYTES + 1, 0);
    const auto before{f.journal->Status().appended_seq};
    RequireCode(f.log.AppendIntent({f.Key(2), std::move(too_big)}), ErrorCode::OVERSIZE, "bounded report body");
    BOOST_CHECK_EQUAL(f.journal->Status().appended_seq, before);
}

BOOST_AUTO_TEST_SUITE_END()

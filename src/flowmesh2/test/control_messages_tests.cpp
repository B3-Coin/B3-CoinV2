// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <boost/test/unit_test.hpp>
#include <flowmesh2/agreement/control_messages.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/test/util/agreement_fixtures.h>
#include <util/chaintype.h>

using namespace flowmesh2;
using namespace flowmesh2::agreement;
using namespace flowmesh2::test;

namespace {
struct ControlMessagesFixture {
    ProfileParams profile{StableQViewRegtestProfile(ChainType::REGTEST).value()};
    SeatSet seats{TestSeats({"s0", "s1", "s2", "s3"})};
    AuthorityVersion authority{0, seats.Root(), ProfileHash(profile)};
    state::HeadView genesis{state::GenesisHead(profile, std::vector<MarketDescriptor>{MarketM1(), MarketM2()}, TestDomain(), authority,
                                               TestAnchor(129), {130, 0}).value()};
    std::unique_ptr<state::StateOwner> owner{state::StateOwner::Create(profile, genesis, 901).value()};
    SlotHeaderV2 Header(uint32_t height = 130) const
    {
        state::SlotInputs inputs;
        inputs.authority = authority;
        inputs.anchor = TestAnchor(height);
        inputs.facts_from = {130, 0};
        inputs.facts_to = {131, 0};
        return owner->Preview(state::ParentRef::Committed(genesis), inputs).value().Header();
    }
    storage::ControlKey Key(uint32_t seat, uint64_t view, storage::ControlPhase phase = storage::ControlPhase::VIEW_CHANGE) const
    {
        return {genesis.Header().domain, authority.profile_hash, seats.Root(), 0, 1, genesis.Header().Hash(), seat, view, phase};
    }
    SlotCertificate Certificate(const SlotHeaderV2& header, uint64_t view, bool commit = false, size_t count = 3) const
    {
        std::vector<bls::Signature> signatures;
        std::vector<uint32_t> indices;
        const auto digest{commit ? VersionedCommitDigest(header, view) : PbftPrepareDigest(header, view)};
        for (uint32_t i{0}; i < count; ++i) {
            indices.push_back(i);
            const auto key{BlsKeyFromLabel(SeatLabel("s" + std::to_string(i)))};
            signatures.push_back(key.Sign(std::span{digest.data(), digest.size()}));
        }
        return SlotCertificate{header.Hash(), SlotCertificate::MakeBitmap(4, indices).value(), bls::AggregateSignatures(signatures)->Compressed(), view};
    }
    template<class T> void Sign(T& message) const
    {
        const auto digest{ControlIntent(message, profile).value().Digest()};
        const auto key{BlsKeyFromLabel(SeatLabel("s" + std::to_string(message.key.seat)))};
        message.signature = key.Sign(std::span{digest.data(), digest.size()}).Compressed();
    }
    ControlReport Report(uint32_t seat, uint64_t target, std::optional<ControlPrepared> prepared = std::nullopt) const
    {
        ControlReport report{Key(seat, target), 5, std::move(prepared), std::nullopt, {}};
        Sign(report);
        return report;
    }
    ControlNewView NewView(uint64_t target, const uint256& selected, std::vector<ControlReport> reports) const
    {
        ControlNewView view{Key(static_cast<uint32_t>((1 + target) % 4), target, storage::ControlPhase::NEW_VIEW), selected, std::move(reports), {}, {}};
        const auto header{Header(131).Hash() == selected ? Header(131) : Header(130)};
        view.selected_proposal = Proposal{0, target, header, std::nullopt, {}};
        Sign(view);
        return view;
    }
};
}

BOOST_AUTO_TEST_SUITE(control_messages_tests)

BOOST_AUTO_TEST_CASE(real_highest_prepared_selection_and_complete_transcript_binding)
{
    ControlMessagesFixture f;
    const auto a{f.Header(130)}, b{f.Header(131)};
    const ControlPrepared older{0, a, f.Certificate(a, 0)}, highest{1, b, f.Certificate(b, 1)};
    auto view{f.NewView(2, b.Hash(), {f.Report(0, 2, highest), f.Report(1, 2, older), f.Report(2, 2)})};
    auto selected{VerifyControlNewView(view, f.genesis.Header(), f.seats, f.profile)};
    RequireOk(selected, "highest complete real BLS proof");
    BOOST_REQUIRE(selected->highest);
    BOOST_CHECK(selected->highest->header.Hash() == b.Hash());
    view.selected = a.Hash();
    f.Sign(view);
    RequireCode(VerifyControlNewView(view, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "cannot choose lower preparation");
    auto report{f.Report(0, 2, highest)};
    report.prepared_watermark++;
    RequireCode(VerifyControlReport(report, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "watermark covered by signature");
    report = f.Report(0, 2, highest);
    report.key.view = 3;
    RequireCode(VerifyControlReport(report, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "target covered by signature");
}

BOOST_AUTO_TEST_CASE(quorum_phase_instance_and_conflicting_equal_view_proofs_fail_closed)
{
    ControlMessagesFixture f;
    const auto a{f.Header(130)}, b{f.Header(131)};
    auto weak{f.Report(0, 1, ControlPrepared{0, a, f.Certificate(a, 0, false, 2)})};
    RequireCode(VerifyControlReport(weak, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "two prepare shares not quorum");
    auto wrong_phase{f.Report(0, 1, ControlPrepared{0, a, f.Certificate(a, 0, true)})};
    RequireCode(VerifyControlReport(wrong_phase, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "commit proof is not prepare");
    auto conflict{f.NewView(1, a.Hash(), {f.Report(0, 1, ControlPrepared{0, a, f.Certificate(a, 0)}),
                                         f.Report(1, 1, ControlPrepared{0, b, f.Certificate(b, 0)}), f.Report(2, 1)})};
    RequireCode(VerifyControlNewView(conflict, f.genesis.Header(), f.seats, f.profile), ErrorCode::EQUIVOCATION_REFUSED, "equal view conflicting preparations halt");
    auto report{f.Report(0, 1)};
    report.key.parent_hash = LabelHash("another parent");
    f.Sign(report);
    RequireCode(VerifyControlReport(report, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "wrong decision instance");
    auto short_view{f.NewView(1, a.Hash(), {f.Report(0, 1), f.Report(1, 1)})};
    RequireCode(VerifyControlNewView(short_view, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "two reports not quorum");
}

BOOST_AUTO_TEST_CASE(known_decision_is_returned_for_catchup_and_omitted_local_preparation_is_not_a_veto)
{
    ControlMessagesFixture f;
    const auto header{f.Header()};
    auto first{f.Report(0, 1)};
    first.decision = CertifiedHeader{header, f.Certificate(header, 0, true)};
    f.Sign(first);
    auto view{f.NewView(1, header.Hash(), {first, f.Report(1, 1), f.Report(2, 1)})};
    auto selected{VerifyControlNewView(view, f.genesis.Header(), f.seats, f.profile)};
    RequireOk(selected, "known commit certificate");
    BOOST_REQUIRE(selected->decision);
    BOOST_CHECK(selected->decision->header.Hash() == header.Hash());
    // The verifier depends on the quorum's reports. A lone local preparation
    // omitted from that quorum cannot impose an additional local veto.
    auto no_preparation{f.NewView(2, header.Hash(), {f.Report(0, 2), f.Report(1, 2), f.Report(2, 2)})};
    RequireOk(VerifyControlNewView(no_preparation, f.genesis.Header(), f.seats, f.profile), "valid quorum without local report");
}

BOOST_AUTO_TEST_CASE(control_codecs_are_exact_and_report_counts_are_capped_before_allocation)
{
    ControlMessagesFixture f;
    auto report{f.Report(0, 1)};
    auto view{f.NewView(1, f.Header().Hash(), {report, f.Report(1, 1), f.Report(2, 1)})};
    for (const Message& message : {Message{report}, Message{view}}) {
        auto bytes{EncodeMessage(message, f.profile).value()};
        BOOST_CHECK(DecodeMessage(bytes, f.profile).value() == message);
        bytes.push_back(0);
        RequireCode(DecodeMessage(bytes, f.profile), ErrorCode::DECODE, "trailing control bytes");
    }
    // The cap is one report per seat of the largest regtest seat set (O-98):
    // MAX_CONTROL_REPORTS encode, one more is refused.
    view.reports.push_back(f.Report(3, 1));
    while (view.reports.size() < MAX_CONTROL_REPORTS) view.reports.push_back(report);
    BOOST_CHECK(EncodeMessage(Message{view}, f.profile).has_value());
    view.reports.push_back(report);
    RequireCode(EncodeMessage(Message{view}, f.profile), ErrorCode::OVERSIZE, "one report past MAX_CONTROL_REPORTS");
    report.signature.fill(0);
    RequireCode(VerifyControlReport(report, f.genesis.Header(), f.seats, f.profile), ErrorCode::VERIFY_FAILED, "opaque invalid BLS bytes do not authenticate");
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The read service (slice R11): envelopes and scope, slot views, the
 * publication rules (a mismatched token is refused), the replies (one
 * lease, one head, label CERTIFIED), maker fill lookup, INCLUDED / EXPIRED /
 * UNKNOWN with proofs that b3verify accepts, the view bound (the oldest
 * slot then reads UNKNOWN), the outcome-index retention (refused below
 * max_delay at Create, kept exactly outcome_index_slots slots), and 4
 * readers against 1 publisher over 2,000 slots (TSan-relevant; this macOS
 * host has no working TSan, O-22). The last four cases pin checks that the
 * R11 mutation run found untested: the token's header hash (a rival header
 * with the same slot and state root), the successor's parent hash (height 2
 * of another chain), the outcome index seen from an older lease, and one
 * group per outcome when an order fills twice in it.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/read/views.h>
#include <flowmesh2/read_slot.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/test/util/read_fixtures.h>
#include <flowmesh2/verify/b3verify.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

template <class T>
T Take(Result<T> result, std::string_view what)
{
    RequireOk(result, what);
    return std::move(result).value();
}

//! The walking skeleton through a ReadRig; each slot is published right after it commits.
struct PublishedSkeleton {
    ReadRig rig;
    std::unique_ptr<read::ReadService> service;
    SkeletonIds ids;
    std::vector<CommittedSlot> slots;
    //! The published views (index = height - 1).
    std::vector<std::shared_ptr<const read::SlotView>> views;
    //! Account replies taken right after each slot was published (index = height - 1).
    std::vector<read::AccountReply> alice_at, bob_at;
    //! Action replies for w right after each slot.
    std::vector<read::ActionReply> w_at;
    //! OrderFills(a) right after each slot.
    std::vector<read::OrderFillsReply> a_fills_at;

    explicit PublishedSkeleton(size_t slot_count = 5, size_t view_slots = 8)
        : service{NewReadService(TestReadLimits(view_slots))}
    {
        slots = RunSkeleton(
            rig.actions,
            [this](std::vector<std::vector<unsigned char>> facts, std::vector<std::vector<unsigned char>> originals) {
                PublishableSlot slot{rig.Run(std::move(facts), std::move(originals))};
                Publish(*service, slot);
                views.push_back(slot.view);
                alice_at.push_back(Take(service->Account(KeyAccount("alice")), "Account(alice)"));
                bob_at.push_back(Take(service->Account(KeyAccount("bob")), "Account(bob)"));
                if (!ids.w_bytes.empty()) w_at.push_back(Take(service->Action(DecodeAction(ids.w_bytes)), "Action(w)"));
                a_fills_at.push_back(Take(service->OrderFills(ids.a), "OrderFills(a)"));
                return slot.slot;
            },
            ids, slot_count);
    }
};

//! The reply head is the lease's: scope and commit of its envelope's header, label CERTIFIED, certificate naming it.
void CheckReplyHead(const read::ReplyHead& head, uint64_t height)
{
    const SlotHeaderV2& header{head.envelope.header};
    BOOST_CHECK_EQUAL(header.height, height);
    BOOST_CHECK(head.commit == read::CommitOf(header));
    BOOST_CHECK(head.scope == read::ScopeOf(header));
    BOOST_CHECK(head.envelope.cert.header_hash == head.commit.id);
    BOOST_CHECK(head.label == read::ReadLabel::CERTIFIED);
}

//! Requires an UNKNOWN reply with exactly `reason`.
template <class Reply, class Status>
void RequireUnknown(const Reply& reply, Status unknown, std::string_view reason, std::string_view what)
{
    BOOST_CHECK_MESSAGE(reply.status == unknown && reply.reason == reason,
                        std::string{what} + ": reason '" + std::string{reply.reason} + "', want '" + std::string{reason} + "'");
}

} // namespace

BOOST_AUTO_TEST_SUITE(read_tests)

BOOST_AUTO_TEST_CASE(envelope_codec_scope_and_commit)
{
    ReadRig rig;
    const PublishableSlot slot{rig.Run(FundingFacts(rig.actions), {})};
    const read::CertifiedEnvelope& envelope{slot.view->Envelope()};
    const std::vector<unsigned char> bytes{envelope.Encode()};
    std::vector<unsigned char> expected{envelope.header.Encode()};
    const std::vector<unsigned char> cert_bytes{envelope.cert.Encode()};
    expected.insert(expected.end(), cert_bytes.begin(), cert_bytes.end());
    BOOST_CHECK(bytes == expected);
    BOOST_CHECK_EQUAL(bytes.size(), SLOT_HEADER_BYTES + 32 + 1 + 1 + 96); // k = 1: a one-byte bitmap
    BOOST_CHECK(Take(read::CertifiedEnvelope::Decode(bytes), "Decode") == envelope);

    std::vector<unsigned char> trailing{bytes};
    trailing.push_back(0x00);
    RequireFailure(read::CertifiedEnvelope::Decode(trailing), ErrorCode::DECODE, "trailing bytes", "trailing byte");
    const std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 1);
    BOOST_CHECK(read::CertifiedEnvelope::Decode(truncated).error().code == ErrorCode::DECODE);
    const std::vector<unsigned char> huge(read::CertifiedEnvelope::MAX_ENCODED_BYTES + 1, 0x00);
    BOOST_CHECK(read::CertifiedEnvelope::Decode(huge).error().code == ErrorCode::OVERSIZE);
    std::vector<unsigned char> bad_version{bytes};
    bad_version[0] = 2;
    BOOST_CHECK(read::CertifiedEnvelope::Decode(bad_version).error().code == ErrorCode::DECODE);
    std::vector<unsigned char> wide_bitmap{bytes};
    wide_bitmap[SLOT_HEADER_BYTES + 32] = static_cast<unsigned char>(MAX_BITMAP_BYTES + 1);
    BOOST_CHECK(read::CertifiedEnvelope::Decode(wide_bitmap).error().code == ErrorCode::OVERSIZE);

    const SlotHeaderV2& header{envelope.header};
    const ReadScope scope{read::ScopeOf(header)};
    BOOST_CHECK(scope.domain == header.domain);
    BOOST_CHECK(scope.configuration == header.profile_hash);
    BOOST_CHECK(scope.validator_set == header.seat_set_root);
    BOOST_CHECK(scope.validator_set == rig.seats.Root());
    BOOST_CHECK(scope.anchor == header.anchor.hash);
    BOOST_CHECK_EQUAL(scope.epoch, header.authority_epoch);
    BOOST_CHECK_EQUAL(scope.authority_revision, header.anchor.height);
    const CommitIdentity commit{read::CommitOf(header)};
    BOOST_CHECK(commit.id == header.Hash());
    BOOST_CHECK(commit.parent == header.parent_hash);
    BOOST_CHECK(commit.state_root == header.state_root);
    BOOST_CHECK_EQUAL(commit.sequence, header.height);
}

BOOST_AUTO_TEST_CASE(create_refuses_bad_limits_and_a_short_outcome_index)
{
    const ProfileParams profile{RegtestProfile()};
    const read::ReadLimits good{TestReadLimits()};
    RequireOk(read::ReadService::Create(good, profile), "good limits");
    for (int field{0}; field < 3; ++field) {
        read::ReadLimits zero{good};
        (field == 0 ? zero.max_readers : field == 1 ? zero.max_envelope_bytes : zero.view_slots) = 0;
        RequireFailure(read::ReadService::Create(zero, profile), ErrorCode::INVALID_INPUT, read::READ_LIMIT_ZERO, "zero limit");
    }
    read::ReadLimits short_index{good};
    short_index.outcome_index_slots = profile.max_delay_slots - 1;
    RequireFailure(read::ReadService::Create(short_index, profile), ErrorCode::INVALID_INPUT, read::READ_OUTCOME_INDEX_SHORT,
                   "1,023 < max_delay_slots");
    short_index.outcome_index_slots = 0;
    RequireFailure(read::ReadService::Create(short_index, profile), ErrorCode::INVALID_INPUT, read::READ_OUTCOME_INDEX_SHORT,
                   "zero outcome index");
    read::ReadLimits exact{good};
    exact.outcome_index_slots = profile.max_delay_slots;
    RequireOk(read::ReadService::Create(exact, profile), "outcome index = max_delay_slots");
    ProfileParams per_side{profile};
    per_side.fee_incidence = FeeIncidence::PER_SIDE;
    RequireStub(read::ReadService::Create(good, per_side), "M3-ENG-08");
    ProfileParams main{profile};
    main.chain = ChainType::MAIN;
    BOOST_CHECK(read::ReadService::Create(good, main).error().code == ErrorCode::UNSUPPORTED_NETWORK);
}

BOOST_AUTO_TEST_CASE(reads_before_the_first_publication_need_data)
{
    const auto service{NewReadService(TestReadLimits())};
    RequireFailure(service->Acquire(), ErrorCode::NEED_DATA, read::READ_NOTHING_PUBLISHED, "Acquire");
    RequireFailure(service->Account(KeyAccount("alice")), ErrorCode::NEED_DATA, read::READ_NOTHING_PUBLISHED, "Account");
    RequireFailure(service->Head(), ErrorCode::NEED_DATA, read::READ_NOTHING_PUBLISHED, "Head");
    BOOST_CHECK(!service->PublishedHeight().has_value());
    BOOST_CHECK_EQUAL(service->ViewCount(), 0U);
}

BOOST_AUTO_TEST_CASE(slot_view_build_refusals)
{
    ReadRig rig;
    const PublishableSlot slot{rig.Run(FundingFacts(rig.actions), {})};
    const state::HeadView& head{*slot.slot.head};
    RequireOk(read::SlotView::Build(head, slot.cert, slot.slot.outcomes), "control");

    RequireFailure(read::SlotView::Build(rig.genesis, slot.cert, {}), ErrorCode::INVALID_INPUT, read::VIEW_GENESIS, "genesis");
    SlotCertificate other_header{slot.cert};
    other_header.header_hash = rig.genesis.HeaderHash();
    RequireFailure(read::SlotView::Build(head, other_header, slot.slot.outcomes), ErrorCode::INVALID_INPUT, read::VIEW_CERT_HEADER,
                   "certificate of another header");
    // Another valid-looking certificate of the same header is not the one that decided it.
    const SlotCertificate other_cert{TestCertificate(head.Header(), 9)};
    RequireFailure(read::SlotView::Build(head, other_cert, slot.slot.outcomes), ErrorCode::INVALID_INPUT, read::VIEW_CERT_HASH,
                   "certificate that did not decide the head");
    std::vector<OutcomeV2> missing{slot.slot.outcomes};
    missing.pop_back();
    RequireFailure(read::SlotView::Build(head, slot.cert, missing), ErrorCode::INVALID_INPUT, read::VIEW_OUTCOMES, "outcome missing");
    std::vector<OutcomeV2> tampered{slot.slot.outcomes};
    tampered.front().deltas.front().available_delta += 1;
    RequireFailure(read::SlotView::Build(head, slot.cert, tampered), ErrorCode::INVALID_INPUT, read::VIEW_OUTCOMES, "outcome tampered");
    // A head whose state is not the one its header commits to.
    const state::HeadView wrong_state{Take(state::HeadView::Make(head.Header(), head.CertHash(), rig.genesis.StatePtr()), "Make")};
    RequireFailure(read::SlotView::Build(wrong_state, slot.cert, slot.slot.outcomes), ErrorCode::INVARIANT, read::VIEW_STATE_ROOT,
                   "state of another slot");
}

BOOST_AUTO_TEST_CASE(slot_view_indexes_and_proofs)
{
    const PublishedSkeleton s{3};
    const CommittedSlot& c3{s.slots.at(2)};
    const std::shared_ptr<const read::SlotView>& view{s.views.at(2)};
    BOOST_CHECK(view->Header() == c3.head->Header());
    BOOST_CHECK(view->HeaderHash() == c3.head->HeaderHash());
    BOOST_CHECK(view->Certificate().Hash() == c3.head->CertHash());
    BOOST_CHECK(view->Outcomes() == c3.outcomes);
    BOOST_CHECK(view->Roots() == c3.head->State().engine.Commit().value());
    for (uint32_t i{0}; i < view->Outcomes().size(); ++i) {
        const read::OutcomeWitness w{Take(view->Outcome(i), "Outcome")};
        BOOST_CHECK_EQUAL(w.index, i);
        RequireOk(verify::VerifyOutcome(view->Header(), w.outcome, w.proof), "outcome proof");
    }
    RequireFailure(view->Outcome(static_cast<uint32_t>(view->Outcomes().size())), ErrorCode::INVALID_INPUT,
                   "outcome index outside the slot", "outcome index outside");
    // Action index: c and d executed in slot 3; a did not.
    const auto c_index{view->ActionOutcome(ActionId{s.ids.c.id})};
    BOOST_REQUIRE(c_index.has_value());
    BOOST_CHECK(view->Outcomes()[*c_index].subject == s.ids.c.id);
    BOOST_CHECK(view->ActionOutcome(ActionId{s.ids.d.id}).has_value());
    BOOST_CHECK(!view->ActionOutcome(ActionId{s.ids.a.id}).has_value());
    // Fill index: a as maker and c as taker of the one fill.
    const auto maker{view->FillsOf(s.ids.a)};
    BOOST_REQUIRE_EQUAL(maker.size(), 1U);
    BOOST_CHECK(maker[0] == (read::FillRef{*c_index, 0, read::FillRole::MAKER}));
    const auto taker{view->FillsOf(s.ids.c)};
    BOOST_REQUIRE_EQUAL(taker.size(), 1U);
    BOOST_CHECK(taker[0] == (read::FillRef{*c_index, 0, read::FillRole::TAKER}));
    BOOST_CHECK(view->FillsOf(s.ids.b).empty());
    // Every account of the state, and no other.
    const state::CoreState& state{c3.head->State()};
    BOOST_CHECK_EQUAL(view->AccountCount(), state.engine.ledger.Accounts().size());
    for (const auto& entry : state.engine.ledger.Accounts()) {
        const auto witness{view->Account(entry.first)};
        BOOST_REQUIRE(witness.has_value());
        RequireOk(verify::VerifyAccountLeaf(view->Header(), witness->leaf, witness->roots, witness->proof, RegtestProfile()), "leaf");
    }
    BOOST_CHECK(!view->Account(KeyAccount("carol")).has_value());
}

BOOST_AUTO_TEST_CASE(mismatched_token_is_refused)
{
    ReadRig rig;
    const auto service{NewReadService(TestReadLimits())};
    const PublishableSlot s1{rig.Run(FundingFacts(rig.actions), {})};
    // Two candidates for height 2; only one is decided.
    const state::Candidate chosen{rig.Preview(rig.Inputs({}, {rig.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)}))};
    const state::Candidate rival{rig.Preview(rig.Inputs({}, {rig.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 2, 1)}))};
    BOOST_REQUIRE(chosen.Header().Hash() != rival.Header().Hash());
    const SlotCertificate rival_cert{rig.CertificateOf(rival.Header())};
    const state::HeadView rival_head{Take(state::HeadView::Make(rival.Header(), rival_cert.Hash(), rival.Next()), "Make")};
    const auto rival_view{Take(read::SlotView::Build(rival_head, rival_cert, rival.Outcomes()), "Build rival")};
    const PublishableSlot s2{rig.Commit(chosen)};

    RequireFailure(service->Publish(s1.applied, nullptr), ErrorCode::INVALID_INPUT, read::READ_NULL_VIEW, "no view");
    // Slot 1's token for slot 2's view; slot 2's token for slot 1's view and for the rival view.
    RequireFailure(service->Publish(s1.applied, s2.view), ErrorCode::VERIFY_FAILED, read::READ_TOKEN_MISMATCH, "old token");
    RequireFailure(service->Publish(s2.applied, s1.view), ErrorCode::VERIFY_FAILED, read::READ_TOKEN_MISMATCH, "newer token");
    RequireFailure(service->Publish(s2.applied, rival_view), ErrorCode::VERIFY_FAILED, read::READ_TOKEN_MISMATCH, "rival view");
    BOOST_CHECK(!service->PublishedHeight().has_value());
    BOOST_CHECK_EQUAL(service->ViewCount(), 0U);
    // Matching tokens publish, in order.
    Publish(*service, s1);
    Publish(*service, s2);
    BOOST_CHECK_EQUAL(*service->PublishedHeight(), 2U);
}

BOOST_AUTO_TEST_CASE(publication_order_profile_and_size)
{
    ReadRig rig;
    const PublishableSlot s1{rig.Run(FundingFacts(rig.actions), {})};
    const PublishableSlot s2{rig.Run({}, {})};
    const PublishableSlot s3{rig.Run({}, {})};
    {
        const auto service{NewReadService(TestReadLimits())};
        Publish(*service, s1);
        RequireFailure(service->Publish(s3.applied, s3.view), ErrorCode::ORDER_VIOLATION, read::READ_NOT_SUCCESSOR, "gap");
        RequireFailure(service->Publish(s1.applied, s1.view), ErrorCode::ORDER_VIOLATION, read::READ_NOT_SUCCESSOR, "repeat");
        Publish(*service, s2);
        Publish(*service, s3);
        BOOST_CHECK_EQUAL(*service->PublishedHeight(), 3U);
    }
    {
        // The first publication may be at any height (a service that starts later).
        const auto service{NewReadService(TestReadLimits())};
        Publish(*service, s2);
        Publish(*service, s3);
    }
    {
        // An envelope above the cap changes nothing.
        read::ReadLimits small{TestReadLimits()};
        small.max_envelope_bytes = SLOT_HEADER_BYTES + 32 + 1 + 1 + 96 - 1;
        const auto service{NewReadService(small)};
        RequireFailure(service->Publish(s1.applied, s1.view), ErrorCode::OVERSIZE, read::READ_ENVELOPE_OVERSIZE, "envelope");
        BOOST_CHECK(!service->PublishedHeight().has_value());
        BOOST_CHECK_EQUAL(service->ViewCount(), 0U);
        BOOST_CHECK_EQUAL(service->IndexedActions(), 0U);
        RequireFailure(service->Acquire(), ErrorCode::NEED_DATA, read::READ_NOTHING_PUBLISHED, "still empty");
    }
    {
        // A view of another profile.
        ReadRigOptions options;
        options.profile.fee_ppm = 200;
        ReadRig other{options};
        const PublishableSlot o1{other.Run(FundingFacts(other.actions), {})};
        const auto service{NewReadService(TestReadLimits())};
        RequireFailure(service->Publish(o1.applied, o1.view), ErrorCode::INVALID_INPUT, read::READ_PROFILE_MISMATCH, "profile");
    }
}

BOOST_AUTO_TEST_CASE(replies_carry_one_head_scope_and_label)
{
    const PublishedSkeleton s;
    const ProfileParams profile{RegtestProfile()};
    for (size_t i{0}; i < s.slots.size(); ++i) {
        const read::AccountReply& reply{s.alice_at[i]};
        CheckReplyHead(reply.head, i + 1);
        BOOST_CHECK(reply.head.envelope.header == s.slots[i].head->Header());
        BOOST_REQUIRE(reply.status == read::LookupStatus::FOUND);
        BOOST_CHECK(reply.reason.empty());
        RequireOk(verify::VerifyCertificate(reply.head.envelope.header, reply.head.envelope.cert, s.rig.seats), "certificate");
        RequireOk(verify::VerifyAccountLeaf(reply.head.envelope.header, reply.witness->leaf, reply.witness->roots, reply.witness->proof,
                                            profile),
                  "leaf");
    }
    // ARCHITECTURE.md 14.3 through verified leaves: Alice and Bob after slot 5.
    const AssetId rusd{TestAsset("rUSD")};
    const AccountLeafV0& alice{s.alice_at.back().witness->leaf};
    const AccountLeafV0& bob{s.bob_at.back().witness->leaf};
    BOOST_CHECK_EQUAL(alice.Available(rusd), 999'600'000);
    BOOST_CHECK_EQUAL(alice.Reserved(rusd), 0);
    BOOST_CHECK_EQUAL(alice.Available(AssetId::Native()), 2'000'000'000);
    BOOST_CHECK(alice.used_nonces == (std::vector<uint64_t>{1, 2, 3, 4}));
    BOOST_CHECK_EQUAL(bob.Available(AssetId::Native()), 5'000'000'000);
    BOOST_CHECK_EQUAL(bob.Reserved(AssetId::Native()), 3'000'000'000);
    BOOST_CHECK_EQUAL(bob.Available(rusd), 399'950);
    BOOST_CHECK(bob.used_nonces == (std::vector<uint64_t>{1, 2}));
    // P5: Alice's slot 2 -> 3 transition from verified leaves and her verified outcome.
    const read::ActionReply c{Take(s.service->Action(DecodeAction(s.ids.c_bytes)), "Action(c)")};
    BOOST_REQUIRE(c.status == read::ActionStatus::INCLUDED);
    RequireOk(verify::VerifyBalanceTransition(s.alice_at[1].witness->leaf, s.alice_at[2].witness->leaf,
                                              std::vector<OutcomeV2>{c.included->outcome.outcome}),
              "P5 transition");
    // Unknown account: UNKNOWN, never a fabricated leaf.
    const read::AccountReply carol{Take(s.service->Account(KeyAccount("carol")), "Account(carol)")};
    RequireUnknown(carol, read::LookupStatus::UNKNOWN, read::UNKNOWN_NO_ACCOUNT, "carol");
    BOOST_CHECK(!carol.witness.has_value());
    CheckReplyHead(carol.head, 5);
    // One handle, several replies: all from the same lease.
    const read::ReadHandle handle{Take(s.service->Acquire(), "Acquire")};
    const read::HeadReply head{Take(handle.Head(), "Head")};
    const read::AccountReply a2{Take(handle.Account(KeyAccount("alice")), "Account")};
    BOOST_CHECK(head.head.commit == handle.Reply().commit);
    BOOST_CHECK(a2.head.commit == handle.Reply().commit);
    BOOST_CHECK_EQUAL(head.head.publication, a2.head.publication);
    BOOST_CHECK_EQUAL(ToString(read::ReadLabel::CERTIFIED), "CERTIFIED");
}

BOOST_AUTO_TEST_CASE(maker_fill_lookup)
{
    const PublishedSkeleton s;
    const ProfileParams profile{RegtestProfile()};
    // a rests in slot 2 and fills against c in slot 3 only.
    RequireUnknown(s.a_fills_at[1], read::LookupStatus::UNKNOWN, read::UNKNOWN_NO_FILLS, "slot 2");
    const read::OrderFillsReply& reply{s.a_fills_at[2]};
    CheckReplyHead(reply.head, 3);
    BOOST_REQUIRE(reply.status == read::LookupStatus::FOUND);
    BOOST_REQUIRE_EQUAL(reply.groups.size(), 1U);
    const read::OrderFillGroup& group{reply.groups.front()};
    BOOST_REQUIRE_EQUAL(group.fills.size(), 1U);
    BOOST_CHECK(group.fills.front().role == read::FillRole::MAKER);
    BOOST_CHECK(group.outcome.outcome.subject == s.ids.c.id);
    RequireOk(verify::VerifyCertificate(reply.head.envelope.header, reply.head.envelope.cert, s.rig.seats), "certificate");
    RequireOk(verify::VerifyOutcome(reply.head.envelope.header, group.outcome.outcome, group.outcome.proof), "outcome");
    const Fill& fill{group.outcome.outcome.fills.at(group.fills.front().fill_index)};
    BOOST_CHECK(fill.maker == s.ids.a);
    RequireOk(verify::VerifyFill(fill, MarketM1(), profile), "fill");
    BOOST_CHECK_EQUAL(fill.quote_atoms, 500'000);
    BOOST_CHECK_EQUAL(fill.seller_fee, 50);
    // Bob's side in the taker's outcome: rUSD +499,950 available, B3 -2,000,000,000 reserved.
    int64_t bob_rusd{0};
    int64_t bob_b3_reserved{0};
    for (const BalanceDelta& d : group.outcome.outcome.deltas) {
        if (d.account != KeyAccount("bob")) continue;
        if (d.asset == TestAsset("rUSD")) bob_rusd += d.available_delta;
        if (d.asset == AssetId::Native()) bob_b3_reserved += d.reserved_delta;
    }
    BOOST_CHECK_EQUAL(bob_rusd, 499'950);
    BOOST_CHECK_EQUAL(bob_b3_reserved, -2'000'000'000);
    RequireOk(verify::VerifyBalanceTransition(s.bob_at[1].witness->leaf, s.bob_at[2].witness->leaf,
                                              std::vector<OutcomeV2>{group.outcome.outcome}),
              "Bob's maker transition");
    // The taker sees the same fill as TAKER; slot 4 has no fill of a.
    const read::OrderFillsReply taker{Take(s.service->OrderFills(s.ids.c), "OrderFills(c)")};
    RequireUnknown(taker, read::LookupStatus::UNKNOWN, read::UNKNOWN_NO_FILLS, "c at head 5");
    RequireUnknown(s.a_fills_at[3], read::LookupStatus::UNKNOWN, read::UNKNOWN_NO_FILLS, "slot 4");
    PublishedSkeleton at3{3};
    const read::OrderFillsReply c_at3{Take(at3.service->OrderFills(at3.ids.c), "OrderFills(c) at 3")};
    BOOST_REQUIRE(c_at3.status == read::LookupStatus::FOUND);
    BOOST_CHECK(c_at3.groups.front().fills.front().role == read::FillRole::TAKER);
}

BOOST_AUTO_TEST_CASE(action_included_expired_or_unknown)
{
    PublishedSkeleton s;
    const ProfileParams profile{RegtestProfile()};
    const ActionV2 w{DecodeAction(s.ids.w_bytes)};
    // w (signed at head 3: ref_slot 3, max_delay 1) is never proposed: UNKNOWN at head 4, EXPIRED at 5 (N3).
    BOOST_REQUIRE_EQUAL(s.w_at.size(), 2U);
    RequireUnknown(s.w_at[0], read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "w at head 4 (4 = 3 + 1)");
    CheckReplyHead(s.w_at[0].head, 4);
    const read::ActionReply& expired{s.w_at[1]};
    CheckReplyHead(expired.head, 5);
    BOOST_REQUIRE(expired.status == read::ActionStatus::EXPIRED);
    BOOST_REQUIRE(expired.expiry.has_value());
    BOOST_CHECK(!expired.included.has_value());
    RequireOk(verify::VerifyExpired(w, expired.head.envelope.header, expired.head.envelope.cert, s.rig.seats, expired.expiry->leaf,
                                    expired.expiry->roots, expired.expiry->proof, profile),
              "N3: VerifyExpired accepts the reply's proof");

    // c: INCLUDED in slot 3 at every later head, with a verifying outcome (N4's IncludedRef{3, i}).
    const read::ActionReply c{Take(s.service->Action(DecodeAction(s.ids.c_bytes)), "Action(c)")};
    BOOST_REQUIRE(c.status == read::ActionStatus::INCLUDED);
    BOOST_CHECK_EQUAL(c.included->ref.height, 3U);
    BOOST_CHECK(c.included->envelope.header == s.slots.at(2).head->Header());
    const auto [index, outcome] = FindOutcome(s.slots.at(2).outcomes, s.ids.c.id);
    BOOST_CHECK_EQUAL(c.included->ref.index, index);
    BOOST_CHECK(c.included->outcome.outcome == outcome);
    BOOST_CHECK(c.included->outcome.outcome.code == OutcomeCode::FILLED);
    RequireOk(verify::VerifyCertificate(c.included->envelope.header, c.included->envelope.cert, s.rig.seats), "certificate");
    RequireOk(verify::VerifyOutcome(c.included->envelope.header, c.included->outcome.outcome, c.included->outcome.proof), "outcome");
    CheckReplyHead(c.head, 5);

    // d: a certified rejection is INCLUDED too (its nonce was consumed).
    const ActionV2 d_action{DecodeAction(s.ids.d_bytes)};
    const read::ActionReply d{Take(s.service->Action(d_action), "Action(d)")};
    BOOST_REQUIRE(d.status == read::ActionStatus::INCLUDED);
    BOOST_CHECK(d.included->outcome.outcome.code == OutcomeCode::REJ_INSUFFICIENT_AVAILABLE);
    BOOST_CHECK_EQUAL(d.included->ref.height, 3U);
    // The same action under another signature has the same ActionId: INCLUDED as well.
    SlotFixture& a{s.rig.actions};
    ActionV2 d_unsigned{d_action};
    d_unsigned.sig = {};
    const ActionV2 d_resigned{DecodeAction(a.Sign(d_unsigned, "alice"))};
    BOOST_REQUIRE(d_resigned.sig != d_action.sig);
    BOOST_CHECK(Take(s.service->Action(d_resigned), "Action(d')").status == read::ActionStatus::INCLUDED);

    // Not provable: a used nonce, an absent account, another domain, a window not yet over.
    const ActionV2 used{a.Unsigned("alice", 2, ActionKind::PLACE, w.payload, 1, 1)};
    RequireUnknown(Take(s.service->Action(used), "used nonce"), read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "nonce used");
    const ActionV2 carol{a.Unsigned("carol", 1, ActionKind::PLACE, w.payload, 1, 1)};
    RequireUnknown(Take(s.service->Action(carol), "carol"), read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "no account");
    ActionV2 foreign{w};
    foreign.domain = LabelHash("fm2/test/other/domain");
    RequireUnknown(Take(s.service->Action(foreign), "foreign"), read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "other domain");
    const ActionV2 pending{a.Unsigned("alice", 6, ActionKind::PLACE, w.payload, 5, 16)};
    RequireUnknown(Take(s.service->Action(pending), "pending"), read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "window open");
    ActionV2 bad_delay{w};
    bad_delay.max_delay = 0;
    RequireUnknown(Take(s.service->Action(bad_delay), "bad delay"), read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "max_delay 0");
}

BOOST_AUTO_TEST_CASE(view_bound_evicts_the_oldest_slot)
{
    ReadRig rig;
    const auto service{NewReadService(TestReadLimits(/*view_slots=*/2))};
    Publish(*service, rig.Run(FundingFacts(rig.actions), {}));
    const read::ReadHandle at1{Take(service->Acquire(), "Acquire at 1")};
    BOOST_CHECK(Take(at1.Account(KeyAccount("alice")), "at 1").status == read::LookupStatus::FOUND);
    Publish(*service, rig.Run({}, {}));
    BOOST_CHECK_EQUAL(service->ViewCount(), 2U);
    BOOST_CHECK(Take(at1.Account(KeyAccount("alice")), "at 1 after 2").status == read::LookupStatus::FOUND);
    Publish(*service, rig.Run({}, {}));
    BOOST_CHECK_EQUAL(service->ViewCount(), 2U);
    // Slot 1's view is gone: every reply of the old lease is UNKNOWN, still naming slot 1.
    const read::AccountReply evicted{Take(at1.Account(KeyAccount("alice")), "at 1 after 3")};
    RequireUnknown(evicted, read::LookupStatus::UNKNOWN, read::UNKNOWN_VIEW_EVICTED, "evicted account");
    CheckReplyHead(evicted.head, 1);
    RequireUnknown(Take(at1.OrderFills(OrderId{}), "fills"), read::LookupStatus::UNKNOWN, read::UNKNOWN_VIEW_EVICTED, "evicted fills");
    RequireUnknown(Take(at1.Action(DecodeAction(rig.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1))), "action"),
                   read::ActionStatus::UNKNOWN, read::UNKNOWN_VIEW_EVICTED, "evicted action");
    // The current head reads normally.
    const read::AccountReply now{Take(service->Account(KeyAccount("alice")), "at 3")};
    BOOST_CHECK(now.status == read::LookupStatus::FOUND);
    CheckReplyHead(now.head, 3);

    // view_slots = 1: a lease on the previous head reads UNKNOWN as soon as the next one is published.
    ReadRig one;
    const auto single{NewReadService(TestReadLimits(/*view_slots=*/1))};
    Publish(*single, one.Run(FundingFacts(one.actions), {}));
    const read::ReadHandle old{Take(single->Acquire(), "Acquire")};
    Publish(*single, one.Run({}, {}));
    BOOST_CHECK_EQUAL(single->ViewCount(), 1U);
    RequireUnknown(Take(old.Account(KeyAccount("alice")), "old"), read::LookupStatus::UNKNOWN, read::UNKNOWN_VIEW_EVICTED, "k = 1");
    BOOST_CHECK(Take(single->Account(KeyAccount("alice")), "new").status == read::LookupStatus::FOUND);
}

BOOST_AUTO_TEST_CASE(outcome_index_keeps_exactly_its_retention)
{
    // A profile whose expiry window is 2 slots, so the retention boundary is reachable.
    ReadRigOptions options;
    options.profile.max_delay_slots = 2;
    for (const uint64_t retention : {uint64_t{2}, uint64_t{3}}) {
        ReadRig rig{options};
        read::ReadLimits limits{TestReadLimits(8, options.profile)};
        limits.outcome_index_slots = retention;
        const auto service{NewReadService(limits, options.profile)};
        RequireFailure(read::ReadService::Create(read::ReadLimits{8, limits.max_envelope_bytes, 8, 1}, options.profile),
                       ErrorCode::INVALID_INPUT, read::READ_OUTCOME_INDEX_SHORT, "retention 1 < 2");
        Publish(*service, rig.Run(FundingFacts(rig.actions), {}));
        const std::vector<unsigned char> x{rig.actions.Sign(
            rig.actions.Unsigned("alice", 1, ActionKind::PLACE,
                                 EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, 1, 1}).value(), 1, 2),
            "alice")};
        Publish(*service, rig.Run({}, {x})); // x included in slot 2
        BOOST_CHECK_EQUAL(service->IndexedActions(), 1U);
        Publish(*service, rig.Run({}, {}));
        BOOST_CHECK(Take(service->Action(DecodeAction(x)), "x at 3").status == read::ActionStatus::INCLUDED);
        Publish(*service, rig.Run({}, {}));
        const read::ActionReply at4{Take(service->Action(DecodeAction(x)), "x at 4")};
        if (retention == 2) {
            // 4 - 2 >= 2: dropped. It is not EXPIRED either: its nonce is used.
            BOOST_CHECK_EQUAL(service->IndexedActions(), 0U);
            RequireUnknown(at4, read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "pruned");
        } else {
            BOOST_CHECK_EQUAL(service->IndexedActions(), 1U);
            BOOST_CHECK(at4.status == read::ActionStatus::INCLUDED);
        }
    }
}

BOOST_AUTO_TEST_CASE(four_readers_one_publisher_see_one_head_per_reply)
{
    constexpr size_t SLOTS{2'000};
    constexpr size_t ITERATIONS{2'000};
    constexpr size_t READERS{4};
    ReadRigOptions options;
    options.real_certificates = false; // the read service never verifies certificates; speed matters here
    ReadRig rig{options};
    const AssetId rusd{TestAsset("rUSD")};
    // Slot h deposits 1 atom to Alice, so her leaf at head h holds exactly h atoms.
    std::vector<PublishableSlot> slots;
    slots.reserve(SLOTS);
    for (size_t h{1}; h <= SLOTS; ++h) {
        slots.push_back(rig.Run({rig.actions.DepositBytes(KeyAccount("alice"), rusd, 1, "tick/" + std::to_string(h))}, {}));
    }
    const auto service{NewReadService(TestReadLimits(/*view_slots=*/4))};

    std::atomic<size_t> published{0};
    std::atomic<bool> publish_failed{false};
    std::vector<std::vector<std::string>> problems(READERS);
    std::vector<size_t> found(READERS, 0), evicted(READERS, 0), empty(READERS, 0), heights_seen(READERS, 0);
    const auto check = [&](size_t r, bool ok, const std::string& what) {
        if (!ok && problems[r].size() < 10) problems[r].push_back(what);
    };
    const auto reader = [&](size_t r) {
        uint64_t last_height{0};
        for (size_t i{0}; i < ITERATIONS; ++i) {
            // Alternate: a one-lease convenience reply, and a handle with two replies.
            std::optional<read::AccountReply> account;
            std::optional<read::HeadReply> head;
            if (i % 2 == 0) {
                Result<read::AccountReply> reply{service->Account(KeyAccount("alice"))};
                if (!reply.has_value()) {
                    check(r, reply.error().code == ErrorCode::NEED_DATA, "unexpected error");
                    ++empty[r];
                    continue;
                }
                account = std::move(reply).value();
            } else {
                Result<read::ReadHandle> handle{service->Acquire()};
                if (!handle.has_value()) {
                    check(r, handle.error().code == ErrorCode::NEED_DATA, "unexpected acquire error");
                    ++empty[r];
                    continue;
                }
                account = handle->Account(KeyAccount("alice")).value();
                head = handle->Head().value();
                check(r, head->head.commit == account->head.commit && head->head.publication == account->head.publication,
                      "two replies of one handle name two heads");
            }
            const read::ReplyHead& h{account->head};
            const SlotHeaderV2& header{h.envelope.header};
            check(r, h.commit == read::CommitOf(header), "commit is not the envelope's");
            check(r, h.scope == read::ScopeOf(header), "scope is not the envelope's");
            check(r, h.envelope.cert.header_hash == h.commit.id, "certificate of another header");
            check(r, h.label == read::ReadLabel::CERTIFIED, "label");
            check(r, header.height >= last_height, "head went backwards");
            if (header.height != last_height) ++heights_seen[r];
            last_height = header.height;
            if (account->status == read::LookupStatus::FOUND) {
                ++found[r];
                const read::AccountWitness& w{*account->witness};
                check(r, w.roots.Root() == header.state_root, "roots of another head");
                check(r, MerkleVerify(w.leaf.LeafValue(), w.proof, w.roots.accounts), "proof of another tree");
                check(r, w.leaf.Available(rusd) == static_cast<Atoms>(header.height), "leaf of another head");
            } else {
                ++evicted[r];
                check(r, account->reason == read::UNKNOWN_VIEW_EVICTED, "UNKNOWN for a present account");
            }
        }
    };
    std::thread publisher{[&] {
        for (const PublishableSlot& slot : slots) {
            if (!service->Publish(slot.applied, slot.view).has_value()) publish_failed = true;
            published.fetch_add(1);
        }
    }};
    std::vector<std::thread> readers;
    for (size_t r{0}; r < READERS; ++r) readers.emplace_back(reader, r);
    publisher.join();
    for (std::thread& t : readers) t.join();

    BOOST_CHECK(!publish_failed.load());
    BOOST_CHECK_EQUAL(*service->PublishedHeight(), SLOTS);
    BOOST_CHECK_EQUAL(service->ViewCount(), 4U);
    BOOST_CHECK_EQUAL(service->Readers(), 0U);
    size_t total_found{0};
    for (size_t r{0}; r < READERS; ++r) {
        for (const std::string& p : problems[r]) BOOST_ERROR("reader " << r << ": " << p);
        BOOST_CHECK_EQUAL(found[r] + evicted[r] + empty[r], ITERATIONS);
        total_found += found[r];
    }
    BOOST_TEST_MESSAGE("found " << total_found << " of " << READERS * ITERATIONS << " replies; distinct heads per reader "
                                << heights_seen[0] << ", " << heights_seen[1] << ", " << heights_seen[2] << ", " << heights_seen[3]
                                << "; evicted " << evicted[0] + evicted[1] + evicted[2] + evicted[3]);
    BOOST_CHECK(total_found > 0);
    // After the run every reader sees the last head.
    const read::AccountReply last{Take(service->Account(KeyAccount("alice")), "final")};
    BOOST_REQUIRE(last.status == read::LookupStatus::FOUND);
    BOOST_CHECK_EQUAL(last.witness->leaf.Available(rusd), static_cast<Atoms>(SLOTS));
}

BOOST_AUTO_TEST_CASE(a_token_of_another_header_with_the_same_state_is_refused)
{
    // Two heartbeats for height 2 that differ only in their anchor: same slot,
    // same state root, different header. Only the header hash tells them apart.
    ReadRig rig;
    const auto service{NewReadService(TestReadLimits())};
    Publish(*service, rig.Run(FundingFacts(rig.actions), {}));
    const state::Candidate chosen{rig.Preview(rig.Inputs({}, {}))};
    state::SlotInputs later{rig.Inputs({}, {})};
    later.anchor = TestAnchor(131);
    const state::Candidate rival{rig.Preview(later)};
    BOOST_REQUIRE(chosen.Header().state_root == rival.Header().state_root);
    BOOST_REQUIRE(chosen.Header().Hash() != rival.Header().Hash());
    const SlotCertificate rival_cert{rig.CertificateOf(rival.Header())};
    const state::HeadView rival_head{Take(state::HeadView::Make(rival.Header(), rival_cert.Hash(), rival.Next()), "Make")};
    const auto rival_view{Take(read::SlotView::Build(rival_head, rival_cert, rival.Outcomes()), "Build rival")};
    const PublishableSlot s2{rig.Commit(chosen)};
    BOOST_REQUIRE_EQUAL(s2.applied.Slot(), rival_view->Header().height);
    BOOST_REQUIRE(s2.applied.Binding().state_root == rival_view->Header().state_root);
    RequireFailure(service->Publish(s2.applied, rival_view), ErrorCode::VERIFY_FAILED, read::READ_TOKEN_MISMATCH,
                   "same slot and state root, another header");
    BOOST_CHECK_EQUAL(*service->PublishedHeight(), 1U);
    Publish(*service, s2);
}

BOOST_AUTO_TEST_CASE(a_successor_height_on_another_chain_is_refused)
{
    // Rig B funds other accounts, so its slot 2 names another parent than rig A's slot 1.
    ReadRig a;
    ReadRig b;
    const PublishableSlot a1{a.Run(FundingFacts(a.actions), {})};
    const PublishableSlot b1{b.Run({b.actions.DepositBytes(KeyAccount("dave"), TestAsset("rUSD"), 5, "other/chain")}, {})};
    const PublishableSlot b2{b.Run({}, {})};
    BOOST_REQUIRE(b2.view->Header().parent_hash != a1.view->HeaderHash());
    const auto service{NewReadService(TestReadLimits())};
    Publish(*service, a1);
    RequireFailure(service->Publish(b2.applied, b2.view), ErrorCode::ORDER_VIOLATION, read::READ_NOT_SUCCESSOR,
                   "height 2 of another chain");
    BOOST_CHECK_EQUAL(*service->PublishedHeight(), 1U);
    BOOST_CHECK_EQUAL(service->ViewCount(), 1U);
}

BOOST_AUTO_TEST_CASE(an_old_lease_does_not_see_a_later_inclusion)
{
    ReadRig rig;
    const auto service{NewReadService(TestReadLimits())};
    Publish(*service, rig.Run(FundingFacts(rig.actions), {}));
    const read::ReadHandle at1{Take(service->Acquire(), "Acquire at 1")};
    const std::vector<unsigned char> x{rig.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::GTC, 1, 1)};
    Publish(*service, rig.Run({}, {x}));
    // The outcome index already holds x (slot 2), but the lease is at slot 1.
    BOOST_CHECK_EQUAL(service->IndexedActions(), 1U);
    RequireUnknown(Take(at1.Action(DecodeAction(x)), "x at 1"), read::ActionStatus::UNKNOWN, read::UNKNOWN_ACTION, "old lease");
    const read::ActionReply now{Take(service->Action(DecodeAction(x)), "x at 2")};
    BOOST_REQUIRE(now.status == read::ActionStatus::INCLUDED);
    BOOST_CHECK_EQUAL(now.included->ref.height, 2U);
}

BOOST_AUTO_TEST_CASE(one_group_per_outcome_with_every_fill_of_the_order)
{
    // Alice's IOC BUY of 1,500 lots takes Bob's asks at 250 (1,000 lots) and 251 (1,000 lots): two fills in one outcome.
    ReadRig rig;
    const auto service{NewReadService(TestReadLimits())};
    Publish(*service, rig.Run(FundingFacts(rig.actions), {}));
    const auto ask1{rig.actions.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 1'000)};
    const auto ask2{rig.actions.Place("bob", 2, MarketM1(), Side::SELL, TimeInForce::GTC, 251, 1'000)};
    Publish(*service, rig.Run({}, {ask1, ask2}));
    const auto taker{rig.actions.Place("alice", 1, MarketM1(), Side::BUY, TimeInForce::IOC, 260, 1'500)};
    Publish(*service, rig.Run({}, {taker}));
    const read::OrderFillsReply t{Take(service->OrderFills(rig.actions.IdOf(taker)), "taker fills")};
    BOOST_REQUIRE(t.status == read::LookupStatus::FOUND);
    BOOST_REQUIRE_EQUAL(t.groups.size(), 1U);
    BOOST_REQUIRE_EQUAL(t.groups.front().fills.size(), 2U);
    BOOST_CHECK_EQUAL(t.groups.front().fills[0].fill_index, 0U);
    BOOST_CHECK_EQUAL(t.groups.front().fills[1].fill_index, 1U);
    BOOST_CHECK(t.groups.front().fills[0].role == read::FillRole::TAKER);
    RequireOk(verify::VerifyOutcome(t.head.envelope.header, t.groups.front().outcome.outcome, t.groups.front().outcome.proof), "outcome");
    for (const auto& ask : {ask1, ask2}) {
        const read::OrderFillsReply m{Take(service->OrderFills(rig.actions.IdOf(ask)), "maker fills")};
        BOOST_REQUIRE(m.status == read::LookupStatus::FOUND);
        BOOST_REQUIRE_EQUAL(m.groups.size(), 1U);
        BOOST_REQUIRE_EQUAL(m.groups.front().fills.size(), 1U);
        BOOST_CHECK(m.groups.front().fills.front().role == read::FillRole::MAKER);
        const Fill& fill{m.groups.front().outcome.outcome.fills.at(m.groups.front().fills.front().fill_index)};
        BOOST_CHECK(fill.maker == rig.actions.IdOf(ask));
        RequireOk(verify::VerifyFill(fill, MarketM1(), RegtestProfile()), "maker fill");
    }
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The in-process client API (slice R13; net/client_api.h,
 * net/in_process_api.h).
 *
 * Submit queues or answers BUSY (not queued); Pump runs admission and
 * TakeReply hands out exactly one reply per RequestId; a cancel and a
 * status query are accepted while the ACTION lane's ordinary share is
 * full; a BUSY submit leaves no queued copy, no record and no reply;
 * REPLIES_FULL keeps requests queued; admission errors are replies too;
 * GetAction answers INCLUDED (with proofs), EXPIRED only with a proof that
 * VerifyExpired accepts from the same lease, PENDING from the pool, and
 * UNKNOWN otherwise, also while the pool and the read service disagree by
 * one publication in either direction; GetState is one lease; queued
 * status and health; four submitting threads, a pump and a publisher get
 * exactly one reply per request.
 */

#include <boost/test/unit_test.hpp>

#include <flowmesh2/admission/admission.h>
#include <flowmesh2/admission/pool.h>
#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/net/in_process_api.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/read/read_service.h>
#include <flowmesh2/test/util/api_fixtures.h>
#include <flowmesh2/verify/b3verify.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using net::ApiActionStatus;
using net::ApiReply;
using net::RequestId;

namespace {

using Frame = std::vector<unsigned char>;

ApiRigOptions RealCertificates(size_t action_messages = 20, size_t max_replies = 64)
{
    ApiRigOptions options;
    options.pool.rig.real_certificates = true;
    options.action_messages = action_messages;
    options.max_replies = max_replies;
    return options;
}

ApiRigOptions TestCertificates(size_t action_messages = 20, size_t max_replies = 64)
{
    ApiRigOptions options{RealCertificates(action_messages, max_replies)};
    options.pool.rig.real_certificates = false;
    return options;
}

//! A small PLACE of `name`: BUY M1 GTC `lots` at `price`, with an explicit window.
Frame SmallPlace(SlotFixture& a, std::string_view name, uint64_t nonce, uint64_t ref_slot, uint16_t max_delay, uint64_t price = 1)
{
    return a.Sign(a.Unsigned(name, nonce, ActionKind::PLACE,
                             EncodePayload(PlacePayload{MarketM1().Id(), Side::BUY, TimeInForce::GTC, price, 1}).value(), ref_slot,
                             max_delay),
                  name);
}

const admission::AdmissionReply& AdmissionOf(const ApiReply& reply)
{
    const auto* admitted{std::get_if<admission::AdmissionReply>(&reply.body)};
    if (admitted == nullptr) BOOST_FAIL("the reply is not an AdmissionReply");
    return *admitted;
}

void RequireBusy(const Result<RequestId>& result, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + ": queued, want BUSY");
    BOOST_CHECK_MESSAGE(result.error().code == ErrorCode::BUSY, std::string{what} + ": " + std::string{ToString(result.error().code)});
    BOOST_CHECK_EQUAL(result.error().detail, detail);
}

//! Commit one slot of `originals` after the head WITHOUT publishing it or telling the pool.
struct HeldSlot {
    PublishableSlot slot;
    SlotBody body;
};
HeldSlot CommitOnly(ApiRig& r, std::vector<Frame> facts, std::vector<Frame> originals)
{
    state::Candidate candidate{r.rig.Preview(r.rig.Inputs(std::move(facts), std::move(originals)))};
    SlotBody body{candidate.Body()};
    return HeldSlot{r.rig.Commit(std::move(candidate)), std::move(body)};
}
void TellPool(ApiRig& r, const HeldSlot& held)
{
    RequireOk(r.pool->OnCommitted(held.slot.applied, *held.slot.slot.head, held.slot.slot.outcomes, held.body), "OnCommitted");
}

/** GetAction of every action in `originals`: any EXPIRED answer must carry a
 * proof that VerifyExpired accepts under the answer's own head, and any
 * INCLUDED one an outcome that verifies under its including slot. Returns
 * how many answers were EXPIRED.
 */
size_t CheckEveryAnswer(const ApiRig& r, const std::vector<ActionV2>& originals, std::string_view what)
{
    size_t expired{0};
    for (const ActionV2& original : originals) {
        const net::ActionStatusReply reply{r.GetAction(original)};
        BOOST_CHECK(net::IsKnown(reply.status));
        if (reply.status == ApiActionStatus::EXPIRED) {
            ++expired;
            BOOST_REQUIRE_MESSAGE(reply.head.has_value() && reply.expiry.has_value(), std::string{what} + ": EXPIRED without its proof");
            RequireOk(verify::VerifyExpired(original, reply.head->envelope.header, reply.head->envelope.cert, r.rig.seats, reply.expiry->leaf,
                                            reply.expiry->roots, reply.expiry->proof, r.profile),
                      std::string{what} + ": VerifyExpired");
            BOOST_CHECK(reply.reason.empty() && !reply.included && !reply.pending);
        } else {
            BOOST_CHECK_MESSAGE(!reply.expiry.has_value(), std::string{what} + ": an expiry witness on a non-EXPIRED answer");
        }
        if (reply.status == ApiActionStatus::INCLUDED) {
            BOOST_REQUIRE(reply.included.has_value());
            if (r.rig.real_certificates) {
                RequireOk(verify::VerifyCertificate(reply.included->envelope.header, reply.included->envelope.cert, r.rig.seats),
                          std::string{what} + ": certificate");
            }
            RequireOk(verify::VerifyOutcome(reply.included->envelope.header, reply.included->outcome.outcome,
                                            reply.included->outcome.proof),
                      std::string{what} + ": outcome");
        }
        if (reply.status == ApiActionStatus::UNKNOWN) BOOST_CHECK(!reply.reason.empty());
    }
    return expired;
}

} // namespace

BOOST_AUTO_TEST_SUITE(in_process_api_tests)

BOOST_AUTO_TEST_CASE(registries_and_request_ids_are_pinned)
{
    BOOST_CHECK_EQUAL(static_cast<int>(net::RequestKind::SUBMIT_ACTION), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(net::RequestKind::ACTION_STATUS), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(net::RequestKind::HEALTH), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(ApiActionStatus::INCLUDED), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(ApiActionStatus::PENDING), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(ApiActionStatus::EXPIRED), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(ApiActionStatus::UNKNOWN), 4);
    BOOST_CHECK_EQUAL(static_cast<int>(net::RequestState::QUEUED), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(net::RequestState::PROCESSING), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(net::RequestState::REPLIED), 3);
    BOOST_CHECK_EQUAL(static_cast<int>(net::PumpStop::EMPTY), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(net::PumpStop::REPLIES_FULL), 2);
    BOOST_CHECK_EQUAL(static_cast<int>(net::PumpStop::MAX_REACHED), 3);
    BOOST_CHECK_EQUAL(net::ToString(net::RequestKind::HEALTH), "HEALTH");
    BOOST_CHECK_EQUAL(net::ToString(static_cast<net::RequestKind>(0)), "UNKNOWN");
    BOOST_CHECK_EQUAL(net::ToString(ApiActionStatus::PENDING), "PENDING");
    BOOST_CHECK(!net::IsKnown(static_cast<ApiActionStatus>(5)));
    BOOST_CHECK_EQUAL(net::ToString(net::RequestState::PROCESSING), "PROCESSING");
    for (const uint64_t value : {uint64_t{0}, uint64_t{1}, uint64_t{0x0102030405060708}, std::numeric_limits<uint64_t>::max()}) {
        const uint256 mailbox{net::ToMailboxId(RequestId{value})};
        BOOST_CHECK(net::RequestIdFromMailbox(mailbox) == RequestId{value});
        for (size_t i{8}; i < 32; ++i) BOOST_CHECK_EQUAL(mailbox.data()[i], 0);
    }
    BOOST_CHECK_EQUAL(net::ToMailboxId(RequestId{0x0102}).data()[0], 0x02);
    uint256 foreign;
    foreign.data()[8] = 1;
    BOOST_CHECK(!net::RequestIdFromMailbox(foreign).has_value());
}

BOOST_AUTO_TEST_CASE(create_refusals)
{
    ApiRig r{TestCertificates()};
    const auto refused = [&](const Result<std::unique_ptr<net::InProcessApi>>& result, ErrorCode code,
                             std::optional<std::string_view> detail) {
        BOOST_REQUIRE(!result.has_value());
        BOOST_CHECK(result.error().code == code);
        if (detail) BOOST_CHECK_EQUAL(result.error().detail, *detail);
    };
    refused(net::InProcessApi::Create(r.profile, *r.lanes, *r.admission, *r.pool, *r.read, net::ApiLimits{0}), ErrorCode::INVALID_INPUT,
            net::API_LIMIT_ZERO);
    ProfileParams other{r.profile};
    other.max_open_orders_per_account = 999;
    refused(net::InProcessApi::Create(other, *r.lanes, *r.admission, *r.pool, *r.read, net::ApiLimits{4}), ErrorCode::INVALID_INPUT,
            net::API_PROFILE_MISMATCH);
    ProfileParams mainnet{r.profile};
    mainnet.chain = ChainType::MAIN;
    refused(net::InProcessApi::Create(mainnet, *r.lanes, *r.admission, *r.pool, *r.read, net::ApiLimits{4}), ErrorCode::UNSUPPORTED_NETWORK,
            std::nullopt);
    // An ACTION lane one byte too small for the largest frame plus the request-kind byte.
    net::LaneSetLimits small{ApiLaneLimits(20, r.profile)};
    small.Of(net::Lane::ACTION) = LaneLimitsOf(20, ActionV2::MaxEncodedSize(r.profile));
    auto small_lanes{NewLaneSet(small)};
    refused(net::InProcessApi::Create(r.profile, *small_lanes, *r.admission, *r.pool, *r.read, net::ApiLimits{4}), ErrorCode::INVALID_INPUT,
            net::API_ACTION_LANE_SMALL);
}

BOOST_AUTO_TEST_CASE(submit_pump_and_take_exactly_once)
{
    ApiRig r{TestCertificates()};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const Frame order{SmallPlace(a, "alice", 1, 1, 16)};
    const RequestId id{r.Submit(order)};
    BOOST_CHECK_EQUAL(id.value, 1U);
    BOOST_CHECK(r.api->State(id) == net::RequestState::QUEUED);
    BOOST_CHECK(!r.api->TakeReply(id).has_value()); // not answered yet
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 1U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 0U); // queue acceptance is not admission
    const Result<net::PumpReport> pumped{r.api->Pump(10)};
    RequireOk(pumped, "Pump");
    BOOST_CHECK_EQUAL(pumped->answered, 1U);
    BOOST_CHECK(pumped->stop == net::PumpStop::EMPTY);
    BOOST_CHECK(r.api->State(id) == net::RequestState::REPLIED);
    std::optional<ApiReply> reply{r.api->TakeReply(id)};
    BOOST_REQUIRE(reply.has_value());
    BOOST_CHECK(reply->id == id);
    BOOST_CHECK(reply->kind == net::RequestKind::SUBMIT_ACTION);
    RequireCode(AdmissionOf(*reply), AdmissionCode::ADMITTED, "submitted PLACE");
    BOOST_CHECK(AdmissionOf(*reply).id == IdOfBytes(order));
    BOOST_CHECK_EQUAL(r.pool->Size(), 1U);
    // Exactly once: the reply is gone, and so is the request.
    BOOST_CHECK(!r.api->TakeReply(id).has_value());
    BOOST_CHECK(!r.api->State(id).has_value());
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 0U);
    // An id the API never issued has no reply.
    BOOST_CHECK(!r.api->TakeReply(RequestId{999}).has_value());
    BOOST_CHECK(!r.api->State(RequestId{0}).has_value());
    // The same bytes again: a new request id, and admission answers DUPLICATE.
    const RequestId again{r.Submit(order)};
    BOOST_CHECK_EQUAL(again.value, 2U);
    RequireCode(AdmissionOf(r.Reply(again)), AdmissionCode::DUPLICATE, "resent PLACE");
    // Pump(0) answers nothing.
    const RequestId third{r.Submit(SmallPlace(a, "alice", 2, 1, 16))};
    const Result<net::PumpReport> none{r.api->Pump(0)};
    RequireOk(none, "Pump(0)");
    BOOST_CHECK_EQUAL(none->answered, 0U);
    BOOST_CHECK(none->stop == net::PumpStop::MAX_REACHED);
    BOOST_CHECK(r.api->State(third) == net::RequestState::QUEUED);
    RequireCode(AdmissionOf(r.Reply(third)), AdmissionCode::ADMITTED, "third");
}

BOOST_AUTO_TEST_CASE(cancel_and_status_are_accepted_while_the_action_lane_is_full)
{
    // 20 messages: the reserved share is 2, so 18 ordinary submissions fill the ordinary share.
    ApiRig r{TestCertificates(20)};
    r.Fund();
    SlotFixture& a{r.Actions()};
    std::vector<RequestId> ids;
    for (uint64_t n{1}; n <= 18; ++n) ids.push_back(r.Submit(SmallPlace(a, "alice", n, 1, 16)));
    const Frame overflow{SmallPlace(a, "alice", 19, 1, 16)};
    BOOST_CHECK(r.api->ClassOf(overflow) == net::TrafficClass::ORDINARY);
    RequireBusy(r.api->Submit(overflow), net::LANE_ORDINARY_FULL, "19th PLACE");
    // A cancel and a status query still go through.
    const Frame cancel{a.Cancel("bob", 1, OrderId{uint256::ONE})};
    BOOST_CHECK(r.api->ClassOf(cancel) == net::TrafficClass::CANCEL);
    const RequestId cancel_id{r.Submit(cancel)};
    const RequestId status_id{[&] {
        Result<RequestId> id{r.api->SubmitStatus(DecodeAction(overflow))};
        RequireOk(id, "SubmitStatus");
        return *id;
    }()};
    // The lane is now full for everyone.
    const Result<RequestId> health{r.api->SubmitHealth()};
    RequireBusy(health, net::LANE_FULL, "health in a full lane");
    BOOST_CHECK((r.lanes->Stats(net::Lane::ACTION).messages == 20U));
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 20U);
    // Pumping answers all 20 in FIFO order: 18 ADMITTED, the cancel ADMITTED (its order is unknown only at execution), the status UNKNOWN.
    for (const RequestId& id : ids) RequireCode(AdmissionOf(r.Reply(id)), AdmissionCode::ADMITTED, "PLACE");
    RequireCode(AdmissionOf(r.Reply(cancel_id)), AdmissionCode::ADMITTED, "cancel");
    const ApiReply status{r.Reply(status_id)};
    BOOST_CHECK(status.kind == net::RequestKind::ACTION_STATUS);
    const auto* status_body{std::get_if<net::ActionStatusReply>(&status.body)};
    BOOST_REQUIRE(status_body != nullptr);
    BOOST_CHECK(status_body->status == ApiActionStatus::UNKNOWN); // the BUSY PLACE was never queued
    BOOST_CHECK_EQUAL(r.pool->Size(), 19U);
    BOOST_CHECK(!r.pool->Status(IdOfBytes(overflow)).has_value());
    // The same with a health probe and a status query in the reserved share.
    std::vector<RequestId> refill;
    for (uint64_t n{20}; n <= 37; ++n) refill.push_back(r.Submit(SmallPlace(a, "alice", n, 1, 16)));
    RequireBusy(r.api->Submit(SmallPlace(a, "alice", 38, 1, 16)), net::LANE_ORDINARY_FULL, "PLACE beyond the ordinary share again");
    Result<RequestId> health_again{r.api->SubmitHealth()};
    RequireOk(health_again, "health in the reserved share");
    Result<RequestId> status_again{r.api->SubmitStatus(DecodeAction(overflow))};
    RequireOk(status_again, "status in the reserved share");
    RequireBusy(r.api->Submit(a.Cancel("bob", 2, OrderId{uint256::ONE})), net::LANE_FULL, "cancel in a full lane");
    for (const RequestId& id : refill) RequireCode(AdmissionOf(r.Reply(id)), AdmissionCode::ADMITTED, "refill PLACE");
    BOOST_CHECK(std::holds_alternative<net::HealthReply>(r.Reply(*health_again).body));
    BOOST_CHECK(std::holds_alternative<net::ActionStatusReply>(r.Reply(*status_again).body));
    // A malformed frame whose kind byte says CANCEL is not a cancel: it gets the ordinary share.
    Frame fake{cancel};
    fake.push_back(0);
    BOOST_CHECK(r.api->ClassOf(fake) == net::TrafficClass::ORDINARY);
}

BOOST_AUTO_TEST_CASE(busy_leaves_no_queued_copy)
{
    ApiRig r{TestCertificates(10)};
    r.Fund();
    SlotFixture& a{r.Actions()};
    for (uint64_t n{1}; n <= 9; ++n) (void)r.Submit(SmallPlace(a, "alice", n, 1, 16));
    const net::LaneStats lane_before{r.lanes->Stats(net::Lane::ACTION)};
    const size_t open_before{r.api->OpenRequests()};
    std::vector<Frame> refused;
    for (uint64_t n{10}; n <= 14; ++n) {
        refused.push_back(SmallPlace(a, "alice", n, 1, 16));
        RequireBusy(r.api->Submit(refused.back()), net::LANE_ORDINARY_FULL, "PLACE beyond the ordinary share");
        BOOST_CHECK(r.lanes->Stats(net::Lane::ACTION) == lane_before);
        BOOST_CHECK_EQUAL(r.api->OpenRequests(), open_before);
    }
    // An oversized frame is refused before queueing too.
    Frame oversized(ActionV2::MaxEncodedSize(r.profile) + 1, 0);
    const Result<RequestId> big{r.api->Submit(oversized)};
    BOOST_REQUIRE(!big.has_value());
    BOOST_CHECK(big.error().code == ErrorCode::OVERSIZE);
    BOOST_CHECK_EQUAL(big.error().detail, net::LANE_MESSAGE_OVERSIZE);
    BOOST_CHECK(r.lanes->Stats(net::Lane::ACTION) == lane_before);
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), open_before);
    // Draining gives exactly the 9 queued requests (ids 1..9; refused attempts queued nothing).
    const Result<net::PumpReport> pumped{r.api->Pump(100)};
    RequireOk(pumped, "Pump");
    BOOST_CHECK_EQUAL(pumped->answered, 9U);
    size_t replies{0};
    for (uint64_t id{1}; id <= 20; ++id) {
        if (std::optional<ApiReply> reply{r.api->TakeReply(RequestId{id})}) {
            ++replies;
            BOOST_CHECK_LE(id, 9U);
        }
    }
    BOOST_CHECK_EQUAL(replies, 9U);
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 0U);
    BOOST_CHECK_EQUAL(r.pool->Size(), 9U);
    for (const Frame& frame : refused) BOOST_CHECK(!r.pool->Status(IdOfBytes(frame)).has_value());
    // Closed lanes: FENCED, still not queued.
    r.lanes->Close();
    const Result<RequestId> closed{r.api->Submit(refused.front())};
    BOOST_REQUIRE(!closed.has_value());
    BOOST_CHECK(closed.error().code == ErrorCode::FENCED);
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 0U);

    // Closed wins over size even when the early size check can reject input.
    const Result<RequestId> closed_oversize{r.api->Submit(oversized)};
    BOOST_REQUIRE(!closed_oversize.has_value());
    BOOST_CHECK(closed_oversize.error().code == ErrorCode::FENCED);
    BOOST_CHECK_EQUAL(closed_oversize.error().detail, net::LANE_CLOSED);
    ActionV2 closed_status;
    closed_status.payload.resize(r.profile.max_action_payload_bytes + 1);
    const Result<RequestId> closed_status_result{r.api->SubmitStatus(closed_status)};
    BOOST_REQUIRE(!closed_status_result.has_value());
    BOOST_CHECK(closed_status_result.error().code == ErrorCode::FENCED);
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 0U);

    // The largest decodable envelope fits exactly, including the request-kind
    // byte; semantic/signature refusal is an admission reply, not a lost request.
    ApiRig boundary{TestCertificates(10)};
    ActionV2 maximum;
    maximum.payload.resize(boundary.profile.max_action_payload_bytes, 0xab);
    const Frame at_limit{maximum.Encode()};
    BOOST_REQUIRE_EQUAL(at_limit.size(), ActionV2::MaxEncodedSize(boundary.profile));
    RequireOk(ActionV2::Decode(at_limit, boundary.profile), "maximum envelope decodes");
    const RequestId maximum_id{boundary.Submit(at_limit)};
    BOOST_CHECK_EQUAL(maximum_id.value, 1U);
    BOOST_CHECK(boundary.api->State(maximum_id) == net::RequestState::QUEUED);
    BOOST_CHECK_EQUAL(boundary.lanes->Stats(net::Lane::ACTION).payload_bytes, at_limit.size() + 1);
    BOOST_CHECK_EQUAL(boundary.api->OpenRequests(), 1U);
    BOOST_CHECK(boundary.Reply(maximum_id).id == maximum_id);
    BOOST_CHECK(!boundary.api->TakeReply(maximum_id).has_value());
    BOOST_CHECK(!boundary.api->State(maximum_id).has_value());
    BOOST_CHECK(boundary.lanes->Stats(net::Lane::ACTION) == net::LaneStats{});

    // Size refusals allocate no request identity or queued state, even when a
    // custom lane is larger than the protocol's action cap.
    Frame above_limit{at_limit};
    above_limit.push_back(0);
    const auto check_oversize = [&](const Result<RequestId>& result) {
        BOOST_REQUIRE(!result.has_value());
        BOOST_CHECK(result.error().code == ErrorCode::OVERSIZE);
        BOOST_CHECK_EQUAL(result.error().detail, net::LANE_MESSAGE_OVERSIZE);
        BOOST_CHECK_EQUAL(boundary.api->OpenRequests(), 0U);
        BOOST_CHECK(boundary.lanes->Stats(net::Lane::ACTION) == net::LaneStats{});
        BOOST_CHECK_EQUAL(boundary.lanes->MailboxStatsOf(net::Lane::ACTION).messages, 0U);
        BOOST_CHECK_EQUAL(boundary.lanes->MailboxStatsOf(net::Lane::ACTION).payload_bytes, 0U);
    };
    check_oversize(boundary.api->Submit(above_limit));
    maximum.payload.push_back(0);
    check_oversize(boundary.api->SubmitStatus(maximum));
    net::LaneSetLimits larger_limits{ApiLaneLimits(10, boundary.profile)};
    larger_limits.Of(net::Lane::ACTION) = LaneLimitsOf(10, 2 * at_limit.size());
    auto large_lanes{NewLaneSet(larger_limits)};
    Result<std::unique_ptr<net::InProcessApi>> larger{net::InProcessApi::Create(
        boundary.profile, *large_lanes, *boundary.admission, *boundary.pool, *boundary.read, net::ApiLimits{4})};
    RequireOk(larger, "API with a larger lane");
    const Result<RequestId> still_over_cap{(*larger)->Submit(above_limit)};
    BOOST_REQUIRE(!still_over_cap.has_value());
    BOOST_CHECK(still_over_cap.error().code == ErrorCode::OVERSIZE);
    BOOST_CHECK_EQUAL((*larger)->OpenRequests(), 0U);
    BOOST_CHECK(large_lanes->Stats(net::Lane::ACTION) == net::LaneStats{});
    const Result<RequestId> after_refusals{boundary.api->SubmitHealth()};
    RequireOk(after_refusals, "health after size refusals");
    BOOST_CHECK_EQUAL(after_refusals->value, maximum_id.value + 1);
    BOOST_CHECK(std::holds_alternative<net::HealthReply>(boundary.Reply(*after_refusals).body));
    BOOST_CHECK_EQUAL(boundary.api->OpenRequests(), 0U);
}

BOOST_AUTO_TEST_CASE(replies_full_stops_the_pump_and_keeps_requests_queued)
{
    ApiRig r{TestCertificates(20, /*max_replies=*/2)};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const RequestId first{r.Submit(SmallPlace(a, "alice", 1, 1, 16))};
    const RequestId second{r.Submit(SmallPlace(a, "alice", 2, 1, 16))};
    const RequestId third{r.Submit(SmallPlace(a, "alice", 3, 1, 16))};
    const Result<net::PumpReport> pumped{r.api->Pump(10)};
    RequireOk(pumped, "Pump");
    BOOST_CHECK_EQUAL(pumped->answered, 2U);
    BOOST_CHECK(pumped->stop == net::PumpStop::REPLIES_FULL);
    BOOST_CHECK(r.api->State(third) == net::RequestState::QUEUED);
    BOOST_CHECK_EQUAL(r.lanes->Stats(net::Lane::ACTION).messages, 1U);
    // Taking one reply makes room for the third.
    BOOST_REQUIRE(r.api->TakeReply(first).has_value());
    const Result<net::PumpReport> again{r.api->Pump(10)};
    RequireOk(again, "Pump again");
    BOOST_CHECK_EQUAL(again->answered, 1U);
    BOOST_CHECK(again->stop == net::PumpStop::REPLIES_FULL);
    BOOST_REQUIRE(r.api->TakeReply(second).has_value());
    BOOST_REQUIRE(r.api->TakeReply(third).has_value());
    const Result<net::PumpReport> empty{r.api->Pump(10)};
    RequireOk(empty, "Pump empty");
    BOOST_CHECK(empty->stop == net::PumpStop::EMPTY);
    // Pumping an empty lane gives its reply reservation back: after several
    // empty pumps two new requests are still answered by one Pump.
    for (int i{0}; i < 5; ++i) RequireOk(r.api->Pump(10), "Pump an empty lane");
    const RequestId fourth{r.Submit(SmallPlace(a, "alice", 4, 1, 16))};
    const RequestId fifth{r.Submit(SmallPlace(a, "alice", 5, 1, 16))};
    const Result<net::PumpReport> both{r.api->Pump(10)};
    RequireOk(both, "Pump both");
    BOOST_CHECK_EQUAL(both->answered, 2U);
    BOOST_CHECK(r.api->TakeReply(fourth).has_value());
    BOOST_CHECK(r.api->TakeReply(fifth).has_value());
}

BOOST_AUTO_TEST_CASE(admission_errors_are_replies_too)
{
    ApiRig r{TestCertificates()};
    r.Fund();
    r.budget.fail = true;
    const RequestId id{r.Submit(SmallPlace(r.Actions(), "alice", 1, 1, 16))};
    const ApiReply reply{r.Reply(id)};
    const auto* error{std::get_if<Error>(&reply.body)};
    BOOST_REQUIRE(error != nullptr);
    BOOST_CHECK(error->code == ErrorCode::INVARIANT);
    BOOST_CHECK_EQUAL(r.Stats().errors, 1U);
    BOOST_CHECK(!r.api->TakeReply(id).has_value());
    // A status query of bytes that do not decode cannot be queued: SubmitStatus takes a decoded action.
    // A fenced admission answers HALTED through the API.
    r.budget.fail = false;
    r.admission->Fence();
    RequireCode(r.AdmitViaApi(SmallPlace(r.Actions(), "alice", 2, 1, 16)), AdmissionCode::HALTED, "fenced");
}

BOOST_AUTO_TEST_CASE(get_action_included_pending_expired_unknown)
{
    ApiRig r{RealCertificates()};
    SlotFixture& a{r.Actions()};
    std::vector<ActionV2> watched;
    size_t expired_answers{0};
    const auto sweep = [&](std::string_view what) { expired_answers += CheckEveryAnswer(r, watched, what); };

    // Slot 1 (funding) is committed and the pool knows it, but nothing is published yet.
    const HeldSlot slot1{CommitOnly(r, FundingFacts(a), {})};
    TellPool(r, slot1);
    const Frame never{SmallPlace(a, "carol", 1, 1, 16)};
    watched.push_back(DecodeAction(never));
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(never))};
        BOOST_CHECK(reply.status == ApiActionStatus::UNKNOWN);
        BOOST_CHECK_EQUAL(reply.reason, net::API_UNKNOWN_NOTHING_PUBLISHED);
        BOOST_CHECK(!reply.head.has_value());
    }
    // a is admitted: PENDING, without a head while nothing is published.
    const Frame order_a{a.Place("bob", 1, MarketM1(), Side::SELL, TimeInForce::GTC, 250, 5'000)};
    watched.push_back(DecodeAction(order_a));
    RequireCode(r.AdmitViaApi(order_a), AdmissionCode::ADMITTED, "a");
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(order_a))};
        BOOST_CHECK(reply.status == ApiActionStatus::PENDING);
        BOOST_CHECK(!reply.head.has_value());
        BOOST_REQUIRE(reply.pending.has_value());
        BOOST_CHECK(reply.pending->status == admission::PoolStatus::PENDING);
        BOOST_CHECK(reply.reason.empty());
    }
    Publish(*r.read, slot1.slot);
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(order_a))};
        BOOST_CHECK(reply.status == ApiActionStatus::PENDING);
        BOOST_REQUIRE(reply.head.has_value());
        BOOST_CHECK_EQUAL(reply.head->commit.sequence, 1U);
        BOOST_CHECK(reply.head->label == read::ReadLabel::CERTIFIED);
    }
    sweep("after slot 1");

    // Slot 2 includes a: INCLUDED with an outcome that verifies under slot 2's certificate.
    r.Seal();
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(order_a))};
        BOOST_CHECK(reply.status == ApiActionStatus::INCLUDED);
        BOOST_REQUIRE(reply.included.has_value());
        BOOST_CHECK_EQUAL(reply.included->ref.height, 2U);
        BOOST_CHECK(reply.included->outcome.outcome.code == OutcomeCode::RESTING);
        BOOST_CHECK(!reply.pending.has_value());
    }
    sweep("after slot 2");

    // w: alice, ref_slot 2, max_delay 1; withheld from slot 3, so PENDING at head 3.
    const Frame w{SmallPlace(a, "alice", 1, 2, 1)};
    const ActionId w_id{IdOfBytes(w)};
    watched.push_back(DecodeAction(w));
    RequireCode(r.AdmitViaApi(w), AdmissionCode::ADMITTED, "w");
    r.Seal({}, [&](const ActionV2& action) { return action.Id() == w_id; });
    BOOST_CHECK(r.GetAction(DecodeAction(w)).status == ApiActionStatus::PENDING);
    sweep("after slot 3");

    // Slot 4 is committed and the pool learns of it first: the pool makes w
    // terminal EXPIRED, but the read lease (head 3) proves nothing: UNKNOWN,
    // never EXPIRED from the pool alone.
    const HeldSlot slot4{CommitOnly(r, {}, {})};
    TellPool(r, slot4);
    BOOST_CHECK(RequireStatus(*r.pool, w_id).status == admission::PoolStatus::EXPIRED);
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(w))};
        BOOST_CHECK(reply.status == ApiActionStatus::UNKNOWN);
        BOOST_CHECK_EQUAL(reply.reason, net::API_UNKNOWN_TERMINAL_UNPROVEN);
        BOOST_REQUIRE(reply.head.has_value());
        BOOST_CHECK_EQUAL(reply.head->commit.sequence, 3U);
    }
    sweep("slot 4 committed, not published");
    // Once slot 4 is published, its lease proves the expiry.
    Publish(*r.read, slot4.slot);
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(w))};
        BOOST_CHECK(reply.status == ApiActionStatus::EXPIRED);
        BOOST_REQUIRE(reply.expiry.has_value());
        BOOST_CHECK_EQUAL(reply.head->commit.sequence, 4U);
    }
    sweep("slot 4 published");

    // The other direction: w2 (ref_slot 4, max_delay 1) is withheld from slot
    // 5; slot 6 is published before the pool hears of it, so the lease proves
    // the expiry while the pool still holds w2 live: EXPIRED (the proof is final).
    const Frame w2{SmallPlace(a, "alice", 2, 4, 1)};
    const ActionId w2_id{IdOfBytes(w2)};
    watched.push_back(DecodeAction(w2));
    RequireCode(r.AdmitViaApi(w2), AdmissionCode::ADMITTED, "w2");
    r.Seal({}, [&](const ActionV2& action) { return action.Id() == w2_id; });
    BOOST_CHECK(r.GetAction(DecodeAction(w2)).status == ApiActionStatus::PENDING);
    const HeldSlot slot6{CommitOnly(r, {}, {})};
    Publish(*r.read, slot6.slot);
    BOOST_CHECK(RequireStatus(*r.pool, w2_id).status == admission::PoolStatus::PENDING);
    BOOST_CHECK(r.GetAction(DecodeAction(w2)).status == ApiActionStatus::EXPIRED);
    sweep("slot 6 published, the pool not told");
    TellPool(r, slot6);
    BOOST_CHECK(r.GetAction(DecodeAction(w2)).status == ApiActionStatus::EXPIRED);

    // x1 is pending; another proposal includes x2 with the same nonce: the
    // pool makes x1 terminal NONCE_USED, which proves nothing to a client.
    const Frame x1{SmallPlace(a, "alice", 3, 6, 16, 2)};
    const Frame x2{SmallPlace(a, "alice", 3, 6, 16, 3)};
    watched.push_back(DecodeAction(x1));
    watched.push_back(DecodeAction(x2));
    RequireCode(r.AdmitViaApi(x1), AdmissionCode::ADMITTED, "x1");
    r.SealOriginals({}, {x2});
    BOOST_CHECK(RequireStatus(*r.pool, IdOfBytes(x1)).status == admission::PoolStatus::NONCE_USED);
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(x1))};
        BOOST_CHECK(reply.status == ApiActionStatus::UNKNOWN);
        BOOST_CHECK_EQUAL(reply.reason, net::API_UNKNOWN_TERMINAL_UNPROVEN);
    }
    BOOST_CHECK(r.GetAction(DecodeAction(x2)).status == ApiActionStatus::INCLUDED);
    sweep("after slot 7");

    // y is included in slot 8, and the pool hears of it before the read
    // service publishes it: UNKNOWN (included after the lease), then INCLUDED.
    const Frame y{SmallPlace(a, "alice", 4, 7, 16)};
    watched.push_back(DecodeAction(y));
    RequireCode(r.AdmitViaApi(y), AdmissionCode::ADMITTED, "y");
    Result<admission::TakenSlot> taken{r.pool->TakeForSlot(r.Height() + 1, 16)};
    RequireOk(taken, "TakeForSlot");
    {
        // Taken for slot 8 but not committed: still PENDING, as IN_PROPOSAL.
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(y))};
        BOOST_CHECK(reply.status == ApiActionStatus::PENDING);
        BOOST_REQUIRE(reply.pending.has_value());
        BOOST_CHECK(reply.pending->status == admission::PoolStatus::IN_PROPOSAL);
        BOOST_CHECK_EQUAL(reply.pending->height, 8U);
    }
    const HeldSlot slot8{CommitOnly(r, {}, taken->originals)};
    TellPool(r, slot8);
    {
        const net::ActionStatusReply reply{r.GetAction(DecodeAction(y))};
        BOOST_CHECK(reply.status == ApiActionStatus::UNKNOWN);
        BOOST_CHECK_EQUAL(reply.reason, net::API_UNKNOWN_INCLUDED_AFTER_LEASE);
    }
    sweep("slot 8 committed, not published");
    Publish(*r.read, slot8.slot);
    BOOST_CHECK(r.GetAction(DecodeAction(y)).status == ApiActionStatus::INCLUDED);

    // Actions that can never be proven expired here: another domain, and max_delay 0.
    ActionV2 foreign{a.Unsigned("alice", 9, ActionKind::PLACE, {}, 1, 1)};
    foreign.domain = LabelHash("fm2/test/api/other-domain");
    watched.push_back(foreign);
    watched.push_back(a.Unsigned("alice", 10, ActionKind::PLACE, {}, 1, 0));
    // An action of an account that does not exist.
    watched.push_back(a.Unsigned("dave", 1, ActionKind::PLACE, {}, 1, 1));
    sweep("final");
    for (size_t i{watched.size() - 3}; i < watched.size(); ++i) {
        const net::ActionStatusReply reply{r.GetAction(watched[i])};
        BOOST_CHECK(reply.status == ApiActionStatus::UNKNOWN);
    }
    BOOST_CHECK_GE(expired_answers, 5U); // w after slots 4..8 and w2 after slots 6..8, all verified
}

BOOST_AUTO_TEST_CASE(get_state_is_one_lease)
{
    ApiRig r{RealCertificates()};
    {
        const Result<read::AccountReply> before{r.api->GetState(KeyAccount("alice"))};
        BOOST_REQUIRE(!before.has_value());
        BOOST_CHECK(before.error().code == ErrorCode::NEED_DATA);
    }
    r.Fund();
    const Result<read::AccountReply> state{r.api->GetState(KeyAccount("alice"))};
    RequireOk(state, "GetState");
    BOOST_REQUIRE(state->status == read::LookupStatus::FOUND);
    BOOST_REQUIRE(state->witness.has_value());
    RequireOk(verify::VerifyCertificate(state->head.envelope.header, state->head.envelope.cert, r.rig.seats), "certificate");
    RequireOk(verify::VerifyAccountLeaf(state->head.envelope.header, state->witness->leaf, state->witness->roots, state->witness->proof,
                                        r.profile),
              "leaf");
    BOOST_CHECK(state->head.commit.id == state->head.envelope.header.Hash());
    const Result<read::AccountReply> absent{r.api->GetState(KeyAccount("dave"))};
    RequireOk(absent, "GetState(dave)");
    BOOST_CHECK(absent->status == read::LookupStatus::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(queued_status_and_health)
{
    ApiRig r{TestCertificates()};
    r.Fund();
    SlotFixture& a{r.Actions()};
    const Frame order{SmallPlace(a, "alice", 1, 1, 16)};
    RequireCode(r.AdmitViaApi(order), AdmissionCode::ADMITTED, "order");
    Result<RequestId> status_id{r.api->SubmitStatus(DecodeAction(order))};
    RequireOk(status_id, "SubmitStatus");
    const ApiReply status{r.Reply(*status_id)};
    BOOST_CHECK(status.kind == net::RequestKind::ACTION_STATUS);
    const auto* body{std::get_if<net::ActionStatusReply>(&status.body)};
    BOOST_REQUIRE(body != nullptr);
    BOOST_CHECK(body->status == ApiActionStatus::PENDING);
    BOOST_CHECK(body->status == r.GetAction(DecodeAction(order)).status);

    Result<RequestId> health_id{r.api->SubmitHealth()};
    RequireOk(health_id, "SubmitHealth");
    const ApiReply health{r.Reply(*health_id)};
    BOOST_CHECK(health.kind == net::RequestKind::HEALTH);
    const auto* h{std::get_if<net::HealthReply>(&health.body)};
    BOOST_REQUIRE(h != nullptr);
    BOOST_CHECK(!h->fenced);
    BOOST_CHECK(h->published_height == std::optional<uint64_t>{1});
    BOOST_CHECK_EQUAL(h->pool_height, 1U);
    BOOST_CHECK_EQUAL(h->pool_live, 1U);
    BOOST_CHECK_EQUAL(h->action_lane.messages, 0U);
    // With two requests queued behind it, the health answer reports them.
    const RequestId probe{r.api->SubmitHealth().value()};
    const RequestId later1{r.Submit(SmallPlace(a, "alice", 2, 1, 16))};
    const RequestId later2{r.Submit(SmallPlace(a, "alice", 3, 1, 16))};
    RequireOk(r.api->Pump(1), "Pump the health probe");
    const std::optional<ApiReply> probed{r.api->TakeReply(probe)};
    BOOST_REQUIRE(probed.has_value());
    const net::LaneStats& lane{std::get<net::HealthReply>(probed->body).action_lane};
    BOOST_CHECK_EQUAL(lane.messages, 2U);
    BOOST_CHECK_EQUAL(lane.ordinary_messages, 2U);
    BOOST_CHECK_GT(lane.payload_bytes, 0U);
    RequireCode(AdmissionOf(r.Reply(later1)), AdmissionCode::ADMITTED, "later 1");
    RequireCode(AdmissionOf(r.Reply(later2)), AdmissionCode::ADMITTED, "later 2");
    r.admission->Fence();
    const ApiReply fenced{r.Reply(r.api->SubmitHealth().value())};
    BOOST_CHECK(std::get<net::HealthReply>(fenced.body).fenced);
}

BOOST_AUTO_TEST_CASE(a_busy_read_service_is_busy)
{
    ApiRig r{TestCertificates()};
    r.Fund();
    const Frame order{SmallPlace(r.Actions(), "alice", 1, 1, 16)};
    RequireCode(r.AdmitViaApi(order), AdmissionCode::ADMITTED, "order");
    std::vector<read::ReadHandle> held;
    while (held.size() <= r.read->Limits().max_readers) {
        Result<read::ReadHandle> handle{r.read->Acquire()};
        if (!handle.has_value()) break;
        held.push_back(std::move(handle).value());
    }
    BOOST_CHECK_EQUAL(held.size(), r.read->Limits().max_readers);
    // BUSY, never a PENDING or UNKNOWN guess: the lease that would answer INCLUDED or EXPIRED is not available.
    const Result<net::ActionStatusReply> busy{r.api->GetAction(DecodeAction(order))};
    BOOST_REQUIRE(!busy.has_value());
    BOOST_CHECK(busy.error().code == ErrorCode::BUSY);
    const Result<read::AccountReply> state{r.api->GetState(KeyAccount("alice"))};
    BOOST_REQUIRE(!state.has_value());
    BOOST_CHECK(state.error().code == ErrorCode::BUSY);
    // A queued status query gets that error as its one reply.
    const ApiReply queued{r.Reply(r.api->SubmitStatus(DecodeAction(order)).value())};
    const auto* error{std::get_if<Error>(&queued.body)};
    BOOST_REQUIRE(error != nullptr);
    BOOST_CHECK(error->code == ErrorCode::BUSY);
    held.clear();
    BOOST_CHECK(r.GetAction(DecodeAction(order)).status == ApiActionStatus::PENDING);
}

BOOST_AUTO_TEST_CASE(foreign_message_on_the_action_lane_is_an_invariant)
{
    ApiRig r{TestCertificates()};
    const std::vector<unsigned char> payload{1};
    RequireOk(r.lanes->TryPush(net::Lane::ACTION, net::TrafficClass::ORDINARY, net::ToMailboxId(RequestId{77}), payload), "foreign push");
    const Result<net::PumpReport> pumped{r.api->Pump(1)};
    BOOST_REQUIRE(!pumped.has_value());
    BOOST_CHECK(pumped.error().code == ErrorCode::INVARIANT);
    BOOST_CHECK_EQUAL(pumped.error().detail, net::API_FOREIGN_REQUEST);
    // The reservation was given back: the pump still works.
    const RequestId id{r.Submit(SmallPlace(r.Actions(), "alice", 1, 1, 16))};
    RequireCode(AdmissionOf(r.Reply(id)), AdmissionCode::NO_ACCOUNT, "after the foreign message");
}

BOOST_AUTO_TEST_CASE(four_submitters_a_pump_and_a_publisher_get_one_reply_per_request)
{
    // Small lanes and few reply places, so BUSY and REPLIES_FULL both happen.
    ApiRig r{TestCertificates(/*action_messages=*/20, /*max_replies=*/16)};
    r.Fund();
    SlotFixture& a{r.Actions()};
    enum class Kind { SUBMIT, STATUS, HEALTH };
    struct Request {
        Kind kind;
        Frame frame;
        ActionV2 action;
    };
    // Signed in advance: signing and the fixtures' Boost checks stay on this thread.
    constexpr int THREADS{4};
    const std::vector<std::string> names{"alice", "bob", "carol", "alice"};
    std::vector<std::vector<Request>> plans(THREADS);
    for (int t{0}; t < THREADS; ++t) {
        const uint64_t base{t == 3 ? 40U : 0U};
        for (uint64_t n{1}; n <= 24; ++n) {
            const Frame frame{SmallPlace(a, names[t], base + n, 1, 1'000)};
            plans[t].push_back(Request{Kind::SUBMIT, frame, DecodeAction(frame)});
            if (n % 6 == 0) plans[t].push_back(Request{Kind::SUBMIT, frame, DecodeAction(frame)}); // a resend
            if (n % 5 == 0) plans[t].push_back(Request{Kind::STATUS, frame, DecodeAction(frame)});
            if (n % 8 == 0) {
                const Frame cancel{a.Cancel(names[t], base + 30 + n / 8, OrderId{uint256::ONE})};
                plans[t].push_back(Request{Kind::SUBMIT, cancel, DecodeAction(cancel)});
                plans[t].push_back(Request{Kind::HEALTH, {}, {}});
            }
        }
    }
    struct Outcome {
        std::vector<RequestId> ids;
        std::vector<ApiReply> replies;
        size_t busy{0};
        size_t double_takes{0};
        size_t unexpected_errors{0};
        bool timed_out{false};
    };
    std::vector<Outcome> outcomes(THREADS);
    std::atomic<bool> stop{false};
    std::atomic<size_t> pump_errors{0};
    std::atomic<size_t> pumped{0};
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{120}};

    std::vector<std::thread> submitters;
    for (int t{0}; t < THREADS; ++t) {
        submitters.emplace_back([&, t] {
            Outcome& out{outcomes[t]};
            std::set<RequestId> outstanding;
            const auto take_some = [&] {
                for (auto it{outstanding.begin()}; it != outstanding.end();) {
                    if (std::optional<ApiReply> reply{r.api->TakeReply(*it)}) {
                        if (r.api->TakeReply(*it).has_value()) ++out.double_takes;
                        out.replies.push_back(std::move(*reply));
                        it = outstanding.erase(it);
                    } else {
                        ++it;
                    }
                }
            };
            for (const Request& request : plans[t]) {
                while (true) {
                    Result<RequestId> id{request.kind == Kind::SUBMIT   ? r.api->Submit(request.frame)
                                         : request.kind == Kind::STATUS ? r.api->SubmitStatus(request.action)
                                                                        : r.api->SubmitHealth()};
                    if (id.has_value()) {
                        out.ids.push_back(*id);
                        outstanding.insert(*id);
                        break;
                    }
                    if (id.error().code != ErrorCode::BUSY) {
                        ++out.unexpected_errors;
                        break;
                    }
                    ++out.busy;
                    take_some();
                    if (std::chrono::steady_clock::now() > deadline) {
                        out.timed_out = true;
                        return;
                    }
                    std::this_thread::yield();
                }
            }
            while (!outstanding.empty()) {
                take_some();
                if (std::chrono::steady_clock::now() > deadline) {
                    out.timed_out = true;
                    return;
                }
                std::this_thread::yield();
            }
        });
    }
    std::thread pump{[&] {
        while (!stop.load()) {
            const Result<net::PumpReport> report{r.api->Pump(4)};
            if (!report.has_value()) {
                ++pump_errors;
                return;
            }
            pumped += report->answered;
            if (report->answered == 0) std::this_thread::yield();
        }
    }};
    // The publisher seals slots while the others run (this thread is the only one using the fixtures' Boost checks).
    std::thread publisher{[&] {
        for (int slot{0}; slot < 12; ++slot) {
            r.Seal();
            std::this_thread::yield();
        }
    }};
    for (std::thread& thread : submitters) thread.join();
    publisher.join();
    stop = true;
    pump.join();

    BOOST_CHECK_EQUAL(pump_errors.load(), 0U);
    std::set<RequestId> all_ids;
    size_t total_ids{0};
    size_t total_busy{0};
    size_t submits{0};
    size_t planned{0};
    for (int t{0}; t < THREADS; ++t) {
        const Outcome& out{outcomes[t]};
        BOOST_CHECK(!out.timed_out);
        BOOST_CHECK_EQUAL(out.unexpected_errors, 0U);
        BOOST_CHECK_EQUAL(out.double_takes, 0U);
        planned += plans[t].size();
        // Every id this thread queued got exactly one reply, carrying its id.
        BOOST_CHECK_EQUAL(out.ids.size(), plans[t].size());
        BOOST_CHECK_EQUAL(out.replies.size(), out.ids.size());
        std::multiset<RequestId> replied;
        for (const ApiReply& reply : out.replies) {
            replied.insert(reply.id);
            if (reply.kind == net::RequestKind::SUBMIT_ACTION) {
                ++submits;
                BOOST_CHECK(std::holds_alternative<admission::AdmissionReply>(reply.body));
            } else if (reply.kind == net::RequestKind::ACTION_STATUS) {
                const auto* status{std::get_if<net::ActionStatusReply>(&reply.body)};
                BOOST_CHECK(status != nullptr && status->status != ApiActionStatus::EXPIRED);
            } else {
                BOOST_CHECK(std::holds_alternative<net::HealthReply>(reply.body));
            }
        }
        BOOST_CHECK((std::set<RequestId>{out.ids.begin(), out.ids.end()} == std::set<RequestId>{replied.begin(), replied.end()}));
        BOOST_CHECK_EQUAL(replied.size(), out.ids.size());
        all_ids.insert(out.ids.begin(), out.ids.end());
        total_ids += out.ids.size();
        total_busy += out.busy;
    }
    // Ids are unique across threads, and nothing is left behind.
    BOOST_CHECK_EQUAL(all_ids.size(), total_ids);
    BOOST_CHECK_EQUAL(total_ids, planned);
    BOOST_CHECK_EQUAL(pumped.load(), total_ids);
    BOOST_CHECK_EQUAL(r.api->OpenRequests(), 0U);
    BOOST_CHECK(r.lanes->Stats(net::Lane::ACTION) == net::LaneStats{});
    // Admission saw exactly the submitted actions, one reply each.
    const admission::AdmissionStats stats{r.Stats()};
    BOOST_CHECK_EQUAL(stats.replies + stats.errors, submits);
    BOOST_CHECK_EQUAL(stats.errors, 0U);
    BOOST_TEST_MESSAGE("requests " << total_ids << ", BUSY retries " << total_busy << ", slots " << r.Height());
}

BOOST_AUTO_TEST_SUITE_END()

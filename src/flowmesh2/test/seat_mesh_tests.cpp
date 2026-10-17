// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/** The seat mesh interface (slice R13; net/seat_mesh.h).
 *
 * A durable vote leaves only through SendVote: it is encoded as the
 * agreement VOTE message of its VoteKey, carries this seat's index and its
 * BLS signature over the digest, and goes on CRITICAL; Send refuses a vote
 * on CRITICAL and PROPOSAL and any frame that is not its lane's agreement
 * message, and passes ACTION and BULK frames as opaque bytes; the
 * MessageSink port routes by message type; route, size and transport
 * refusals transmit nothing; Poll hands out received frames.
 */

#include <boost/test/unit_test.hpp>

#include <crypto/bls.h>
#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/seat_mesh.h>
#include <flowmesh2/test/util/mesh_fixtures.h>

#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;
using net::Lane;

namespace {

using Frame = std::vector<unsigned char>;

Frame Encoded(const agreement::Message& message)
{
    Result<Frame> bytes{agreement::EncodeMessage(message, RegtestProfile())};
    RequireOk(bytes, "EncodeMessage");
    return std::move(bytes).value();
}

Frame ProposalFrame()
{
    const StorageSlot slot{MakeStorageSlot(4, 0, "proposal")};
    return Encoded(agreement::Proposal{0, 0, slot.header, std::nullopt, slot.body});
}

Frame VoteFrame()
{
    agreement::Vote vote;
    vote.slot = 4;
    vote.seat = 1;
    return Encoded(vote);
}

Frame ViewChangeFrame() { return Encoded(agreement::ViewChange{0, 4, 1, 0, std::nullopt, {}}); }
Frame NewViewFrame() { return Encoded(agreement::NewView{0, 4, 1, 0, uint256{}, {agreement::ViewChange{0, 4, 1, 0, std::nullopt, {}}}, {}}); }
Frame EvidenceFrame()
{
    agreement::Vote first;
    first.slot = 4;
    agreement::Vote second{first};
    second.header_hash = uint256::ONE;
    return Encoded(agreement::Evidence{agreement::DoubleVote{first, second}});
}

void RequireRefused(const Result<void>& result, ErrorCode code, std::string_view detail, std::string_view what)
{
    if (result.has_value()) BOOST_FAIL(std::string{what} + ": sent, want a refusal");
    BOOST_CHECK_MESSAGE(result.error().code == code, std::string{what} + ": code " + std::string{ToString(result.error().code)});
    BOOST_CHECK_MESSAGE(result.error().detail == detail, std::string{what} + ": detail " + std::string{result.error().detail});
}

} // namespace

// The base class is abstract: a transport supplies Transmit and Poll, never the send rules.
static_assert(std::is_abstract_v<net::SeatMesh>);
static_assert(std::is_base_of_v<agreement::MessageSink, net::SeatMesh>);

BOOST_AUTO_TEST_SUITE(seat_mesh_tests)

BOOST_AUTO_TEST_CASE(config_refusals)
{
    RequireOk(net::SeatMesh::CheckConfig(TestMeshConfig(0, 1)), "one seat");
    RequireOk(net::SeatMesh::CheckConfig(TestMeshConfig(3, 4)), "seat 3 of 4");
    const auto refused = [](const net::MeshConfig& config) {
        const Result<void> checked{net::SeatMesh::CheckConfig(config)};
        BOOST_REQUIRE(!checked.has_value());
        BOOST_CHECK(checked.error().code == ErrorCode::INVALID_INPUT);
        BOOST_CHECK_EQUAL(checked.error().detail, net::MESH_CONFIG_INVALID);
    };
    refused(TestMeshConfig(0, 0));
    refused(TestMeshConfig(4, 4));
    net::MeshConfig no_frames{TestMeshConfig(0, 4)};
    no_frames.max_frame_bytes = 0;
    refused(no_frames);
    net::MeshConfig mainnet{TestMeshConfig(0, 4)};
    mainnet.profile.chain = ChainType::MAIN;
    const Result<void> network{net::SeatMesh::CheckConfig(mainnet)};
    BOOST_REQUIRE(!network.has_value());
    BOOST_CHECK(network.error().code == ErrorCode::UNSUPPORTED_NETWORK);
}

BOOST_AUTO_TEST_CASE(send_vote_sends_the_durable_vote_on_the_critical_lane)
{
    DurableVoteFixture f;
    const agreement::OutboundVote vote{f.Vote()};
    auto mesh{RecordingMesh::Make(TestMeshConfig(2, 4))};
    const uint256 header_hash{f.slot.header.Hash()};
    RequireOk(mesh->SendVote(1, vote, header_hash), "SendVote");
    BOOST_REQUIRE_EQUAL(mesh->sent.size(), 1U);
    BOOST_CHECK_EQUAL(mesh->sent[0].seat, 1U);
    BOOST_CHECK(mesh->sent[0].lane == Lane::CRITICAL);
    // The frame is the VOTE message of the durable record, from this seat.
    Result<agreement::Message> decoded{agreement::DecodeMessage(mesh->sent[0].frame, RegtestProfile())};
    RequireOk(decoded, "DecodeMessage");
    const auto* sent{std::get_if<agreement::Vote>(&*decoded)};
    BOOST_REQUIRE(sent != nullptr);
    BOOST_CHECK(sent->phase == vote.Key().phase);
    BOOST_CHECK_EQUAL(sent->epoch, vote.Key().epoch);
    BOOST_CHECK_EQUAL(sent->view, vote.Key().view);
    BOOST_CHECK_EQUAL(sent->slot, f.slot.header.height);
    BOOST_CHECK(sent->header_hash == header_hash);
    BOOST_CHECK_EQUAL(sent->seat, 2U);
    BOOST_CHECK(sent->signature == vote.Signature());
    BOOST_CHECK(sent->inclusion.empty());
    RequireOk(agreement::CheckVote(*sent), "CheckVote");
    // The signature is the seat's BLS signature over the vote's digest (the CommitDigest).
    const std::optional<bls::Signature> signature{bls::Signature::Decode(sent->signature)};
    BOOST_REQUIRE(signature.has_value());
    const uint256 digest{f.slot.header.CommitDigest()};
    BOOST_CHECK(vote.Digest() == digest);
    BOOST_CHECK(bls::Verify(f.signer.PublicKey(), std::span<const unsigned char>{digest.data(), digest.size()}, *signature));
    // Resending the durable vote sends the same bytes.
    RequireOk(mesh->SendVote(3, vote, header_hash), "SendVote to seat 3");
    BOOST_REQUIRE_EQUAL(mesh->sent.size(), 2U);
    BOOST_CHECK(mesh->sent[1].frame == mesh->sent[0].frame);
    // Route refusals transmit nothing.
    RequireRefused(mesh->SendVote(2, vote, header_hash), ErrorCode::INVALID_INPUT, net::MESH_SELF, "to itself");
    RequireRefused(mesh->SendVote(4, vote, header_hash), ErrorCode::INVALID_INPUT, net::MESH_SEAT_RANGE, "seat 4 of 4");
    net::MeshConfig tiny{TestMeshConfig(2, 4)};
    tiny.max_frame_bytes = 16;
    auto small{RecordingMesh::Make(tiny)};
    RequireRefused(small->SendVote(1, vote, header_hash), ErrorCode::OVERSIZE, net::MESH_FRAME_OVERSIZE, "oversize vote");
    BOOST_CHECK(small->sent.empty());
    // A full lane at the transport: BUSY, nothing queued.
    mesh->refuse = Error{ErrorCode::BUSY, "test lane full"};
    RequireRefused(mesh->SendVote(1, vote, header_hash), ErrorCode::BUSY, "test lane full", "busy transport");
    BOOST_CHECK_EQUAL(mesh->sent.size(), 2U);
}

BOOST_AUTO_TEST_CASE(send_refuses_votes_and_frames_of_another_lane)
{
    auto mesh{RecordingMesh::Make(TestMeshConfig(0, 4))};
    const Frame vote{VoteFrame()};
    const Frame proposal{ProposalFrame()};
    // No vote leaves through Send: neither on CRITICAL nor on PROPOSAL.
    RequireRefused(mesh->Send(1, Lane::CRITICAL, vote), ErrorCode::INVALID_INPUT, net::MESH_VOTE_NOT_TYPED, "vote on CRITICAL");
    RequireRefused(mesh->Send(1, Lane::PROPOSAL, vote), ErrorCode::INVALID_INPUT, net::MESH_VOTE_NOT_TYPED, "vote on PROPOSAL");
    // Each agreement lane carries only its own message types.
    RequireRefused(mesh->Send(1, Lane::CRITICAL, proposal), ErrorCode::INVALID_INPUT, net::MESH_LANE_CONTENT, "proposal on CRITICAL");
    RequireRefused(mesh->Send(1, Lane::PROPOSAL, ViewChangeFrame()), ErrorCode::INVALID_INPUT, net::MESH_LANE_CONTENT, "view change on PROPOSAL");
    RequireRefused(mesh->Send(1, Lane::CRITICAL, Frame{}), ErrorCode::INVALID_INPUT, net::MESH_LANE_CONTENT, "empty on CRITICAL");
    Frame other_version{ViewChangeFrame()};
    other_version[0] = 2;
    RequireRefused(mesh->Send(1, Lane::CRITICAL, other_version), ErrorCode::INVALID_INPUT, net::MESH_LANE_CONTENT, "another version");
    Frame unknown_type{ViewChangeFrame()};
    unknown_type[1] = 9;
    RequireRefused(mesh->Send(1, Lane::CRITICAL, unknown_type), ErrorCode::INVALID_INPUT, net::MESH_LANE_CONTENT, "unknown type");
    BOOST_CHECK(mesh->sent.empty());
    // Route and size refusals come first.
    RequireRefused(mesh->Send(1, static_cast<Lane>(0), proposal), ErrorCode::INVALID_INPUT, net::MESH_LANE_UNKNOWN, "unregistered lane");
    RequireRefused(mesh->Send(4, Lane::CRITICAL, vote), ErrorCode::INVALID_INPUT, net::MESH_SEAT_RANGE, "seat out of range");
    RequireRefused(mesh->Send(0, Lane::CRITICAL, vote), ErrorCode::INVALID_INPUT, net::MESH_SELF, "to itself");
    RequireRefused(mesh->Send(1, Lane::BULK, Frame(mesh->Config().max_frame_bytes + 1, 0)), ErrorCode::OVERSIZE, net::MESH_FRAME_OVERSIZE,
                   "oversize bulk");
    BOOST_CHECK(mesh->sent.empty());
    // What may pass.
    RequireOk(mesh->Send(1, Lane::PROPOSAL, proposal), "proposal");
    RequireOk(mesh->Send(2, Lane::CRITICAL, ViewChangeFrame()), "view change");
    RequireOk(mesh->Send(2, Lane::CRITICAL, NewViewFrame()), "new view");
    RequireOk(mesh->Send(3, Lane::CRITICAL, EvidenceFrame()), "evidence");
    // ACTION and BULK are opaque: their receivers never read a frame as a vote.
    RequireOk(mesh->Send(1, Lane::ACTION, vote), "opaque ACTION frame");
    RequireOk(mesh->Send(1, Lane::BULK, Frame{0x01}), "opaque BULK frame");
    BOOST_REQUIRE_EQUAL(mesh->sent.size(), 6U);
    BOOST_CHECK(mesh->sent[0].lane == Lane::PROPOSAL);
    BOOST_CHECK(mesh->sent[1].lane == Lane::CRITICAL);
    BOOST_CHECK(mesh->sent[4].lane == Lane::ACTION);
    BOOST_CHECK(mesh->sent[5].lane == Lane::BULK);
}

BOOST_AUTO_TEST_CASE(message_sink_routes_by_type)
{
    auto mesh{RecordingMesh::Make(TestMeshConfig(1, 3))};
    agreement::MessageSink& sink{*mesh};
    RequireOk(sink.Send(0, ProposalFrame()), "proposal");
    RequireOk(sink.Send(2, ViewChangeFrame()), "view change");
    RequireOk(sink.Send(2, NewViewFrame()), "new view");
    RequireOk(sink.Send(0, EvidenceFrame()), "evidence");
    RequireRefused(sink.Send(0, VoteFrame()), ErrorCode::INVALID_INPUT, net::MESH_VOTE_NOT_TYPED, "vote through the sink");
    RequireRefused(sink.Send(0, Frame{0x05}), ErrorCode::INVALID_INPUT, net::MESH_LANE_CONTENT, "no envelope");
    RequireRefused(sink.Send(1, ProposalFrame()), ErrorCode::INVALID_INPUT, net::MESH_SELF, "to itself through the sink");
    BOOST_REQUIRE_EQUAL(mesh->sent.size(), 4U);
    BOOST_CHECK(mesh->sent[0].lane == Lane::PROPOSAL);
    BOOST_CHECK(mesh->sent[1].lane == Lane::CRITICAL);
    BOOST_CHECK(mesh->sent[2].lane == Lane::CRITICAL);
    BOOST_CHECK(mesh->sent[3].lane == Lane::CRITICAL);
    BOOST_CHECK_EQUAL(mesh->sent[1].seat, 2U);
}

BOOST_AUTO_TEST_CASE(poll_hands_out_received_frames)
{
    auto mesh{RecordingMesh::Make(TestMeshConfig(0, 2))};
    Result<std::optional<net::InboundFrame>> none{mesh->Poll()};
    RequireOk(none, "Poll empty");
    BOOST_CHECK(!none->has_value());
    mesh->inbound.push_back(net::InboundFrame{1, Lane::CRITICAL, VoteFrame()});
    mesh->inbound.push_back(net::InboundFrame{1, Lane::BULK, Frame{7}});
    Result<std::optional<net::InboundFrame>> first{mesh->Poll()};
    RequireOk(first, "Poll first");
    BOOST_REQUIRE(first->has_value());
    BOOST_CHECK((*first)->lane == Lane::CRITICAL);
    BOOST_CHECK_EQUAL((*first)->from_seat, 1U);
    Result<std::optional<net::InboundFrame>> second{mesh->Poll()};
    RequireOk(second, "Poll second");
    BOOST_REQUIRE(second->has_value());
    BOOST_CHECK((*second)->bytes == Frame{7});
    BOOST_CHECK(!mesh->Poll().value().has_value());
}

BOOST_AUTO_TEST_SUITE_END()

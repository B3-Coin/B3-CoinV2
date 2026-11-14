// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/messages.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/net/sync.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/test/util/require.h>
#include <flowmesh2/test/util/slot_fixtures.h>
#include <flowmesh2/test/util/storage_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <vector>

/** The BULK-lane sync codec (O-101): exact round trips, typed refusals, and
 * the longest frame of every kind fits the BULK lane and the mesh cap.
 */
using namespace flowmesh2;
using namespace flowmesh2::net;
using namespace flowmesh2::test;

BOOST_AUTO_TEST_SUITE(sync_tests)

BOOST_AUTO_TEST_CASE(every_kind_round_trips_exactly)
{
    const ProfileParams profile{RegtestProfile()};
    const std::vector<SyncMessage> messages{
        DecisionsRequest{7, 3},
        DecisionsReply{7, std::vector<unsigned char>{1, 2, 3, 4}},
        DecisionsReply{8, {}},
        ProposalRequest{9, uint256::ONE},
        ProposalReply{9, uint256::ONE, std::vector<unsigned char>{9, 8, 7}},
        ProposalReply{9, uint256::ONE, {}},
    };
    for (const SyncMessage& message : messages) {
        const Result<std::vector<unsigned char>> bytes{EncodeSync(message, profile)};
        RequireOk(bytes, "EncodeSync");
        BOOST_CHECK_EQUAL(bytes->at(0), SYNC_VERSION);
        BOOST_CHECK_EQUAL(bytes->at(1), static_cast<unsigned char>(KindOf(message)));
        const Result<SyncMessage> back{DecodeSync(*bytes, profile)};
        RequireOk(back, "DecodeSync");
        BOOST_CHECK(*back == message);
        BOOST_CHECK(KindOf(*back) == KindOf(message));
        BOOST_CHECK(IsKnown(KindOf(message)));
        // Trailing and truncated bytes are refused.
        std::vector<unsigned char> longer{*bytes};
        longer.push_back(0);
        RequireRefusal(DecodeSync(longer, profile), ErrorCode::DECODE, SYNC_TRAILING, "trailing");
        std::vector<unsigned char> shorter{bytes->begin(), bytes->end() - 1};
        BOOST_CHECK(!DecodeSync(shorter, profile).has_value());
    }
    BOOST_CHECK_EQUAL(std::string{ToString(SyncKind::DECISIONS_REQUEST)}, "DECISIONS_REQUEST");
    BOOST_CHECK_EQUAL(std::string{ToString(SyncKind{9})}, "UNKNOWN");
    BOOST_CHECK(!IsKnown(SyncKind{0}));
}

BOOST_AUTO_TEST_CASE(refusals_are_typed)
{
    const ProfileParams profile{RegtestProfile()};
    RequireRefusal(EncodeSync(DecisionsRequest{1, 0}, profile), ErrorCode::INVALID_INPUT, SYNC_MAX_RANGE, "max 0");
    RequireRefusal(EncodeSync(DecisionsRequest{1, MAX_SYNC_DECISIONS + 1}, profile), ErrorCode::INVALID_INPUT, SYNC_MAX_RANGE, "max 9");
    RequireRefusal(EncodeSync(DecisionsReply{1, std::vector<unsigned char>(MaxSyncRecordBytes(profile) + 1)}, profile), ErrorCode::OVERSIZE,
                   SYNC_OVERSIZE, "record above the cap");
    RequireRefusal(EncodeSync(ProposalReply{1, uint256{}, std::vector<unsigned char>(agreement::MaxMessageBytes(profile) + 1)}, profile),
                   ErrorCode::OVERSIZE, SYNC_OVERSIZE, "proposal above the cap");
    const std::vector<unsigned char> other_version{2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    RequireRefusal(DecodeSync(other_version, profile), ErrorCode::DECODE, SYNC_VERSION_UNKNOWN, "version");
    const std::vector<unsigned char> other_kind{1, 9, 0};
    RequireRefusal(DecodeSync(other_kind, profile), ErrorCode::DECODE, SYNC_KIND_UNKNOWN, "kind");
    const std::vector<unsigned char> short_request{1, 1, 0, 0};
    RequireRefusal(DecodeSync(short_request, profile), ErrorCode::DECODE, SYNC_TRUNCATED, "truncated");
    std::vector<unsigned char> bad_max{1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    RequireRefusal(DecodeSync(bad_max, profile), ErrorCode::INVALID_INPUT, SYNC_MAX_RANGE, "decoded max 0");
    // A declared length beyond the cap is refused before any allocation.
    std::vector<unsigned char> huge{1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f};
    RequireRefusal(DecodeSync(huge, profile), ErrorCode::OVERSIZE, SYNC_OVERSIZE, "declared length");
    std::vector<unsigned char> frame(MaxBulkFrameBytes(profile) + 1);
    RequireRefusal(DecodeSync(frame, profile), ErrorCode::OVERSIZE, SYNC_OVERSIZE, "frame above the cap");
}

BOOST_AUTO_TEST_CASE(the_longest_frames_fit_the_bulk_lane)
{
    const ProfileParams profile{RegtestProfile()};
    const Result<std::vector<unsigned char>> reply{
        EncodeSync(DecisionsReply{1, std::vector<unsigned char>(MaxSyncRecordBytes(profile))}, profile)};
    RequireOk(reply, "maximal decisions reply");
    const Result<std::vector<unsigned char>> proposal{
        EncodeSync(ProposalReply{1, uint256{}, std::vector<unsigned char>(agreement::MaxMessageBytes(profile))}, profile)};
    RequireOk(proposal, "maximal proposal reply");
    const size_t cap{MaxBulkFrameBytes(profile)};
    BOOST_CHECK_LE(reply->size(), cap);
    BOOST_CHECK_LE(proposal->size(), cap);
    BOOST_CHECK_EQUAL(MaxSyncRecordBytes(profile), storage::DecisionRecord::MaxEncodedSize(profile));
    BOOST_CHECK_GT(cap, agreement::MaxMessageBytes(profile));
    const LaneSetLimits limits{ProvisionalLaneLimits(profile)};
    BOOST_CHECK_EQUAL(limits.Of(Lane::BULK).max_message_bytes, cap);
    BOOST_CHECK_EQUAL(limits.Of(Lane::CRITICAL).max_message_bytes, agreement::MaxMessageBytes(profile));
}

BOOST_AUTO_TEST_SUITE_END()

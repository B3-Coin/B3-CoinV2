// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_TEST_UTIL_API_FIXTURES_H
#define B3_FLOWMESH2_TEST_UTIL_API_FIXTURES_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/net/client_api.h>
#include <flowmesh2/net/in_process_api.h>
#include <flowmesh2/net/lanes.h>
#include <flowmesh2/test/util/admission_fixtures.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/** TEST fixtures of the lane and in-process API suites (slice R13).
 *
 * An ApiRig is an AdmissionRig (a state owner, read service, pool, test
 * budget and admission; admission_fixtures.h) plus a LaneSet and an
 * InProcessApi over them. The ACTION lane has `action_messages` messages
 * of up to ActionV2::MaxEncodedSize + 1 bytes, so its reserved share is
 * ReservedShare(action_messages) messages; the other lanes keep their
 * ProvisionalLaneLimits.
 */
namespace flowmesh2::test {

//! A lane of `messages` messages of up to `message_bytes` (bytes = messages * message_bytes, at least 10 * message_bytes + 10).
inline net::LaneLimits LaneLimitsOf(size_t messages, size_t message_bytes)
{
    return net::LaneLimits{messages, std::max(messages * message_bytes, 10 * message_bytes + 10), message_bytes};
}

//! ProvisionalLaneLimits(profile) with an ACTION lane of `action_messages` messages.
inline net::LaneSetLimits ApiLaneLimits(size_t action_messages, const ProfileParams& profile = RegtestProfile())
{
    net::LaneSetLimits limits{net::ProvisionalLaneLimits(profile)};
    limits.Of(net::Lane::ACTION) = LaneLimitsOf(action_messages, ActionV2::MaxEncodedSize(profile) + 1);
    return limits;
}

inline std::unique_ptr<net::LaneSet> NewLaneSet(const net::LaneSetLimits& limits)
{
    Result<std::unique_ptr<net::LaneSet>> lanes{net::LaneSet::Create(limits)};
    RequireOk(lanes, "LaneSet::Create");
    return std::move(lanes).value();
}

struct ApiRigOptions {
    PoolRigOptions pool{};
    size_t action_messages{20};
    size_t max_replies{64};
};

struct ApiRig : AdmissionRig {
    std::unique_ptr<net::LaneSet> lanes;
    std::unique_ptr<net::InProcessApi> api;

    explicit ApiRig(const ApiRigOptions& options = {})
        : AdmissionRig{options.pool}, lanes{NewLaneSet(ApiLaneLimits(options.action_messages, options.pool.rig.profile))}
    {
        Result<std::unique_ptr<net::InProcessApi>> created{
            net::InProcessApi::Create(profile, *lanes, *admission, *pool, *read, net::ApiLimits{options.max_replies})};
        RequireOk(created, "InProcessApi::Create");
        api = std::move(created).value();
    }

    //! Submit exact bytes; requires the request to be queued.
    net::RequestId Submit(std::span<const unsigned char> frame)
    {
        Result<net::RequestId> id{api->Submit(frame)};
        RequireOk(id, "InProcessApi::Submit");
        return *id;
    }

    //! Pump until `id` is answered, then take its reply (requires it).
    net::ApiReply Reply(net::RequestId id)
    {
        for (int round{0}; round < 1'000; ++round) {
            if (std::optional<net::ApiReply> reply{api->TakeReply(id)}) return std::move(*reply);
            const Result<net::PumpReport> pumped{api->Pump(1)};
            RequireOk(pumped, "InProcessApi::Pump");
        }
        BOOST_FAIL("request " + std::to_string(id.value) + " was never answered");
        return net::ApiReply{};
    }

    //! Submit, pump and take: the admission reply of exact bytes.
    admission::AdmissionReply AdmitViaApi(std::span<const unsigned char> frame)
    {
        const net::ApiReply reply{Reply(Submit(frame))};
        const auto* admitted{std::get_if<admission::AdmissionReply>(&reply.body)};
        if (admitted == nullptr) BOOST_FAIL("the reply of a submitted action is not an AdmissionReply");
        return *admitted;
    }

    //! GetAction; requires an answer (not an Error).
    net::ActionStatusReply GetAction(const ActionV2& original) const
    {
        Result<net::ActionStatusReply> reply{api->GetAction(original)};
        RequireOk(reply, "InProcessApi::GetAction");
        return std::move(reply).value();
    }
};

} // namespace flowmesh2::test

#endif // B3_FLOWMESH2_TEST_UTIL_API_FIXTURES_H

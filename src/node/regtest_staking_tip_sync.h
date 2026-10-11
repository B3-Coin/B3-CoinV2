// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_REGTEST_STAKING_TIP_SYNC_H
#define B3COIN_NODE_REGTEST_STAKING_TIP_SYNC_H

#include <uint256.h>
#include <util/time.h>

#include <chrono>
#include <optional>
#include <utility>

namespace node {

/** Bounded, connection-local availability hint for regtest H1 staking.
 *
 * Callers hold cs_main. Only an actually sent GETHEADERS arms a request;
 * only a normally validated, nonempty, non-full response ending at that
 * exact active tip may confirm it. This is NOT proof of global sync or
 * finality: an isolated or dishonest peer can still omit another branch.
 * The connection owner destroys this state on disconnect/restart.
 */
class RegtestStakingTipSync
{
public:
    static constexpr auto REQUEST_TIMEOUT{std::chrono::minutes{2}};
    static constexpr auto PROBE_INTERVAL{std::chrono::minutes{2}};
    static constexpr auto REPLY_LIFETIME{std::chrono::minutes{5}};

    void Requested(const uint256& tip, NodeClock::time_point now)
    {
        m_request = Observation{tip, now};
        m_last_request = now;
        m_reply.reset();
    }

    /** Consume at most one request before examining a HEADERS response.
     * Invalid/empty/unsolicited replies cannot leave a reusable request.
     * Compact-block announcements do not call this method.
     */
    std::optional<uint256> TakeRequest(NodeClock::time_point now)
    {
        const auto request{std::exchange(m_request, std::nullopt)};
        m_reply.reset();
        if (!request || now < request->time ||
            now - request->time > REQUEST_TIMEOUT) return std::nullopt;
        return request->tip;
    }

    //! Only after normal header processing and exact request/tip matching.
    void Confirmed(const uint256& tip, NodeClock::time_point now)
    {
        m_reply = Observation{tip, now};
    }

    bool Fresh(const uint256& tip, NodeClock::time_point now) const
    {
        return m_reply && m_reply->tip == tip && now >= m_reply->time &&
               now - m_reply->time <= REPLY_LIFETIME;
    }

    /** Independent of peer.m_last_getheaders_timestamp, which incoming
     * traffic can clear. Unsolicited replies cannot accelerate our probes.
     */
    bool ShouldProbe(NodeClock::time_point now) const
    {
        return !m_last_request ||
               (now >= *m_last_request && now - *m_last_request >= PROBE_INTERVAL);
    }

private:
    struct Observation {
        uint256 tip;
        NodeClock::time_point time;
    };
    std::optional<Observation> m_request;
    std::optional<Observation> m_reply;
    std::optional<NodeClock::time_point> m_last_request;
};

} // namespace node

#endif // B3COIN_NODE_REGTEST_STAKING_TIP_SYNC_H

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_fresh_transport.h>

#include <hash.h>
#include <util/time.h>

#include <algorithm>
#include <utility>

namespace node {

namespace {

void Refill(FreshVoteTokenBucket& bucket, const FreshVoteBucketLimits& limits, const std::chrono::microseconds now)
{
    // A clock that does not advance adds nothing and never moves `updated`
    // backwards, so no interval is credited twice.
    if (now <= bucket.updated) return;
    bucket.tokens = std::min<double>(bucket.tokens + Ticks<SecondsDouble>(now - bucket.updated) * limits.rate_per_second,
                                     static_cast<double>(limits.capacity));
    bucket.updated = now;
}

} // namespace

FreshVoteTokenBucket FullFreshVoteBucket(const FreshVoteBucketLimits& limits, const std::chrono::microseconds now)
{
    return FreshVoteTokenBucket{static_cast<double>(limits.capacity), now};
}

bool ConsumeFreshVoteToken(FreshVoteTokenBucket& peer, const FreshVoteBucketLimits& peer_limits,
                           FreshVoteTokenBucket& global, const FreshVoteBucketLimits& global_limits,
                           const std::chrono::microseconds now)
{
    Refill(peer, peer_limits, now);
    Refill(global, global_limits, now);
    if (peer.tokens < 1.0 || global.tokens < 1.0) return false;
    peer.tokens -= 1.0;
    global.tokens -= 1.0;
    return true;
}

uint256 FreshVoteId(const FreshLineageSig& sig)
{
    return (HashWriter{} << sig).GetSHA256();
}

FreshVoteExport::FreshVoteExport(std::vector<FreshLineageSig> verified) : votes{std::move(verified)}
{
    ids.reserve(votes.size());
    for (const FreshLineageSig& sig : votes) ids.push_back(FreshVoteId(sig));
    live.insert(ids.begin(), ids.end());
}

void FreshVoteExport::Clear()
{
    votes.clear();
    ids.clear();
    live.clear();
}

std::vector<FreshLineageSig> FreshVoteReplay::Take(const FreshVoteExport& exported,
                                                   const std::chrono::microseconds now)
{
    std::vector<FreshLineageSig> out;
    if (!Due(now)) return out;
    m_next = now + TICK;

    // Finalized, reorged-out or evicted votes are forgotten once RESEND has
    // passed; a vote that is verified again later is simply sent again. A
    // vote marked sent but not yet in the export (the export lags an
    // acceptance by up to a second) is kept, so it is not echoed back.
    for (auto it{m_sent.begin()}; it != m_sent.end();) {
        it = exported.live.contains(it->first) || now - it->second < RESEND ? std::next(it) : m_sent.erase(it);
    }
    for (size_t i{0}; i < exported.votes.size() && out.size() < MAX_BATCH; ++i) {
        const auto [it, inserted]{m_sent.try_emplace(exported.ids[i], now)};
        if (!inserted) {
            if (now - it->second < RESEND) continue;
            it->second = now;
        }
        out.push_back(exported.votes[i]);
    }
    return out;
}

void FreshVoteReplay::MarkSent(const FreshLineageSig& sig, const std::chrono::microseconds now)
{
    const uint256 id{FreshVoteId(sig)};
    const auto it{m_sent.find(id)};
    if (it != m_sent.end()) {
        it->second = std::max(it->second, now);
        return;
    }
    if (m_sent.size() >= MAX_TRACKED) return;
    m_sent.emplace(id, now);
}

} // namespace node

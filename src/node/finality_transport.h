// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FINALITY_TRANSPORT_H
#define B3COIN_NODE_FINALITY_TRANSPORT_H

#include <node/finality_signature.h>

#include <chrono>
#include <functional>
#include <map>
#include <utility>

namespace node {

/** Cheap transport admission only. This does NOT verify a signature or relax
 * the pool's depth requirement. The returned digest binds a pending message
 * to the precise local branch, withdrawal root, and successor set. */
std::optional<uint256> FinalityTransportDigest(
    const FinalitySig& sig, const FinalityTracker::State& state,
    const CChain& chain, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index = nullptr);

/** Unverified, fixed-size messages waiting for the local chain to reach depth.
 * Entries never count toward quorum. Admission callers must check the near-tip
 * checkpoint/epoch/index and bind the exact digest before calling Add(). */
class PendingFinalitySignatures
{
public:
    static constexpr size_t MAX_ENTRIES{2 * modern::MAX_FINALITY_SET};
    static constexpr size_t MAX_PEER_ENTRIES{modern::MAX_FINALITY_SET};
    static constexpr auto EXPIRY{std::chrono::minutes{5}};
    static constexpr size_t MAX_RETRY_BATCH{64};
    static constexpr size_t MAX_RETRY_PER_PEER{32};
    static constexpr size_t MAX_RETRY_PER_COORDINATE{2};

    struct Entry {
        FinalitySig sig;
        int64_t peer;
        uint256 digest;
        std::chrono::microseconds added;
    };

    /** Complete-message deduplication: an invalid signature claiming an index
     * must not suppress a different, valid signature for the same index.
     * Duplicates cannot extend expiry or change the charged source peer. */
    bool Add(const FinalitySig& sig, int64_t peer, const uint256& digest,
             std::chrono::microseconds now);

    /** Prune expired/disconnected/stale/fork entries; remove at most one
     * bounded batch of depth-ready messages whose ORIGINAL source peer and
     * global verification budgets permit another Submit(). A false budget
     * result leaves the entry pending, never bypasses the limits. */
    std::vector<Entry> TakeReady(
        int ready_height, std::chrono::microseconds now,
        const std::function<bool(const Entry&)>& still_valid,
        const std::function<bool(int64_t)>& spend_budget);

    void Expire(std::chrono::microseconds now);
    size_t Size() const { return m_entries.size(); }
    size_t PeerSize(int64_t peer) const;

private:
    using Entries = std::map<uint256, Entry>;
    Entries::iterator Erase(Entries::iterator it);
    Entries m_entries;
    std::map<int64_t, size_t> m_peer_counts;
};

/** Per-connection, bounded walk through a shared, freshly validated pool
 * snapshot. Reconnecting constructs a fresh cursor. No new signatures are
 * produced, and duplicate reception never resets this timer. */
class FinalityRelayCursor
{
public:
    static constexpr size_t MAX_BATCH{2};
    static constexpr size_t NODE_REPLAY_RATE{64};
    static constexpr auto BATCH_INTERVAL{std::chrono::seconds{2}};
    static constexpr auto ROUND_INTERVAL{std::chrono::seconds{30}};

    bool Due(std::chrono::microseconds now) const { return now >= m_next; }
    std::pair<size_t, size_t> Take(size_t available, std::chrono::microseconds now,
                                 size_t peer_count = 1);

private:
    size_t m_offset{0};
    std::chrono::microseconds m_next{0};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_TRANSPORT_H

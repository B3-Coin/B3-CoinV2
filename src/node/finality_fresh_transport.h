// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FINALITY_FRESH_TRANSPORT_H
#define B3COIN_NODE_FINALITY_FRESH_TRANSPORT_H

#include <modern/finality_types.h>
#include <node/finality_fresh_signature.h>
#include <uint256.h>

#include <chrono>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace node {

/**
 * P2P transport of fresh-lineage votes (bridge V2 stage S3,
 * b3-bridge-v2-finality-dependency.md section 6.5 "Votes"): the `finsigv2`
 * message carries exactly one FreshLineageSig (148 bytes). LIVENESS ONLY,
 * like V1 `finsig`: a vote reaches the pool only through
 * FreshLineageSignaturePool::Submit, and a fresh checkpoint becomes final
 * only through a type-11 record judged in a valid block.
 *
 * net_processing.cpp handles the message only when the chain configures a
 * fresh lineage (regtest -b3freshlineageheight). Every other node, mainnet
 * and testnet included, ignores it without deserializing it and without a
 * peer penalty, exactly as an older node ignores the unknown message type.
 *
 * Budgets: cheap wire admission and the single BLS verification are charged
 * to separate per-peer and whole-node token buckets, distinct from the V1
 * `finsig` buckets so neither vote family can starve the other. The values
 * repeat V1's: fresh committees are still full validator snapshots (the
 * bounded committee of B-22 is out of scope), so one complete maximum-size
 * committee fits in a peer's verification burst.
 */
struct FreshVoteBucketLimits {
    double rate_per_second;
    size_t capacity;
};
inline constexpr FreshVoteBucketLimits FRESH_VOTE_PEER_WIRE{128.0, 2 * modern::MAX_FINALITY_SET};
inline constexpr FreshVoteBucketLimits FRESH_VOTE_GLOBAL_WIRE{512.0, 4 * modern::MAX_FINALITY_SET};
inline constexpr FreshVoteBucketLimits FRESH_VOTE_PEER_VERIFY{32.0, modern::MAX_FINALITY_SET};
inline constexpr FreshVoteBucketLimits FRESH_VOTE_GLOBAL_VERIFY{128.0, 2 * modern::MAX_FINALITY_SET};

/** One token bucket. All clocks here use one monotonic microsecond epoch
 * (MockableSteadyClock in net_processing.cpp), never wall-clock time. */
struct FreshVoteTokenBucket {
    double tokens{0};
    std::chrono::microseconds updated{0};
};

//! A bucket holding `limits.capacity` tokens at `now`.
FreshVoteTokenBucket FullFreshVoteBucket(const FreshVoteBucketLimits& limits, std::chrono::microseconds now);

/** Refill both buckets up to `now`, then take one token from both or from
 * neither. A refusal costs nothing and never disconnects: forks and epoch
 * races make an honest flood indistinguishable from a hostile one. */
bool ConsumeFreshVoteToken(FreshVoteTokenBucket& peer, const FreshVoteBucketLimits& peer_limits,
                           FreshVoteTokenBucket& global, const FreshVoteBucketLimits& global_limits,
                           std::chrono::microseconds now);

//! SHA256 of the exact 148 serialized bytes (complete-message identity).
uint256 FreshVoteId(const FreshLineageSig& sig);

/**
 * One shared export of the pool's verified votes with their ids, computed
 * once per refresh (net_processing refreshes it on a tip-hash change and at
 * most once a second) and read by every connection's replay, so no replay
 * hashes a vote again.
 */
struct FreshVoteExport {
    std::vector<FreshLineageSig> votes; //!< newest checkpoint first (pool order)
    std::vector<uint256> ids;           //!< FreshVoteId(votes[i])
    std::set<uint256> live;             //!< the same ids, for pruning

    FreshVoteExport() = default;
    explicit FreshVoteExport(std::vector<FreshLineageSig> verified);
    void Clear();
};

/**
 * Per-connection replay of the shared, freshly verified pool export. At most
 * one batch per TICK; a vote goes out when this connection has not been sent
 * it yet, or when it was last sent at least RESEND ago (lost bytes, a peer
 * that was too shallow or out of budget then). There is no unverified holding
 * area: a receiver drops a vote it cannot verify yet and the sender's replay
 * retries it. Reconnecting constructs a new replay, so a new peer receives
 * every useful pooled vote within one TICK. A remembered id is pruned once
 * its vote is no longer in the export AND it was sent (or marked) at least
 * RESEND ago: a vote the peer relayed to us is remembered even while the
 * shared export does not contain it yet, so it never echoes back before
 * RESEND. Memory stays bounded by about two full pools (MarkSent stops at
 * MAX_TRACKED).
 */
class FreshVoteReplay
{
public:
    static constexpr auto TICK{std::chrono::seconds{1}};
    static constexpr auto RESEND{std::chrono::seconds{15}};
    static constexpr size_t MAX_BATCH{64};
    static constexpr size_t MAX_TRACKED{FreshLineageSignaturePool::MAX_TRACKED_CHECKPOINTS *
                                        modern::MAX_FINALITY_SET};

    bool Due(std::chrono::microseconds now) const { return now >= m_next; }

    /** The batch to send now (newest checkpoint first, as the export is
     * ordered), at most MAX_BATCH; empty and unchanged while not Due. */
    std::vector<FreshLineageSig> Take(const FreshVoteExport& exported, std::chrono::microseconds now);

    /** Record a vote this connection already has (it relayed the vote to us,
     * or we flooded it on acceptance), so the replay does not echo it before
     * RESEND. A no-op once MAX_TRACKED ids are remembered. */
    void MarkSent(const FreshLineageSig& sig, std::chrono::microseconds now);

    size_t Tracked() const { return m_sent.size(); }

private:
    std::map<uint256, std::chrono::microseconds> m_sent;
    std::chrono::microseconds m_next{0};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_FRESH_TRANSPORT_H

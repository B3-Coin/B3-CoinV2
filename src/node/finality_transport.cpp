// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_transport.h>

#include <chain.h>
#include <consensus/era.h>
#include <hash.h>
#include <modern/chain_domain.h>
#include <modern/finality_schedule.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <tuple>

namespace node {

std::optional<FinalityTransportCandidate> InspectFinalityTransport(
    const FinalitySig& sig, const FinalityTracker::State& state,
    const CChain& chain, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index)
{
    if (!params.legacy_b3coin || !params.modern_pos || !params.legacy_final_hash ||
        !state.bootstrapped || state.lineage_broken || !chain.Tip() ||
        chain.Height() < 0 || sig.height > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    if (sig.height <= static_cast<uint64_t>(chain.Height())) {
        const auto digest{FinalityTransportDigest(sig, state, chain, params, bridge_index)};
        if (!digest) return std::nullopt;
        return FinalityTransportCandidate{digest};
    }
    if (sig.height - static_cast<uint64_t>(chain.Height()) > MAX_FINALITY_TRANSPORT_AHEAD_BLOCKS) {
        return std::nullopt;
    }
    const auto start{Consensus::ModernPosStartHeight(params)};
    if (!start || !modern::IsCheckpointHeight(static_cast<int>(sig.height), *start,
                                              params.modern_pos->checkpoint_interval) ||
        (state.finalized && sig.height <= static_cast<uint64_t>(state.finalized->height))) {
        return std::nullopt;
    }
    // Do not speculate that a successor epoch has started. Only the locally
    // retained signing sets can admit bytes into the look-ahead queue.
    const auto size{state.SetSize(sig.epoch)};
    if (!size || sig.index >= *size) return std::nullopt;
    return FinalityTransportCandidate{std::nullopt};
}

std::optional<uint256> FinalityTransportDigest(
    const FinalitySig& sig, const FinalityTracker::State& state,
    const CChain& chain, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index)
{
    if (!params.legacy_b3coin || !params.modern_pos || !params.legacy_final_hash ||
        !state.bootstrapped || state.lineage_broken || !chain.Tip() ||
        sig.height > static_cast<uint64_t>(chain.Height())) return std::nullopt;
    const auto start{Consensus::ModernPosStartHeight(params)};
    const int height{static_cast<int>(sig.height)};
    if (!start || !modern::IsCheckpointHeight(height, *start, params.modern_pos->checkpoint_interval) ||
        (state.finalized && height <= state.finalized->height)) return std::nullopt;
    if (sig.epoch != state.epoch && !(state.epoch > 0 && sig.epoch == state.epoch - 1)) return std::nullopt;
    const auto epoch{modern::EpochOfHeight(state.epoch_starts, height)};
    const auto size{state.SetSize(sig.epoch)};
    if (!epoch || *epoch != sig.epoch || !size || sig.index >= *size) return std::nullopt;
    const auto fb{FinalitySignaturePool::ExpectedFinalizedBlock(
        sig.epoch, sig.height, state, chain, params, bridge_index)};
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    if (!fb || !domain) return std::nullopt;
    return modern::FinalityDigest(*domain, *fb);
}

void PendingFinalitySignatures::RemoveSource(const int64_t peer)
{
    auto count{m_peer_counts.find(peer)};
    assert(count != m_peer_counts.end() && count->second > 0);
    if (--count->second == 0) m_peer_counts.erase(count);
}

PendingFinalitySignatures::Entries::iterator PendingFinalitySignatures::Erase(Entries::iterator it)
{
    for (const auto peer : it->second.sources) RemoveSource(peer);
    return m_entries.erase(it);
}

void PendingFinalitySignatures::Expire(const std::chrono::microseconds now)
{
    for (auto it{m_entries.begin()}; it != m_entries.end();) {
        if (now < it->second.added || now - it->second.added >= EXPIRY) {
            it = Erase(it);
        } else {
            ++it;
        }
    }
}

size_t PendingFinalitySignatures::PeerSize(const int64_t peer) const
{
    const auto it{m_peer_counts.find(peer)};
    return it == m_peer_counts.end() ? 0 : it->second;
}

bool PendingFinalitySignatures::Add(const FinalitySig& sig, const int64_t peer,
                                  const uint256& digest, const std::chrono::microseconds now)
{
    return Add(sig, peer, std::optional<uint256>{digest}, now);
}

bool PendingFinalitySignatures::Add(const FinalitySig& sig, const int64_t peer,
                                  const std::optional<uint256>& digest, const std::chrono::microseconds now)
{
    // Payload identity does not change when its block arrives and the digest
    // becomes derivable. Only complete byte-identical messages share sources.
    const uint256 id{(HashWriter{} << sig).GetHash()};
    if (auto found{m_entries.find(id)}; found != m_entries.end()) {
        Entry& entry{found->second};
        if (entry.digest_bound && digest && entry.digest != *digest) return false;
        if (!entry.digest_bound && digest) {
            entry.digest = *digest;
            entry.digest_bound = true;
        }
        if (std::find(entry.sources.begin(), entry.sources.end(), peer) != entry.sources.end() ||
            entry.sources.size() >= MAX_SOURCES || PeerSize(peer) >= MAX_PEER_ENTRIES) return false;
        entry.sources.push_back(peer);
        ++m_peer_counts[peer];
        return false;
    }
    // Expiry is serviced once per second by the caller, not once per hostile
    // incoming message. Full queues refuse additions without evicting others.
    if (m_entries.size() >= MAX_ENTRIES || PeerSize(peer) >= MAX_PEER_ENTRIES) return false;
    // Do not add a small shared-coordinate admission cap: forged variants
    // from one peer must not reserve that validator's slot against an honest
    // later message. Global/per-peer storage caps and retry fairness still apply.
    m_entries.emplace(id, Entry{sig, peer, digest.value_or(uint256{}), now, digest.has_value(), {peer}});
    ++m_peer_counts[peer];
    return true;
}

std::vector<PendingFinalitySignatures::Entry> PendingFinalitySignatures::TakeReady(
    const int ready_height, const std::chrono::microseconds now,
    const std::function<bool(const Entry&)>& still_valid,
    const std::function<bool(int64_t)>& spend_budget)
{
    return TakeReadyWithSources(ready_height, now,
        [&](const Entry& entry) -> std::optional<uint256> {
            if (!entry.digest_bound || !still_valid(entry)) return std::nullopt;
            return entry.digest;
        }, [](int64_t) { return true; }, spend_budget);
}

std::vector<PendingFinalitySignatures::Entry> PendingFinalitySignatures::TakeReadyWithSources(
    const int ready_height, const std::chrono::microseconds now,
    const std::function<std::optional<uint256>(const Entry&)>& resolver,
    const std::function<bool(int64_t)>& source_available,
    const std::function<bool(int64_t)>& spend_budget)
{
    std::vector<Entry> ready;
    std::map<int64_t, size_t> peer_attempts;
    std::map<Coordinate, size_t> coordinate_attempts;
    for (auto it{m_entries.begin()}; it != m_entries.end();) {
        Entry& entry{it->second};
        const auto coordinate{std::make_tuple(entry.sig.epoch, entry.sig.height, entry.sig.index)};
        if (now < entry.added || now - entry.added >= EXPIRY) {
            it = Erase(it);
            continue;
        }
        std::erase_if(entry.sources, [&](int64_t peer) {
            if (source_available(peer)) return false;
            RemoveSource(peer);
            return true;
        });
        if (entry.sources.empty()) {
            it = Erase(it);
            continue;
        }
        entry.peer = entry.sources.front();
        const bool depth_ready{ready_height >= 0 && entry.sig.height <= static_cast<uint64_t>(ready_height)};
        const auto digest{resolver(entry)};
        if (!depth_ready) {
            if (digest && !entry.digest_bound) {
                entry.digest = *digest;
                entry.digest_bound = true;
            }
            // A missing block or temporarily unavailable derived state is
            // not an invalidity verdict before this checkpoint reaches depth.
            // Never replace an already bound digest, even across a reorg.
            ++it;
            continue;
        }
        if (!digest || (entry.digest_bound && entry.digest != *digest)) {
            it = Erase(it);
            continue;
        }
        entry.digest = *digest;
        entry.digest_bound = true;
        if (ready.size() >= MAX_RETRY_BATCH ||
            coordinate_attempts[coordinate] >= MAX_RETRY_PER_COORDINATE) {
            ++it;
            continue;
        }
        const auto source{std::find_if(entry.sources.begin(), entry.sources.end(), [&](int64_t peer) {
            return peer_attempts[peer] < MAX_RETRY_PER_PEER && spend_budget(peer);
        })};
        if (source == entry.sources.end()) {
            ++it;
            continue;
        }
        entry.peer = *source;
        ++peer_attempts[*source];
        ++coordinate_attempts[coordinate];
        ready.push_back(entry);
        it = Erase(it);
    }
    return ready;
}

std::pair<size_t, size_t> FinalityRelayCursor::Take(const size_t available,
                                                 const std::chrono::microseconds now,
                                                 const size_t peer_count)
{
    if (!Due(now)) return {0, 0};
    if (m_offset >= available) m_offset = 0;
    const size_t offset{m_offset};
    const size_t count{std::min(MAX_BATCH, available - offset)};
    m_offset += count;
    // Scale every peer fairly rather than letting the first peers in the
    // message loop exhaust a shared bucket. Small batches also limit replay
    // fan-in at a hub whose neighbours each have few connections.
    const auto spacing{std::max(BATCH_INTERVAL, std::chrono::seconds{
        static_cast<int64_t>((MAX_BATCH * peer_count + NODE_REPLAY_RATE - 1) / NODE_REPLAY_RATE)})};
    if (available > 0 && m_offset == available) {
        m_offset = 0;
        m_next = now + std::max(ROUND_INTERVAL, spacing);
    } else {
        m_next = now + spacing;
    }
    return {offset, count};
}

} // namespace node

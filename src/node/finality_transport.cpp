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
#include <tuple>

namespace node {

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

PendingFinalitySignatures::Entries::iterator PendingFinalitySignatures::Erase(Entries::iterator it)
{
    auto count{m_peer_counts.find(it->second.peer)};
    assert(count != m_peer_counts.end() && count->second > 0);
    if (--count->second == 0) m_peer_counts.erase(count);
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
    // Expiry is serviced once per second by the caller, not once per hostile
    // incoming message. Full queues refuse additions without evicting others.
    if (m_entries.size() >= MAX_ENTRIES || PeerSize(peer) >= MAX_PEER_ENTRIES) return false;
    const uint256 id{(HashWriter{} << sig << digest).GetHash()};
    if (!m_entries.emplace(id, Entry{sig, peer, digest, now}).second) return false;
    ++m_peer_counts[peer];
    return true;
}

std::vector<PendingFinalitySignatures::Entry> PendingFinalitySignatures::TakeReady(
    const int ready_height, const std::chrono::microseconds now,
    const std::function<bool(const Entry&)>& still_valid,
    const std::function<bool(int64_t)>& spend_budget)
{
    std::vector<Entry> ready;
    std::map<int64_t, size_t> peer_attempts;
    std::map<std::tuple<uint64_t, uint64_t, uint32_t>, size_t> coordinate_attempts;
    for (auto it{m_entries.begin()}; it != m_entries.end();) {
        const Entry& entry{it->second};
        const auto coordinate{std::make_tuple(entry.sig.epoch, entry.sig.height, entry.sig.index)};
        if (now < entry.added || now - entry.added >= EXPIRY || !still_valid(entry)) {
            it = Erase(it);
        } else if (ready.size() < MAX_RETRY_BATCH && ready_height >= 0 &&
                   entry.sig.height <= static_cast<uint64_t>(ready_height) &&
                   peer_attempts[entry.peer] < MAX_RETRY_PER_PEER &&
                   coordinate_attempts[coordinate] < MAX_RETRY_PER_COORDINATE && spend_budget(entry.peer)) {
            ++peer_attempts[entry.peer];
            ++coordinate_attempts[coordinate];
            ready.push_back(entry);
            it = Erase(it);
        } else {
            ++it;
        }
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

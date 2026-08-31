// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/fn_seat_index.h>

#include <chain.h>
#include <consensus/era.h>
#include <flowmesh/bls_certificate.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/flowmesh_seat.h>
#include <node/blockstorage.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace node {
namespace {

bool SeatBefore(const FnSeatRecord& a, const FnSeatRecord& b)
{
    return a.seat_id < b.seat_id ||
           (a.seat_id == b.seat_id && a.outpoint < b.outpoint);
}

} // namespace

std::vector<flowmesh::FlowMeshSeatSetMember> FnSeatSnapshot::SetMembers() const
{
    std::vector<flowmesh::FlowMeshSeatSetMember> out;
    out.reserve(members.size());
    for (const FnSeatRecord& member : members) out.push_back(member.SetMember());
    return out;
}

std::optional<uint256> FnSeatSnapshot::SetHash(const uint256& domain,
                                               const uint256& market_id,
                                               const uint64_t epoch) const
{
    if (anchor_height < 0) return std::nullopt;
    const auto set_members{SetMembers()};
    return flowmesh::ComputeFlowMeshSeatSetHash(
        domain, market_id, epoch, static_cast<uint64_t>(anchor_height),
        anchor_hash, set_members);
}

std::optional<FnSeatRecord> FnSeatIndex::Get(const COutPoint& outpoint) const
{
    const auto it{m_by_outpoint.find(outpoint)};
    if (it == m_by_outpoint.end()) return std::nullopt;
    return it->second;
}

std::optional<COutPoint> FnSeatIndex::OwnerOf(const FnSeatBlsKey& key) const
{
    const auto it{m_owner_of_key.find(key)};
    if (it == m_owner_of_key.end()) return std::nullopt;
    return it->second;
}

bool FnSeatIndex::VerifyBlock(const CBlock& block, const int height,
                              const uint256& block_hash,
                              const Consensus::Params& params,
                              FnSeatBlockDelta& out, std::string& error) const
{
    out = FnSeatBlockDelta{};
    if (!Consensus::FlowMeshSeatBindingScheduleConfigured(params)) {
        error = "FlowMesh FN-seat schedule is unavailable";
        return false;
    }
    if (height < *params.asset_activation_height) {
        error = "FlowMesh FN-seat index is not active";
        return false;
    }
    if (block_hash.IsNull()) {
        error = "FlowMesh FN-seat block hash is null";
        return false;
    }
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) {
        error = "FlowMesh FN-seat chain domain is unavailable";
        return false;
    }

    // Candidate validation is all-or-nothing: only these scratch maps change.
    auto scratch_by_outpoint{m_by_outpoint};
    auto scratch_owner_of_key{m_owner_of_key};
    std::vector<FnSeatRecord> removed_from_parent;
    std::vector<FnSeatRecord> created_in_order;

    for (const CTransactionRef& tx : block.vtx) {
        // Frozen ordering: all spends in this transaction release their live
        // seats before any output attempts to claim a BLS key.
        for (const CTxIn& input : tx->vin) {
            const auto seat{scratch_by_outpoint.find(input.prevout)};
            if (seat == scratch_by_outpoint.end()) continue;
            const FnSeatRecord removed{seat->second};
            const auto key_owner{scratch_owner_of_key.find(removed.bls_pubkey)};
            if (key_owner == scratch_owner_of_key.end() ||
                key_owner->second != removed.outpoint) {
                error = "FlowMesh FN-seat reverse index is inconsistent";
                return false;
            }
            scratch_owner_of_key.erase(key_owner);
            scratch_by_outpoint.erase(seat);
            // A seat created earlier in this same block is temporary state,
            // not a removal from the parent snapshot.
            if (m_by_outpoint.contains(removed.outpoint)) {
                removed_from_parent.push_back(removed);
            }
        }

        std::vector<modern::VerifiedFlowMeshSeatBinding> bindings;
        if (!modern::ExtractVerifiedFlowMeshSeatBindings(
                *tx, height, params, bindings, error)) {
            return false;
        }
        for (const modern::VerifiedFlowMeshSeatBinding& binding : bindings) {
            const COutPoint outpoint{tx->GetHash(), binding.output_index};
            if (scratch_by_outpoint.contains(outpoint)) {
                error = "duplicate FlowMesh FN-seat outpoint";
                return false;
            }
            if (scratch_owner_of_key.contains(binding.public_key)) {
                error = "duplicate live FlowMesh FN-seat BLS key";
                return false;
            }
            FnSeatRecord record;
            record.outpoint = outpoint;
            record.seat_id = flowmesh::ComputeFlowMeshSeatId(*domain, outpoint);
            record.bls_pubkey = binding.public_key;
            record.proof_of_possession = binding.proof_of_possession;
            record.created_height = height;
            record.created_block = block_hash;
            scratch_by_outpoint.emplace(outpoint, record);
            scratch_owner_of_key.emplace(record.bls_pubkey, outpoint);
            created_in_order.push_back(record);
            if (scratch_by_outpoint.size() >
                flowmesh::FLOWMESH_MAX_ACTIVE_FN_SEATS) {
                error = "too many live FlowMesh FN seats";
                return false;
            }
        }
    }

    out.height = height;
    out.block_hash = block_hash;
    out.removed = std::move(removed_from_parent);
    // Same-block parent seats consumed by children cancel from the durable
    // delta. Surviving creations retain their original tx/vout order.
    for (const FnSeatRecord& record : created_in_order) {
        const auto live{scratch_by_outpoint.find(record.outpoint)};
        if (live != scratch_by_outpoint.end() && live->second == record) {
            out.added.push_back(record);
        }
    }
    return true;
}

bool FnSeatIndex::ConnectBlock(const FnSeatBlockDelta& delta,
                               std::string& error)
{
    if (delta.height < 0 || delta.block_hash.IsNull()) {
        error = "invalid FlowMesh FN-seat block delta identity";
        return false;
    }
    if (!m_history.empty() && delta.height != m_history.back().height + 1) {
        error = "non-contiguous FlowMesh FN-seat block delta";
        return false;
    }

    const bool was_ready{m_by_outpoint.size() >= 4};
    auto next_by_outpoint{m_by_outpoint};
    auto next_owner_of_key{m_owner_of_key};
    for (const FnSeatRecord& removed : delta.removed) {
        const auto seat{next_by_outpoint.find(removed.outpoint)};
        if (seat == next_by_outpoint.end() || !(seat->second == removed)) {
            error = "FlowMesh FN-seat delta removes the wrong outpoint";
            return false;
        }
        const auto owner{next_owner_of_key.find(removed.bls_pubkey)};
        if (owner == next_owner_of_key.end() || owner->second != removed.outpoint) {
            error = "FlowMesh FN-seat delta removes the wrong key owner";
            return false;
        }
        next_owner_of_key.erase(owner);
        next_by_outpoint.erase(seat);
    }
    for (const FnSeatRecord& added : delta.added) {
        if (next_by_outpoint.contains(added.outpoint)) {
            error = "FlowMesh FN-seat delta duplicates an outpoint";
            return false;
        }
        if (next_owner_of_key.contains(added.bls_pubkey)) {
            error = "FlowMesh FN-seat delta duplicates a live BLS key";
            return false;
        }
        next_by_outpoint.emplace(added.outpoint, added);
        next_owner_of_key.emplace(added.bls_pubkey, added.outpoint);
        if (next_by_outpoint.size() > flowmesh::FLOWMESH_MAX_ACTIVE_FN_SEATS) {
            error = "FlowMesh FN-seat delta exceeds the live-seat cap";
            return false;
        }
    }

    auto next_readiness_transitions{m_readiness_transitions};
    const bool is_ready{next_by_outpoint.size() >= 4};
    if (was_ready != is_ready) {
        next_readiness_transitions.emplace_back(delta.height, is_ready);
    }

    m_by_outpoint = std::move(next_by_outpoint);
    m_owner_of_key = std::move(next_owner_of_key);
    m_readiness_transitions = std::move(next_readiness_transitions);
    m_history.push_back(delta); // includes empty A2+ blocks
    return true;
}

bool FnSeatIndex::DisconnectBlock(const int height, const uint256& block_hash,
                                  std::string& error)
{
    if (m_history.empty() || m_history.back().height != height ||
        m_history.back().block_hash != block_hash) {
        error = "FlowMesh FN-seat disconnect does not match the index tip";
        return false;
    }
    const FnSeatBlockDelta& delta{m_history.back()};
    const bool was_ready{m_by_outpoint.size() >= 4};
    auto previous_by_outpoint{m_by_outpoint};
    auto previous_owner_of_key{m_owner_of_key};
    for (auto it{delta.added.rbegin()}; it != delta.added.rend(); ++it) {
        const auto seat{previous_by_outpoint.find(it->outpoint)};
        if (seat == previous_by_outpoint.end() || !(seat->second == *it)) {
            error = "FlowMesh FN-seat undo cannot remove its addition";
            return false;
        }
        const auto owner{previous_owner_of_key.find(it->bls_pubkey)};
        if (owner == previous_owner_of_key.end() || owner->second != it->outpoint) {
            error = "FlowMesh FN-seat undo has the wrong added-key owner";
            return false;
        }
        previous_owner_of_key.erase(owner);
        previous_by_outpoint.erase(seat);
    }
    for (auto it{delta.removed.rbegin()}; it != delta.removed.rend(); ++it) {
        if (previous_by_outpoint.contains(it->outpoint) ||
            previous_owner_of_key.contains(it->bls_pubkey)) {
            error = "FlowMesh FN-seat undo cannot restore a removed seat";
            return false;
        }
        previous_by_outpoint.emplace(it->outpoint, *it);
        previous_owner_of_key.emplace(it->bls_pubkey, it->outpoint);
    }

    auto previous_readiness_transitions{m_readiness_transitions};
    const bool previous_ready{previous_by_outpoint.size() >= 4};
    if (was_ready != previous_ready) {
        if (previous_readiness_transitions.empty() ||
            previous_readiness_transitions.back() !=
                std::pair{height, was_ready}) {
            error = "FlowMesh FN-seat readiness history is inconsistent";
            return false;
        }
        previous_readiness_transitions.pop_back();
    }

    m_by_outpoint = std::move(previous_by_outpoint);
    m_owner_of_key = std::move(previous_owner_of_key);
    m_readiness_transitions = std::move(previous_readiness_transitions);
    m_snapshots.erase(block_hash);
    m_history.pop_back();
    return true;
}

std::optional<FnSeatSnapshot> FnSeatIndex::SnapshotAt(
    const CBlockIndex& anchor) const
{
    const uint256 anchor_hash{anchor.GetBlockHash()};
    if (const auto cached{m_snapshots.find(anchor_hash)};
        cached != m_snapshots.end() &&
        cached->second.anchor_height == anchor.nHeight) {
        return cached->second;
    }

    std::map<COutPoint, FnSeatRecord> at_anchor{m_by_outpoint};
    bool found{false};
    for (auto it{m_history.rbegin()}; it != m_history.rend(); ++it) {
        if (it->height == anchor.nHeight) {
            if (it->block_hash != anchor_hash) return std::nullopt;
            found = true;
            break;
        }
        if (it->height < anchor.nHeight) return std::nullopt;
        for (auto added{it->added.rbegin()}; added != it->added.rend(); ++added) {
            const auto live{at_anchor.find(added->outpoint)};
            if (live == at_anchor.end() || !(live->second == *added)) {
                return std::nullopt;
            }
            at_anchor.erase(live);
        }
        for (auto removed{it->removed.rbegin()};
             removed != it->removed.rend(); ++removed) {
            if (!at_anchor.emplace(removed->outpoint, *removed).second) {
                return std::nullopt;
            }
        }
    }
    if (!found) return std::nullopt;

    FnSeatSnapshot snapshot;
    snapshot.anchor_height = anchor.nHeight;
    snapshot.anchor_hash = anchor_hash;
    snapshot.members.reserve(at_anchor.size());
    for (const auto& [_, seat] : at_anchor) snapshot.members.push_back(seat);
    std::sort(snapshot.members.begin(), snapshot.members.end(), SeatBefore);
    m_snapshots.insert_or_assign(anchor_hash, snapshot);
    return snapshot;
}

std::optional<FnSeatSnapshot> FnSeatIndex::AnchoredSnapshot(
    const CChain& chain, const CBlockIndex& anchor, const int candidate_height,
    const Consensus::Params& params, std::string& error) const
{
    if (!Consensus::FlowMeshRulesActive(candidate_height, params)) {
        error = "FlowMesh service is not active";
        return std::nullopt;
    }
    if (anchor.nHeight < 0 || chain[anchor.nHeight] != &anchor) {
        error = "FlowMesh seat anchor is not on the active chain";
        return std::nullopt;
    }
    if (static_cast<int64_t>(candidate_height) -
            static_cast<int64_t>(anchor.nHeight) <
        Consensus::FLOWMESH_ANCHOR_DEPTH) {
        error = "FlowMesh seat anchor is too shallow";
        return std::nullopt;
    }
    const auto snapshot{SnapshotAt(anchor)};
    if (!snapshot) error = "FlowMesh seat anchor snapshot is unavailable";
    return snapshot;
}

std::optional<FnSeatSnapshot> FnSeatIndex::EarliestFlowMeshReadySnapshot(
    const CChain& chain, const int first_height, const int candidate_height,
    const Consensus::Params& params, std::string& error) const
{
    if (!Consensus::FlowMeshRulesActive(candidate_height, params)) {
        error = "FlowMesh service is not active";
        return std::nullopt;
    }
    if (!params.asset_activation_height ||
        first_height < *params.asset_activation_height) {
        error = "FlowMesh bootstrap begins before seat history";
        return std::nullopt;
    }
    const int64_t latest_height{
        static_cast<int64_t>(candidate_height) -
        Consensus::FLOWMESH_ANCHOR_DEPTH};
    if (first_height < 0 || latest_height < first_height ||
        latest_height > std::numeric_limits<int>::max()) {
        error = "FlowMesh bootstrap anchor is not deep enough";
        return std::nullopt;
    }
    const CBlockIndex* first{chain[first_height]};
    if (first == nullptr) {
        error = "FlowMesh bootstrap start is not canonical";
        return std::nullopt;
    }

    const auto after_first{std::upper_bound(
        m_readiness_transitions.begin(), m_readiness_transitions.end(),
        first_height, [](const int height, const auto& transition) {
            return height < transition.first;
        })};
    const bool ready_at_first{
        after_first != m_readiness_transitions.begin() &&
        std::prev(after_first)->second};
    int selected_height{first_height};
    if (!ready_at_first) {
        const auto ready{std::find_if(
            after_first, m_readiness_transitions.end(),
            [](const auto& transition) { return transition.second; })};
        if (ready == m_readiness_transitions.end() ||
            ready->first > latest_height) {
            error = "FlowMesh has fewer than four anchor-final bootstrap seats";
            return std::nullopt;
        }
        selected_height = ready->first;
    }
    if (selected_height > latest_height) {
        error = "FlowMesh bootstrap anchor is not deep enough";
        return std::nullopt;
    }
    const CBlockIndex* selected{chain[selected_height]};
    if (selected == nullptr) {
        error = "FlowMesh bootstrap anchor is not canonical";
        return std::nullopt;
    }
    auto snapshot{AnchoredSnapshot(chain, *selected, candidate_height, params,
                                   error)};
    if (!snapshot || !snapshot->FlowMeshReady()) {
        if (error.empty()) {
            error = "FlowMesh bootstrap readiness history is inconsistent";
        }
        return std::nullopt;
    }
    return snapshot;
}

void FnSeatIndex::Clear()
{
    m_by_outpoint.clear();
    m_owner_of_key.clear();
    m_history.clear();
    m_readiness_transitions.clear();
    m_snapshots.clear();
}

bool FnSeatTracker::ApplyBlock(const CBlock& block, const CBlockIndex& index,
                               const Consensus::Params& params)
{
    FnSeatBlockDelta delta;
    std::string error;
    if (!m_index.VerifyBlock(block, index.nHeight, index.GetBlockHash(), params,
                             delta, error) ||
        !m_index.ConnectBlock(delta, error)) {
        LogWarning("FnSeatTracker: block at height %d failed seat-index verification (%s); index unavailable",
                   index.nHeight, error);
        m_index.Clear();
        m_dirty = true;
        return false;
    }
    return true;
}

bool FnSeatTracker::Sync(const CChain& chain, const BlockManager& blockman,
                         const Consensus::Params& params,
                         const CBlockIndex& target)
{
    if (Synced(target.GetBlockHash())) return true;
    if (chain[target.nHeight] != &target ||
        !Consensus::FlowMeshSeatBindingScheduleConfigured(params)) {
        return false;
    }
    const int activation{*params.asset_activation_height};
    if (target.nHeight < activation) {
        m_index.Clear();
        m_synced_tip = target.GetBlockHash();
        m_synced_height = target.nHeight;
        m_dirty = false;
        return true;
    }

    int start{activation};
    if (!m_dirty && m_synced_height >= activation - 1 &&
        m_synced_height <= target.nHeight && chain[m_synced_height] != nullptr &&
        chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = std::max(activation, m_synced_height + 1);
    } else {
        m_index.Clear();
        m_dirty = true;
    }
    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* index{chain[height]};
        CBlock block;
        if (index == nullptr || !blockman.ReadBlock(block, *index)) {
            LogWarning("FnSeatTracker: cannot read A2+ block at height %d; index unavailable",
                       height);
            m_index.Clear();
            m_dirty = true;
            return false;
        }
        if (!ApplyBlock(block, *index, params)) return false;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

std::optional<FnSeatSnapshot> FnSeatTracker::AnchoredSnapshot(
    const CChain& chain, const CBlockIndex& anchor, const int candidate_height,
    const Consensus::Params& params, std::string& error) const
{
    const CBlockIndex* tip{chain.Tip()};
    if (m_dirty || tip == nullptr || m_synced_tip != tip->GetBlockHash()) {
        error = "FlowMesh FN-seat index is not synced to the active tip";
        return std::nullopt;
    }
    return m_index.AnchoredSnapshot(chain, anchor, candidate_height, params,
                                    error);
}

void FnSeatTracker::BlockConnected(const CBlock& block,
                                   const CBlockIndex& index,
                                   const Consensus::Params& params)
{
    if (!Consensus::FlowMeshSeatBindingScheduleConfigured(params)) return;
    if (m_dirty || index.pprev == nullptr ||
        m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    if (index.nHeight >= *params.asset_activation_height &&
        !ApplyBlock(block, index, params)) {
        return;
    }
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

void FnSeatTracker::BlockDisconnected(const CBlockIndex& index,
                                      const Consensus::Params& params)
{
    if (m_dirty || m_synced_tip != index.GetBlockHash() ||
        index.pprev == nullptr) {
        m_dirty = true;
        return;
    }
    if (Consensus::FlowMeshSeatBindingScheduleConfigured(params) &&
        index.nHeight >= *params.asset_activation_height) {
        std::string error;
        if (!m_index.DisconnectBlock(index.nHeight, index.GetBlockHash(), error)) {
            LogWarning("FnSeatTracker: failed to disconnect height %d (%s); index unavailable",
                       index.nHeight, error);
            m_index.Clear();
            m_dirty = true;
            return;
        }
    }
    m_synced_tip = index.pprev->GetBlockHash();
    m_synced_height = index.pprev->nHeight;
}

} // namespace node

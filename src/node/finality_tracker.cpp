// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_tracker.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <node/blockstorage.h>
#include <util/check.h>

namespace node {

// ----------------------------------------------------------------- State

modern::FinalityEpochView FinalityTracker::State::View() const
{
    modern::FinalityEpochView v;
    v.current_epoch = epoch;
    v.epoch_starts = epoch_starts;
    v.lineage_broken = lineage_broken;
    if (finalized) v.finalized_height = static_cast<uint64_t>(finalized->height);
    if (current) {
        v.current_set = &current->View();
        v.current_set_hash = current->SetHash();
    }
    if (next) v.next_set_hash = next->SetHash();
    if (previous) v.previous_set = &previous->View();
    return v;
}

std::optional<uint32_t> FinalityTracker::State::SetSize(const uint64_t e) const
{
    if (!bootstrapped) return std::nullopt;
    if (e == epoch && current) return static_cast<uint32_t>(current->Size());
    if (epoch >= 1 && e == epoch - 1 && previous) return static_cast<uint32_t>(previous->Size());
    return std::nullopt;
}

// --------------------------------------------------------------- helpers

std::optional<ValidatorSetSnapshot> FinalityTracker::SnapshotAt(const uint64_t epoch, const int height) const
{
    return ValidatorSetSnapshot::BuildAt(epoch, m_stakes, height, m_bindings.Index());
}

std::shared_ptr<const ValidatorSetSnapshot> FinalityTracker::SuccessorSet(const State& s, const int boundary_height,
                                                                          const Consensus::ModernPosParams& pos) const
{
    // s is already rotated: s.current = Set_e (e = s.epoch); we need Set_{e+1}.
    const uint64_t successor_epoch{s.epoch + 1};
    const auto snap{SnapshotAt(successor_epoch, boundary_height)};
    if (snap && snap->Size() >= static_cast<size_t>(pos.min_finality_set)) {
        return std::make_shared<const ValidatorSetSnapshot>(*snap);
    }
    // Carry-over: Set_{e+1} = Set_e re-stamped with epoch e+1.
    return std::make_shared<const ValidatorSetSnapshot>(s.current->WithEpoch(successor_epoch));
}

void FinalityTracker::Reset()
{
    m_state = State{};
    m_stakes.MarkDirty();
    m_bindings.MarkDirty();
    m_synced_tip.SetNull();
    m_synced_height = -1;
    m_dirty = true;
}

// ------------------------------------------------------------ projection

FinalityTracker::State FinalityTracker::Projected(const int height, const Consensus::Params& params) const
{
    State s{m_state};
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    if (!modern_start || !params.modern_pos || height < *modern_start) return s;
    // The private trackers hold the state exactly as of height - 1.
    if (!Assume(m_synced_height == height - 1)) return s;
    const Consensus::ModernPosParams& pos{*params.modern_pos};
    const int E{pos.finality_epoch_blocks};

    if (height == *modern_start) {
        // Bootstrap: epoch 0 starts at M; Set_0 = Set_1 = Snapshot(M-1).
        s = State{};
        s.epoch = 0;
        s.epoch_starts = {*modern_start};
        const auto snap{SnapshotAt(0, height - 1)};
        if (snap && snap->Size() >= static_cast<size_t>(pos.min_finality_set)) {
            s.current = std::make_shared<const ValidatorSetSnapshot>(*snap);
            s.next = std::make_shared<const ValidatorSetSnapshot>(snap->WithEpoch(1));
            s.bootstrapped = true;
        }
        return s;
    }
    if (!s.bootstrapped || s.epoch_starts.empty()) return s;

    // Handover-gated rotation: first h >= epoch_start + E with the epoch-e
    // certificate already included below h.
    if (s.handover_certified && height >= s.epoch_starts.back() + E) {
        s.previous = s.current;
        s.current = s.next;
        s.epoch += 1;
        s.epoch_starts.push_back(height);
        s.handover_certified = false;
        s.next = SuccessorSet(s, height - 1, pos);
    }
    // Extension exhausted: the lineage is broken (no further certificate valid).
    if (!s.handover_certified && !s.lineage_broken &&
        height - s.epoch_starts.back() >= E + pos.max_epoch_extension) {
        s.lineage_broken = true;
    }
    return s;
}

std::shared_ptr<const ValidatorSetSnapshot> FinalityTracker::SetInForceAt(const int height,
                                                                          const Consensus::Params& params) const
{
    // Cheap form of Projected(): the set governing `height` is Set_{e+1}
    // when the rotation fires there, else Set_e (Set_{e+2} need not be built).
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    if (!modern_start || !params.modern_pos || height < *modern_start) return nullptr;
    if (height == *modern_start) {
        const State s{Projected(height, params)};
        return s.current;
    }
    const State& s{m_state};
    if (!s.bootstrapped || s.epoch_starts.empty()) return nullptr;
    if (s.handover_certified && height >= s.epoch_starts.back() + params.modern_pos->finality_epoch_blocks) {
        return s.next;
    }
    return s.current;
}

// ----------------------------------------------------------- certificate

namespace {

bool JudgeCoinbaseCertificate(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params,
                              const FinalityTracker::State& projected,
                              std::optional<modern::FinalityCertificatePair>& pair, std::string& error)
{
    pair.reset();
    if (block.vtx.empty()) { error = "finality-cert-no-coinbase"; return false; }
    if (!modern::MatchFinalityCertificateForEpoch(
            *block.vtx[0], [&](uint64_t e) { return projected.SetSize(e); }, pair, error)) {
        return false;
    }
    if (!pair) return true;
    if (!params.modern_pos) { error = "no-modern-pos-rules"; return false; }
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                          : std::nullopt};
    if (!domain) { error = "finality-domain-unpinned"; return false; }
    const modern::FinalityEpochView view{projected.View()};
    const auto hash_at{[&index](const int h) -> std::optional<uint256> {
        const CBlockIndex* anc{index.GetAncestor(h)};
        if (!anc) return std::nullopt;
        return anc->GetBlockHash();
    }};
    return modern::JudgeFinalityCertificate(*domain, pair->finalized_block, pair->certificate, index.nHeight, view,
                                            *params.modern_pos, hash_at, error);
}

} // namespace

bool FinalityTracker::CheckBlockCertificate(const CBlock& block, const CBlockIndex& index,
                                            const Consensus::Params& params, std::string& error) const
{
    if (!index.pprev || !Synced(index.pprev->GetBlockHash())) {
        error = "finality-state-unavailable";
        return false;
    }
    const State projected{Projected(index.nHeight, params)};
    std::optional<modern::FinalityCertificatePair> pair;
    return JudgeCoinbaseCertificate(block, index, params, projected, pair, error);
}

bool FinalityTracker::ApplyModern(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params)
{
    State s{Projected(index.nHeight, params)};
    std::optional<modern::FinalityCertificatePair> pair;
    std::string error;
    if (!JudgeCoinbaseCertificate(block, index, params, s, pair, error)) {
        LogWarning("FinalityTracker: connected block %s (height %d) carries an invalid certificate (%s)",
                   index.GetBlockHash().ToString(), index.nHeight, error);
        return false;
    }
    if (pair) {
        const auto& fb{pair->finalized_block};
        s.finalized = FinalizedCheckpoint{static_cast<int>(fb.height), fb.block_hash, fb.epoch, index.nHeight};
        if (fb.epoch == s.epoch) s.handover_certified = true;
    }
    m_state = std::move(s);
    return true;
}

// ---------------------------------------------------------------- sync

bool FinalityTracker::StepTrackers(const CBlock& block, const CBlockIndex& index, const CChain* chain,
                                   const BlockManager* blockman, const Consensus::Params& params)
{
    const uint256 parent{index.pprev ? index.pprev->GetBlockHash() : uint256{}};
    if (index.pprev && m_stakes.Synced(parent) && m_bindings.Synced(parent)) {
        m_stakes.BlockConnected(block, index, params);
        m_bindings.BlockConnected(block, index, params);
    } else {
        if (!chain || !blockman) return false;
        if (!m_stakes.Sync(*chain, *blockman, params, index) || !m_bindings.Sync(*chain, *blockman, params, index)) {
            return false;
        }
    }
    return m_stakes.Synced(index.GetBlockHash()) && m_bindings.Synced(index.GetBlockHash());
}

bool FinalityTracker::Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
                           const CBlockIndex& target)
{
    if (Synced(target.GetBlockHash())) return true;
    if (chain[target.nHeight] != &target) return false; // Target must lie on the chain.

    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height) return false; // No modern era configured: nothing to track.
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};

    int start{*final_height + 1};
    if (!m_dirty && m_synced_height >= *final_height && m_synced_height <= target.nHeight &&
        chain[m_synced_height] != nullptr && chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = m_synced_height + 1;
    } else {
        Reset();
        // Empty state "as of H": the first modern-era block extends it.
        m_synced_height = *final_height;
        if (const CBlockIndex* h{chain[*final_height]}) m_synced_tip = h->GetBlockHash();
    }

    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* pindex{chain[height]};
        CBlock block;
        if (pindex == nullptr || !blockman.ReadBlock(block, *pindex)) {
            LogWarning("FinalityTracker: cannot read modern block at height %d; finality state unavailable", height);
            Reset();
            return false;
        }
        if (modern_start && height >= *modern_start && !ApplyModern(block, *pindex, params)) {
            Reset();
            return false;
        }
        if (!StepTrackers(block, *pindex, &chain, &blockman, params)) {
            LogWarning("FinalityTracker: stake/binding state unavailable at height %d", height);
            Reset();
            return false;
        }
        m_synced_tip = pindex->GetBlockHash();
        m_synced_height = height;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void FinalityTracker::BlockConnected(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params)
{
    if (Consensus::GetB3Era(index.nHeight, params) != Consensus::B3Era::MODERN) return;
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    const std::optional<int> modern_start{Consensus::ModernPosStartHeight(params)};
    if (modern_start && index.nHeight >= *modern_start && !ApplyModern(block, index, params)) {
        m_dirty = true;
        return;
    }
    if (!StepTrackers(block, index, nullptr, nullptr, params)) {
        m_dirty = true;
        return;
    }
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

} // namespace node

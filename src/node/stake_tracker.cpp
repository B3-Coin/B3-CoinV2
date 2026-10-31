// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/stake_tracker.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/stake.h>
#include <node/blockstorage.h>

#include <string>

namespace node {

size_t StakeTracker::UndoDepth(const Consensus::Params& params)
{
    if (params.modern_pos && params.modern_pos->reorg_horizon) {
        return static_cast<size_t>(*params.modern_pos->reorg_horizon);
    }
    return DEFAULT_UNDO_DEPTH;
}

void StakeTracker::ApplyBlock(const CBlock& block, const uint256& hash, const int height, const size_t undo_depth)
{
    BlockUndo undo{.hash = hash, .ops = {}};
    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const CTxIn& in : tx->vin) {
                const auto it{m_stakes.find(in.prevout)};
                if (it == m_stakes.end()) continue;
                undo.ops.push_back(UndoOp{.outpoint = it->first, .prior = it->second});
                m_stakes.erase(it);
            }
        }
        for (uint32_t i{0}; i < tx->vout.size(); ++i) {
            const CTxOut& out{tx->vout[i]};
            if (!modern::ClaimsStakeMagic(out.scriptPubKey)) continue;
            std::string error;
            const auto view{modern::ParseStakeOutput(out, error)};
            // A connected modern block cannot contain an invalid claiming
            // output (ContextualCheckBlock enforces it); stay total anyway.
            if (!view) continue;
            const COutPoint outpoint{tx->GetHash(), i};
            if (m_stakes.emplace(outpoint, Entry{view->amount, view->validator_key, height}).second) {
                undo.ops.push_back(UndoOp{.outpoint = outpoint, .prior = std::nullopt});
            }
        }
    }
    if (undo_depth == 0) return;
    m_undo.push_back(std::move(undo));
    while (m_undo.size() > undo_depth) m_undo.pop_front();
}

bool StakeTracker::Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
                        const CBlockIndex& target)
{
    if (Synced(target.GetBlockHash())) return true;
    if (chain[target.nHeight] != &target) return false; // Target must lie on the chain.

    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height) return false; // No modern era configured: nothing to track.

    // Extend forward when synced to an ancestor on this same chain;
    // otherwise rebuild the whole modern span.
    int start{*final_height + 1};
    if (!m_dirty && m_synced_height >= *final_height && m_synced_height <= target.nHeight &&
        chain[m_synced_height] != nullptr &&
        chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = m_synced_height + 1;
    } else {
        m_stakes.clear();
        m_undo.clear();
        m_dirty = true;
        ++m_rebuilds;
    }

    const size_t undo_depth{UndoDepth(params)};
    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* pindex{chain[height]};
        CBlock block;
        if (pindex == nullptr || !blockman.ReadBlock(block, *pindex)) {
            LogWarning("StakeTracker: cannot read modern block at height %d; registry unavailable", height);
            m_stakes.clear();
            m_undo.clear();
            m_dirty = true;
            return false;
        }
        ApplyBlock(block, pindex->GetBlockHash(), height, undo_depth);
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void StakeTracker::BlockConnected(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params)
{
    if (Consensus::GetB3Era(index.nHeight, params) != Consensus::B3Era::MODERN) return;
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.pprev->GetBlockHash()) {
        // Not in step with this block's parent (first modern block, restart,
        // or a gap): fall back to the rebuild path on next Sync().
        m_dirty = true;
        return;
    }
    ApplyBlock(block, index.GetBlockHash(), index.nHeight, UndoDepth(params));
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

void StakeTracker::BlockDisconnected(const CBlockIndex& index)
{
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.GetBlockHash() ||
        m_undo.empty() || m_undo.back().hash != m_synced_tip) {
        // Not synced to this block, or its record is no longer kept (a
        // disconnect deeper than the history): rebuild on the next Sync().
        m_dirty = true;
        return;
    }
    // Reverse the block's changes in the opposite order of their application,
    // so an output created and spent inside the block ends absent.
    const BlockUndo& undo{m_undo.back()};
    for (auto op{undo.ops.rbegin()}; op != undo.ops.rend(); ++op) {
        if (op->prior) {
            m_stakes.emplace(op->outpoint, *op->prior);
        } else {
            m_stakes.erase(op->outpoint);
        }
    }
    m_undo.pop_back();
    m_synced_tip = index.pprev->GetBlockHash();
    m_synced_height = index.pprev->nHeight;
}

std::map<ValidatorKey, CAmount> StakeTracker::ActiveWeights(const int eval_height, CAmount& total) const
{
    std::map<ValidatorKey, CAmount> out;
    total = 0;
    for (const auto& [outpoint, entry] : m_stakes) {
        if (!modern::IsStakeMature(entry.creation_height, eval_height)) continue;
        out[entry.key] += entry.amount;
        total += entry.amount;
    }
    return out;
}

std::pair<CAmount, CAmount> StakeTracker::ActiveWeight(const ValidatorKey& key, const int eval_height) const
{
    CAmount w{0};
    CAmount total{0};
    for (const auto& [outpoint, entry] : m_stakes) {
        if (!modern::IsStakeMature(entry.creation_height, eval_height)) continue;
        total += entry.amount;
        if (entry.key == key) w += entry.amount;
    }
    return {w, total};
}

} // namespace node

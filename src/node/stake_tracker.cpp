// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/stake_tracker.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/stake.h>
#include <node/blockstorage.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <ios>
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

std::vector<unsigned char> StakeTracker::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter w{out, 0};
    const auto write_entry{[&w](const Entry& entry) {
        w << entry.amount << entry.key << int32_t{entry.creation_height};
    }};
    WriteCompactSize(w, m_stakes.size());
    for (const auto& [outpoint, entry] : m_stakes) {
        w << outpoint;
        write_entry(entry);
    }
    WriteCompactSize(w, m_undo.size());
    for (const BlockUndo& undo : m_undo) {
        w << undo.hash;
        WriteCompactSize(w, undo.ops.size());
        for (const UndoOp& op : undo.ops) {
            w << op.outpoint << uint8_t{op.prior.has_value()};
            if (op.prior) write_entry(*op.prior);
        }
    }
    return out;
}

std::optional<StakeTracker> StakeTracker::Decode(const std::span<const unsigned char> bytes, const CBlockIndex& block,
                                                 const Consensus::Params& params)
{
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height || block.nHeight < *final_height) return std::nullopt;
    // Every entry, kept or spent, was created by a modern block up to `block`.
    const auto read_entry{[&](SpanReader& r) -> std::optional<Entry> {
        Entry entry;
        int32_t height;
        r >> entry.amount >> entry.key >> height;
        entry.creation_height = height;
        if (!MoneyRange(entry.amount) || height <= *final_height || height > block.nHeight) return std::nullopt;
        return entry;
    }};
    StakeTracker tracker;
    try {
        SpanReader r{bytes};
        CAmount total{0};
        const uint64_t entries{ReadCompactSize(r)};
        for (uint64_t i{0}; i < entries; ++i) {
            COutPoint outpoint;
            r >> outpoint;
            const auto entry{read_entry(r)};
            if (!entry) return std::nullopt;
            // Strictly ascending outpoints: a registry has exactly one encoding.
            if (!tracker.m_stakes.empty() && !(tracker.m_stakes.rbegin()->first < outpoint)) return std::nullopt;
            // No more stake than money: ActiveWeights' sums cannot overflow.
            total += entry->amount;
            if (!MoneyRange(total)) return std::nullopt;
            tracker.m_stakes.emplace_hint(tracker.m_stakes.end(), outpoint, *entry);
        }
        const uint64_t records{ReadCompactSize(r)};
        if (records > UndoDepth(params)) return std::nullopt;
        for (uint64_t i{0}; i < records; ++i) {
            BlockUndo undo;
            r >> undo.hash;
            const uint64_t ops{ReadCompactSize(r)};
            for (uint64_t j{0}; j < ops; ++j) {
                UndoOp op;
                uint8_t has_prior;
                r >> op.outpoint >> has_prior;
                if (has_prior > 1) return std::nullopt;
                if (has_prior) {
                    op.prior = read_entry(r);
                    if (!op.prior) return std::nullopt;
                }
                undo.ops.push_back(std::move(op));
            }
            tracker.m_undo.push_back(std::move(undo));
        }
        if (!r.empty()) return std::nullopt;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    // The undo records name the chain's modern blocks ending at `block`,
    // newest last, as BlockDisconnected takes them.
    const CBlockIndex* index{&block};
    for (auto undo{tracker.m_undo.rbegin()}; undo != tracker.m_undo.rend(); ++undo) {
        if (index == nullptr || index->nHeight <= *final_height || index->GetBlockHash() != undo->hash) return std::nullopt;
        index = index->pprev;
    }
    tracker.m_synced_tip = block.GetBlockHash();
    tracker.m_synced_height = block.nHeight;
    tracker.m_dirty = false;
    return tracker;
}

bool StakeTracker::SameState(const StakeTracker& rebuilt) const
{
    if (m_dirty || rebuilt.m_dirty || m_synced_tip != rebuilt.m_synced_tip ||
        m_synced_height != rebuilt.m_synced_height || m_stakes != rebuilt.m_stakes ||
        m_undo.size() > rebuilt.m_undo.size()) {
        return false;
    }
    return std::equal(m_undo.begin(), m_undo.end(), rebuilt.m_undo.end() - static_cast<std::ptrdiff_t>(m_undo.size()));
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

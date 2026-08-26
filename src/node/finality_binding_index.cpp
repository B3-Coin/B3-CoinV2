// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_binding_index.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/mpa.h>
#include <node/blockstorage.h>

#include <cassert>
#include <map>

namespace node {

std::optional<modern::BindingRecord> FinalityBindingIndex::Get(const modern::ValidatorKeyBytes& validator_key) const
{
    const auto it{m_by_validator.find(validator_key)};
    if (it == m_by_validator.end()) return std::nullopt;
    return it->second;
}

std::optional<modern::ValidatorKeyBytes> FinalityBindingIndex::OwnerOf(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    if (modern::IsZeroBlsKey(bls_pubkey)) return std::nullopt;
    const auto it{m_owner_of_key.find(bls_pubkey)};
    if (it == m_owner_of_key.end()) return std::nullopt;
    return it->second;
}

modern::BlsKeyOwnerLookup FinalityBindingIndex::OwnerLookup() const
{
    return [this](const modern::BlsPubkeyBytes& pk) { return OwnerOf(pk); };
}

void FinalityBindingIndex::Set(const modern::ValidatorKeyBytes& validator_key, const modern::BindingRecord& record)
{
    // Release the validator's previous nonzero key, then claim the new one.
    if (const auto it{m_by_validator.find(validator_key)}; it != m_by_validator.end() && !it->second.IsRevoked()) {
        const auto owner{m_owner_of_key.find(it->second.bls_pubkey)};
        if (owner != m_owner_of_key.end() && owner->second == validator_key) m_owner_of_key.erase(owner);
    }
    m_by_validator[validator_key] = record;
    if (!record.IsRevoked()) m_owner_of_key[record.bls_pubkey] = validator_key;
}

void FinalityBindingIndex::Restore(const UndoEntry& undo)
{
    // Release whatever the validator holds now.
    if (const auto it{m_by_validator.find(undo.validator_key)}; it != m_by_validator.end()) {
        if (!it->second.IsRevoked()) {
            const auto owner{m_owner_of_key.find(it->second.bls_pubkey)};
            if (owner != m_owner_of_key.end() && owner->second == undo.validator_key) m_owner_of_key.erase(owner);
        }
        m_by_validator.erase(it);
    }
    // Reinstate the previous record, if there was one.
    if (undo.previous) {
        m_by_validator[undo.validator_key] = *undo.previous;
        if (!undo.previous->IsRevoked()) m_owner_of_key[undo.previous->bls_pubkey] = undo.validator_key;
    }
}

void FinalityBindingIndex::ConnectBlock(const int height, const std::vector<Transition>& transitions)
{
    assert(m_heights.empty() || height > m_heights.back());
    std::vector<UndoEntry> undo;
    undo.reserve(transitions.size());
    for (const Transition& t : transitions) {
        undo.push_back(UndoEntry{t.validator_key, Get(t.validator_key)});
        Set(t.validator_key, t.record);
    }
    m_heights.push_back(height);
    m_undo.push_back(std::move(undo));
}

void FinalityBindingIndex::DisconnectBlock(const int height)
{
    assert(!m_heights.empty() && m_heights.back() == height);
    const std::vector<UndoEntry>& undo{m_undo.back()};
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) Restore(*it);
    m_undo.pop_back();
    m_heights.pop_back();
}

void FinalityBindingIndex::Clear()
{
    m_by_validator.clear();
    m_owner_of_key.clear();
    m_heights.clear();
    m_undo.clear();
}

std::map<modern::ValidatorKeyBytes, modern::BindingRecord> FinalityBindingIndex::SnapshotActive() const
{
    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> out;
    for (const auto& [vk, rec] : m_by_validator) {
        if (!rec.IsRevoked()) out.emplace(vk, rec);
    }
    return out;
}


bool VerifyBlockFinalityBindings(const CBlock& block, const int height, const uint256& chain_domain,
                                 const Consensus::Params& params, const FinalityBindingIndex& index,
                                 std::vector<FinalityBindingIndex::Transition>& out, std::string& error)
{
    out.clear();
    // In-block overlay: transitions accepted earlier in this block.
    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> pending;
    std::map<modern::BlsPubkeyBytes, modern::ValidatorKeyBytes> pending_owner;
    const auto previous_of = [&](const modern::ValidatorKeyBytes& vk) -> std::optional<modern::BindingRecord> {
        if (const auto it{pending.find(vk)}; it != pending.end()) return it->second;
        return index.Get(vk);
    };
    const modern::BlsKeyOwnerLookup owner_of = [&](const modern::BlsPubkeyBytes& pk) -> std::optional<modern::ValidatorKeyBytes> {
        if (const auto it{pending_owner.find(pk)}; it != pending_owner.end()) return it->second;
        const auto owner{index.OwnerOf(pk)};
        if (!owner) return std::nullopt;
        // The index owner may have moved off this key earlier in this block.
        if (const auto p{pending.find(*owner)}; p != pending.end() && p->second.bls_pubkey != pk) return std::nullopt;
        return owner;
    };
    std::vector<FinalityBindingIndex::Transition> collected;
    for (const CTransactionRef& tx : block.vtx) {
        std::vector<modern::FinalityKeyPair> pairs;
        if (!modern::MatchFinalityKeyPairs(*tx, pairs, error)) return false;
        for (const auto& pair : pairs) {
            modern::ValidatorKeyBytes vk;
            std::copy(pair.commitment.begin(), pair.commitment.end(), vk.begin());
            const auto prev{previous_of(vk)};
            const auto check{modern::CheckFinalityKeyTransition(chain_domain, pair.commitment, pair.params, pair.evidence,
                                                                prev, owner_of)};
            if (check != modern::FinalityKeyCheck::OK) {
                error = std::string{"finality-key-"} + modern::FinalityKeyCheckName(check);
                return false;
            }
            modern::BindingRecord rec;
            rec.bls_pubkey = pair.params.bls_pubkey;
            rec.seq = pair.params.seq;
            rec.height = height;
            // Update the overlay (release the validator's previous pending/index key).
            if (prev && !prev->IsRevoked()) {
                if (const auto po{pending_owner.find(prev->bls_pubkey)}; po != pending_owner.end() && po->second == vk) {
                    pending_owner.erase(po);
                }
            }
            pending[vk] = rec;
            if (!rec.IsRevoked()) pending_owner[rec.bls_pubkey] = vk;
            collected.push_back(FinalityBindingIndex::Transition{vk, rec});
        }
    }
    out = std::move(collected);
    return true;
}

bool FinalityBindingTracker::ApplyBlock(const CBlock& block, const int height, const Consensus::Params& params)
{
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, params.legacy_final_hash.value_or(uint256{}))};
    if (!params.legacy_final_hash || !domain) {
        m_dirty = true;
        return false;
    }
    std::vector<FinalityBindingIndex::Transition> transitions;
    std::string error;
    if (!VerifyBlockFinalityBindings(block, height, *domain, params, m_index, transitions, error)) {
        LogWarning("FinalityBindingTracker: block at height %d failed binding verification (%s); index unavailable",
                   height, error);
        m_index.Clear();
        m_dirty = true;
        return false;
    }
    if (!transitions.empty()) m_index.ConnectBlock(height, transitions);
    return true;
}

bool FinalityBindingTracker::Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
                                  const CBlockIndex& target)
{
    if (Synced(target.GetBlockHash())) return true;
    if (chain[target.nHeight] != &target) return false;
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height) return false;
    int start{*final_height + 1};
    if (!m_dirty && m_synced_height >= *final_height && m_synced_height <= target.nHeight &&
        chain[m_synced_height] != nullptr && chain[m_synced_height]->GetBlockHash() == m_synced_tip) {
        start = m_synced_height + 1;
    } else {
        m_index.Clear();
        m_dirty = true;
    }
    for (int height{start}; height <= target.nHeight; ++height) {
        const CBlockIndex* pindex{chain[height]};
        CBlock block;
        if (pindex == nullptr || !blockman.ReadBlock(block, *pindex)) {
            LogWarning("FinalityBindingTracker: cannot read modern block at height %d; index unavailable", height);
            m_index.Clear();
            m_dirty = true;
            return false;
        }
        if (!ApplyBlock(block, height, params)) return false;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void FinalityBindingTracker::BlockConnected(const CBlock& block, const CBlockIndex& index, const Consensus::Params& params)
{
    if (Consensus::GetB3Era(index.nHeight, params) != Consensus::B3Era::MODERN) return;
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    if (!ApplyBlock(block, index.nHeight, params)) return;
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

void FinalityBindingTracker::BlockDisconnected(const CBlockIndex& index)
{
    if (m_dirty || m_synced_tip != index.GetBlockHash() || index.pprev == nullptr) {
        m_dirty = true;
        return;
    }
    if (m_index.ConnectedHeight() == index.nHeight) m_index.DisconnectBlock(index.nHeight);
    m_synced_tip = index.pprev->GetBlockHash();
    m_synced_height = index.pprev->nHeight;
}

} // namespace node

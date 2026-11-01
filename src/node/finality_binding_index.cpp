// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_binding_index.h>

#include <chain.h>
#include <consensus/era.h>
#include <hash.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/mpa.h>
#include <node/blockstorage.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <cassert>
#include <ios>
#include <map>
#include <set>

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
        // B0b (P1): keep the key's proof. A zero one (a revocation, or a
        // transition built without a proof) is never kept, so KeyPops()
        // reports a key without one.
        if (std::ranges::any_of(t.pop, [](const unsigned char c) { return c != 0; })) {
            m_pops.emplace(t.record.bls_pubkey, t.pop);
        }
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
    m_pops.clear();
}

std::map<modern::ValidatorKeyBytes, modern::BindingRecord> FinalityBindingIndex::SnapshotActive() const
{
    std::map<modern::ValidatorKeyBytes, modern::BindingRecord> out;
    for (const auto& [vk, rec] : m_by_validator) {
        if (!rec.IsRevoked()) out.emplace(vk, rec);
    }
    return out;
}

namespace {
template <typename Stream>
void WriteBindingRecord(Stream& s, const modern::BindingRecord& record)
{
    s << record.bls_pubkey << record.seq << int32_t{record.height};
}

template <typename Stream>
modern::BindingRecord ReadBindingRecord(Stream& s)
{
    modern::BindingRecord record;
    int32_t height;
    s >> record.bls_pubkey >> record.seq >> height;
    record.height = height;
    return record;
}
} // namespace

std::vector<unsigned char> FinalityBindingIndex::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter w{out, 0};
    WriteCompactSize(w, m_by_validator.size());
    for (const auto& [vk, record] : m_by_validator) {
        w << vk;
        WriteBindingRecord(w, record);
    }
    WriteCompactSize(w, m_heights.size());
    for (size_t i{0}; i < m_heights.size(); ++i) {
        w << int32_t{m_heights[i]};
        WriteCompactSize(w, m_undo[i].size());
        for (const UndoEntry& entry : m_undo[i]) {
            w << entry.validator_key << uint8_t{entry.previous.has_value()};
            if (entry.previous) WriteBindingRecord(w, *entry.previous);
        }
    }
    return out;
}

std::optional<FinalityBindingIndex> FinalityBindingIndex::Decode(const std::span<const unsigned char> bytes,
                                                                  const int first_height, const int last_height)
{
    FinalityBindingIndex index;
    try {
        SpanReader r{bytes};
        const uint64_t records{ReadCompactSize(r)};
        for (uint64_t i{0}; i < records; ++i) {
            modern::ValidatorKeyBytes vk;
            r >> vk;
            // Strictly ascending keys: an index has exactly one encoding.
            if (!index.m_by_validator.empty() && !(index.m_by_validator.rbegin()->first < vk)) return std::nullopt;
            index.m_by_validator.emplace_hint(index.m_by_validator.end(), vk, ReadBindingRecord(r));
        }
        const uint64_t blocks{ReadCompactSize(r)};
        for (uint64_t i{0}; i < blocks; ++i) {
            int32_t height;
            r >> height;
            if (height < first_height || height > last_height) return std::nullopt;
            if (!index.m_heights.empty() && height <= index.m_heights.back()) return std::nullopt;
            std::vector<UndoEntry> undo;
            const uint64_t entries{ReadCompactSize(r)};
            for (uint64_t j{0}; j < entries; ++j) {
                UndoEntry entry;
                uint8_t has_previous;
                r >> entry.validator_key >> has_previous;
                if (has_previous > 1) return std::nullopt;
                if (has_previous) entry.previous = ReadBindingRecord(r);
                undo.push_back(std::move(entry));
            }
            index.m_heights.push_back(height);
            index.m_undo.push_back(std::move(undo));
        }
        if (!r.empty()) return std::nullopt;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    // Replay the trail's shape: each entry's previous record is the one the
    // validator's entry before it set, so ConnectBlock could have built it.
    std::map<modern::ValidatorKeyBytes, int> set_at;
    for (size_t i{0}; i < index.m_heights.size(); ++i) {
        for (const UndoEntry& entry : index.m_undo[i]) {
            const auto it{set_at.find(entry.validator_key)};
            if (it == set_at.end() ? entry.previous.has_value()
                                   : !entry.previous || entry.previous->height != it->second) {
                return std::nullopt;
            }
            set_at[entry.validator_key] = index.m_heights[i];
        }
    }
    if (set_at.size() != index.m_by_validator.size()) return std::nullopt;
    for (const auto& [vk, record] : index.m_by_validator) {
        const auto it{set_at.find(vk)};
        if (it == set_at.end() || it->second != record.height) return std::nullopt;
        if (!record.IsRevoked() && !index.m_owner_of_key.emplace(record.bls_pubkey, vk).second) return std::nullopt;
    }
    return index;
}

bool FinalityBindingIndex::SameState(const FinalityBindingIndex& other) const
{
    return m_by_validator == other.m_by_validator && m_owner_of_key == other.m_owner_of_key &&
           m_heights == other.m_heights && m_undo == other.m_undo;
}

std::set<modern::BlsPubkeyBytes> FinalityBindingIndex::HeldKeys() const
{
    std::set<modern::BlsPubkeyBytes> keys;
    for (const auto& [vk, record] : m_by_validator) {
        if (!record.IsRevoked()) keys.insert(record.bls_pubkey);
    }
    for (const std::vector<UndoEntry>& block : m_undo) {
        for (const UndoEntry& entry : block) {
            if (entry.previous && !entry.previous->IsRevoked()) keys.insert(entry.previous->bls_pubkey);
        }
    }
    return keys;
}

std::optional<std::vector<std::pair<modern::BlsPubkeyBytes, FinalityBindingIndex::PopBytes>>>
FinalityBindingIndex::KeyPops() const
{
    const std::set<modern::BlsPubkeyBytes> keys{HeldKeys()};
    std::vector<std::pair<modern::BlsPubkeyBytes, PopBytes>> out;
    out.reserve(keys.size());
    for (const modern::BlsPubkeyBytes& key : keys) {
        const auto it{m_pops.find(key)};
        if (it == m_pops.end()) return std::nullopt;
        out.emplace_back(key, it->second);
    }
    return out;
}

std::optional<std::vector<unsigned char>> FinalityBindingIndex::EncodePops() const
{
    const auto pops{KeyPops()};
    if (!pops) return std::nullopt;
    std::vector<unsigned char> out;
    VectorWriter w{out, 0};
    WriteCompactSize(w, pops->size());
    for (const auto& [key, pop] : *pops) w << key << pop;
    return out;
}

bool FinalityBindingIndex::DecodePops(const std::span<const unsigned char> bytes)
{
    std::map<modern::BlsPubkeyBytes, PopBytes> pops;
    try {
        SpanReader r{bytes};
        const uint64_t count{ReadCompactSize(r)};
        for (uint64_t i{0}; i < count; ++i) {
            modern::BlsPubkeyBytes key;
            PopBytes pop;
            r >> key >> pop;
            // Strictly ascending keys and no zero proof: only what
            // EncodePops() writes, in its one order.
            if (!pops.empty() && !(pops.rbegin()->first < key)) return false;
            if (std::ranges::none_of(pop, [](const unsigned char c) { return c != 0; })) return false;
            pops.emplace_hint(pops.end(), key, pop);
        }
        if (!r.empty()) return false;
    } catch (const std::ios_base::failure&) {
        return false;
    }
    // Exactly the keys this index holds: none missing, none foreign.
    const std::set<modern::BlsPubkeyBytes> held{HeldKeys()};
    if (!std::ranges::equal(held, pops, {}, {}, [](const auto& entry) { return entry.first; })) return false;
    m_pops = std::move(pops);
    return true;
}

bool FinalityBindingOverlay::ApplyTransaction(
    const CTransaction& tx,
    std::vector<FinalityBindingIndex::Transition>& out,
    std::string& error)
{
    out.clear();
    std::vector<modern::FinalityKeyPair> pairs;
    if (!modern::MatchFinalityKeyPairs(tx, pairs, error)) return false;

    const auto previous_of = [&](const modern::ValidatorKeyBytes& vk)
        -> std::optional<modern::BindingRecord> {
        if (const auto it{m_pending.find(vk)}; it != m_pending.end()) return it->second;
        return m_base->Get(vk);
    };
    const modern::BlsKeyOwnerLookup owner_of = [&](const modern::BlsPubkeyBytes& pk)
        -> std::optional<modern::ValidatorKeyBytes> {
        if (const auto it{m_pending_owner.find(pk)}; it != m_pending_owner.end()) return it->second;
        const auto owner{m_base->OwnerOf(pk)};
        if (!owner) return std::nullopt;
        // The confirmed owner may have moved off this key earlier in the
        // candidate block.
        if (const auto pending{m_pending.find(*owner)};
            pending != m_pending.end() && pending->second.bls_pubkey != pk) {
            return std::nullopt;
        }
        return owner;
    };

    std::vector<FinalityBindingIndex::Transition> collected;
    collected.reserve(pairs.size());
    for (const auto& pair : pairs) {
        modern::ValidatorKeyBytes vk;
        std::copy(pair.commitment.begin(), pair.commitment.end(), vk.begin());
        const auto previous{previous_of(vk)};
        const auto check{modern::CheckFinalityKeyTransition(
            m_chain_domain, pair.commitment, pair.params, pair.evidence,
            previous, owner_of)};
        if (check != modern::FinalityKeyCheck::OK) {
            error = std::string{"finality-key-"} + modern::FinalityKeyCheckName(check);
            return false;
        }

        modern::BindingRecord record;
        record.bls_pubkey = pair.params.bls_pubkey;
        record.seq = pair.params.seq;
        record.height = m_height;
        // Release the validator's previous candidate/confirmed key, then
        // claim its new nonzero key in the candidate overlay.
        if (previous && !previous->IsRevoked()) {
            if (const auto owner{m_pending_owner.find(previous->bls_pubkey)};
                owner != m_pending_owner.end() && owner->second == vk) {
                m_pending_owner.erase(owner);
            }
        }
        m_pending[vk] = record;
        if (!record.IsRevoked()) m_pending_owner[record.bls_pubkey] = vk;
        collected.push_back(FinalityBindingIndex::Transition{vk, record, pair.evidence.pop});
    }
    out = std::move(collected);
    return true;
}


bool VerifyBlockFinalityBindings(const CBlock& block, const int height, const uint256& chain_domain,
                                 const Consensus::Params& params, const FinalityBindingIndex& index,
                                 std::vector<FinalityBindingIndex::Transition>& out, std::string& error)
{
    out.clear();
    (void)params;
    FinalityBindingOverlay overlay{index, height, chain_domain};
    std::vector<FinalityBindingIndex::Transition> collected;
    for (const CTransactionRef& tx : block.vtx) {
        std::vector<FinalityBindingIndex::Transition> tx_transitions;
        if (!overlay.ApplyTransaction(*tx, tx_transitions, error)) return false;
        collected.insert(collected.end(), tx_transitions.begin(), tx_transitions.end());
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

uint256 FinalityBindingTracker::ParamsDigest(const Consensus::Params& params)
{
    // Keep in step with ApplyBlock, Sync and BlockConnected: the era and the
    // replay start read legacy_b3coin and hard_fork_height, the chain domain
    // reads the genesis hash and legacy_final_hash, and
    // VerifyBlockFinalityBindings reads no parameter.
    HashWriter h{};
    h << std::string{"B3/derived-state/finality-bindings"} << params.hashGenesisBlock << params.legacy_b3coin
      << params.hard_fork_height.has_value() << int32_t{params.hard_fork_height.value_or(0)}
      << params.legacy_final_hash.has_value() << params.legacy_final_hash.value_or(uint256{});
    return h.GetSHA256();
}

void FinalityBindingTracker::Install(FinalityBindingIndex index, const CBlockIndex& block)
{
    m_index = std::move(index);
    m_synced_tip = block.GetBlockHash();
    m_synced_height = block.nHeight;
    m_dirty = false;
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
        ++m_rebuilds;
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

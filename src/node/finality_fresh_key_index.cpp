// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_fresh_key_index.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/mpa.h>
#include <node/blockstorage.h>
#include <node/finality_tracker.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <set>

namespace node {

// ------------------------------------------------------------------ index

bool FreshKeyBindingIndex::EverBound(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    return m_ever_bound.contains(bls_pubkey);
}

std::optional<int> FreshKeyBindingIndex::FirstBoundHeight(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    const auto it{m_ever_bound.find(bls_pubkey)};
    if (it == m_ever_bound.end()) return std::nullopt;
    return it->second;
}

std::vector<FreshKeyBinding> FreshKeyBindingIndex::Bindings(const modern::ValidatorKeyBytes& validator_key,
                                                            const uint64_t target_epoch, const int lo,
                                                            const int hi) const
{
    std::vector<FreshKeyBinding> out;
    const auto it{m_bindings.find(BindingKey{validator_key, target_epoch})};
    if (it == m_bindings.end()) return out;
    for (const FreshKeyBinding& b : it->second) {
        if (b.height >= lo && b.height <= hi) out.push_back(b);
    }
    return out;
}

std::vector<FreshKeyBindingEntry> FreshKeyBindingIndex::BindingsFrom(const uint64_t min_epoch) const
{
    std::vector<FreshKeyBindingEntry> out;
    for (const auto& [key, list] : m_bindings) {
        if (key.second < min_epoch) continue;
        for (const FreshKeyBinding& b : list) out.push_back(FreshKeyBindingEntry{key.first, key.second, b});
    }
    return out;
}

size_t FreshKeyBindingIndex::BindingCount() const
{
    size_t n{0};
    for (const auto& [key, list] : m_bindings) n += list.size();
    return n;
}

std::optional<uint64_t> FreshKeyBindingIndex::OwnFreshEpoch(const modern::ValidatorKeyBytes& validator_key,
                                                             const modern::BlsPubkeyBytes& bls_pubkey) const
{
    for (auto it{m_bindings.lower_bound(BindingKey{validator_key, 0})};
         it != m_bindings.end() && it->first.first == validator_key; ++it) {
        for (const FreshKeyBinding& b : it->second) {
            if (b.bls_pubkey == bls_pubkey) return it->first.second;
        }
    }
    return std::nullopt;
}

void FreshKeyBindingIndex::ConnectBlock(const int height, const std::vector<Claim>& claims)
{
    assert(height > m_last_height);
    m_last_height = height;
    for (const Claim& claim : claims) {
        // The first claim of a key wins; any later claim (either kind) is inert here.
        if (!m_ever_bound.emplace(claim.bls_pubkey, height).second) continue;
        if (claim.fresh) {
            m_bindings[BindingKey{claim.validator_key, claim.epoch}].push_back(FreshKeyBinding{claim.bls_pubkey, height});
        }
    }
}

void FreshKeyBindingIndex::Clear()
{
    m_ever_bound.clear();
    m_bindings.clear();
    m_last_height = -1;
}

// ----------------------------------------------------------------- claims

bool BlockKeyClaims(const CBlock& block, std::vector<FreshKeyBindingIndex::Claim>& out, std::string& error)
{
    out.clear();
    std::vector<FreshKeyBindingIndex::Claim> claims;
    for (const CTransactionRef& tx : block.vtx) {
        std::vector<modern::FinalityKeyPair> v1_pairs;
        if (!modern::MatchFinalityKeyPairs(*tx, v1_pairs, error)) return false;
        for (const modern::FinalityKeyPair& pair : v1_pairs) {
            if (modern::IsZeroBlsKey(pair.params.bls_pubkey)) continue; // a revocation claims no key
            FreshKeyBindingIndex::Claim claim;
            std::copy(pair.commitment.begin(), pair.commitment.end(), claim.validator_key.begin());
            claim.bls_pubkey = pair.params.bls_pubkey;
            claims.push_back(claim);
        }
        std::vector<modern::FreshFinalityKeyPair> fresh_pairs;
        if (!modern::MatchFreshFinalityKeyPairs(*tx, fresh_pairs, error)) return false;
        for (const modern::FreshFinalityKeyPair& pair : fresh_pairs) {
            FreshKeyBindingIndex::Claim claim;
            claim.fresh = true;
            std::copy(pair.commitment.begin(), pair.commitment.end(), claim.validator_key.begin());
            claim.epoch = pair.params.target_epoch;
            claim.bls_pubkey = pair.params.bls_pubkey;
            claims.push_back(claim);
        }
    }
    out = std::move(claims);
    return true;
}

// ---------------------------------------------------------------- tracker

bool FreshKeyBindingTracker::Apply(const CBlock& block, const int height, const Consensus::Params& params)
{
    std::vector<FreshKeyBindingIndex::Claim> claims;
    std::string error;
    bool ok{BlockKeyClaims(block, claims, error)};
    if (ok && std::any_of(claims.begin(), claims.end(), [](const auto& c) { return c.fresh; })) {
        // Re-verify before counting: a connected block carries fresh pairs
        // only from R_key, each with a valid identity signature and PoP.
        const auto domain{params.legacy_final_hash
                              ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
                              : std::nullopt};
        if (!params.FreshKeyRulesActive(height)) {
            error = "fresh-key-inactive";
            ok = false;
        } else if (!domain) {
            error = "fresh-key-domain-unpinned";
            ok = false;
        } else {
            ok = modern::VerifyFreshKeyBindingsInBlock(block, *domain, error);
        }
    }
    if (!ok) {
        LogWarning("FreshKeyBindingTracker: block at height %d failed fresh key verification (%s); index unavailable",
                   height, error);
        m_index.Clear();
        m_dirty = true;
        return false;
    }
    if (!claims.empty()) m_index.ConnectBlock(height, claims);
    return true;
}

bool FreshKeyBindingTracker::Sync(const CChain& chain, const BlockManager& blockman, const Consensus::Params& params,
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
            LogWarning("FreshKeyBindingTracker: cannot read modern block at height %d; index unavailable", height);
            m_index.Clear();
            m_dirty = true;
            return false;
        }
        if (!Apply(block, height, params)) return false;
    }
    m_synced_tip = target.GetBlockHash();
    m_synced_height = target.nHeight;
    m_dirty = false;
    return true;
}

void FreshKeyBindingTracker::BlockConnected(const CBlock& block, const CBlockIndex& index,
                                            const Consensus::Params& params)
{
    if (Consensus::GetB3Era(index.nHeight, params) != Consensus::B3Era::MODERN) return;
    if (m_dirty || index.pprev == nullptr || m_synced_tip != index.pprev->GetBlockHash()) {
        m_dirty = true;
        return;
    }
    if (!Apply(block, index.nHeight, params)) return;
    m_synced_tip = index.GetBlockHash();
    m_synced_height = index.nHeight;
}

// ------------------------------------------------------------------ seats

FreshSeat FreshSeatFor(const FreshKeyBindingIndex& index, const modern::ValidatorKeyBytes& validator_key,
                       const uint64_t target_epoch, const int lo, const int hi)
{
    FreshSeat seat;
    for (const FreshKeyBinding& b : index.Bindings(validator_key, target_epoch, lo, hi)) {
        seat.pubkeys.push_back(b.bls_pubkey);
    }
    seat.voided = seat.pubkeys.size() >= 2;
    return seat;
}

std::optional<std::pair<int, int>> FreshSeatWindow(const uint64_t target_epoch, const Consensus::Params& params,
                                                   const FreshLineageState* state, const int tip_height)
{
    if (!FreshKeyModeConfigured(params)) return std::nullopt;
    const Consensus::FreshLineageParams& fresh{*params.fresh_lineage};
    int lo{-1};
    int hi{-1};
    if (target_epoch == 0) {
        lo = *fresh.key_height;
        hi = std::min(fresh.GenesisHeight(), tip_height);
    } else if (target_epoch == 1) {
        return std::nullopt; // Set'_1 shares the genesis bindings
    } else {
        if (!state || !state->bootstrapped) return std::nullopt;
        const uint64_t k{target_epoch - 2};
        if (k > state->epoch || k >= state->epoch_starts.size()) return std::nullopt;
        lo = state->epoch_starts[k];
        const bool closed{k + 1 <= state->epoch && k + 1 < state->epoch_starts.size()};
        hi = closed ? state->epoch_starts[k + 1] - 1 : tip_height;
    }
    if (hi < lo) return std::nullopt;
    return std::make_pair(lo, hi);
}

std::optional<ValidatorSetSnapshot> BuildFreshCommittee(const std::map<ValidatorKey, CAmount>& active_weights,
                                                        const FinalityBindingIndex& v1_bindings,
                                                        const FreshKeyBindingIndex& fresh_keys,
                                                        const FreshCommitteeRequest& request)
{
    // Seat each member of Snapshot(snapshot_height) on its single counted
    // fresh key; ValidatorSetSnapshot::Build then applies the snapshot's own
    // weight rules (positive whole weight, ascending validator_key order)
    // over a binding index that holds only the seated fresh keys.
    std::vector<FinalityBindingIndex::Transition> seated;
    for (const auto& [vk, amount] : active_weights) {
        if (amount <= 0) continue;
        const auto v1{v1_bindings.Get(vk)};
        if (!v1 || v1->IsRevoked()) continue; // not a member of Snapshot(h)
        const FreshSeat seat{FreshSeatFor(fresh_keys, vk, request.target_epoch, request.window_lo,
                                          request.snapshot_height)};
        const auto key{seat.Key()};
        if (!key) continue; // sits out (no binding) or void (two or more)
        modern::BindingRecord record;
        record.bls_pubkey = *key;
        record.height = request.snapshot_height;
        seated.push_back(FinalityBindingIndex::Transition{vk, record});
    }
    if (seated.empty()) return std::nullopt;
    FinalityBindingIndex fresh_index;
    fresh_index.ConnectBlock(request.snapshot_height, seated);
    return ValidatorSetSnapshot::Build(0, active_weights, fresh_index);
}

// ----------------------------------------------------------------- policy

FreshBindingWindow FreshBindingWindowAt(const int height, const Consensus::Params& params,
                                        const std::optional<FreshLineageState>& projected)
{
    FreshBindingWindow w;
    if (!FreshKeyModeConfigured(params) || !params.FreshKeyRulesActive(height)) return w; // NOT_ACTIVE
    const Consensus::FreshLineageParams& fresh{*params.fresh_lineage};
    if (height <= fresh.GenesisHeight()) {
        // Genesis binding: one binding serves Set'_0 and Set'_1.
        w.status = FreshBindingWindow::Status::OPEN;
        w.target_epoch = 0;
        w.window_start = *fresh.key_height;
        return w;
    }
    if (!projected || !projected->bootstrapped || projected->lineage_broken || projected->epoch_starts.empty() ||
        projected->epoch > std::numeric_limits<uint64_t>::max() - 2) {
        w.status = FreshBindingWindow::Status::LINEAGE_UNAVAILABLE;
        return w;
    }
    w.status = FreshBindingWindow::Status::OPEN;
    w.target_epoch = projected->epoch + 2;
    w.window_start = projected->epoch_starts.back();
    return w;
}

const char* FreshBindingWindowStatusName(const FreshBindingWindow::Status status)
{
    switch (status) {
    case FreshBindingWindow::Status::OPEN: return "open";
    case FreshBindingWindow::Status::NOT_ACTIVE: return "not-active";
    case FreshBindingWindow::Status::LINEAGE_UNAVAILABLE: return "lineage-unavailable";
    }
    return "unknown";
}

bool CheckFreshKeyBindingPolicy(const CTransaction& tx, const std::vector<modern::FreshFinalityKeyPair>& pairs,
                                const FreshKeyBindingIndex& index, const FreshBindingWindow& window,
                                std::string& reason)
{
    // Keys this transaction claims ahead of each fresh pair: its V1 nonzero
    // keys first (claim order, BlockKeyClaims), then earlier fresh pairs.
    std::set<modern::BlsPubkeyBytes> claimed;
    {
        std::vector<modern::FinalityKeyPair> v1_pairs;
        std::string error;
        if (modern::MatchFinalityKeyPairs(tx, v1_pairs, error)) {
            for (const auto& pair : v1_pairs) {
                if (!modern::IsZeroBlsKey(pair.params.bls_pubkey)) claimed.insert(pair.params.bls_pubkey);
            }
        }
    }
    for (const modern::FreshFinalityKeyPair& pair : pairs) {
        if (index.EverBound(pair.params.bls_pubkey) || !claimed.insert(pair.params.bls_pubkey).second) {
            reason = "fresh-key-ever-bound";
            return false;
        }
        if (window.status == FreshBindingWindow::Status::LINEAGE_UNAVAILABLE) {
            reason = "fresh-key-lineage-unavailable";
            return false;
        }
        if (window.status != FreshBindingWindow::Status::OPEN || pair.params.target_epoch != window.target_epoch) {
            reason = "fresh-key-binding-outside-window";
            return false;
        }
    }
    return true;
}

// ----------------------------------------------------------------- status

FreshKeyChainStatus FreshKeyStatusAt(const FinalityTracker& tracker, const CChain& chain,
                                     const Consensus::Params& params,
                                     const std::optional<modern::ValidatorKeyBytes>& validator_key,
                                     const std::vector<modern::BlsPubkeyBytes>& probe_keys)
{
    FreshKeyChainStatus out;
    out.configured = FreshKeyModeConfigured(params);
    if (!out.configured) return out;
    const Consensus::FreshLineageParams& fresh{*params.fresh_lineage};
    out.key_height = fresh.key_height;
    out.activation_height = fresh.activation_height;
    if (params.legacy_final_hash) {
        out.chain_domain = modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash);
    }
    const CBlockIndex* tip{chain.Tip()};
    if (!tip) return out;
    out.tip_height = tip->nHeight;
    const FreshKeyBindingIndex* index{tracker.FreshKeys(params)};
    if (!index || !tracker.Synced(tip->GetBlockHash())) return out;
    out.available = true;

    const std::optional<FreshLineageState>& current{tracker.Current().fresh};
    const FreshLineageState* state{current && current->bootstrapped ? &*current : nullptr};
    if (state) {
        out.bootstrapped = true;
        out.broken = state->lineage_broken;
        out.epoch = state->epoch;
    }

    // The window of the next block: below R_f it needs no projection.
    const int next_height{tip->nHeight + 1};
    std::optional<FreshLineageState> projected;
    if (next_height > fresh.GenesisHeight()) projected = tracker.Projected(next_height, params).fresh;
    const FreshBindingWindow window{FreshBindingWindowAt(next_height, params, projected)};
    out.window_status = window.status;
    if (window.status == FreshBindingWindow::Status::OPEN) {
        out.next_binding_epoch = window.target_epoch;
        out.window_start = window.window_start;
    }

    // Bindings naming e - 2 or later, each marked counted (inside the window
    // of its target) and void (two or more counted for one seat).
    const uint64_t min_epoch{state && state->epoch >= 2 ? state->epoch - 2 : 0};
    std::map<uint64_t, std::optional<std::pair<int, int>>> windows;
    std::map<std::pair<modern::ValidatorKeyBytes, uint64_t>, size_t> counted;
    for (const FreshKeyBindingEntry& entry : index->BindingsFrom(min_epoch)) {
        if (validator_key && entry.validator_key != *validator_key) continue;
        auto w{windows.find(entry.target_epoch)};
        if (w == windows.end()) {
            w = windows.emplace(entry.target_epoch, FreshSeatWindow(entry.target_epoch, params, state, tip->nHeight)).first;
        }
        FreshKeyChainStatus::Binding binding;
        binding.validator_key = entry.validator_key;
        binding.epoch = entry.target_epoch;
        binding.bls_pubkey = entry.binding.bls_pubkey;
        binding.height = entry.binding.height;
        binding.counted = w->second && binding.height >= w->second->first && binding.height <= w->second->second;
        if (binding.counted) ++counted[{binding.validator_key, binding.epoch}];
        out.bindings.push_back(binding);
    }
    for (FreshKeyChainStatus::Binding& binding : out.bindings) {
        binding.voided = binding.counted && counted[{binding.validator_key, binding.epoch}] >= 2;
    }

    // The first claim of every probed key (any epoch).
    for (const modern::BlsPubkeyBytes& pk : probe_keys) {
        FreshKeyChainStatus::ProbeClaim claim;
        claim.bls_pubkey = pk;
        claim.first_height = index->FirstBoundHeight(pk);
        if (claim.first_height && validator_key) claim.own_epoch = index->OwnFreshEpoch(*validator_key, pk);
        out.probe_claims.push_back(claim);
    }

    // The committees that can still sign or be signed for.
    if (state) {
        for (const auto* set : {state->previous.get(), state->current.get(), state->next.get()}) {
            if (!set || !set->members) continue;
            for (const ValidatorSetMember& member : set->members->Members()) {
                if (validator_key && member.validator_key != *validator_key) continue;
                out.seats.push_back(
                    FreshKeyChainStatus::Seat{set->Epoch(), member.validator_key, member.bls_pubkey, member.weight});
            }
        }
    }
    return out;
}

} // namespace node

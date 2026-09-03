// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_vault_index.h>

#include <chain.h>
#include <consensus/era.h>
#include <logging.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/chain_domain.h>
#include <node/blockstorage.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/fn_seat_index.h>
#include <sync.h>
#include <validation.h>

#include <algorithm>
#include <utility>

namespace node {

std::optional<FlowMeshVaultRecord> FlowMeshVaultIndex::Get(
    const COutPoint& outpoint) const
{
    const auto it{m_live.find(outpoint)};
    if (it == m_live.end()) return std::nullopt;
    return it->second;
}

std::optional<FlowMeshMarketRecord> FlowMeshVaultIndex::Market(
    const flowmesh::MarketId& market_id) const
{
    const auto it{std::find_if(m_markets.begin(), m_markets.end(),
                               [&](const auto& item) {
                                   return item.second.market_id == market_id;
                               })};
    return it == m_markets.end()
               ? std::nullopt
               : std::optional<FlowMeshMarketRecord>{it->second};
}

std::optional<FlowMeshMarketRecord> FlowMeshVaultIndex::MarketAt(
    const flowmesh::MarketId& market_id, const CBlockIndex& anchor) const
{
    const auto market{Market(market_id)};
    if (!market || market->created_height > anchor.nHeight) {
        return std::nullopt;
    }

    const auto anchor_delta{std::find_if(
        m_history.begin(), m_history.end(), [&](const auto& delta) {
            return delta.height == anchor.nHeight;
        })};
    if (anchor_delta == m_history.end() ||
        anchor_delta->block_hash != anchor.GetBlockHash()) {
        return std::nullopt;
    }

    const auto creation{std::find_if(
        m_history.begin(), m_history.end(), [&](const auto& delta) {
            return delta.height == market->created_height &&
                   delta.block_hash == market->created_block;
        })};
    if (creation == m_history.end() ||
        std::find(creation->markets_added.begin(),
                  creation->markets_added.end(), *market) ==
            creation->markets_added.end()) {
        return std::nullopt;
    }
    return market;
}

bool FlowMeshVaultIndex::VerifyBlock(
    const CBlock& block, const int height, const uint256& block_hash,
    const Consensus::Params& params, FlowMeshVaultBlockDelta& out,
    std::string& error) const
{
    out = FlowMeshVaultBlockDelta{};
    if (!Consensus::FlowMeshVaultPreparationRulesActive(height, params)) {
        error = "FlowMesh vault history is not active";
        return false;
    }
    if (block_hash.IsNull()) {
        error = "FlowMesh vault block hash is null";
        return false;
    }

    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(
                                params.hashGenesisBlock,
                                *params.legacy_final_hash)
                          : std::nullopt};
    const auto fn_asset{modern::ConfiguredFnAssetId(params)};
    if (!domain || !fn_asset) {
        error = "FlowMesh market domain is unavailable";
        return false;
    }

    auto scratch{m_live};
    auto market_scratch{m_markets};
    std::map<flowmesh::VaultId, FlowMeshMarketRecord> introduced;
    std::vector<FlowMeshVaultRecord> removed_from_parent;
    std::vector<FlowMeshVaultRecord> created_in_order;
    for (const CTransactionRef& tx : block.vtx) {
        // A same-transaction/same-block child sees input removals before new
        // outputs, exactly like the UTXO transition it shadows.
        for (const CTxIn& input : tx->vin) {
            const auto existing{scratch.find(input.prevout)};
            if (existing == scratch.end()) continue;
            const FlowMeshVaultRecord removed{existing->second};
            scratch.erase(existing);
            if (m_live.contains(removed.outpoint)) {
                removed_from_parent.push_back(removed);
            }
        }

        for (size_t output_index{0}; output_index < tx->vout.size();
             ++output_index) {
            const CTxOut& txout{tx->vout[output_index]};
            if (!modern::ClaimsAssetOutput(txout)) continue;
            std::string parse_error;
            const auto parsed{modern::ViewAssetAwareOutput(
                txout, height, params, parse_error)};
            if (!parsed) {
                error = "FlowMesh vault output parse failed at output " +
                        std::to_string(output_index) + ": " + parse_error;
                return false;
            }
            if (parsed->policy_type !=
                static_cast<uint16_t>(modern::PolicyType::DEX_VAULT)) {
                continue;
            }
            const auto vault_params{modern::ParseVaultParams(
                parsed->policy_params)};
            if (!vault_params ||
                !modern::CheckVaultParams(parsed->policy_commitment,
                                          parsed->policy_params)) {
                error = "FlowMesh vault output has invalid semantic params";
                return false;
            }
            if (output_index > UINT32_MAX) {
                error = "FlowMesh vault output index exceeds u32";
                return false;
            }
            const COutPoint outpoint{
                tx->GetHash(), static_cast<uint32_t>(output_index)};
            if (scratch.contains(outpoint)) {
                error = "duplicate FlowMesh vault outpoint";
                return false;
            }
            FlowMeshVaultRecord record;
            record.outpoint = outpoint;
            record.asset = parsed->asset;
            record.amount = parsed->amount;
            record.vault_id = parsed->policy_commitment;
            record.kind = vault_params->kind;
            record.shard = vault_params->shard;
            record.account = vault_params->account;
            record.created_height = height;
            record.created_block = block_hash;

            const auto established{market_scratch.find(record.vault_id)};
            if (record.kind == modern::VAULT_KIND_USER_DEPOSIT) {
                if (record.asset == modern::NativeAsset()) {
                    if (established == market_scratch.end()) {
                        error = "native FlowMesh deposit precedes its colored market deposit";
                        return false;
                    }
                } else {
                    if (record.asset == *fn_asset) {
                        error = "FN Coin cannot be a FlowMesh spot-market base";
                        return false;
                    }
                    const auto market{flowmesh::ComputeFlowMeshMarketId(
                        *domain, record.asset)};
                    const auto vault{market
                                         ? flowmesh::ComputeFlowMeshVaultId(
                                               *domain, *market)
                                         : std::nullopt};
                    if (!market || !vault || *vault != record.vault_id) {
                        error = "colored FlowMesh deposit names the wrong vault";
                        return false;
                    }
                    if (established == market_scratch.end()) {
                        FlowMeshMarketRecord market_record{
                            record.asset, *market, *vault, record.outpoint,
                            height, block_hash};
                        market_scratch.emplace(*vault, market_record);
                        introduced.emplace(*vault, std::move(market_record));
                    } else if (established->second.base_asset != record.asset ||
                               established->second.market_id != *market) {
                        error = "FlowMesh vault already belongs to a different base asset";
                        return false;
                    }
                }
            } else {
                if (established == market_scratch.end()) {
                    error = "FlowMesh pool change precedes market establishment";
                    return false;
                }
            }
            const auto resolved{market_scratch.find(record.vault_id)};
            if (resolved == market_scratch.end() ||
                (record.asset != resolved->second.base_asset &&
                 record.asset != modern::NativeAsset())) {
                error = "FlowMesh vault carries an asset outside its market pair";
                return false;
            }
            scratch.emplace(outpoint, record);
            created_in_order.push_back(record);
        }
    }

    out.height = height;
    out.block_hash = block_hash;
    out.removed = std::move(removed_from_parent);
    // Outputs created and consumed inside one block are absent at the block
    // boundary and therefore cancel from its durable history delta.
    for (const FlowMeshVaultRecord& record : created_in_order) {
        const auto live{scratch.find(record.outpoint)};
        if (live != scratch.end() && live->second == record) {
            out.added.push_back(record);
        }
    }
    for (const auto& [vault_id, market] : introduced) {
        (void)vault_id;
        const auto live{scratch.find(market.establishing_deposit)};
        if (live == scratch.end() ||
            live->second.kind != modern::VAULT_KIND_USER_DEPOSIT ||
            live->second.asset != market.base_asset ||
            live->second.vault_id != market.vault_id) {
            error = "FlowMesh market-establishing deposit is not live at the block boundary";
            return false;
        }
        out.markets_added.push_back(market);
    }
    return true;
}

bool FlowMeshVaultIndex::ConnectBlock(const FlowMeshVaultBlockDelta& delta,
                                      std::string& error)
{
    if (delta.height < 0 || delta.block_hash.IsNull()) {
        error = "invalid FlowMesh vault block delta identity";
        return false;
    }
    if (!m_history.empty() && delta.height != m_history.back().height + 1) {
        error = "non-contiguous FlowMesh vault block delta";
        return false;
    }
    auto next{m_live};
    auto next_markets{m_markets};
    for (const FlowMeshVaultRecord& removed : delta.removed) {
        const auto existing{next.find(removed.outpoint)};
        if (existing == next.end() || !(existing->second == removed)) {
            error = "FlowMesh vault delta removes the wrong outpoint";
            return false;
        }
        next.erase(existing);
    }
    for (const FlowMeshVaultRecord& added : delta.added) {
        if (!next.emplace(added.outpoint, added).second) {
            error = "FlowMesh vault delta duplicates an outpoint";
            return false;
        }
    }
    for (const FlowMeshMarketRecord& market : delta.markets_added) {
        const auto duplicate_market{std::find_if(
            next_markets.begin(), next_markets.end(), [&](const auto& item) {
                return item.second.market_id == market.market_id ||
                       item.second.base_asset == market.base_asset;
            })};
        const auto establishing{next.find(market.establishing_deposit)};
        if (market.created_height != delta.height ||
            market.created_block != delta.block_hash ||
            market.base_asset.IsNull() ||
            market.base_asset == modern::NativeAsset() ||
            market.market_id.IsNull() || market.vault_id.IsNull() ||
            duplicate_market != next_markets.end() ||
            establishing == next.end() ||
            establishing->second.asset != market.base_asset ||
            establishing->second.vault_id != market.vault_id ||
            establishing->second.kind !=
                modern::VAULT_KIND_USER_DEPOSIT ||
            !establishing->second.account ||
            establishing->second.account->IsNull() ||
            establishing->second.shard !=
                modern::FlowMeshUserDepositShard(
                    market.vault_id, *establishing->second.account) ||
            std::find(delta.added.begin(), delta.added.end(),
                      establishing->second) == delta.added.end() ||
            !next_markets.emplace(market.vault_id, market).second) {
            error = "FlowMesh vault delta has an invalid market establishment";
            return false;
        }
    }
    m_live = std::move(next);
    m_markets = std::move(next_markets);
    m_history.push_back(delta); // empty A2+ blocks are consensus history too
    return true;
}

bool FlowMeshVaultIndex::DisconnectBlock(const int height,
                                         const uint256& block_hash,
                                         std::string& error)
{
    if (m_history.empty() || m_history.back().height != height ||
        m_history.back().block_hash != block_hash) {
        error = "FlowMesh vault disconnect does not match the index tip";
        return false;
    }
    const FlowMeshVaultBlockDelta& delta{m_history.back()};
    auto previous{m_live};
    auto previous_markets{m_markets};
    for (auto it{delta.added.rbegin()}; it != delta.added.rend(); ++it) {
        const auto existing{previous.find(it->outpoint)};
        if (existing == previous.end() || !(existing->second == *it)) {
            error = "FlowMesh vault undo cannot remove its addition";
            return false;
        }
        previous.erase(existing);
    }
    for (auto it{delta.removed.rbegin()}; it != delta.removed.rend(); ++it) {
        if (!previous.emplace(it->outpoint, *it).second) {
            error = "FlowMesh vault undo cannot restore its removal";
            return false;
        }
    }
    for (auto it{delta.markets_added.rbegin()};
         it != delta.markets_added.rend(); ++it) {
        const auto existing{previous_markets.find(it->vault_id)};
        if (existing == previous_markets.end() ||
            !(existing->second == *it)) {
            error = "FlowMesh vault undo cannot remove its market";
            return false;
        }
        previous_markets.erase(existing);
    }
    m_live = std::move(previous);
    m_markets = std::move(previous_markets);
    m_history.pop_back();
    return true;
}

std::optional<FlowMeshVaultRecord> FlowMeshVaultIndex::LookupAt(
    const COutPoint& outpoint, const CBlockIndex& anchor) const
{
    std::optional<FlowMeshVaultRecord> state{Get(outpoint)};
    bool found_anchor{false};
    for (auto delta{m_history.rbegin()}; delta != m_history.rend(); ++delta) {
        if (delta->height == anchor.nHeight) {
            if (delta->block_hash != anchor.GetBlockHash()) return std::nullopt;
            found_anchor = true;
            break;
        }
        if (delta->height < anchor.nHeight) return std::nullopt;
        for (auto added{delta->added.rbegin()}; added != delta->added.rend();
             ++added) {
            if (added->outpoint != outpoint) continue;
            if (!state || !(*state == *added)) return std::nullopt;
            state.reset();
        }
        for (auto removed{delta->removed.rbegin()};
             removed != delta->removed.rend(); ++removed) {
            if (removed->outpoint != outpoint) continue;
            if (state) return std::nullopt;
            state = *removed;
        }
    }
    if (!found_anchor) return std::nullopt;
    return state;
}

std::vector<FlowMeshMarketRecord> FlowMeshVaultIndex::MarketsAt(
    const CBlockIndex& anchor) const
{
    std::vector<FlowMeshMarketRecord> out;
    const auto anchor_delta{std::find_if(
        m_history.begin(), m_history.end(), [&](const auto& delta) {
            return delta.height == anchor.nHeight;
        })};
    if (anchor_delta == m_history.end() ||
        anchor_delta->block_hash != anchor.GetBlockHash()) {
        return out;
    }
    for (const auto& [vault_id, market] : m_markets) {
        (void)vault_id;
        if (market.created_height <= anchor.nHeight &&
            MarketAt(market.market_id, anchor)) {
            out.push_back(market);
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.market_id < b.market_id;
    });
    return out;
}

std::optional<std::vector<FlowMeshVaultRecord>>
FlowMeshVaultIndex::LargestWithdrawalInputsAt(
    const flowmesh::VaultId& vault_id, const modern::AssetId& asset,
    const CBlockIndex& anchor) const
{
    if (vault_id.IsNull() || anchor.nHeight < 0) {
        return std::nullopt;
    }
    const auto anchor_delta{std::find_if(
        m_history.begin(), m_history.end(), [&](const auto& delta) {
            return delta.height == anchor.nHeight;
        })};
    if (anchor_delta == m_history.end() ||
        anchor_delta->block_hash != anchor.GetBlockHash()) {
        return std::nullopt;
    }

    const auto better = [](const FlowMeshVaultRecord& a,
                           const FlowMeshVaultRecord& b) {
        return a.amount > b.amount ||
               (a.amount == b.amount && a.outpoint < b.outpoint);
    };
    std::vector<FlowMeshVaultRecord> selected;
    selected.reserve(flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS);
    const auto consider = [&](const FlowMeshVaultRecord& record) {
        if (record.created_height > anchor.nHeight ||
            record.vault_id != vault_id || record.asset != asset ||
            record.kind != modern::VAULT_KIND_POOL_CHANGE ||
            record.account || record.amount <= 0 ||
            record.amount > MAX_MONEY) {
            return;
        }
        const auto duplicate{std::find_if(
            selected.begin(), selected.end(), [&](const auto& existing) {
                return existing.outpoint == record.outpoint;
            })};
        if (duplicate != selected.end()) return;
        const auto position{
            std::lower_bound(selected.begin(), selected.end(), record,
                             better)};
        if (position == selected.end() &&
            selected.size() ==
                flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS) {
            return;
        }
        selected.insert(position, record);
        if (selected.size() >
            flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS) {
            selected.pop_back();
        }
    };

    // Every output live at `anchor` is either still live at the index tip,
    // or appears exactly once as a removal after the anchor. Enumerating
    // those two disjoint sources avoids reconstructing an unbounded live map;
    // only the best 64 records are ever retained.
    for (const auto& [outpoint, record] : m_live) {
        (void)outpoint;
        consider(record);
    }
    for (auto delta{m_history.rbegin()}; delta != m_history.rend(); ++delta) {
        if (delta->height <= anchor.nHeight) break;
        for (const FlowMeshVaultRecord& removed : delta->removed) {
            consider(removed);
        }
    }
    return selected;
}

std::optional<CAmount> FlowMeshVaultIndex::WithdrawalCapacityAt(
    const flowmesh::VaultId& vault_id, const modern::AssetId& asset,
    const CBlockIndex& anchor) const
{
    const auto selected{LargestWithdrawalInputsAt(vault_id, asset, anchor)};
    if (!selected) return std::nullopt;
    CAmount capacity{0};
    for (const FlowMeshVaultRecord& record : *selected) {
        if (record.amount > MAX_MONEY - capacity) return std::nullopt;
        capacity += record.amount;
    }
    return capacity;
}

std::optional<flowmesh::DepositInfo> FlowMeshVaultIndex::ResolveDepositAt(
    const COutPoint& outpoint, const CBlockIndex& anchor,
    const uint256& domain, const modern::AssetId& base_asset,
    const flowmesh::MarketId& expected_market,
    const int vault_preparation_height,
    const int flowmesh_activation_height) const
{
    if (vault_preparation_height < 0 ||
        flowmesh_activation_height < vault_preparation_height ||
        anchor.nHeight < vault_preparation_height || domain.IsNull() ||
        base_asset.IsNull() || base_asset == modern::NativeAsset()) {
        return std::nullopt;
    }
    const auto derived_market{
        flowmesh::ComputeFlowMeshMarketId(domain, base_asset)};
    if (!derived_market || *derived_market != expected_market) {
        return std::nullopt;
    }
    const auto expected_vault{
        flowmesh::ComputeFlowMeshVaultId(domain, *derived_market)};
    if (!expected_vault) return std::nullopt;

    const auto registered_market{MarketAt(expected_market, anchor)};
    if (!registered_market ||
        registered_market->base_asset != base_asset ||
        registered_market->vault_id != *expected_vault) {
        return std::nullopt;
    }

    const auto record{LookupAt(outpoint, anchor)};
    if (!record || record->created_height < vault_preparation_height ||
        record->created_height > anchor.nHeight ||
        record->vault_id != *expected_vault ||
        record->kind != modern::VAULT_KIND_USER_DEPOSIT ||
        !record->account || record->account->IsNull() ||
        (record->asset != base_asset &&
         record->asset != modern::NativeAsset()) ||
        record->amount <= 0 || record->amount > MAX_MONEY ||
        record->shard != modern::FlowMeshUserDepositShard(
                             record->vault_id, *record->account)) {
        return std::nullopt;
    }

    // Do not trust a detached record even if an in-memory caller somehow
    // supplied one: its immutable creation fact must be present in the exact
    // retained active-chain block delta that introduced it.
    const auto creation{std::find_if(
        m_history.begin(), m_history.end(), [&](const auto& delta) {
            return delta.height == record->created_height;
        })};
    if (creation == m_history.end() ||
        creation->block_hash != record->created_block ||
        std::find(creation->added.begin(), creation->added.end(), *record) ==
            creation->added.end()) {
        return std::nullopt;
    }
    return flowmesh::DepositInfo{record->asset, record->amount,
                                 *record->account};
}

void FlowMeshVaultIndex::Clear()
{
    m_live.clear();
    m_markets.clear();
    m_history.clear();
}

bool FlowMeshVaultTracker::ApplyBlock(const CBlock& block,
                                      const CBlockIndex& index,
                                      const Consensus::Params& params)
{
    FlowMeshVaultBlockDelta delta;
    std::string error;
    if (!m_index.VerifyBlock(block, index.nHeight, index.GetBlockHash(), params,
                             delta, error) ||
        !m_index.ConnectBlock(delta, error)) {
        LogWarning(
            "FlowMeshVaultTracker: block at height %d failed vault-history verification (%s); index unavailable",
            index.nHeight, error);
        m_index.Clear();
        m_dirty = true;
        return false;
    }
    return true;
}

bool FlowMeshVaultTracker::Sync(const CChain& chain,
                                const BlockManager& blockman,
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
            LogWarning(
                "FlowMeshVaultTracker: cannot read A2+ block at height %d; index unavailable",
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

void FlowMeshVaultTracker::BlockConnected(const CBlock& block,
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

void FlowMeshVaultTracker::BlockDisconnected(
    const CBlockIndex& index, const Consensus::Params& params)
{
    if (m_dirty || m_synced_tip != index.GetBlockHash() ||
        index.pprev == nullptr) {
        m_dirty = true;
        return;
    }
    if (Consensus::FlowMeshSeatBindingScheduleConfigured(params) &&
        index.nHeight >= *params.asset_activation_height) {
        std::string error;
        if (!m_index.DisconnectBlock(index.nHeight, index.GetBlockHash(),
                                     error)) {
            LogWarning(
                "FlowMeshVaultTracker: failed to disconnect height %d (%s); index unavailable",
                index.nHeight, error);
            m_index.Clear();
            m_dirty = true;
            return;
        }
    }
    m_synced_tip = index.pprev->GetBlockHash();
    m_synced_height = index.pprev->nHeight;
}

std::optional<flowmesh::DepositInfo> ChainDepositVerifier::GetDeposit(
    const COutPoint& outpoint, const flowmesh::AnchorRef& anchor) const
{
    if (anchor.height < 0 || anchor.hash.IsNull()) return std::nullopt;
    LOCK(::cs_main);
    const Consensus::Params& params{m_chainstate.m_chainman.GetConsensus()};
    // A deposit may be created and selected from an A2 anchor so it is
    // already deep enough when A3 opens. The runtime itself remains gated on
    // full A3 rules; this verifier only resolves the immutable chain fact.
    if (!Consensus::FlowMeshVaultPreparationRulesActive(anchor.height,
                                                        params) ||
        m_chainstate.m_assumeutxo != Assumeutxo::VALIDATED) {
        return std::nullopt;
    }
    const CBlockIndex* anchor_index{
        m_chainstate.m_blockman.LookupBlockIndex(anchor.hash)};
    if (anchor_index == nullptr || anchor_index->nHeight != anchor.height ||
        m_chainstate.m_chain[anchor.height] != anchor_index) {
        return std::nullopt;
    }
    const CBlockIndex* tip{m_chainstate.m_chain.Tip()};
    if (tip == nullptr) return std::nullopt;
    FlowMeshVaultTracker& tracker{m_chainstate.ModernFlowMeshVaults()};
    if (!tracker.Sync(m_chainstate.m_chain, m_chainstate.m_blockman, params,
                      *tip)) {
        return std::nullopt; // pruned/unavailable history fails closed
    }
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    const auto fn_asset{modern::ConfiguredFnAssetId(params)};
    if (!domain || (fn_asset && m_base_asset == *fn_asset)) {
        return std::nullopt;
    }
    return tracker.Index().ResolveDepositAt(
        outpoint, *anchor_index, *domain, m_base_asset, m_market_id,
        *params.asset_activation_height,
        *params.flowmesh_activation_height);
}

std::optional<CAmount> ChainDepositVerifier::GetWithdrawalCapacity(
    const modern::AssetId& asset, const flowmesh::AnchorRef& anchor) const
{
    if (anchor.height < 0 || anchor.hash.IsNull() ||
        (asset != m_base_asset && asset != modern::NativeAsset())) {
        return std::nullopt;
    }
    LOCK(::cs_main);
    const Consensus::Params& params{m_chainstate.m_chainman.GetConsensus()};
    // The runtime is A3-gated, but its newest acceptable anchor is 30 blocks
    // older. Capacity is an immutable vault fact, so the A2 preparation gate
    // is the correct boundary just as it is for deposit resolution.
    if (!Consensus::FlowMeshVaultPreparationRulesActive(anchor.height,
                                                        params) ||
        m_chainstate.m_assumeutxo != Assumeutxo::VALIDATED) {
        return std::nullopt;
    }
    const CBlockIndex* anchor_index{
        m_chainstate.m_blockman.LookupBlockIndex(anchor.hash)};
    if (anchor_index == nullptr || anchor_index->nHeight != anchor.height ||
        m_chainstate.m_chain[anchor.height] != anchor_index) {
        return std::nullopt;
    }
    const CBlockIndex* tip{m_chainstate.m_chain.Tip()};
    if (tip == nullptr) return std::nullopt;
    FlowMeshVaultTracker& tracker{m_chainstate.ModernFlowMeshVaults()};
    if (!tracker.Sync(m_chainstate.m_chain, m_chainstate.m_blockman, params,
                      *tip)) {
        return std::nullopt;
    }
    const auto domain{params.legacy_final_hash
                          ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                      *params.legacy_final_hash)
                          : std::nullopt};
    const auto expected_market{
        domain ? flowmesh::ComputeFlowMeshMarketId(*domain, m_base_asset)
               : std::nullopt};
    const auto expected_vault{
        domain ? flowmesh::ComputeFlowMeshVaultId(*domain, m_market_id)
               : std::nullopt};
    const auto registered{
        tracker.Index().MarketAt(m_market_id, *anchor_index)};
    if (!domain || !expected_market || *expected_market != m_market_id ||
        !expected_vault || !registered ||
        registered->base_asset != m_base_asset ||
        registered->vault_id != *expected_vault) {
        return std::nullopt;
    }
    return tracker.Index().WithdrawalCapacityAt(*expected_vault, asset,
                                                *anchor_index);
}

std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
ChainDepositVerifier::GetWithdrawalSettlements(
    const std::optional<flowmesh::AnchorRef>& after_exclusive,
    const flowmesh::AnchorRef& through_inclusive) const
{
    // Sequence zero has no prior B3 interval and therefore cannot settle an
    // earlier FlowMesh withdrawal.
    if (!after_exclusive) {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }
    if (after_exclusive->height < 0 || after_exclusive->hash.IsNull() ||
        through_inclusive.height < 0 || through_inclusive.hash.IsNull()) {
        return std::nullopt;
    }
    if (*after_exclusive == through_inclusive) {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }
    if (after_exclusive->height >= through_inclusive.height) {
        return std::nullopt;
    }

    LOCK(::cs_main);
    const Consensus::Params& params{m_chainstate.m_chainman.GetConsensus()};
    const CBlockIndex* after_index{
        m_chainstate.m_blockman.LookupBlockIndex(after_exclusive->hash)};
    const CBlockIndex* through_index{
        m_chainstate.m_blockman.LookupBlockIndex(through_inclusive.hash)};
    if (after_index == nullptr || through_index == nullptr ||
        after_index->nHeight != after_exclusive->height ||
        through_index->nHeight != through_inclusive.height ||
        m_chainstate.m_chain[after_index->nHeight] != after_index ||
        m_chainstate.m_chain[through_index->nHeight] != through_index ||
        through_index->GetAncestor(after_index->nHeight) != after_index) {
        return std::nullopt;
    }

    // Sequence zero is deliberately anchored during the A2 preparation
    // runway. Immediately after its checkpoint connects, Current() can still
    // be pre-A3 for up to 29 blocks. That canonical interval is known empty:
    // type-9 withdrawals are consensus-invalid before A3. Requiring the
    // through-anchor itself to be A3 here would keep every market paused until
    // A3 was 30 blocks deep and defeat the ruled A2 -> A3 runway.
    if (FlowMeshIntervalProvablyHasNoWithdrawals(
            after_exclusive->height, through_inclusive.height, params)) {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }
    if (!Consensus::FlowMeshRulesActive(through_inclusive.height, params) ||
        m_chainstate.m_assumeutxo != Assumeutxo::VALIDATED) {
        return std::nullopt;
    }
    const CBlockIndex* settlement_after{after_index};
    if (after_index->nHeight < *params.flowmesh_activation_height) {
        // The pre-A3 prefix is consensus-proven empty. Clamp a mixed interval
        // to A3-1 so the A3+ checkpoint index can scan A3 itself without
        // requiring it to retain impossible pre-activation deltas.
        const int boundary_height{*params.flowmesh_activation_height - 1};
        settlement_after = m_chainstate.m_chain[boundary_height];
        if (settlement_after == nullptr ||
            through_index->GetAncestor(boundary_height) != settlement_after) {
            return std::nullopt;
        }
    }
    const CBlockIndex* tip{m_chainstate.m_chain.Tip()};
    if (tip == nullptr) return std::nullopt;

    FnSeatTracker& seats{m_chainstate.ModernFnSeats()};
    FlowMeshVaultTracker& vaults{m_chainstate.ModernFlowMeshVaults()};
    FlowMeshCheckpointTracker& checkpoints{
        m_chainstate.ModernFlowMeshCheckpoints()};
    if (!seats.Sync(m_chainstate.m_chain, m_chainstate.m_blockman, params,
                    *tip) ||
        !vaults.Sync(m_chainstate.m_chain, m_chainstate.m_blockman, params,
                     *tip) ||
        !checkpoints.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                          params, seats.Index(), vaults.Index(), *tip)) {
        return std::nullopt;
    }
    std::vector<flowmesh::WithdrawalSettlementFactV1> out;
    std::string error;
    if (!checkpoints.Index().WithdrawalSettlementsBetween(
            m_market_id, *settlement_after, *through_index, out, error)) {
        return std::nullopt;
    }
    return out;
}

std::optional<flowmesh::WithdrawalSettlementPlan>
ChainDepositVerifier::PlanWithdrawalSettlements(
    const std::optional<flowmesh::AnchorRef>& after_exclusive,
    const flowmesh::AnchorRef& through_inclusive) const
{
    // Sequence zero has no earlier liability interval. The runtime pins its
    // bootstrap anchor independently, so no history scan is required here.
    if (!after_exclusive) {
        return flowmesh::WithdrawalSettlementPlan{through_inclusive, 0};
    }
    if (after_exclusive->height < 0 || after_exclusive->hash.IsNull() ||
        through_inclusive.height < 0 || through_inclusive.hash.IsNull()) {
        return std::nullopt;
    }
    if (*after_exclusive == through_inclusive) {
        return flowmesh::WithdrawalSettlementPlan{through_inclusive, 0};
    }
    if (after_exclusive->height >= through_inclusive.height) {
        return std::nullopt;
    }

    LOCK(::cs_main);
    const Consensus::Params& params{m_chainstate.m_chainman.GetConsensus()};
    const CBlockIndex* after_index{
        m_chainstate.m_blockman.LookupBlockIndex(after_exclusive->hash)};
    const CBlockIndex* through_index{
        m_chainstate.m_blockman.LookupBlockIndex(through_inclusive.hash)};
    if (after_index == nullptr || through_index == nullptr ||
        after_index->nHeight != after_exclusive->height ||
        through_index->nHeight != through_inclusive.height ||
        m_chainstate.m_chain[after_index->nHeight] != after_index ||
        m_chainstate.m_chain[through_index->nHeight] != through_index ||
        through_index->GetAncestor(after_index->nHeight) != after_index) {
        return std::nullopt;
    }
    if (FlowMeshIntervalProvablyHasNoWithdrawals(
            after_exclusive->height, through_inclusive.height, params)) {
        return flowmesh::WithdrawalSettlementPlan{through_inclusive, 0};
    }
    if (!Consensus::FlowMeshRulesActive(through_inclusive.height, params) ||
        m_chainstate.m_assumeutxo != Assumeutxo::VALIDATED) {
        return std::nullopt;
    }

    const CBlockIndex* settlement_after{after_index};
    if (after_index->nHeight < *params.flowmesh_activation_height) {
        const int boundary_height{*params.flowmesh_activation_height - 1};
        settlement_after = m_chainstate.m_chain[boundary_height];
        if (settlement_after == nullptr ||
            through_index->GetAncestor(boundary_height) != settlement_after) {
            return std::nullopt;
        }
    }
    const CBlockIndex* tip{m_chainstate.m_chain.Tip()};
    if (tip == nullptr) return std::nullopt;

    FnSeatTracker& seats{m_chainstate.ModernFnSeats()};
    FlowMeshVaultTracker& vaults{m_chainstate.ModernFlowMeshVaults()};
    FlowMeshCheckpointTracker& checkpoints{
        m_chainstate.ModernFlowMeshCheckpoints()};
    if (!seats.Sync(m_chainstate.m_chain, m_chainstate.m_blockman, params,
                    *tip) ||
        !vaults.Sync(m_chainstate.m_chain, m_chainstate.m_blockman, params,
                     *tip) ||
        !checkpoints.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                          params, seats.Index(), vaults.Index(), *tip)) {
        return std::nullopt;
    }

    int selected_height{-1};
    size_t selected_count{0};
    std::string error;
    if (!checkpoints.Index().WithdrawalSettlementCatchupHeight(
            m_market_id, *settlement_after, *through_index,
            flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY,
            selected_height, selected_count, error)) {
        return std::nullopt;
    }
    const CBlockIndex* selected{m_chainstate.m_chain[selected_height]};
    if (selected == nullptr ||
        through_index->GetAncestor(selected_height) != selected) {
        return std::nullopt;
    }
    return flowmesh::WithdrawalSettlementPlan{
        flowmesh::AnchorRef{selected->nHeight, selected->GetBlockHash()},
        selected_count};
}

} // namespace node

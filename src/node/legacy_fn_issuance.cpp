// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/legacy_fn_issuance.h>

#include <chain.h>
#include <consensus/boundary.h>
#include <modern/chain_domain.h>
#include <modern/fn_genesis.h>
#include <node/blockstorage.h>
#include <node/fn_pod.h>
#include <primitives/block.h>
#include <sync.h>
#include <tinyformat.h>
#include <undo.h>
#include <validation.h>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace node {

bool BuildLegacyFnGenesisManifestFromRecords(
    const Consensus::Params& params,
    const int final_height,
    const uint256& final_hash,
    const std::span<const PodRecord> records,
    const LegacyFnBlockAt& block_at,
    LegacyFnGenesisManifest& out,
    std::string& error)
{
    error.clear();
    if (final_height < 1 || final_height == std::numeric_limits<int>::max()) {
        error = "FN Genesis final legacy height is out of range";
        return false;
    }
    if (final_hash.IsNull()) {
        error = "FN Genesis final legacy hash X is null";
        return false;
    }
    if (!params.hard_fork_height || *params.hard_fork_height != final_height + 1) {
        error = "FN Genesis height is not the configured H+1";
        return false;
    }
    if (params.legacy_final_hash && *params.legacy_final_hash != final_hash) {
        error = "FN Genesis X contradicts the configured final legacy hash";
        return false;
    }
    if (!block_at) {
        error = "FN Genesis block source is unavailable";
        return false;
    }

    const auto chain_domain{modern::ModernChainDomain(params.hashGenesisBlock, final_hash)};
    if (!chain_domain) {
        error = "FN Genesis chain domain cannot be derived";
        return false;
    }

    // Bind the supplied records to the same sealed history that binds the
    // root. This is intentionally repeated even when the caller already ran
    // the UTXO-equivalence check: an artifact builder must never accept an
    // unrelated block source by accident.
    std::map<int, CBlock> blocks;
    CBlock final_block;
    if (!block_at(final_height, final_block)) {
        error = strprintf("FN Genesis block data unavailable at final legacy height %d",
                          final_height);
        return false;
    }
    if (final_block.GetMarkerHash(params) != final_hash) {
        error = "FN Genesis block source does not carry X at H";
        return false;
    }
    blocks.emplace(final_height, std::move(final_block));

    LegacyFnGenesisManifest built;
    built.chain_domain = *chain_domain;
    built.genesis_height = static_cast<uint32_t>(final_height + 1);
    built.manifest_version = modern::FN_GENESIS_MANIFEST_VERSION_V1;
    built.rights.reserve(records.size());

    for (const PodRecord& record : records) {
        if (record.height < 1 || record.height > final_height) {
            error = strprintf("FN Genesis PoD %s has height %d outside sealed prefix [1, %d]",
                              record.pod_id.ToString(), record.height, final_height);
            return false;
        }

        auto it{blocks.find(record.height)};
        if (it == blocks.end()) {
            CBlock block;
            if (!block_at(record.height, block)) {
                error = strprintf("FN Genesis block data unavailable at PoD height %d",
                                  record.height);
                return false;
            }
            it = blocks.emplace(record.height, std::move(block)).first;
        }

        const CTransaction* pod_tx{nullptr};
        for (const CTransactionRef& tx : it->second.vtx) {
            if (tx->GetHash() != record.pod_id) continue;
            if (pod_tx != nullptr) {
                error = strprintf("FN Genesis PoD %s appears more than once in its block",
                                  record.pod_id.ToString());
                return false;
            }
            pod_tx = tx.get();
        }
        if (pod_tx == nullptr) {
            error = strprintf("FN Genesis PoD %s is absent from its recorded block",
                              record.pod_id.ToString());
            return false;
        }

        const std::optional<uint32_t> recipient_vout{
            modern::FindLegacyFnRecipientVout(*pod_tx)};
        if (!recipient_vout) {
            // A qualifying value-gap event without the historical client's
            // exact 1-old-B3 P2PKH designation never established an FN right.
            continue;
        }
        const CScript& script{pod_tx->vout[*recipient_vout].scriptPubKey};
        // FindLegacyFnRecipientVout already proved the frozen 25-byte form:
        // OP_DUP OP_HASH160 PUSH20 <HASH160> OP_EQUALVERIFY OP_CHECKSIG.
        Consensus::FnGenesisRight right;
        right.pod_id = record.pod_id.ToUint256();
        std::copy_n(script.begin() + 3, right.recipient_key_hash.size(),
                    right.recipient_key_hash.begin());
        built.rights.push_back(right);
    }

    std::sort(built.rights.begin(), built.rights.end(),
              [](const Consensus::FnGenesisRight& a,
                 const Consensus::FnGenesisRight& b) {
                  return a.pod_id.Compare(b.pod_id) < 0;
              });
    const auto root{modern::ComputeFnGenesisManifestRootV1(
        built.chain_domain, built.genesis_height, built.rights, &error)};
    if (!root) return false; // includes empty, over-cap, and duplicate-PoD rejection
    built.root = *root;

    out = std::move(built);
    error.clear();
    return true;
}

bool BuildLegacyFnGenesisManifest(ChainstateManager& chainman,
                                  LegacyFnGenesisManifest& out,
                                  std::string& error)
{
    error.clear();
    LOCK(cs_main);
    const Consensus::Params& params{chainman.GetConsensus()};
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height || !params.legacy_final_hash) {
        error = "legacy boundary is not pinned; there is no sealed prefix for FN Genesis";
        return false;
    }
    const CChain& chain{chainman.ActiveChain()};
    const CBlockIndex* at_h{chain[*final_height]};
    if (!at_h || at_h->GetBlockHash() != *params.legacy_final_hash) {
        error = "active chain does not carry X at the final legacy height";
        return false;
    }

    const LegacyFnBlockAt read_block{[&](const int height, CBlock& block) {
        const CBlockIndex* pindex{chain[height]};
        return pindex != nullptr && chainman.m_blockman.ReadBlock(block, *pindex);
    }};

    std::vector<PodRecord> all_records;
    for (int height{1}; height <= *final_height; ++height) {
        CBlock block;
        if (!read_block(height, block)) {
            error = strprintf("block data unavailable at height %d (redownload or full "
                              "reindex required)", height);
            return false;
        }
        if (block.vtx.size() <= 1) continue;
        CBlockUndo undo;
        if (!chainman.m_blockman.ReadBlockUndo(undo, *chain[height])) {
            error = strprintf("undo data unavailable at height %d (reindex required)", height);
            return false;
        }
        std::vector<PodRecord> records;
        if (!DerivePodRecords(block, undo, height, params, records, error)) return false;
        for (PodRecord& record : records) all_records.push_back(std::move(record));
    }

    LegacyFnGenesisManifest built;
    if (!BuildLegacyFnGenesisManifestFromRecords(
            params, *final_height, *params.legacy_final_hash, all_records,
            read_block, built, error)) {
        return false;
    }
    out = std::move(built);
    return true;
}

} // namespace node

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/legacy_fn_issuance.h>

#include <chain.h>
#include <consensus/boundary.h>
#include <node/blockstorage.h>
#include <node/fn_pod.h>
#include <primitives/block.h>
#include <sync.h>
#include <tinyformat.h>
#include <undo.h>
#include <validation.h>

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace node {

bool BuildAllLegacyFnIssuances(ChainstateManager& chainman,
                               std::vector<LegacyFnIssuanceCandidate>& out, std::string& error)
{
    error.clear();
    out.clear();
    LOCK(cs_main);
    const Consensus::Params& params{chainman.GetConsensus()};
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height || !params.legacy_final_hash) {
        error = "legacy boundary is not pinned; there is no sealed prefix to issue from";
        return false;
    }
    const CChain& chain{chainman.ActiveChain()};
    const CBlockIndex* at_h{chain[*final_height]};
    if (!at_h || at_h->GetBlockHash() != *params.legacy_final_hash) {
        error = "active chain does not carry X at the final legacy height";
        return false;
    }

    const auto read_block{[&](const int height, CBlock& block) {
        const CBlockIndex* pindex{chain[height]};
        return pindex != nullptr && chainman.m_blockman.ReadBlock(block, *pindex);
    }};

    // ---- Pass 1: discovery. The undo-based detector (the same one the
    // PoD scan uses) finds qualifying disintegrations; the issuance
    // eligibility filter — the 1-coin P2PKH designation — is applied on
    // the actual transaction, and ignored PoDs are dropped here. Collect
    // the funding txids the proofs will embed.
    struct Candidate {
        int32_t height;
        uint32_t position;
    };
    std::vector<Candidate> candidates;
    std::set<Txid> needed;
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
        for (const PodRecord& record : records) {
            for (uint32_t position{1}; position < block.vtx.size(); ++position) {
                if (block.vtx[position]->GetHash() != record.pod_id) continue;
                if (modern::FindLegacyFnRecipientVout(*block.vtx[position])) {
                    candidates.push_back({static_cast<int32_t>(height), position});
                    for (const CTxIn& in : block.vtx[position]->vin) {
                        needed.insert(in.prevout.hash);
                    }
                }
                break;
            }
        }
    }

    // ---- Pass 2: locate the needed funding transactions with one more
    // sweep (no txindex dependency; memory holds only the needed ids).
    std::map<Txid, std::pair<int32_t, uint32_t>> locations;
    for (int height{1}; height <= *final_height && locations.size() < needed.size(); ++height) {
        CBlock block;
        if (!read_block(height, block)) {
            error = strprintf("block data unavailable at height %d (redownload or full "
                              "reindex required)", height);
            return false;
        }
        for (uint32_t position{0}; position < block.vtx.size(); ++position) {
            const Txid txid{block.vtx[position]->GetHash()};
            if (needed.contains(txid) && !locations.contains(txid)) {
                locations.emplace(txid, std::make_pair(static_cast<int32_t>(height), position));
            }
        }
    }

    // ---- Pass 3: build each proof through the pure builder, with data
    // access and committed merkle roots bound to this node's own chain,
    // and the builder's self-verification standing in the way of any
    // defective product.
    const modern::LegacyBlockAt block_at{read_block};
    const modern::LegacyTxLocator locate{
        [&](const Txid& txid) -> std::optional<std::pair<int32_t, uint32_t>> {
            const auto it{locations.find(txid)};
            if (it == locations.end()) return std::nullopt;
            return it->second;
        }};
    const modern::LegacyChainView view{
        .block_hash_at = [&](const int height) -> std::optional<uint256> {
            if (height <= 0 || height > *final_height) return std::nullopt;
            const CBlockIndex* pindex{chain[height]};
            if (!pindex) return std::nullopt;
            return pindex->GetBlockHash();
        },
        .merkle_root_at = [&](const int height) -> std::optional<uint256> {
            if (height <= 0 || height > *final_height) return std::nullopt;
            const CBlockIndex* pindex{chain[height]};
            if (!pindex) return std::nullopt;
            return pindex->hashMerkleRoot;
        }};
    std::vector<LegacyFnIssuanceCandidate> built;
    for (const Candidate& candidate : candidates) {
        LegacyFnIssuanceCandidate item;
        if (!modern::BuildLegacyFnIssuanceProof(candidate.height, candidate.position, params,
                                                block_at, locate, view, item.proof,
                                                item.facts, error)) {
            return false;
        }
        built.push_back(std::move(item));
    }
    out = std::move(built);
    return true;
}

} // namespace node

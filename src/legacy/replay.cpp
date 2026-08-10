// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/replay.h>

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/block_codec.h>
#include <consensus/merkle.h>
#include <legacy/codec.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <tinyformat.h>

#include <exception>
#include <utility>

namespace legacy {

TrustedReplay::TrustedReplay(const Consensus::Params& params, const int final_height,
                             std::map<int, uint256> checkpoints)
    : m_params{params}, m_final_height{final_height}, m_checkpoints{std::move(checkpoints)}
{
}

void TrustedReplay::ResumeAt(const int next_height, const uint256& tip_hash)
{
    m_next_height = next_height;
    m_tip_hash = tip_hash;
}

bool TrustedReplay::ApplyBlock(const CBlock& block, CCoinsViewCache& view, std::string& error)
{
    const int height{m_next_height};
    if (height > m_final_height) {
        error = strprintf("height %d is beyond the finalized legacy boundary %d", height, m_final_height);
        return false;
    }

    // Only the legacy codec exists at or below the boundary.
    if (Consensus::HasB3BlockCodecV2(block.nVersion)) {
        error = strprintf("modern codec marker below the legacy boundary at height %d", height);
        return false;
    }
    const uint256 hash{block.GetMarkerHash(m_params)};

    // Previous-block linkage, anchored on the configured genesis.
    if (height == 0) {
        if (!block.hashPrevBlock.IsNull() || hash != m_params.hashGenesisBlock) {
            error = "block at height 0 is not the configured genesis";
            return false;
        }
    } else if (block.hashPrevBlock != m_tip_hash) {
        error = strprintf("broken linkage at height %d: prev %s, tip %s",
                          height, block.hashPrevBlock.ToString(), m_tip_hash.ToString());
        return false;
    }

    // Configured checkpoint hashes.
    if (const auto it{m_checkpoints.find(height)}; it != m_checkpoints.end() && it->second != hash) {
        error = strprintf("checkpoint mismatch at height %d: got %s, want %s",
                          height, hash.ToString(), it->second.ToString());
        return false;
    }

    // Structural shape needed for deterministic classification.
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        error = strprintf("first transaction is not a coinbase at height %d", height);
        return false;
    }
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        if (block.vtx[i]->IsCoinBase()) {
            error = strprintf("unexpected extra coinbase at height %d", height);
            return false;
        }
        // Historical identity requires the legacy transaction encoding.
        if (!block.vtx[i]->IsLegacyEncoded()) {
            error = strprintf("non-legacy transaction encoding at height %d", height);
            return false;
        }
    }
    if (!block.vtx[0]->IsLegacyEncoded()) {
        error = strprintf("non-legacy coinbase encoding at height %d", height);
        return false;
    }

    // Merkle integrity; a mutated root means duplicated transactions.
    bool mutated{false};
    if (BlockMerkleRoot(block, &mutated) != block.hashMerkleRoot) {
        error = strprintf("merkle root mismatch at height %d", height);
        return false;
    }
    if (mutated) {
        error = strprintf("duplicated transactions at height %d", height);
        return false;
    }

    // Mechanical UTXO transition, atomic per block: nothing reaches `view`
    // unless every transaction applies.
    CCoinsViewCache block_view(&view);
    uint32_t tx_offset{static_cast<uint32_t>(GetSerializeSize(CBlockHeader{})) +
                       static_cast<uint32_t>(GetSizeOfCompactSize(block.vtx.size()))};
    for (const CTransactionRef& ptx : block.vtx) {
        const CTransaction& tx{*ptx};
        const Txid txid{tx.GetHash()};

        CAmount value_in{0};
        if (!tx.IsCoinBase()) {
            for (const CTxIn& txin : tx.vin) {
                // Missing prevouts and duplicate spends (within a
                // transaction, a block, or against history) fail here.
                if (!block_view.HaveCoin(txin.prevout)) {
                    error = strprintf("missing or already-spent input %s:%u of %s at height %d",
                                      txin.prevout.hash.ToString(), txin.prevout.n,
                                      txid.ToString(), height);
                    return false;
                }
                const Coin& coin{block_view.AccessCoin(txin.prevout)};
                if (!MoneyRange(coin.out.nValue) || coin.out.nValue > MAX_MONEY - value_in) {
                    error = strprintf("input value overflow in %s at height %d", txid.ToString(), height);
                    return false;
                }
                value_in += coin.out.nValue;
                block_view.SpendCoin(txin.prevout);
            }
        }

        CAmount value_out{0};
        for (const CTxOut& txout : tx.vout) {
            if (txout.nValue < 0 || !MoneyRange(txout.nValue) || txout.nValue > MAX_MONEY - value_out) {
                error = strprintf("output value overflow in %s at height %d", txid.ToString(), height);
                return false;
            }
            value_out += txout.nValue;
        }
        // Rewards are attested, not validated, so coinbase and coinstake may
        // create value; a plain transaction creating value is internally
        // inconsistent data.
        if (!tx.IsCoinBase() && !tx.IsCoinStake() && value_out > value_in) {
            error = strprintf("plain transaction %s creates value at height %d", txid.ToString(), height);
            return false;
        }

        // Exact output creation, preserving txid, vout index, amount,
        // scriptPubKey, height, coinbase/coinstake class, transaction time
        // and in-block offset. Overwrites mirror historical pre-BIP30
        // behavior deterministically.
        AddCoins(block_view, tx, height, /*check=*/true, tx_offset);
        tx_offset += static_cast<uint32_t>(GetSerializeSize(TX_LEGACY(tx)));
    }
    block_view.Flush();

    m_tip_hash = hash;
    ++m_next_height;
    return true;
}

bool TrustedReplay::ApplyRawBlock(const std::span<const std::byte> raw, CCoinsViewCache& view,
                                  std::string& error)
{
    CBlock block;
    try {
        SpanReader reader{raw};
        reader >> TX_LEGACY(block);
    } catch (const std::exception& e) {
        error = strprintf("undecodable legacy block at height %d: %s", m_next_height, e.what());
        return false;
    }
    return ApplyBlock(block, view, error);
}

} // namespace legacy

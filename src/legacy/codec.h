// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_CODEC_H
#define B3COIN_LEGACY_CODEC_H

#include <hash.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>

namespace legacy {

/**
 * The explicit legacy B3 codec.
 *
 * Historical B3Coin wire encoding: transactions carry an nTime field after
 * the version and have no witness; blocks append a trailing proof-of-stake
 * signature after the transactions. A transaction deserialized through this
 * codec is marked legacy-encoded, which makes the legacy bytes its identity:
 * GetHash() returns the exact historical txid, so merkle roots, outpoints
 * and UTXO keys stay byte-compatible with the existing chain without any
 * further caller involvement.
 *
 * Usage mirrors the modern params wrappers and must be explicit at the call
 * site, from a trusted format context (a legacy peer connection, legacy
 * chain block storage, or legacy genesis construction):
 *
 *     stream >> TX_LEGACY(block);     // decode a legacy block
 *     stream << TX_LEGACY(tx);        // encode a legacy transaction
 *     GetSerializeSize(TX_LEGACY(tx)) // historical serialized size
 *
 * NOTE ON FORMAT SELECTION: never choose this codec from a block height —
 * the height of undecoded bytes is unknown until after decoding. Selection
 * comes from transport or storage context, and for whole blocks the codec
 * is additionally marker-aware: the fixed-size header parses first, so a
 * post-fork block carrying the permanent B3 codec marker
 * (consensus/block_codec.h) keeps the unmodified Core body even when read
 * through TX_LEGACY. Whether the marker may appear at a given height is
 * enforced separately in ContextualCheckBlockHeader. Standalone
 * transactions have no header, so they rely on connection/era context
 * alone. See doc/design/b3-era-architecture.md.
 */
static constexpr TransactionSerParams TX_LEGACY{.allow_witness = false, .legacy_time = true};

/** Exact historical serialized size of a legacy transaction. */
inline size_t TxSerializedSize(const CTransaction& tx)
{
    return GetSerializeSize(TX_LEGACY(tx));
}

/**
 * The historical transaction ID of `tx` regardless of how it was decoded:
 * the cached hash when the transaction is legacy-encoded, otherwise a fresh
 * hash of the legacy byte layout.
 */
inline Txid TxId(const CTransaction& tx)
{
    if (tx.IsLegacyEncoded()) return tx.GetHash();
    return Txid::FromUint256((HashWriter{} << TX_LEGACY(tx)).GetHash());
}

} // namespace legacy

#endif // B3COIN_LEGACY_CODEC_H

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UTXO_COMMITMENT_H
#define BITCOIN_NODE_UTXO_COMMITMENT_H

#include <coins.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <optional>
#include <vector>

class CCoinsView;

namespace node {

//! One (outpoint, coin) pair from a UTXO set.
struct UtxoEntry {
    COutPoint outpoint;
    Coin coin;
};

/**
 * Deterministic, canonical commitment to a UTXO set, for DIAGNOSTIC use only:
 * checking that trusted legacy replay reconstructs the exact same state as
 * full legacy validation at a given height. It is NOT a consensus commitment
 * and is never referenced by validation.
 *
 * The commitment is domain-separated and length-prefixed, and folds in each
 * entry's outpoint together with the full Coin serialization -- output value
 * and script, height, the coinbase and coinstake flags, and the legacy
 * nTime/nTxOffset metadata -- so it distinguishes exact coin contents, not
 * merely an aggregate such as total supply. `entries` must be sorted ascending
 * by outpoint (EnumerateUtxos and CompareUtxoSets guarantee this).
 */
uint256 UtxoSetCommitment(const std::vector<UtxoEntry>& sorted_entries);

//! Enumerate every unspent coin of a cursor-supporting view (e.g. a
//! CCoinsViewDB), sorted canonically by outpoint. Empty if the view exposes
//! no cursor.
std::vector<UtxoEntry> EnumerateUtxos(const CCoinsView& view);

//! How a single outpoint differs between two sets: which side(s) hold it and,
//! when both do, the differing coins.
struct UtxoMismatch {
    COutPoint outpoint;
    std::optional<Coin> in_a; //!< nullopt => present only in b
    std::optional<Coin> in_b; //!< nullopt => present only in a
};

struct UtxoComparison {
    uint256 commitment_a;
    uint256 commitment_b;
    size_t count_a{0};
    size_t count_b{0};
    //! Every one-sided or differing outpoint, in canonical order. Empty iff the
    //! two sets are byte-for-byte identical (in which case the commitments are
    //! also equal).
    std::vector<UtxoMismatch> mismatches;
    bool Equal() const { return commitment_a == commitment_b && mismatches.empty(); }
};

//! Compare two UTXO sets (sorted internally), reporting a canonical commitment
//! for each and every mismatched or one-sided outpoint.
UtxoComparison CompareUtxoSets(std::vector<UtxoEntry> a, std::vector<UtxoEntry> b);

//! Compare two cursor-supporting coin views: enumerate both, then compare.
UtxoComparison CompareUtxoViews(const CCoinsView& a, const CCoinsView& b);

} // namespace node

#endif // BITCOIN_NODE_UTXO_COMMITMENT_H

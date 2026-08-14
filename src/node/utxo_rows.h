// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UTXO_ROWS_H
#define BITCOIN_NODE_UTXO_ROWS_H

#include <node/utxo_commitment.h>
#include <uint256.h>

#include <iosfwd>
#include <string>
#include <vector>

namespace node {

/**
 * The canonical logical UTXO row file ("b3-utxo-rows/v1"): the exchange
 * format of the three-way migration invariant
 *
 *     U_master(T) == U_port(T) == U_replay(T)
 *
 * (doc/design/b3-utxo-equivalence.md). Rows are logical coins — outpoint,
 * raw amount, exact script bytes, creation height, coinbase/coinstake
 * flags, transaction time, in-block offset — sorted ascending by the raw
 * serialized outpoint, and byte-identical across producers, so two row
 * files can be diffed directly. The legacy master client's exporter writes
 * the same grammar with its own code; this reader/writer is the port-side
 * implementation.
 */
struct UtxoRowsFile {
    uint256 tip_hash{};
    int tip_height{-1};
    //! Sorted ascending by outpoint (WriteUtxoRows sorts, ReadUtxoRows
    //! rejects unsorted input).
    std::vector<UtxoEntry> entries;
};

//! One canonical row line, without the newline:
//! "<txid>:<n> <value> <height> <cb> <cs> <ntime> <ntxoffset> <script-hex|->"
//! (an empty script is written as "-").
std::string UtxoRowLine(const UtxoEntry& entry);

//! Write the canonical row file. `file.entries` are sorted internally.
//! Returns false (with `error` set) only on a stream write failure.
bool WriteUtxoRows(std::ostream& out, UtxoRowsFile file, std::string& error);

/**
 * Strict parse of a canonical row file. Rejects a missing or foreign format
 * tag, malformed headers or fields, out-of-order or duplicate outpoints, a
 * count line that disagrees with the rows read, and trailing content. Row
 * MEMBERSHIP is deliberately not validated (e.g. a marker output another
 * producer should never have exported): such rows must surface as
 * comparison differences, not be hidden by a parse failure.
 */
bool ReadUtxoRows(std::istream& in, UtxoRowsFile& out, std::string& error);

} // namespace node

#endif // BITCOIN_NODE_UTXO_ROWS_H

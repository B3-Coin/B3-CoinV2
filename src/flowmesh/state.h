// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_STATE_H
#define B3COIN_FLOWMESH_STATE_H

#include <flowmesh/clearing.h>
#include <flowmesh/ledger.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace flowmesh {

/**
 * The complete FlowMesh execution state as ONE copyable value: the asset
 * ledger, the persistent clearing book, every signer's next sequence, and
 * the consumed-deposit set. Candidate microblock execution copies a
 * state, applies to the copy, and commits by replacement — a failed or
 * mismatching candidate can never leave a half-mutated committed state
 * (the MB-0 atomicity rule).
 *
 * Root() is the PURE state commitment: a function of state only, with no
 * per-slot execution results mixed in (those live in the separate
 * ExecutionResultCommitment). All collections are count-framed and
 * iterate in std::map/std::set order, which is the canonical order.
 */
class FlowMeshState
{
public:
    Ledger ledger;
    ClearingEngine book;
    //! Per-signer next expected sequence (nonce) for signed actions.
    std::map<AccountId, uint64_t> next_seq;
    //! B3 outpoints already consumed as deposits: each credits at most once.
    std::set<COutPoint> consumed_deposits;

    FlowMeshState(const uint256& vault_commitment, const AssetId& base, const AssetId& quote,
                  size_t max_k = 8)
        : ledger{vault_commitment}, book{base, quote, ledger, max_k}
    {
    }

    //! Copy/assign rebind the copied book to the copied ledger — a copied
    //! state must never settle against the original's ledger.
    FlowMeshState(const FlowMeshState& other)
        : ledger{other.ledger}, book{other.book}, next_seq{other.next_seq},
          consumed_deposits{other.consumed_deposits}
    {
        book.Rebind(ledger);
    }
    FlowMeshState& operator=(const FlowMeshState& other)
    {
        if (this != &other) {
            ledger = other.ledger;
            book = other.book;
            book.Rebind(ledger);
            next_seq = other.next_seq;
            consumed_deposits = other.consumed_deposits;
        }
        return *this;
    }

    uint64_t NextSequence(const AccountId& signer) const
    {
        const auto it{next_seq.find(signer)};
        return it == next_seq.end() ? 0 : it->second;
    }

    /**
     * Canonical whole-state serialization (snapshots). Deserializes INTO
     * a state constructed with the same configuration (vault, market,
     * max_k) — the embedded book stream enforces the market config and
     * throws on mismatch. A decoded snapshot is UNTRUSTED until its
     * Root() is checked against certified history (the log's
     * resulting_state_root at the snapshot sequence).
     */
    SERIALIZE_METHODS(FlowMeshState, obj)
    {
        READWRITE(obj.ledger, obj.book, obj.next_seq, obj.consumed_deposits);
        SER_READ(obj, obj.book.Rebind(obj.ledger));
    }

    //! Pure, canonically framed state root. The book root already frames
    //! the ledger root (and the slot counter) inside it.
    uint256 Root() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/state-root/v1"};
        h << book.StateRoot();
        h << static_cast<uint64_t>(next_seq.size());
        for (const auto& [signer, seq] : next_seq) h << signer << seq;
        h << static_cast<uint64_t>(consumed_deposits.size());
        for (const COutPoint& outpoint : consumed_deposits) h << outpoint;
        return h.GetHash();
    }
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_STATE_H

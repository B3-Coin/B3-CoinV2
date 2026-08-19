// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_STATE_H
#define B3COIN_FLOWMESH_STATE_H

#include <flowmesh/clearing.h>
#include <flowmesh/ledger.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <ios>
#include <map>
#include <set>
#include <string>

namespace flowmesh {

//! Snapshot decode bounds, enforced before elements are read.
inline constexpr uint64_t STATE_SNAPSHOT_MAX_SIGNERS{uint64_t{1} << 22};
inline constexpr uint64_t STATE_SNAPSHOT_MAX_DEPOSITS{uint64_t{1} << 22};

/**
 * The complete FlowMesh execution state as ONE copyable value: the asset
 * ledger, the persistent clearing book, every signer's next sequence, and
 * the consumed-deposit set. Candidate microblock execution copies a
 * state, applies to the copy, and commits by replacement — a failed or
 * mismatching candidate can never leave a half-mutated committed state
 * (the MB-0 atomicity rule).
 *
 * OWNERSHIP: the book stores no ledger binding (ClearingEngine methods
 * take the ledger explicitly), so the executor/book/ledger pairing is
 * correct by construction — there is no rebinding operation to misuse
 * and no way to attach this state's book to a different ledger.
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
        : ledger{vault_commitment}, book{base, quote, max_k}
    {
    }

    uint64_t NextSequence(const AccountId& signer) const
    {
        const auto it{next_seq.find(signer)};
        return it == next_seq.end() ? 0 : it->second;
    }

    //! Pure, canonically framed state root. The book root frames the
    //! ledger root (and the slot counter) inside it.
    uint256 Root() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/state-root/v1"};
        h << book.StateRoot(ledger);
        h << static_cast<uint64_t>(next_seq.size());
        for (const auto& [signer, seq] : next_seq) h << signer << seq;
        h << static_cast<uint64_t>(consumed_deposits.size());
        for (const COutPoint& outpoint : consumed_deposits) h << outpoint;
        return h.GetHash();
    }

    /**
     * Canonical whole-state serialization (snapshots). Deserializes INTO
     * a state constructed with the same configuration (vault, market,
     * max_k) — the embedded book stream enforces the market config and
     * throws on mismatch; collection counts are bounded before elements
     * are read and keys must be strictly ascending. A decoded snapshot
     * is UNTRUSTED until validated against certified history (see
     * FlowMeshStore::ReplayFromBestSnapshot).
     */
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << ledger << book;
        WriteCompactSize(s, next_seq.size());
        for (const auto& [signer, seq] : next_seq) s << signer << seq;
        WriteCompactSize(s, consumed_deposits.size());
        for (const COutPoint& outpoint : consumed_deposits) s << outpoint;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> ledger >> book;
        {
            const uint64_t n{ReadCompactSize(s)};
            if (n > STATE_SNAPSHOT_MAX_SIGNERS) {
                throw std::ios_base::failure("flowmesh state snapshot signer map too large");
            }
            std::map<AccountId, uint64_t> fresh;
            for (uint64_t i{0}; i < n; ++i) {
                AccountId signer;
                uint64_t seq;
                s >> signer >> seq;
                if (!fresh.empty() && !(std::prev(fresh.end())->first < signer)) {
                    throw std::ios_base::failure("flowmesh state snapshot signers not canonical");
                }
                fresh.emplace_hint(fresh.end(), signer, seq);
            }
            next_seq = std::move(fresh);
        }
        {
            const uint64_t n{ReadCompactSize(s)};
            if (n > STATE_SNAPSHOT_MAX_DEPOSITS) {
                throw std::ios_base::failure("flowmesh state snapshot deposit set too large");
            }
            std::set<COutPoint> fresh;
            for (uint64_t i{0}; i < n; ++i) {
                COutPoint outpoint;
                s >> outpoint;
                if (!fresh.empty() && !(*std::prev(fresh.end()) < outpoint)) {
                    throw std::ios_base::failure("flowmesh state snapshot deposits not canonical");
                }
                fresh.emplace_hint(fresh.end(), outpoint);
            }
            consumed_deposits = std::move(fresh);
        }
    }
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_STATE_H

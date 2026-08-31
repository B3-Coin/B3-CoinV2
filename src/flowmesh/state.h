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
#include <optional>
#include <set>
#include <string>

namespace flowmesh {

class BatchExecutor;

namespace test_only {
struct StateFunding;
} // namespace test_only

//! Snapshot decode bounds, enforced before elements are read.
inline constexpr uint64_t STATE_SNAPSHOT_MAX_SIGNERS{uint64_t{1} << 22};
inline constexpr uint64_t STATE_SNAPSHOT_MAX_DEPOSITS{uint64_t{1} << 22};

/**
 * The complete FlowMesh execution state as ONE copyable value: the asset
 * ledger, the persistent clearing book, every signer's next sequence, and
 * the consumed-deposit set. Candidate microblock execution copies a
 * state, applies to the copy, and commits by replacement (MB-0).
 *
 * OWNERSHIP IS STRUCTURAL: the LEDGER and the BOOK are both PRIVATE
 * members. Every mutation exists only as a FlowMeshState method that
 * pairs the book with THIS state's ledger, the engine's ledger-taking
 * mutators are private with FlowMeshState their sole friend caller,
 * and outside code sees the ledger only through the read-only
 * LedgerView(). No code path can combine order/curve state A with
 * ledger B.
 *
 * consumed_deposits: provisional model state.
 * OWNER DECISION REQUIRED — retain or defer consumed-deposit state, and
 * decide same-slot deposit/trading semantics. Production deposits are
 * fail-closed; nothing here activates or extends deposit behavior.
 *
 * Root() is the PURE state commitment: a function of state only (the
 * separate ExecutionResultCommitment carries execution metadata).
 */
//! Canonical id of one immutable market/execution configuration: the
//! vault commitment, market pair and curve bound. Actions and evidence
//! are authorized AGAINST a configuration (auth.h binds this id into
//! the signature digest), so authorization can never be replayed into a
//! different market or execution setup.
inline uint256 ComputeExecutionConfigId(const uint256& vault_commitment, const AssetId& base,
                                        const AssetId& quote, const uint64_t max_k)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/execconfig/v1"} << vault_commitment << base << quote << max_k;
    return h.GetHash();
}

class FlowMeshState
{
public:
    FlowMeshState(const uint256& vault_commitment, const AssetId& base, const AssetId& quote,
                  size_t max_k = 8)
        : ledger{vault_commitment}, book{base, quote, max_k},
          config_id{ComputeExecutionConfigId(vault_commitment, base, quote, max_k)}
    {
    }

    //! The immutable market/execution configuration this state runs
    //! under. Authorization is bound to it; a foreign configuration's
    //! actions/evidence must be rejected by the authorizing layer.
    const uint256& ConfigId() const { return config_id; }

    //! READ-ONLY view of the authoritative ledger. The Ledger instance
    //! itself is private: every mutation goes through a state-owned
    //! method, so order/curve state can never be combined with a
    //! different Ledger through the production API.
    const Ledger& LedgerView() const { return ledger; }

    // NO raw funding here: production custody enters ONLY through
    // CreditDeposit behind a chain-fact DepositVerifier (fail-closed
    // today). Test fixtures fund via test_only::StateFunding, which is
    // defined only under src/test/util/ — a production build has no
    // callable API that fabricates balances, custody or nonces.

    uint64_t NextSequence(const AccountId& signer) const
    {
        const auto it{next_seq.find(signer)};
        return it == next_seq.end() ? 0 : it->second;
    }

    uint64_t Slot() const { return ledger.Slot(); }

    bool DepositConsumed(const COutPoint& outpoint) const
    {
        return consumed_deposits.count(outpoint) > 0;
    }

    std::optional<modern::WithdrawalReceipt> RequestWithdrawal(
        const AccountId& account, const AssetId& asset, const CAmount amount,
        const uint256& destination, const CAmount withdrawal_capacity)
    {
        return ledger.RequestWithdrawal(account, asset, amount, destination,
                                        withdrawal_capacity);
    }

    // ---- The ONLY book-mutation surface: always this state's ledger. ----

    bool SubmitCurve(const AccountId& account, const ClearingEngine::Side side,
                     const std::vector<ClearingEngine::Breakpoint>& points)
    {
        return book.SubmitCurve(ledger, account, side, points);
    }
    bool CancelCurve(const AccountId& account, const ClearingEngine::Side side)
    {
        return book.CancelCurve(ledger, account, side);
    }
    [[nodiscard]] std::optional<ClearingEngine::ClearingResult> ClearSlot(
        const FlowMeshFeeContext* fee_context = nullptr)
    {
        return book.ClearSlot(ledger, fee_context);
    }

    // ---- Read-only book views. ----

    bool CurveIsValid(const ClearingEngine::Side side,
                      const std::vector<ClearingEngine::Breakpoint>& points) const
    {
        return book.CurveIsValid(side, points);
    }
    CAmount EffectiveQty(const ClearingEngine::Side side, const AccountId& account,
                         const CAmount price) const
    {
        return book.EffectiveQty(side, account, price);
    }
    uint256 BookRoot() const { return book.StateRoot(ledger); }

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
     * a state constructed with the same configuration; the embedded book
     * stream enforces the market config and throws on mismatch;
     * collection counts are bounded before elements are read and keys
     * must be strictly ascending. A decoded snapshot is UNTRUSTED until
     * validated against certified history (FlowMeshStore).
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
        const uint256 expected_vault{ledger.VaultCommitment()};
        s >> ledger >> book;
        // The embedded book stream enforces base/quote/max_k; the vault
        // must be enforced here — a snapshot for a different vault must
        // never decode into this configuration (ConfigId() hashes the
        // vault and is derived, never deserialized).
        if (ledger.VaultCommitment() != expected_vault) {
            throw std::ios_base::failure("flowmesh state snapshot is for a different vault");
        }
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
        // A decoded snapshot must not be able to construct impossible
        // fill/reservation state or book/ledger divergence.
        book.CheckDecodedAccounting(ledger);
    }

private:
    //! RAW STATE TRANSITIONS ARE EXECUTOR-ONLY: sequence advancement and
    //! verifier-established deposit credits exist solely for
    //! BatchExecutor's canonical execution path — no other caller can
    //! fabricate custody, nonces or consumed-deposit state.
    friend class BatchExecutor;
    friend struct test_only::StateFunding; // defined only in test code

    void AdvanceSequence(const AccountId& signer, const uint64_t next)
    {
        next_seq[signer] = next;
    }
    //! Credit a verifier-established deposit and mark its outpoint
    //! consumed, atomically from the caller's perspective.
    bool CreditDeposit(const COutPoint& outpoint, const AccountId& account,
                       const AssetId& asset, const CAmount amount)
    {
        if (consumed_deposits.count(outpoint) > 0) return false;
        if (!ledger.Deposit(account, asset, amount)) return false;
        consumed_deposits.insert(outpoint);
        return true;
    }

    bool ConsumeWithdrawalSettlement(
        const WithdrawalSettlementFactV1& settlement)
    {
        return ledger.ConsumeRequest(settlement);
    }

    //! Per-signer next expected sequence (nonce) for signed actions.
    std::map<AccountId, uint64_t> next_seq;
    //! B3 outpoints already consumed as deposits (owner-decision note
    //! above).
    std::set<COutPoint> consumed_deposits;
    Ledger ledger;
    ClearingEngine book;
    uint256 config_id;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_STATE_H

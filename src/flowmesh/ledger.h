// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_LEDGER_H
#define B3COIN_FLOWMESH_LEDGER_H

#include <consensus/amount.h>
#include <hash.h>
#include <modern/policy.h>
#include <modern/vault.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <ios>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace flowmesh {

using AccountId = uint256;
using modern::AssetId;

//! The protocol-fee account: fees are internal transfers to it, so they
//! never change total liabilities or custody.
inline const AccountId& FeeAccount()
{
    static const AccountId account{
        uint256{"b3fee000000000000000000000000000000000000000000000000000000000f0"}};
    return account;
}

//! Snapshot decode bound per collection: enforced BEFORE elements are
//! read, so attacker-sized counts cannot drive allocation.
inline constexpr uint64_t LEDGER_SNAPSHOT_MAX_ENTRIES{uint64_t{1} << 22};

/**
 * FlowMesh internal asset ledger (off-consensus; no matching here).
 *
 * Deterministic per-account, per-asset balances with exact integer
 * arithmetic. The ledger upholds the vault solvency invariant at every
 * step, per asset A:
 *
 *     total DEX vault custody for A
 *         == total FlowMesh liabilities for A
 *         == available(A) + reserved(A) + pending withdrawal requests(A)
 *
 * Custody and liability always move together. Every rejected operation
 * leaves the state — and therefore the deterministic state root —
 * completely untouched: no lookup here ever inserts before all checks
 * pass (a rejected deposit must not even leave a zero-valued entry).
 *
 * WITHDRAWAL LIFECYCLE (accurate names; the stages beyond the first two
 * are NOT reachable yet):
 *
 *     REQUESTED             — RequestWithdrawal() debited the balance
 *                             and recorded the pending request (this is
 *                             the only transition the ledger performs
 *                             for a user action);
 *     MICROBLOCK_CERTIFIED  — the request sits in a state committed
 *                             under a microblock certificate (derived
 *                             from log position, not stored; the
 *                             certificate-commit model itself is
 *                             provisional — certificate ==
 *                             irreversible finality is an unresolved
 *                             owner decision);
 *     B3_FINAL / REDEEMABLE — requires the OWNER-DECIDED trustless B3
 *                             vault authorization; NOTHING in this
 *                             codebase reports a request as redeemable;
 *     CONSUMED              — ConsumeRequest(): the on-chain vault
 *                             spend removed custody and liability
 *                             together (exercised by tests only).
 *
 * The ledger deliberately does NOT implement the modern
 * FinalizedReceiptView: FlowMesh certification alone must never present
 * a request as an authorized B3 spend.
 */
class Ledger final
{
public:
    struct Balance {
        CAmount available{0};
        CAmount reserved{0};

        SERIALIZE_METHODS(Balance, obj) { READWRITE(obj.available, obj.reserved); }
    };

    explicit Ledger(const uint256& vault_commitment) : m_vault{vault_commitment} {}

    const uint256& VaultCommitment() const { return m_vault; }

    // ---- Deposits -------------------------------------------------------

    //! Custody entered the vault on-chain; credit the account. A refusal
    //! (non-positive, out-of-range, or overflowing amount) leaves every
    //! persistent field byte-identical — no entry is created.
    bool Deposit(const AccountId& account, const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0 || amount > MAX_MONEY) return false;
        const auto balance_it{m_balances.find({account, asset})};
        const CAmount available{balance_it == m_balances.end() ? 0
                                                               : balance_it->second.available};
        const auto custody_it{m_custody.find(asset)};
        const CAmount custody{custody_it == m_custody.end() ? 0 : custody_it->second};
        if (amount > MAX_MONEY - available || amount > MAX_MONEY - custody) return false;

        m_balances[{account, asset}].available = available + amount;
        m_custody[asset] = custody + amount;
        return true;
    }

    // ---- Balances and reservations -------------------------------------

    CAmount Available(const AccountId& account, const AssetId& asset) const
    {
        const auto it{m_balances.find({account, asset})};
        return it == m_balances.end() ? 0 : it->second.available;
    }

    CAmount Reserved(const AccountId& account, const AssetId& asset) const
    {
        const auto it{m_balances.find({account, asset})};
        return it == m_balances.end() ? 0 : it->second.reserved;
    }

    //! Lock available funds (for matching). Internal move only.
    bool Reserve(const AccountId& account, const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0) return false;
        const auto it{m_balances.find({account, asset})};
        if (it == m_balances.end() || it->second.available < amount) return false;
        it->second.available -= amount;
        it->second.reserved += amount; // cannot overflow: total was bounded
        return true;
    }

    bool Release(const AccountId& account, const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0) return false;
        const auto it{m_balances.find({account, asset})};
        if (it == m_balances.end() || it->second.reserved < amount) return false;
        it->second.reserved -= amount;
        it->second.available += amount;
        return true;
    }

    /**
     * Internal settlement move: reserved funds of `from` become available
     * funds of `to`, same asset. Total liability and per-asset custody
     * are both unchanged, so the solvency invariant is preserved. A
     * refusal (including destination overflow) leaves the state
     * untouched — the destination entry is only created on success.
     * from == to degenerates to Release.
     */
    bool MoveReservedToAvailable(const AccountId& from, const AccountId& to,
                                 const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0) return false;
        const auto it{m_balances.find({from, asset})};
        if (it == m_balances.end() || it->second.reserved < amount) return false;
        const auto dst_it{m_balances.find({to, asset})};
        const CAmount dst_available{dst_it == m_balances.end() ? 0 : dst_it->second.available};
        if (from != to && amount > MAX_MONEY - dst_available) return false;

        it->second.reserved -= amount;
        m_balances[{to, asset}].available += amount; // creates dst only on this success path
        Prune(from, asset);
        return true;
    }

    // ---- Protocol fees --------------------------------------------------

    //! Charge a fee: an internal transfer to the protocol fee account.
    //! Refusals leave the state untouched.
    bool ChargeFee(const AccountId& account, const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0 || account == FeeAccount()) return false;
        const auto it{m_balances.find({account, asset})};
        if (it == m_balances.end() || it->second.available < amount) return false;
        const auto fee_it{m_balances.find({FeeAccount(), asset})};
        const CAmount fee_available{fee_it == m_balances.end() ? 0 : fee_it->second.available};
        if (amount > MAX_MONEY - fee_available) return false;

        it->second.available -= amount;
        m_balances[{FeeAccount(), asset}].available = fee_available + amount;
        Prune(account, asset);
        return true;
    }

    // ---- Withdrawal requests --------------------------------------------

    /**
     * REQUESTED: debit the account and record a pending withdrawal
     * request. This is a provisional intent — it does NOT authorize any
     * B3 vault spend, and nothing here (or anywhere in FlowMesh) can
     * promote it to redeemable; that authorization is an unresolved
     * owner decision. The request id derives deterministically from the
     * ledger's slot and sequence, so identical histories yield identical
     * requests.
     */
    std::optional<modern::WithdrawalReceipt> RequestWithdrawal(const AccountId& account,
                                                               const AssetId& asset,
                                                               const CAmount amount,
                                                               const uint256& destination)
    {
        if (amount <= 0) return std::nullopt;
        const auto it{m_balances.find({account, asset})};
        if (it == m_balances.end() || it->second.available < amount) return std::nullopt;

        modern::WithdrawalReceipt request;
        request.asset = asset;
        request.amount = amount;
        request.destination = destination;
        request.finalized_slot = m_slot;
        request.vault_commitment = m_vault;
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/receipt/v1"} << m_slot << m_next_receipt_seq
              << account << asset << amount << destination << m_vault;
            request.receipt_id = h.GetHash();
        }

        it->second.available -= amount;
        Prune(account, asset);
        ++m_next_receipt_seq;
        m_requests.emplace(request.receipt_id, request);
        return request;
    }

    //! A pending request, by id (REQUESTED / MICROBLOCK_CERTIFIED — the
    //! distinction is the caller's, from log position). NOT a statement
    //! of B3 redeemability.
    std::optional<modern::WithdrawalReceipt> GetRequest(const uint256& request_id) const
    {
        const auto it{m_requests.find(request_id)};
        if (it == m_requests.end()) return std::nullopt;
        return it->second;
    }

    //! CONSUMED: the on-chain withdrawal connected; custody and
    //! request-liability leave together. A request can be consumed
    //! exactly once. (Reachable only through the future owner-approved
    //! B3 authorization; tests exercise the accounting.)
    bool ConsumeRequest(const uint256& request_id)
    {
        const auto it{m_requests.find(request_id)};
        if (it == m_requests.end()) return false;
        const auto custody{m_custody.find(it->second.asset)};
        if (custody == m_custody.end() || custody->second < it->second.amount) return false;
        custody->second -= it->second.amount;
        if (custody->second == 0) m_custody.erase(custody);
        m_requests.erase(it);
        return true;
    }

    // ---- Solvency invariant ---------------------------------------------

    CAmount Custody(const AssetId& asset) const
    {
        const auto it{m_custody.find(asset)};
        return it == m_custody.end() ? 0 : it->second;
    }

    //! Read-only visit of every balance entry (decode reconciliation,
    //! diagnostics). Deterministic map order; no mutation possible.
    template <typename Fn>
    void ForEachBalance(Fn&& fn) const
    {
        for (const auto& [key, balance] : m_balances) fn(key.first, key.second, balance);
    }

    //! available + reserved + pending requests, per asset.
    CAmount Liabilities(const AssetId& asset) const
    {
        CAmount total{0};
        for (const auto& [key, balance] : m_balances) {
            if (key.second != asset) continue;
            total += balance.available + balance.reserved; // bounded by custody
        }
        for (const auto& [id, request] : m_requests) {
            if (request.asset == asset) total += request.amount;
        }
        return total;
    }

    //! The block/slot invariant: custody equals liabilities for every
    //! asset the ledger touches.
    bool SolvencyHolds() const
    {
        std::map<AssetId, CAmount> liabilities;
        for (const auto& [key, balance] : m_balances) {
            liabilities[key.second] += balance.available + balance.reserved;
        }
        for (const auto& [id, request] : m_requests) {
            liabilities[request.asset] += request.amount;
        }
        for (const auto& [asset, total] : liabilities) {
            if (Custody(asset) != total) return false;
        }
        for (const auto& [asset, custody] : m_custody) {
            if (liabilities.count(asset) == 0 && custody != 0) return false;
        }
        return true;
    }

    // ---- Determinism -----------------------------------------------------

    //! Advance to the next clearing slot.
    void AdvanceSlot() { ++m_slot; }
    uint64_t Slot() const { return m_slot; }

    //! Deterministic root over the canonical (zero-pruned, ordered) state.
    //! CANONICALLY FRAMED (v2): each variable-length collection — balances,
    //! custody, pending requests — is preceded by its entry count, so the
    //! boundaries between collections are part of the preimage and no two
    //! distinct layouts can flatten to one byte stream. (Byte-identical to
    //! the pre-rename preimage: the request map serializes exactly as the
    //! former pending-receipt map did.)
    uint256 StateRoot() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/state/v2"} << m_vault << m_slot << m_next_receipt_seq;
        h << static_cast<uint64_t>(m_balances.size());
        for (const auto& [key, balance] : m_balances) {
            h << key.first << key.second << balance.available << balance.reserved;
        }
        h << static_cast<uint64_t>(m_custody.size());
        for (const auto& [asset, custody] : m_custody) {
            h << asset << custody;
        }
        h << static_cast<uint64_t>(m_requests.size());
        for (const auto& [id, request] : m_requests) {
            h << request;
        }
        return h.GetHash();
    }

    /**
     * Canonical whole-ledger serialization (snapshots), wire-compatible
     * with the standard map encodings but with every collection count
     * checked against LEDGER_SNAPSHOT_MAX_ENTRIES BEFORE its elements
     * are read, and strictly ascending keys required (one byte
     * representation per state). Snapshot consumers must additionally
     * verify the decoded state's root against certified history.
     */
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_vault << m_slot << m_next_receipt_seq;
        WriteCompactSize(s, m_balances.size());
        for (const auto& [key, balance] : m_balances) s << key << balance;
        WriteCompactSize(s, m_custody.size());
        for (const auto& [asset, custody] : m_custody) s << asset << custody;
        WriteCompactSize(s, m_requests.size());
        for (const auto& [id, request] : m_requests) s << id << request;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_vault >> m_slot >> m_next_receipt_seq;
        ReadBoundedMap(s, m_balances);
        ReadBoundedMap(s, m_custody);
        ReadBoundedMap(s, m_requests);
    }

private:
    template <typename Stream, typename Map>
    static void ReadBoundedMap(Stream& s, Map& out)
    {
        const uint64_t count{ReadCompactSize(s)};
        if (count > LEDGER_SNAPSHOT_MAX_ENTRIES) {
            throw std::ios_base::failure("flowmesh ledger snapshot collection too large");
        }
        Map fresh;
        auto hint{fresh.end()};
        for (uint64_t i{0}; i < count; ++i) {
            typename Map::key_type key;
            typename Map::mapped_type value;
            s >> key >> value;
            if (!fresh.empty() && !(std::prev(fresh.end())->first < key)) {
                throw std::ios_base::failure("flowmesh ledger snapshot keys not canonical");
            }
            hint = fresh.emplace_hint(fresh.end(), std::move(key), std::move(value));
        }
        out = std::move(fresh);
    }

    //! Canonical form: fully-empty balance entries are removed so equal
    //! states serialize identically.
    void Prune(const AccountId& account, const AssetId& asset)
    {
        const auto it{m_balances.find({account, asset})};
        if (it != m_balances.end() && it->second.available == 0 && it->second.reserved == 0) {
            m_balances.erase(it);
        }
    }

    // Not const so the ledger is assignable (candidate execution copies
    // and replaces whole states); never reassigned outside
    // copy/assignment and snapshot decode.
    uint256 m_vault;
    uint64_t m_slot{0};
    uint64_t m_next_receipt_seq{0};
    std::map<std::pair<AccountId, AssetId>, Balance> m_balances;
    std::map<AssetId, CAmount> m_custody;
    std::map<uint256, modern::WithdrawalReceipt> m_requests;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_LEDGER_H

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

/**
 * FlowMesh internal asset ledger (off-consensus; no matching yet).
 *
 * Deterministic per-account, per-asset balances with exact integer
 * arithmetic. The ledger upholds the vault solvency invariant at every
 * step, per asset A:
 *
 *     total DEX vault custody for A
 *         == total finalized FlowMesh liabilities for A
 *         == available(A) + reserved(A) + pending withdrawal receipts(A)
 *
 * Custody and liability always move together: a deposit raises both; a
 * reservation or protocol fee is an internal move; finalizing a
 * withdrawal converts balance-liability into receipt-liability; consuming
 * a receipt (the on-chain vault spend) removes custody and liability at
 * once. Every rejected operation leaves the state — and therefore the
 * deterministic state root — completely untouched.
 *
 * The ledger is the FinalizedReceiptView the DEX_VAULT withdrawal
 * checker consumes: a receipt is visible exactly while finalized and
 * unconsumed.
 */
class Ledger final : public modern::FinalizedReceiptView
{
public:
    struct Balance {
        CAmount available{0};
        CAmount reserved{0};
    };

    explicit Ledger(const uint256& vault_commitment) : m_vault{vault_commitment} {}

    // ---- Deposits -------------------------------------------------------

    //! Custody entered the vault on-chain; credit the account.
    bool Deposit(const AccountId& account, const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0) return false;
        Balance& balance{m_balances[{account, asset}]};
        CAmount& custody{m_custody[asset]};
        if (amount > MAX_MONEY - balance.available || amount > MAX_MONEY - custody) {
            Prune(account, asset);
            return false;
        }
        balance.available += amount;
        custody += amount;
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

    //! Lock available funds (for future matching). Internal move only.
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
     * funds of `to`, same asset. Total liability (available + reserved
     * across accounts) and per-asset custody are both unchanged, so the
     * solvency invariant is preserved. This is how a batch fill settles —
     * a payer's reserved balance moves to a payee's available balance with
     * no per-fill UTXO spend. from == to degenerates to Release.
     */
    bool MoveReservedToAvailable(const AccountId& from, const AccountId& to,
                                 const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0) return false;
        const auto it{m_balances.find({from, asset})};
        if (it == m_balances.end() || it->second.reserved < amount) return false;
        Balance& dst{m_balances[{to, asset}]};
        if (amount > MAX_MONEY - dst.available) { Prune(from, asset); return false; }
        it->second.reserved -= amount;
        dst.available += amount;
        Prune(from, asset);
        return true;
    }

    // ---- Protocol fees --------------------------------------------------

    //! Charge a fee: an internal transfer to the protocol fee account.
    bool ChargeFee(const AccountId& account, const AssetId& asset, const CAmount amount)
    {
        if (amount <= 0 || account == FeeAccount()) return false;
        const auto it{m_balances.find({account, asset})};
        if (it == m_balances.end() || it->second.available < amount) return false;
        Balance& fee_balance{m_balances[{FeeAccount(), asset}]};
        if (amount > MAX_MONEY - fee_balance.available) {
            Prune(FeeAccount(), asset);
            return false;
        }
        it->second.available -= amount;
        fee_balance.available += amount;
        Prune(account, asset);
        return true;
    }

    // ---- Withdrawals ----------------------------------------------------

    /**
     * Finalize a withdrawal: debit the account and create the receipt that
     * authorizes the on-chain vault spend. Liability moves from balance to
     * pending receipt; custody is untouched until the receipt is consumed.
     * The receipt id derives deterministically from the ledger's slot and
     * sequence, so identical histories yield identical receipts.
     */
    std::optional<modern::WithdrawalReceipt> FinalizeWithdrawal(const AccountId& account,
                                                                const AssetId& asset,
                                                                const CAmount amount,
                                                                const uint256& destination)
    {
        if (amount <= 0) return std::nullopt;
        const auto it{m_balances.find({account, asset})};
        if (it == m_balances.end() || it->second.available < amount) return std::nullopt;

        modern::WithdrawalReceipt receipt;
        receipt.asset = asset;
        receipt.amount = amount;
        receipt.destination = destination;
        receipt.finalized_slot = m_slot;
        receipt.vault_commitment = m_vault;
        {
            HashWriter h;
            h << std::string{"b3/flowmesh/receipt/v1"} << m_slot << m_next_receipt_seq
              << account << asset << amount << destination << m_vault;
            receipt.receipt_id = h.GetHash();
        }

        it->second.available -= amount;
        Prune(account, asset);
        ++m_next_receipt_seq;
        m_pending.emplace(receipt.receipt_id, receipt);
        return receipt;
    }

    //! FinalizedReceiptView: visible while finalized and unconsumed.
    std::optional<modern::WithdrawalReceipt> GetFinalized(const uint256& receipt_id) const override
    {
        const auto it{m_pending.find(receipt_id)};
        if (it == m_pending.end()) return std::nullopt;
        return it->second;
    }

    //! The on-chain withdrawal connected: custody and receipt-liability
    //! leave together. A receipt can be consumed exactly once.
    bool ConsumeReceipt(const uint256& receipt_id)
    {
        const auto it{m_pending.find(receipt_id)};
        if (it == m_pending.end()) return false;
        const auto custody{m_custody.find(it->second.asset)};
        if (custody == m_custody.end() || custody->second < it->second.amount) return false;
        custody->second -= it->second.amount;
        if (custody->second == 0) m_custody.erase(custody);
        m_pending.erase(it);
        return true;
    }

    // ---- Solvency invariant ---------------------------------------------

    CAmount Custody(const AssetId& asset) const
    {
        const auto it{m_custody.find(asset)};
        return it == m_custody.end() ? 0 : it->second;
    }

    //! available + reserved + pending receipts, per asset.
    CAmount Liabilities(const AssetId& asset) const
    {
        CAmount total{0};
        for (const auto& [key, balance] : m_balances) {
            if (key.second != asset) continue;
            total += balance.available + balance.reserved; // bounded by custody
        }
        for (const auto& [id, receipt] : m_pending) {
            if (receipt.asset == asset) total += receipt.amount;
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
        for (const auto& [id, receipt] : m_pending) {
            liabilities[receipt.asset] += receipt.amount;
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
    uint256 StateRoot() const
    {
        HashWriter h;
        h << std::string{"b3/flowmesh/state/v1"} << m_vault << m_slot << m_next_receipt_seq;
        for (const auto& [key, balance] : m_balances) {
            h << key.first << key.second << balance.available << balance.reserved;
        }
        for (const auto& [asset, custody] : m_custody) {
            h << asset << custody;
        }
        for (const auto& [id, receipt] : m_pending) {
            h << receipt;
        }
        return h.GetHash();
    }

private:
    //! Canonical form: fully-empty balance entries are removed so equal
    //! states serialize identically.
    void Prune(const AccountId& account, const AssetId& asset)
    {
        const auto it{m_balances.find({account, asset})};
        if (it != m_balances.end() && it->second.available == 0 && it->second.reserved == 0) {
            m_balances.erase(it);
        }
    }

    const uint256 m_vault;
    uint64_t m_slot{0};
    uint64_t m_next_receipt_seq{0};
    std::map<std::pair<AccountId, AssetId>, Balance> m_balances;
    std::map<AssetId, CAmount> m_custody;
    std::map<uint256, modern::WithdrawalReceipt> m_pending;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_LEDGER_H

// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_STATE_ROOT_V1_H
#define B3_FLOWMESH2_CORE_STATE_ROOT_V1_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/smt.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/** Value types of state commitment v1 shared by the ledger and b3verify
 * (M2-LED-02; PROVISIONAL REGTEST, ARCHITECTURE.md O-96).
 *
 * Commitment v1 keeps the six components and the leaf values of the sorted
 * commitments (core/state_root.h) and puts each component in a compact
 * sparse Merkle tree (core/smt.h) instead of a sorted RFC 6962 tree:
 *   accounts    path SmtKeyPath(ACCOUNTS, AccountId)    value AccountLeafV0::LeafValue()
 *   orders      path SmtKeyPath(ORDERS, OrderId)        value RestingOrderLeafV0::LeafValue()
 *   markets     path SmtKeyPath(MARKETS, MarketId)      value MarketLeafValue
 *   nullifiers  path SmtKeyPath(NULLIFIERS, txid || u32 LE vout) value NullifierLeafValue
 *   totals      path SmtKeyPath(TOTALS, AssetId)        value TotalsLeafV0::LeafValue()
 *   withdrawals path SmtKeyPath(WITHDRAWALS, ActionId)  value WithdrawalRecord::LeafValue()
 * state_root = StateRootsV1::Root() = Tagged(STATE, u8 1 || the six roots).
 *
 * Not live: no profile commits v1 and the header codec refuses
 * commitment_version 1 (core/slot.h). Which profiles switch, and with
 * which certificate format, is BLOCKED-ON-RULING (M2-LED-02).
 */
namespace flowmesh2 {

//! The six component roots of commitment v1.
struct StateRootsV1 {
    uint256 accounts;
    uint256 orders;
    uint256 markets;
    uint256 nullifiers;
    uint256 totals;
    uint256 withdrawals;

    static constexpr size_t ENCODED_BYTES{6 * 32};
    friend bool operator==(const StateRootsV1&, const StateRootsV1&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << accounts << orders << markets << nullifiers << totals << withdrawals;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> accounts >> orders >> markets >> nullifiers >> totals >> withdrawals;
    }

    //! Tagged(STATE, u8 COMMITMENT_VERSION_SMT || the six roots).
    uint256 Root() const;
};

//! The accounts path of `account`: SmtKeyPath(ACCOUNTS, account).
uint256 AccountPathV1(const AccountId& account);

//! What an account proof shows. APPEND-ONLY; zero is never a value.
enum class AccountProofKind : uint8_t {
    PRESENT = 1, //!< the account's leaf is in the accounts tree
    ABSENT = 2,  //!< no leaf of the accounts tree is the account's
};

/** A typed v1 account proof: the account, its leaf if it has one, and the
 * SMT proof along its path. Kind() is PRESENT iff `leaf` is set.
 *
 * Encoding: account (32) || u8 kind || for PRESENT, CompactSize length ||
 * the leaf's bytes || CompactSize length || the SmtProof's bytes. Decode is
 * exact under the profile's leaf caps: DECODE for an unknown kind, a leaf
 * of another account, a non-canonical leaf or proof, a PRESENT proof that
 * does not end at the account's own leaf, an ABSENT one that does, and
 * trailing bytes; OVERSIZE above MaxEncodedSize(profile).
 */
struct AccountProofV1 {
    AccountId account;
    std::optional<AccountLeafV0> leaf;
    SmtProof proof;

    friend bool operator==(const AccountProofV1&, const AccountProofV1&) = default;

    AccountProofKind Kind() const { return leaf.has_value() ? AccountProofKind::PRESENT : AccountProofKind::ABSENT; }
    static size_t MaxEncodedSize(const ProfileParams& profile);
    std::vector<unsigned char> Encode() const;
    static Result<AccountProofV1> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);

    //! True iff the proof shows its claim under the accounts root `accounts_root`.
    bool VerifiesUnder(const uint256& accounts_root) const;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_STATE_ROOT_V1_H

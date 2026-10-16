// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_ACCOUNT_LEAF_H
#define B3_FLOWMESH2_CORE_ACCOUNT_LEAF_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <vector>

/** The account leaf of state commitment v0 (slice R4; ARCHITECTURE.md
 * section 9, O-9).
 *
 * One leaf per account in the sorted accounts tree of commitment v0 (R5).
 * It lives in core/ so that b3verify (R11) can decode and check a leaf
 * without linking the ledger: VerifyAccountLeaf, VerifyBalanceTransition
 * and VerifyExpired all read this type.
 *
 * Encoding (Bitcoin serialization, little-endian):
 *   account | CompactSize #balances, each (asset | i64 available | i64 reserved)
 *   | CompactSize #used_nonces, each u64 | u32 open_orders
 *
 * Canonical form, enforced by Decode (DECODE otherwise) and reported by
 * IsCanonical:
 * - balances strictly ascending by AssetId, no entry with available and
 *   reserved both zero (an absent asset means zero), each amount in
 *   [0, MAX_ATOMS];
 * - used_nonces strictly ascending (the D7 window, core/nonce_rule.h);
 * - open_orders <= profile.max_open_orders_per_account.
 * Caps (OVERSIZE, checked before allocation): MAX_LEAF_ASSETS balances,
 * profile.nonce_window nonces, MaxEncodedSize(profile) bytes.
 * Encode() is total so that LeafValue() exists for any value.
 *
 * LeafValue() = Tagged(STATE_ACCOUNTS, Encode()): the value the accounts tree
 * of commitment v0 commits to (R5 builds the tree, sorted by AccountId).
 * exit_value per asset (M2-LED-04) and futures equity are not in v0.
 */
namespace flowmesh2 {

//! Most assets one account leaf may hold. PROVISIONAL (O-27).
inline constexpr size_t MAX_LEAF_ASSETS{256};

struct AssetBalance {
    AssetId asset;
    Atoms available{0};
    Atoms reserved{0};

    static constexpr size_t ENCODED_BYTES{32 + 8 + 8};
    friend bool operator==(const AssetBalance&, const AssetBalance&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << asset << available << reserved;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> asset >> available >> reserved;
        if (!InAtomRange(available) || !InAtomRange(reserved)) {
            throw std::ios_base::failure("AssetBalance outside [0, MAX_ATOMS]");
        }
    }
};

struct AccountLeafV0 {
    AccountId account;
    std::vector<AssetBalance> balances;
    std::vector<uint64_t> used_nonces;
    uint32_t open_orders{0};

    friend bool operator==(const AccountLeafV0&, const AccountLeafV0&) = default;

    static size_t MaxEncodedSize(const ProfileParams& profile);
    std::vector<unsigned char> Encode() const;
    static Result<AccountLeafV0> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);
    //! Tagged(STATE_ACCOUNTS, Encode()).
    uint256 LeafValue() const;

    //! The canonical-form rules above, including the caps.
    bool IsCanonical(const ProfileParams& profile) const;
    //! Lookups on a canonical leaf; 0 for an asset the leaf does not list.
    Atoms Available(const AssetId& asset) const;
    Atoms Reserved(const AssetId& asset) const;
    bool NonceUsed(uint64_t nonce) const;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_ACCOUNT_LEAF_H

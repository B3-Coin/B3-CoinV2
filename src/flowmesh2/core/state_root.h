// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_STATE_ROOT_H
#define B3_FLOWMESH2_CORE_STATE_ROOT_H

#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <serialize.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <ios>
#include <span>
#include <vector>

/** Value types of state commitment v0 shared by the ledger and b3verify
 * (slice R5; ARCHITECTURE.md section 9, O-9).
 *
 * The ledger (ledger/state_commitment.h) builds the trees; b3verify (R11)
 * checks a leaf against a certified header without linking the ledger, so
 * everything a verifier needs to recompute is here in core/: the protocol
 * account ids, the totals leaf, the market and nullifier leaf values and the
 * root of roots. The account leaf is core/account_leaf.h.
 *
 * Commitment v0 (commitment_version = 0): five components, each the
 * size-bound RFC 6962 MerkleRoot (core/merkle.h) of its leaf values sorted by
 * key, every key at most once:
 *   accounts   key AccountId  value AccountLeafV0::LeafValue()  (STATE_ACCOUNTS)
 *   orders     key OrderId    value defined by R7              (STATE_ORDERS)
 *   markets    key MarketId   value MarketLeafValue            (STATE_MARKETS)
 *   nullifiers key Outpoint   value NullifierLeafValue         (STATE_NULLIFIERS)
 *   totals     key AssetId    value TotalsLeafV0::LeafValue()  (STATE_TOTALS)
 * state_root = StateRootsV0::Root() = Tagged(STATE, u8 0 || the five roots in
 * that order). The planned sparse Merkle trees are commitment v1 (M2-LED-02).
 *
 * Protocol accounts (D5): the seat pool receives floor(seat_share_percent) of
 * each slot's fees and the treasury the rest. Their ids are
 * Tagged(PROTOCOL_ACCOUNT, name || profile_hash), name the ASCII text
 * "SEAT_POOL" or "TREASURY" and profile_hash the 32 bytes of ProfileHash. No
 * key controls them: a signer's AccountId is Tagged(ACCOUNT, key), another tag.
 *
 * Deterministic: no allocation besides the returned vectors, no I/O, clock
 * or locale.
 */
namespace flowmesh2 {

//! SlotHeaderV2.commitment_version of the sorted-leaf commitment.
inline constexpr uint8_t COMMITMENT_VERSION_V0{0};

//! Tagged(PROTOCOL_ACCOUNT, "SEAT_POOL" || profile_hash).
// TODO(rebirth:M7-FEE-01) per-seat accrual and claims by BLS key replace the single seat-pool account.
AccountId SeatPoolAccount(const uint256& profile_hash);
AccountId SeatPoolAccount(const ProfileParams& profile);
//! Tagged(PROTOCOL_ACCOUNT, "TREASURY" || profile_hash).
AccountId TreasuryAccount(const uint256& profile_hash);
AccountId TreasuryAccount(const ProfileParams& profile);

/** Ledger totals of one asset. Every field is in [0, MAX_ATOMS].
 *
 * Conservation (checked by the ledger after every operation):
 *   sum over accounts of (available + reserved) + pending + fees_held
 *     == deposits - effects
 */
struct AssetTotals {
    Atoms deposits{0};  //!< cumulative credited deposits (D)
    Atoms effects{0};   //!< cumulative effects issued: withdrawals, payouts (E); 0 until M2-LED-05
    Atoms pending{0};   //!< withdrawals reserved and not yet effects; 0 until M2-LED-05
    Atoms fees_held{0}; //!< seller fees withheld from fills, not yet credited to a protocol account

    static constexpr size_t ENCODED_BYTES{4 * 8};
    bool IsZero() const { return deposits == 0 && effects == 0 && pending == 0 && fees_held == 0; }
    friend bool operator==(const AssetTotals&, const AssetTotals&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << deposits << effects << pending << fees_held;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> deposits >> effects >> pending >> fees_held;
        if (!InAtomRange(deposits) || !InAtomRange(effects) || !InAtomRange(pending) || !InAtomRange(fees_held)) {
            throw std::ios_base::failure("AssetTotals outside [0, MAX_ATOMS]");
        }
    }
};

/** One leaf of the totals component, 64 bytes: asset | i64 deposits |
 * i64 effects | i64 pending | i64 fees_held. Canonical form: not all four
 * zero (an asset the ledger never saw has no leaf); Decode refuses anything
 * else with DECODE (OVERSIZE above 64 bytes).
 */
struct TotalsLeafV0 {
    AssetId asset;
    AssetTotals totals;

    static constexpr size_t ENCODED_BYTES{32 + AssetTotals::ENCODED_BYTES};
    friend bool operator==(const TotalsLeafV0&, const TotalsLeafV0&) = default;

    std::vector<unsigned char> Encode() const;
    static Result<TotalsLeafV0> Decode(std::span<const unsigned char> bytes);
    //! Tagged(STATE_TOTALS, Encode()).
    uint256 LeafValue() const;
};

//! Tagged(STATE_MARKETS, the 80-byte MarketDescriptor serialization).
uint256 MarketLeafValue(const MarketDescriptor& market);

//! Tagged(STATE_NULLIFIERS, txid || u32 LE vout): one credited deposit outpoint.
uint256 NullifierLeafValue(const Outpoint& outpoint);

/** The five component roots of commitment v0, 160 bytes in field order. */
struct StateRootsV0 {
    uint256 accounts;
    uint256 orders;
    uint256 markets;
    uint256 nullifiers;
    uint256 totals;

    static constexpr size_t ENCODED_BYTES{5 * 32};
    friend bool operator==(const StateRootsV0&, const StateRootsV0&) = default;
    SERIALIZE_METHODS(StateRootsV0, obj) { READWRITE(obj.accounts, obj.orders, obj.markets, obj.nullifiers, obj.totals); }

    //! state_root: Tagged(STATE, u8 COMMITMENT_VERSION_V0 || the five roots).
    uint256 Root() const;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_STATE_ROOT_H

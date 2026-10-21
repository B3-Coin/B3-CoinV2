// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_STATE_ROOT_H
#define B3_FLOWMESH2_CORE_STATE_ROOT_H

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
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
 *   orders     key OrderId    value RestingOrderLeafV0::LeafValue() (STATE_ORDERS, R7)
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

//! Pure profile-to-state/certificate version boundary. Call Validate(profile) first.
inline uint8_t ExpectedCommitmentVersion(const ProfileParams& profile)
{
    return profile.view_bound_commits ? COMMITMENT_VERSION_VIEW_BOUND :
           profile.withdrawals_enabled ? COMMITMENT_VERSION_WITHDRAWALS : COMMITMENT_VERSION_V0;
}

// COMMITMENT_VERSION_V0 (the SlotHeaderV2.commitment_version of the
// sorted-leaf commitment) is defined once, in core/slot.h. Until slice R7 this
// header defined it a second time, so no translation unit could include both
// headers (engine/engine.h needs both).

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

/** One resting order in the orders component (slice R7), 153 bytes:
 *   order | market | owner | u8 side | u64 price_ticks | u64 remaining_lots
 *   | u64 arrival_rank | i64 reserved_left | i64 cum_quote | i64 fee_charged
 *   | u64 fills
 * The fields are those of the engine's RestingOrder (engine/book.h) plus the
 * market it rests in; the key is `order`. PROVISIONAL REGTEST (O-32).
 *
 * Canonical form: a known side; price_ticks and remaining_lots of at least
 * 1; reserved_left, cum_quote and fee_charged in [0, MAX_ATOMS];
 * fee_charged <= cum_quote; a BUY has fee_charged 0 (the seller pays, D5).
 * Decode refuses anything else with DECODE (OVERSIZE above 153 bytes).
 * Whether reserved_left is the exact reservation of the remaining lots needs
 * the market descriptor; the engine checks that (book invariant 2), not the
 * leaf. Encode() is total.
 *
 * LeafValue() = Tagged(STATE_ORDERS, Encode()).
 */
struct RestingOrderLeafV0 {
    OrderId order;
    MarketId market;
    AccountId owner;
    Side side{Side::BUY};
    uint64_t price_ticks{0};
    uint64_t remaining_lots{0};
    uint64_t arrival_rank{0};
    Atoms reserved_left{0};
    Atoms cum_quote{0};
    Atoms fee_charged{0};
    uint64_t fills{0};

    static constexpr size_t ENCODED_BYTES{3 * 32 + 1 + 3 * 8 + 3 * 8 + 8};
    friend bool operator==(const RestingOrderLeafV0&, const RestingOrderLeafV0&) = default;

    //! True iff the leaf has the canonical form above.
    bool IsCanonical() const;
    std::vector<unsigned char> Encode() const;
    static Result<RestingOrderLeafV0> Decode(std::span<const unsigned char> bytes);
    //! Tagged(STATE_ORDERS, Encode()).
    uint256 LeafValue() const;
};

/** One immutable withdrawal identity and its monotone unpaid -> paid state.
 * Paid records are retained: their payout event is a committed nullifier.
 * Encode is fixed-width; LeafValue uses STATE_TOTALS with prefix byte 2 and
 * 177-byte payload, distinct from the 64-byte legacy totals leaf domain.
 */
struct WithdrawalRecord {
    ActionId id;
    AccountId account;
    AssetId asset;
    Atoms amount{0};
    Atoms fee{0};
    std::array<unsigned char, 20> recipient{};
    uint64_t slot{0};
    bool paid{false};
    Outpoint payout_event;

    static constexpr size_t ENCODED_BYTES{3 * 32 + 2 * 8 + 20 + 8 + 1 + 36};
    bool IsCanonical() const;
    std::vector<unsigned char> Encode() const;
    static Result<WithdrawalRecord> Decode(std::span<const unsigned char> bytes);
    uint256 LeafValue() const;
    friend bool operator==(const WithdrawalRecord&, const WithdrawalRecord&) = default;
};

/** Legacy v0 wire encoding remains five roots/160 bytes. The in-memory
 * carrier also holds versions 2/3's sixth withdrawals root and explicit
 * version. Both use six components with distinct root-of-roots prefixes;
 * they require a versioned transport, never this legacy codec.
 */
struct StateRootsV0 {
    uint256 accounts;
    uint256 orders;
    uint256 markets;
    uint256 nullifiers;
    uint256 totals;
    uint256 withdrawals;
    uint8_t version{COMMITMENT_VERSION_V0};

    static constexpr size_t ENCODED_BYTES{5 * 32};
    friend bool operator==(const StateRootsV0&, const StateRootsV0&) = default;
    template <typename Stream> void Serialize(Stream& s) const
    {
        if (version != COMMITMENT_VERSION_V0 || !withdrawals.IsNull()) {
            throw std::ios_base::failure("extended state roots require a versioned transport");
        }
        s << accounts << orders << markets << nullifiers << totals;
    }
    template <typename Stream> void Unserialize(Stream& s)
    {
        s >> accounts >> orders >> markets >> nullifiers >> totals;
        withdrawals = uint256{};
        version = COMMITMENT_VERSION_V0;
    }

    //! state_root: Tagged(STATE, u8 COMMITMENT_VERSION_V0 || the five roots).
    uint256 Root() const;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_STATE_ROOT_H

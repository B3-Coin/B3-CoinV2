// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_FEE_ALLOCATION_H
#define B3COIN_FLOWMESH_FEE_ALLOCATION_H

#include <consensus/amount.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <flowmesh/ledger.h>
#include <flowmesh/seat_id.h>
#include <hash.h>
#include <util/int128.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace flowmesh {

inline constexpr CAmount FLOWMESH_FEE_DENOMINATOR{10'000}; // exactly 0.01%
inline constexpr CAmount FLOWMESH_TREASURY_PERCENT{20};
inline constexpr size_t FLOWMESH_MAX_FEE_SEATS{5'000};
inline constexpr const char* FLOWMESH_SEAT_REWARD_ACCOUNT_TAG{
    "B3/FLOWMESH/SEAT-REWARD/V1"};
inline constexpr const char* FLOWMESH_TREASURY_FEE_ACCOUNT_TAG{
    "B3/FLOWMESH/TREASURY-FEE/V1"};

/**
 * The immutable identity needed to credit one epoch seat's fee rewards.
 * Seat order is the anchored SeatId order used by the certificate bitmap.
 * The BLS key remains in the account derivation so a reward stays claimable
 * by that key after the backing FN output is spent or rebound.
 */
struct FlowMeshFeeSeat {
    SeatId seat_id;
    std::array<unsigned char, bls::PUBKEY_SIZE> bls_pubkey{};

    friend bool operator==(const FlowMeshFeeSeat&, const FlowMeshFeeSeat&) = default;
};

/** Frozen fee recipients for one market epoch. */
struct FlowMeshFeeContext {
    uint256 market_id;
    uint64_t epoch{0};
    uint256 treasury_owner_commitment;
    std::vector<FlowMeshFeeSeat> seats;
};

inline AccountId FlowMeshSeatRewardAccount(const uint256& market_id,
                                           const uint64_t epoch,
                                           const FlowMeshFeeSeat& seat)
{
    std::array<unsigned char, 8> epoch_bytes{};
    WriteBE64(epoch_bytes.data(), epoch);
    HashWriter writer{TaggedHash(FLOWMESH_SEAT_REWARD_ACCOUNT_TAG)};
    writer << std::span<const unsigned char>{market_id.begin(), 32}
           << std::span<const unsigned char>{epoch_bytes}
           << std::span<const unsigned char>{seat.seat_id.begin(), 32}
           << std::span<const unsigned char>{seat.bls_pubkey};
    return writer.GetSHA256();
}

inline AccountId FlowMeshSeatRewardAccount(const FlowMeshFeeContext& context,
                                           const FlowMeshFeeSeat& seat)
{
    return FlowMeshSeatRewardAccount(context.market_id, context.epoch, seat);
}

inline AccountId FlowMeshTreasuryFeeAccount(const FlowMeshFeeContext& context)
{
    HashWriter writer{TaggedHash(FLOWMESH_TREASURY_FEE_ACCOUNT_TAG)};
    writer << std::span<const unsigned char>{context.market_id.begin(), 32}
           << std::span<const unsigned char>{context.treasury_owner_commitment.begin(), 32};
    return writer.GetSHA256();
}

inline bool FlowMeshFeeContextIsCanonical(const FlowMeshFeeContext& context)
{
    if (context.market_id.IsNull() || context.treasury_owner_commitment.IsNull() ||
        context.seats.size() < 4 || context.seats.size() > FLOWMESH_MAX_FEE_SEATS) {
        return false;
    }
    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> unique_keys;
    for (size_t i{0}; i < context.seats.size(); ++i) {
        if (context.seats[i].seat_id.IsNull()) return false;
        if (!bls::PublicKey::Decode(context.seats[i].bls_pubkey) ||
            !unique_keys.insert(context.seats[i].bls_pubkey).second) {
            return false;
        }
        if (i > 0 && !(context.seats[i - 1].seat_id < context.seats[i].seat_id)) return false;
    }
    return true;
}

struct SellerQuoteProceeds {
    AccountId account;
    CAmount amount{0};

    friend bool operator==(const SellerQuoteProceeds&, const SellerQuoteProceeds&) = default;
};

struct SellerFeeShare {
    AccountId account;
    CAmount gross_quote_proceeds{0};
    CAmount fee{0};
    CAmount net_quote_proceeds{0};

    friend bool operator==(const SellerFeeShare&, const SellerFeeShare&) = default;
};

struct SeatFeeShare {
    SeatId seat_id;
    CAmount reward{0};

    friend bool operator==(const SeatFeeShare&, const SeatFeeShare&) = default;
};

/**
 * Complete deterministic allocation for one cleared FlowMesh slot.
 *
 * `seller_fees` is always in AccountId order. `seat_rewards` preserves the
 * caller-supplied canonical SeatId order. The aggregate fee is withheld once
 * from sellers' B3 proceeds; buyers are not represented here and are never
 * charged by this primitive.
 */
struct FlowMeshFeeAllocation {
    CAmount matched_b3_quote_notional{0};
    CAmount fee_total{0};
    CAmount treasury_fee{0};
    CAmount seat_fee{0};
    std::vector<SellerFeeShare> seller_fees;
    std::vector<SeatFeeShare> seat_rewards;

    friend bool operator==(const FlowMeshFeeAllocation&, const FlowMeshFeeAllocation&) = default;
};

enum class FeeAllocationCheck : uint8_t {
    OK = 0,
    MATCHED_NOTIONAL_OUT_OF_RANGE,
    SELLER_PROCEEDS_OUT_OF_RANGE,
    DUPLICATE_SELLER,
    SELLER_TOTAL_OUT_OF_RANGE,
    SELLER_TOTAL_MISMATCH,
    TOO_MANY_SEATS,
    NON_CANONICAL_SEATS,
    NO_SEATS_FOR_REWARD,
    ARITHMETIC_INVARIANT_FAILURE,
};

inline const char* FeeAllocationCheckName(const FeeAllocationCheck check)
{
    switch (check) {
    case FeeAllocationCheck::OK: return "ok";
    case FeeAllocationCheck::MATCHED_NOTIONAL_OUT_OF_RANGE:
        return "matched-notional-out-of-range";
    case FeeAllocationCheck::SELLER_PROCEEDS_OUT_OF_RANGE:
        return "seller-proceeds-out-of-range";
    case FeeAllocationCheck::DUPLICATE_SELLER: return "duplicate-seller";
    case FeeAllocationCheck::SELLER_TOTAL_OUT_OF_RANGE: return "seller-total-out-of-range";
    case FeeAllocationCheck::SELLER_TOTAL_MISMATCH: return "seller-total-mismatch";
    case FeeAllocationCheck::TOO_MANY_SEATS: return "too-many-seats";
    case FeeAllocationCheck::NON_CANONICAL_SEATS: return "non-canonical-seats";
    case FeeAllocationCheck::NO_SEATS_FOR_REWARD: return "no-seats-for-reward";
    case FeeAllocationCheck::ARITHMETIC_INVARIANT_FAILURE:
        return "arithmetic-invariant-failure";
    }
    return "unknown";
}

/**
 * Allocate the frozen FlowMesh v1 spot fee.
 *
 * Formula:
 *
 *     fee_total    = floor(matched B3 quote notional / 10,000)
 *     treasury_fee = floor(fee_total * 20 / 100)
 *     seat_fee     = fee_total - treasury_fee
 *
 * Seller shares use exact widened products and largest remainder. Equal
 * remainders are resolved by ascending AccountId. Seat rewards are an equal
 * division in the supplied strict SeatId order, with the first remainder
 * seats receiving one additional base unit.
 *
 * Seller input may be in any order, but it must already be aggregated to one
 * positive entry per seller and must sum exactly to `matched_notional`.
 * `canonical_seats` must be strictly ascending and contain at most the FN cap
 * of 5,000. An empty seat set is accepted only when the seat pool is zero;
 * the production active-set minimum (four) belongs to the seat-epoch layer.
 */
inline std::optional<FlowMeshFeeAllocation> AllocateFlowMeshFees(
    const CAmount matched_notional,
    std::span<const SellerQuoteProceeds> seller_proceeds,
    std::span<const SeatId> canonical_seats,
    FeeAllocationCheck& check)
{
    check = FeeAllocationCheck::ARITHMETIC_INVARIANT_FAILURE;
    if (!MoneyRange(matched_notional)) {
        check = FeeAllocationCheck::MATCHED_NOTIONAL_OUT_OF_RANGE;
        return std::nullopt;
    }
    if (canonical_seats.size() > FLOWMESH_MAX_FEE_SEATS) {
        check = FeeAllocationCheck::TOO_MANY_SEATS;
        return std::nullopt;
    }
    for (size_t i{1}; i < canonical_seats.size(); ++i) {
        if (!(canonical_seats[i - 1] < canonical_seats[i])) {
            check = FeeAllocationCheck::NON_CANONICAL_SEATS;
            return std::nullopt;
        }
    }

    std::vector<SellerQuoteProceeds> sellers{seller_proceeds.begin(), seller_proceeds.end()};
    std::sort(sellers.begin(), sellers.end(), [](const auto& a, const auto& b) {
        return a.account < b.account;
    });

    util::Unsigned128 seller_total{0};
    for (size_t i{0}; i < sellers.size(); ++i) {
        if (!MoneyRange(sellers[i].amount) || sellers[i].amount == 0) {
            check = FeeAllocationCheck::SELLER_PROCEEDS_OUT_OF_RANGE;
            return std::nullopt;
        }
        if (i > 0 && sellers[i - 1].account == sellers[i].account) {
            check = FeeAllocationCheck::DUPLICATE_SELLER;
            return std::nullopt;
        }
        seller_total += static_cast<util::Unsigned128>(sellers[i].amount);
        if (seller_total > static_cast<util::Unsigned128>(MAX_MONEY)) {
            check = FeeAllocationCheck::SELLER_TOTAL_OUT_OF_RANGE;
            return std::nullopt;
        }
    }
    if (seller_total != static_cast<util::Unsigned128>(matched_notional)) {
        check = FeeAllocationCheck::SELLER_TOTAL_MISMATCH;
        return std::nullopt;
    }

    FlowMeshFeeAllocation out;
    out.matched_b3_quote_notional = matched_notional;
    out.fee_total = matched_notional / FLOWMESH_FEE_DENOMINATOR;
    const util::Unsigned128 treasury_wide{
        static_cast<util::Unsigned128>(out.fee_total) * FLOWMESH_TREASURY_PERCENT / 100};
    if (treasury_wide > static_cast<util::Unsigned128>(MAX_MONEY)) return std::nullopt;
    out.treasury_fee = static_cast<CAmount>(treasury_wide);
    out.seat_fee = out.fee_total - out.treasury_fee;
    if (!MoneyRange(out.fee_total) || !MoneyRange(out.treasury_fee) ||
        !MoneyRange(out.seat_fee) || out.treasury_fee > out.fee_total) {
        return std::nullopt;
    }
    if (out.seat_fee > 0 && canonical_seats.empty()) {
        check = FeeAllocationCheck::NO_SEATS_FOR_REWARD;
        return std::nullopt;
    }

    struct SellerRemainder {
        size_t index{0};
        CAmount remainder{0};
    };
    std::vector<SellerRemainder> remainders;
    remainders.reserve(sellers.size());
    out.seller_fees.reserve(sellers.size());

    CAmount allocated_seller_fee{0};
    for (size_t i{0}; i < sellers.size(); ++i) {
        const util::Unsigned128 product{static_cast<util::Unsigned128>(sellers[i].amount) *
                                        static_cast<util::Unsigned128>(out.fee_total)};
        const CAmount base_fee{matched_notional == 0
                                   ? 0
                                   : static_cast<CAmount>(
                                         product / static_cast<util::Unsigned128>(matched_notional))};
        const CAmount remainder{matched_notional == 0
                                    ? 0
                                    : static_cast<CAmount>(
                                          product % static_cast<util::Unsigned128>(matched_notional))};
        if (!MoneyRange(base_fee) || base_fee > sellers[i].amount ||
            base_fee > MAX_MONEY - allocated_seller_fee) {
            return std::nullopt;
        }
        allocated_seller_fee += base_fee;
        out.seller_fees.push_back(
            {sellers[i].account, sellers[i].amount, base_fee, sellers[i].amount - base_fee});
        remainders.push_back({i, remainder});
    }
    if (allocated_seller_fee > out.fee_total) return std::nullopt;

    const CAmount seller_extras_amount{out.fee_total - allocated_seller_fee};
    if (seller_extras_amount < 0 ||
        (seller_extras_amount > 0 &&
         static_cast<util::Unsigned128>(seller_extras_amount) >= sellers.size())) {
        return std::nullopt;
    }
    std::sort(remainders.begin(), remainders.end(), [&](const auto& a, const auto& b) {
        if (a.remainder != b.remainder) return a.remainder > b.remainder;
        return sellers[a.index].account < sellers[b.index].account;
    });
    const size_t seller_extras{static_cast<size_t>(seller_extras_amount)};
    for (size_t i{0}; i < seller_extras; ++i) {
        SellerFeeShare& share{out.seller_fees[remainders[i].index]};
        if (share.fee >= share.gross_quote_proceeds) return std::nullopt;
        ++share.fee;
        --share.net_quote_proceeds;
        ++allocated_seller_fee;
    }
    if (allocated_seller_fee != out.fee_total) return std::nullopt;

    out.seat_rewards.reserve(canonical_seats.size());
    CAmount allocated_seat_fee{0};
    if (!canonical_seats.empty()) {
        const CAmount seat_count{static_cast<CAmount>(canonical_seats.size())};
        const CAmount quotient{out.seat_fee / seat_count};
        const size_t remainder{static_cast<size_t>(out.seat_fee % seat_count)};
        for (size_t i{0}; i < canonical_seats.size(); ++i) {
            const CAmount reward{quotient + (i < remainder ? 1 : 0)};
            if (!MoneyRange(reward) || reward > MAX_MONEY - allocated_seat_fee) {
                return std::nullopt;
            }
            allocated_seat_fee += reward;
            out.seat_rewards.push_back({canonical_seats[i], reward});
        }
    }
    if (allocated_seat_fee != out.seat_fee ||
        out.treasury_fee > MAX_MONEY - allocated_seat_fee ||
        out.treasury_fee + allocated_seat_fee != out.fee_total) {
        return std::nullopt;
    }

    check = FeeAllocationCheck::OK;
    return out;
}

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_FEE_ALLOCATION_H

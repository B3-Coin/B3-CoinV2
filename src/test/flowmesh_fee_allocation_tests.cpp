// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/fee_allocation.h>
#include <flowmesh/market.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace {

flowmesh::SeatId Id(const uint32_t value)
{
    flowmesh::SeatId out;
    out.begin()[0] = static_cast<unsigned char>(value >> 24);
    out.begin()[1] = static_cast<unsigned char>(value >> 16);
    out.begin()[2] = static_cast<unsigned char>(value >> 8);
    out.begin()[3] = static_cast<unsigned char>(value);
    return out;
}

std::vector<flowmesh::SeatId> Seats(const size_t count)
{
    std::vector<flowmesh::SeatId> out;
    out.reserve(count);
    for (size_t i{0}; i < count; ++i) out.push_back(Id(static_cast<uint32_t>(i + 1)));
    return out;
}

CAmount SellerFeeSum(const flowmesh::FlowMeshFeeAllocation& allocation)
{
    return std::accumulate(allocation.seller_fees.begin(), allocation.seller_fees.end(),
                           CAmount{0}, [](const CAmount sum, const auto& share) {
                               return sum + share.fee;
                           });
}

CAmount SeatRewardSum(const flowmesh::FlowMeshFeeAllocation& allocation)
{
    return std::accumulate(allocation.seat_rewards.begin(), allocation.seat_rewards.end(),
                           CAmount{0}, [](const CAmount sum, const auto& share) {
                               return sum + share.reward;
                           });
}

void CheckInvariants(const flowmesh::FlowMeshFeeAllocation& allocation)
{
    BOOST_CHECK(MoneyRange(allocation.matched_b3_quote_notional));
    BOOST_CHECK(MoneyRange(allocation.fee_total));
    BOOST_CHECK(MoneyRange(allocation.treasury_fee));
    BOOST_CHECK(MoneyRange(allocation.seat_fee));
    BOOST_CHECK_EQUAL(allocation.fee_total,
                      allocation.matched_b3_quote_notional /
                          flowmesh::FLOWMESH_FEE_DENOMINATOR);
    BOOST_CHECK_EQUAL(allocation.treasury_fee,
                      allocation.fee_total * flowmesh::FLOWMESH_TREASURY_PERCENT / 100);
    BOOST_CHECK_EQUAL(allocation.treasury_fee + allocation.seat_fee,
                      allocation.fee_total);
    BOOST_CHECK_EQUAL(SellerFeeSum(allocation), allocation.fee_total);
    BOOST_CHECK_EQUAL(SeatRewardSum(allocation), allocation.seat_fee);
    for (const auto& share : allocation.seller_fees) {
        BOOST_CHECK(MoneyRange(share.gross_quote_proceeds));
        BOOST_CHECK(MoneyRange(share.fee));
        BOOST_CHECK(MoneyRange(share.net_quote_proceeds));
        BOOST_CHECK_EQUAL(share.fee + share.net_quote_proceeds,
                          share.gross_quote_proceeds);
    }
    for (const auto& share : allocation.seat_rewards) {
        BOOST_CHECK(MoneyRange(share.reward));
    }
}

std::optional<flowmesh::FlowMeshFeeAllocation> Allocate(
    const CAmount matched,
    const std::vector<flowmesh::SellerQuoteProceeds>& sellers,
    const std::vector<flowmesh::SeatId>& seats,
    flowmesh::FeeAllocationCheck& check)
{
    return flowmesh::AllocateFlowMeshFees(matched, sellers, seats, check);
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_fee_allocation_tests)

BOOST_AUTO_TEST_CASE(golden_largest_remainder_and_four_seat_split)
{
    // fee=floor(70,002/10,000)=7. Equal seller remainders award the one
    // leftover unit to the lowest AccountId, independently of input order.
    const std::vector<flowmesh::SellerQuoteProceeds> sellers{
        {Id(3), 23'334}, {Id(1), 23'334}, {Id(2), 23'334}};
    const auto seats{Seats(4)};
    flowmesh::FeeAllocationCheck check;
    const auto allocation{Allocate(70'002, sellers, seats, check)};
    BOOST_REQUIRE(allocation.has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::OK);
    BOOST_CHECK_EQUAL(allocation->fee_total, 7);
    BOOST_CHECK_EQUAL(allocation->treasury_fee, 1);
    BOOST_CHECK_EQUAL(allocation->seat_fee, 6);

    BOOST_REQUIRE_EQUAL(allocation->seller_fees.size(), 3U);
    BOOST_CHECK(allocation->seller_fees[0].account == Id(1));
    BOOST_CHECK(allocation->seller_fees[1].account == Id(2));
    BOOST_CHECK(allocation->seller_fees[2].account == Id(3));
    BOOST_CHECK_EQUAL(allocation->seller_fees[0].fee, 3);
    BOOST_CHECK_EQUAL(allocation->seller_fees[1].fee, 2);
    BOOST_CHECK_EQUAL(allocation->seller_fees[2].fee, 2);
    BOOST_CHECK_EQUAL(allocation->seller_fees[0].net_quote_proceeds, 23'331);

    BOOST_REQUIRE_EQUAL(allocation->seat_rewards.size(), 4U);
    BOOST_CHECK_EQUAL(allocation->seat_rewards[0].reward, 2);
    BOOST_CHECK_EQUAL(allocation->seat_rewards[1].reward, 2);
    BOOST_CHECK_EQUAL(allocation->seat_rewards[2].reward, 1);
    BOOST_CHECK_EQUAL(allocation->seat_rewards[3].reward, 1);
    CheckInvariants(*allocation);
}

BOOST_AUTO_TEST_CASE(fees_zero_and_one_have_exact_floor_semantics)
{
    flowmesh::FeeAllocationCheck check;

    // A zero fee has no reward pool, so an empty active-seat view is valid.
    const std::vector<flowmesh::SellerQuoteProceeds> zero_sellers{
        {Id(2), 4'999}, {Id(1), 5'000}};
    const std::vector<flowmesh::SeatId> no_seats;
    const auto zero{Allocate(9'999, zero_sellers, no_seats, check)};
    BOOST_REQUIRE(zero.has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::OK);
    BOOST_CHECK_EQUAL(zero->fee_total, 0);
    BOOST_CHECK_EQUAL(zero->treasury_fee, 0);
    BOOST_CHECK_EQUAL(zero->seat_fee, 0);
    BOOST_CHECK_EQUAL(SellerFeeSum(*zero), 0);
    BOOST_CHECK(zero->seat_rewards.empty());
    CheckInvariants(*zero);

    // One base unit cannot be rounded into the 20% treasury share. Seller
    // and seat tie-breaks both award it to the first canonical id.
    const std::vector<flowmesh::SellerQuoteProceeds> one_sellers{
        {Id(2), 5'000}, {Id(1), 5'000}};
    const auto five_seats{Seats(5)};
    const auto one{Allocate(10'000, one_sellers, five_seats, check)};
    BOOST_REQUIRE(one.has_value());
    BOOST_CHECK_EQUAL(one->fee_total, 1);
    BOOST_CHECK_EQUAL(one->treasury_fee, 0);
    BOOST_CHECK_EQUAL(one->seat_fee, 1);
    BOOST_CHECK_EQUAL(one->seller_fees[0].fee, 1);
    BOOST_CHECK_EQUAL(one->seller_fees[1].fee, 0);
    BOOST_REQUIRE_EQUAL(one->seat_rewards.size(), 5U);
    BOOST_CHECK_EQUAL(one->seat_rewards[0].reward, 1);
    for (size_t i{1}; i < one->seat_rewards.size(); ++i) {
        BOOST_CHECK_EQUAL(one->seat_rewards[i].reward, 0);
    }
    CheckInvariants(*one);
}

BOOST_AUTO_TEST_CASE(five_and_five_thousand_seat_rounding_is_canonical)
{
    flowmesh::FeeAllocationCheck check;

    const auto five{Seats(5)};
    const std::vector<flowmesh::SellerQuoteProceeds> sellers_five{{Id(1), 50'000}};
    const auto split_five{Allocate(50'000, sellers_five, five, check)};
    BOOST_REQUIRE(split_five.has_value());
    // fee=5, treasury=1, seats=4: first four receive one.
    for (size_t i{0}; i < five.size(); ++i) {
        BOOST_CHECK_EQUAL(split_five->seat_rewards[i].reward, i < 4 ? 1 : 0);
    }
    CheckInvariants(*split_five);

    const auto five_thousand{Seats(5'000)};
    const std::vector<flowmesh::SellerQuoteProceeds> sellers_max{{Id(1), 62'510'000}};
    const auto split_max{Allocate(62'510'000, sellers_max, five_thousand, check)};
    BOOST_REQUIRE(split_max.has_value());
    BOOST_REQUIRE_EQUAL(split_max->seat_rewards.size(), 5'000U);
    // fee=6,251, treasury=1,250, seats=5,001: quotient one and the first
    // canonical seat receives the single remainder unit.
    BOOST_CHECK_EQUAL(split_max->fee_total, 6'251);
    BOOST_CHECK_EQUAL(split_max->treasury_fee, 1'250);
    BOOST_CHECK_EQUAL(split_max->seat_fee, 5'001);
    BOOST_CHECK_EQUAL(split_max->seat_rewards.front().reward, 2);
    for (size_t i{1}; i < split_max->seat_rewards.size(); ++i) {
        BOOST_CHECK_EQUAL(split_max->seat_rewards[i].reward, 1);
    }
    CheckInvariants(*split_max);
}

BOOST_AUTO_TEST_CASE(max_money_products_and_sums_do_not_overflow)
{
    const CAmount first{MAX_MONEY / 3};
    const CAmount second{MAX_MONEY - first};
    const std::vector<flowmesh::SellerQuoteProceeds> sellers{
        {Id(2), second}, {Id(1), first}};
    const auto seats{Seats(4)};
    flowmesh::FeeAllocationCheck check;
    const auto allocation{Allocate(MAX_MONEY, sellers, seats, check)};
    BOOST_REQUIRE(allocation.has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::OK);
    BOOST_CHECK_EQUAL(allocation->fee_total, MAX_MONEY / 10'000);
    BOOST_CHECK_EQUAL(allocation->matched_b3_quote_notional, MAX_MONEY);
    CheckInvariants(*allocation);

    // Both widened proportional products are far above int64, yet their
    // exact largest-remainder shares still sum to the one protocol fee.
    BOOST_CHECK(allocation->seller_fees[0].fee <= first);
    BOOST_CHECK(allocation->seller_fees[1].fee <= second);
}

BOOST_AUTO_TEST_CASE(seller_input_order_does_not_change_output)
{
    std::vector<flowmesh::SellerQuoteProceeds> forward{
        {Id(1), 10'001}, {Id(2), 20'002}, {Id(3), 40'004}};
    auto reverse{forward};
    std::reverse(reverse.begin(), reverse.end());
    const auto seats{Seats(4)};
    flowmesh::FeeAllocationCheck check_a;
    flowmesh::FeeAllocationCheck check_b;
    const auto a{Allocate(70'007, forward, seats, check_a)};
    const auto b{Allocate(70'007, reverse, seats, check_b)};
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());
    BOOST_CHECK(check_a == flowmesh::FeeAllocationCheck::OK);
    BOOST_CHECK(check_b == flowmesh::FeeAllocationCheck::OK);
    BOOST_CHECK(*a == *b);
}

BOOST_AUTO_TEST_CASE(invalid_amounts_aggregation_and_overflow_fail_explicitly)
{
    const auto seats{Seats(4)};
    const std::vector<flowmesh::SellerQuoteProceeds> none;
    flowmesh::FeeAllocationCheck check;

    BOOST_CHECK(!Allocate(-1, none, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::MATCHED_NOTIONAL_OUT_OF_RANGE);
    BOOST_CHECK(!Allocate(MAX_MONEY + 1, none, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::MATCHED_NOTIONAL_OUT_OF_RANGE);

    BOOST_CHECK(!Allocate(1, {{Id(1), -1}}, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::SELLER_PROCEEDS_OUT_OF_RANGE);
    BOOST_CHECK(!Allocate(1, {{Id(1), 0}}, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::SELLER_PROCEEDS_OUT_OF_RANGE);
    BOOST_CHECK(!Allocate(MAX_MONEY, {{Id(1), MAX_MONEY + 1}}, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::SELLER_PROCEEDS_OUT_OF_RANGE);

    BOOST_CHECK(!Allocate(2, {{Id(1), 1}, {Id(1), 1}}, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::DUPLICATE_SELLER);
    BOOST_CHECK(!Allocate(2, {{Id(1), 1}}, seats, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::SELLER_TOTAL_MISMATCH);

    // Individually valid amounts whose aggregate exceeds MAX_MONEY fail
    // before any narrowed sum or proportional multiplication is attempted.
    BOOST_CHECK(!Allocate(MAX_MONEY,
                          {{Id(1), MAX_MONEY}, {Id(2), 1}}, seats, check)
                     .has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::SELLER_TOTAL_OUT_OF_RANGE);
}

BOOST_AUTO_TEST_CASE(seat_set_shape_and_nonzero_pool_fail_closed)
{
    const std::vector<flowmesh::SellerQuoteProceeds> sellers{{Id(1), 10'000}};
    flowmesh::FeeAllocationCheck check;

    const std::vector<flowmesh::SeatId> none;
    BOOST_CHECK(!Allocate(10'000, sellers, none, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::NO_SEATS_FOR_REWARD);

    auto reversed{Seats(4)};
    std::reverse(reversed.begin(), reversed.end());
    BOOST_CHECK(!Allocate(10'000, sellers, reversed, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::NON_CANONICAL_SEATS);

    auto duplicate{Seats(4)};
    duplicate[2] = duplicate[1];
    BOOST_CHECK(!Allocate(10'000, sellers, duplicate, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::NON_CANONICAL_SEATS);

    const auto too_many{Seats(flowmesh::FLOWMESH_MAX_FEE_SEATS + 1)};
    BOOST_CHECK(!Allocate(10'000, sellers, too_many, check).has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::TOO_MANY_SEATS);
}

BOOST_AUTO_TEST_CASE(zero_trade_is_a_valid_empty_allocation)
{
    const std::vector<flowmesh::SellerQuoteProceeds> no_sellers;
    const std::vector<flowmesh::SeatId> no_seats;
    flowmesh::FeeAllocationCheck check;
    const auto allocation{Allocate(0, no_sellers, no_seats, check)};
    BOOST_REQUIRE(allocation.has_value());
    BOOST_CHECK(check == flowmesh::FeeAllocationCheck::OK);
    BOOST_CHECK(allocation->seller_fees.empty());
    BOOST_CHECK(allocation->seat_rewards.empty());
    CheckInvariants(*allocation);
}

BOOST_AUTO_TEST_CASE(v1_market_and_vault_identity_are_domain_and_pair_bound)
{
    const uint256 domain{Id(91)};
    const modern::AssetId base{Id(92)};
    const auto market{flowmesh::ComputeFlowMeshMarketId(domain, base)};
    BOOST_REQUIRE(market.has_value());
    const auto vault{flowmesh::ComputeFlowMeshVaultId(domain, *market)};
    BOOST_REQUIRE(vault.has_value());
    BOOST_CHECK(!market->IsNull());
    BOOST_CHECK(!vault->IsNull());
    BOOST_CHECK(*market != *vault);

    BOOST_CHECK(flowmesh::ComputeFlowMeshMarketId(Id(93), base) != market);
    BOOST_CHECK(flowmesh::ComputeFlowMeshMarketId(domain, Id(94)) != market);
    BOOST_CHECK(flowmesh::ComputeFlowMeshVaultId(Id(93), *market) != vault);
    BOOST_CHECK(!flowmesh::ComputeFlowMeshMarketId(uint256{}, base));
    BOOST_CHECK(!flowmesh::ComputeFlowMeshMarketId(domain, modern::NativeAsset()));
    BOOST_CHECK(!flowmesh::ComputeFlowMeshMarketId(domain, base, Id(95)));
    BOOST_CHECK(!flowmesh::ComputeFlowMeshVaultId(domain, uint256{}));

    static_assert(flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS == 8);
    static_assert(flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS == 1024);
    static_assert(flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES == 2U * 1024U * 1024U);
}

BOOST_AUTO_TEST_SUITE_END()

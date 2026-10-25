// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/state/snapshot.h>
#include <flowmesh2/test/util/state_fixtures.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::test;

namespace {

/** Genesis, the walking-skeleton slots 1-5 and a sixth slot that rests
 * three more orders, two of them in one price level (FIFO), and one bid.
 */
std::vector<std::shared_ptr<const state::HeadView>> TradedHeads(OwnerFixture& f)
{
    std::vector<std::shared_ptr<const state::HeadView>> heads{f.Head()};
    const SkeletonRun run{RunWalkingSkeleton(f)};
    heads.insert(heads.end(), run.heads.begin(), run.heads.end());
    SlotFixture& a{f.actions};
    const auto first{a.Place("bob", 3, MarketM1(), Side::SELL, TimeInForce::GTC, 251, 100)};
    const auto second{a.Place("bob", 4, MarketM1(), Side::SELL, TimeInForce::GTC, 251, 200)};
    const auto bid{a.Place("alice", 5, MarketM1(), Side::BUY, TimeInForce::GTC, 200, 1'000)};
    heads.push_back(f.Run(f.Inputs({}, {first, second, bid})));
    return heads;
}

std::vector<unsigned char> MustEncode(const state::CoreState& state)
{
    Result<std::vector<unsigned char>> bytes{state::EncodeSnapshot(state)};
    RequireOk(bytes, "EncodeSnapshot");
    return std::move(bytes).value();
}

//! The parts of a snapshot, to compose bytes the encoder would never write.
struct Composed {
    uint256 profile_hash;
    uint256 seat_set_root;
    std::vector<MarketDescriptor> markets;
    std::vector<AccountLeafV0> accounts;
    std::vector<Outpoint> nullifiers;
    std::vector<std::pair<AssetId, AssetTotals>> totals;
    std::vector<WithdrawalRecord> withdrawals;
    std::vector<RestingOrderLeafV0> orders;

    explicit Composed(const state::CoreState& state)
        : profile_hash{ProfileHash(state.Profile())}, seat_set_root{state.seat_set_root}, markets{state.engine.Markets()},
          nullifiers{state.engine.ledger.Nullifiers().begin(), state.engine.ledger.Nullifiers().end()},
          totals{state.engine.ledger.Totals().begin(), state.engine.ledger.Totals().end()}, orders{state.engine.OrderLeaves()}
    {
        for (const auto& [account, entry] : state.engine.ledger.Accounts()) accounts.push_back(state.engine.ledger.Leaf(account).value());
        for (const auto& [id, record] : state.engine.ledger.Withdrawals()) withdrawals.push_back(record);
    }

    std::vector<unsigned char> Bytes() const
    {
        std::vector<unsigned char> out;
        VectorWriter writer{out, 0};
        writer.write(MakeByteSpan(state::SNAPSHOT_MAGIC));
        writer << state::SNAPSHOT_FORMAT_VERSION << profile_hash << seat_set_root;
        WriteCompactSize(writer, markets.size());
        for (const MarketDescriptor& market : markets) writer << market;
        WriteCompactSize(writer, accounts.size());
        for (const AccountLeafV0& leaf : accounts) writer << leaf.Encode();
        WriteCompactSize(writer, nullifiers.size());
        for (const Outpoint& outpoint : nullifiers) writer << outpoint;
        WriteCompactSize(writer, totals.size());
        for (const auto& [asset, value] : totals) writer << asset << value;
        WriteCompactSize(writer, withdrawals.size());
        for (const WithdrawalRecord& record : withdrawals) writer.write(MakeByteSpan(record.Encode()));
        WriteCompactSize(writer, orders.size());
        for (const RestingOrderLeafV0& order : orders) writer.write(MakeByteSpan(order.Encode()));
        return out;
    }
};

template <class T>
void RequireRefused(const Result<T>& result, ErrorCode code, std::string_view detail, const std::string& what)
{
    BOOST_REQUIRE_MESSAGE(!result.has_value(), what + ": unexpectedly accepted");
    BOOST_CHECK_MESSAGE(result.error().code == code && result.error().detail == detail,
                        what + ": got " + std::string{ToString(result.error().code)} + " '" + std::string{result.error().detail} + "'");
}

} // namespace

BOOST_AUTO_TEST_SUITE(snapshot_tests)

BOOST_AUTO_TEST_CASE(every_head_round_trips_through_its_snapshot)
{
    OwnerFixture f;
    const std::vector<std::shared_ptr<const state::HeadView>> heads{TradedHeads(f)};
    BOOST_REQUIRE_EQUAL(heads.size(), 7U);
    // The last state has resting orders on both sides (bids first), two asks in one level, and fees paid out.
    const std::vector<RestingOrderLeafV0> orders{heads.back()->State().engine.OrderLeaves()};
    BOOST_REQUIRE_EQUAL(orders.size(), 4U);
    BOOST_CHECK(orders[0].side == Side::BUY && orders[1].side == Side::SELL && orders[1].price_ticks == 250);
    BOOST_CHECK(orders[2].price_ticks == 251 && orders[3].price_ticks == 251 && orders[2].arrival_rank < orders[3].arrival_rank);
    for (size_t k{0}; k < heads.size(); ++k) {
        const std::string what{"head " + std::to_string(k)};
        const state::HeadView& head{*heads[k]};
        const std::vector<unsigned char> bytes{MustEncode(head.State())};
        const Result<std::shared_ptr<const state::CoreState>> decoded{state::DecodeSnapshot(bytes, RegtestProfile())};
        RequireOk(decoded, what + ": DecodeSnapshot");
        BOOST_CHECK_MESSAGE(**decoded == head.State(), what + ": the decoded state differs");
        BOOST_CHECK_MESSAGE(MustEncode(**decoded) == bytes, what + ": re-encoding differs");
        const Result<state::HeadView> restored{state::SnapshotHead(bytes, RegtestProfile(), head.Header(), head.CertHash())};
        RequireOk(restored, what + ": SnapshotHead");
        BOOST_CHECK(restored->Header() == head.Header());
        BOOST_CHECK(restored->HeaderHash() == head.HeaderHash());
        BOOST_CHECK(restored->CertHash() == head.CertHash());
        BOOST_CHECK(restored->State() == head.State());
        // A head's snapshot is not the previous head's state, unless the slot changed nothing (slot 5 is a heartbeat).
        if (k > 0 && heads[k - 1]->Header().state_root != head.Header().state_root) {
            RequireRefused(state::SnapshotHead(bytes, RegtestProfile(), heads[k - 1]->Header(), heads[k - 1]->CertHash()),
                           ErrorCode::VERIFY_FAILED, state::SNAPSHOT_HEADER, what + " under the previous header");
        }
    }
}

BOOST_AUTO_TEST_CASE(every_changed_byte_of_a_snapshot_is_refused)
{
    // A snapshot is checked against the certified header: no single changed
    // byte gives a head, whatever part of the encoding it hits.
    OwnerFixture f;
    const std::vector<std::shared_ptr<const state::HeadView>> heads{TradedHeads(f)};
    const state::HeadView& head{*heads.back()};
    const std::vector<unsigned char> bytes{MustEncode(head.State())};
    size_t refused{0};
    for (size_t i{0}; i < bytes.size(); ++i) {
        for (const unsigned char flip : {static_cast<unsigned char>(0x01), static_cast<unsigned char>(0x80)}) {
            std::vector<unsigned char> changed{bytes};
            changed[i] ^= flip;
            const Result<state::HeadView> restored{state::SnapshotHead(changed, RegtestProfile(), head.Header(), head.CertHash())};
            BOOST_CHECK_MESSAGE(!restored.has_value(), "byte " + std::to_string(i) + " flipped by " + std::to_string(flip) + " was accepted");
            refused += restored.has_value() ? 0 : 1;
        }
    }
    BOOST_CHECK_EQUAL(refused, 2 * bytes.size());
}

BOOST_AUTO_TEST_CASE(a_snapshot_of_another_shape_is_refused_before_it_becomes_a_state)
{
    OwnerFixture f;
    const std::vector<std::shared_ptr<const state::HeadView>> heads{TradedHeads(f)};
    const state::CoreState& state{heads.back()->State()};
    const std::vector<unsigned char> bytes{MustEncode(state)};
    const ProfileParams& profile{RegtestProfile()};

    // Cut inside the orders: their count no longer fits the bytes that remain.
    std::vector<unsigned char> changed{bytes.begin(), bytes.end() - 1};
    RequireRefused(state::DecodeSnapshot(changed, profile), ErrorCode::OVERSIZE, "count exceeds its cap", "truncated orders");
    // Cut the last count off a snapshot without orders: the read runs out.
    const std::vector<unsigned char> genesis{MustEncode(heads.front()->State())};
    changed.assign(genesis.begin(), genesis.end() - 1);
    RequireRefused(state::DecodeSnapshot(changed, profile), ErrorCode::DECODE, state::SNAPSHOT_FORMAT, "truncated count");
    changed = bytes;
    changed.push_back(0);
    RequireRefused(state::DecodeSnapshot(changed, profile), ErrorCode::DECODE, state::SNAPSHOT_FORMAT, "a trailing byte");
    changed = bytes;
    changed[0] ^= 1;
    RequireRefused(state::DecodeSnapshot(changed, profile), ErrorCode::DECODE, state::SNAPSHOT_FORMAT, "another magic");
    changed = bytes;
    changed[8] ^= 1;
    RequireRefused(state::DecodeSnapshot(changed, profile), ErrorCode::DECODE, state::SNAPSHOT_FORMAT, "another version");

    ProfileParams other{profile};
    other.max_open_orders_per_account += 1;
    RequireOk(Validate(other), "the other profile is valid");
    RequireRefused(state::DecodeSnapshot(bytes, other), ErrorCode::VERIFY_FAILED, state::SNAPSHOT_PROFILE, "another profile");

    // A count larger than the bytes that remain is refused before allocation.
    std::vector<unsigned char> bomb{bytes.begin(), bytes.begin() + 8 + 4 + 32 + 32};
    {
        VectorWriter writer{bomb, bomb.size()};
        WriteCompactSize(writer, uint64_t{1} << 40);
    }
    RequireRefused(state::DecodeSnapshot(bomb, profile), ErrorCode::OVERSIZE, "count exceeds its cap", "a count bomb");

    const Composed parts{state};
    BOOST_REQUIRE(parts.Bytes() == bytes); // the composer writes what the encoder writes
    {
        Composed swapped{parts};
        std::swap(swapped.accounts[0], swapped.accounts[1]);
        RequireRefused(state::DecodeSnapshot(swapped.Bytes(), profile), ErrorCode::INVALID_INPUT, ledger::LEDGER_LEAVES_ORDER, "accounts out of order");
    }
    {
        Composed swapped{parts};
        std::swap(swapped.markets[0], swapped.markets[1]);
        RequireRefused(state::DecodeSnapshot(swapped.Bytes(), profile), ErrorCode::INVALID_INPUT, state::SNAPSHOT_ORDER, "markets out of order");
    }
    {
        // Two orders of one level in the wrong FIFO order: Rest refuses the lower arrival rank.
        Composed swapped{parts};
        std::swap(swapped.orders[1], swapped.orders[2]);
        const auto decoded{state::DecodeSnapshot(swapped.Bytes(), profile)};
        BOOST_CHECK_MESSAGE(!decoded.has_value() && decoded.error().code == ErrorCode::INVALID_INPUT, "orders out of FIFO order");
    }
    {
        // A bid listed before the asks of its book: the rebuilt book lists it elsewhere.
        Composed moved{parts};
        BOOST_REQUIRE(moved.orders[0].side == Side::BUY && moved.orders[1].side == Side::SELL);
        std::rotate(moved.orders.begin(), moved.orders.begin() + 1, moved.orders.begin() + 3);
        RequireRefused(state::DecodeSnapshot(moved.Bytes(), profile), ErrorCode::INVALID_INPUT, state::SNAPSHOT_ORDER, "orders out of book order");
    }
    {
        // One atom more for an account: canonical leaves, but conservation fails.
        Composed richer{parts};
        richer.accounts[0].balances[0].available += 1;
        const auto decoded{state::DecodeSnapshot(richer.Bytes(), profile)};
        BOOST_CHECK_MESSAGE(!decoded.has_value() && decoded.error().code == ErrorCode::INVARIANT, "an atom created from nothing");
    }
    {
        // A reservation no resting order holds: conservation holds, the engine invariants do not (O-54).
        Composed reserved{parts};
        bool moved{false};
        for (AccountLeafV0& leaf : reserved.accounts) {
            for (AssetBalance& balance : leaf.balances) {
                if (!moved && balance.available > 0) {
                    balance.available -= 1;
                    balance.reserved += 1;
                    moved = true;
                }
            }
        }
        BOOST_REQUIRE(moved);
        const auto decoded{state::DecodeSnapshot(reserved.Bytes(), profile)};
        BOOST_CHECK_MESSAGE(!decoded.has_value() && decoded.error().code == ErrorCode::INVARIANT, "a reservation without an order");
    }
}

BOOST_AUTO_TEST_CASE(a_ledger_from_leaves_is_the_ledger_or_is_refused)
{
    OwnerFixture f;
    const std::vector<std::shared_ptr<const state::HeadView>> heads{TradedHeads(f)};
    const ledger::LedgerState& live{heads.back()->State().engine.ledger};
    const ProfileParams& profile{RegtestProfile()};
    const Composed parts{heads.back()->State()};
    const auto from{[&](const Composed& c) {
        return ledger::LedgerState::FromLeaves(profile, c.accounts, c.nullifiers, c.totals, c.withdrawals);
    }};
    const Result<ledger::LedgerState> rebuilt{from(parts)};
    RequireOk(rebuilt, "FromLeaves");
    BOOST_CHECK(*rebuilt == live);
    BOOST_REQUIRE(parts.accounts.size() >= 2 && parts.nullifiers.size() >= 2 && parts.totals.size() >= 2);
    {
        Composed c{parts};
        c.accounts[1] = c.accounts[0];
        RequireRefused(from(c), ErrorCode::INVALID_INPUT, ledger::LEDGER_LEAVES_ORDER, "a repeated account");
    }
    {
        Composed c{parts};
        std::swap(c.nullifiers[0], c.nullifiers[1]);
        RequireRefused(from(c), ErrorCode::INVALID_INPUT, ledger::LEDGER_LEAVES_ORDER, "nullifiers out of order");
    }
    {
        Composed c{parts};
        std::swap(c.totals[0], c.totals[1]);
        RequireRefused(from(c), ErrorCode::INVALID_INPUT, ledger::LEDGER_LEAVES_ORDER, "totals out of order");
    }
    {
        Composed c{parts};
        c.totals[0].second = AssetTotals{};
        RequireRefused(from(c), ErrorCode::INVALID_INPUT, ledger::LEDGER_LEAVES_ORDER, "all-zero totals");
    }
    {
        Composed c{parts};
        c.accounts[0].balances.push_back(c.accounts[0].balances.front());
        RequireRefused(from(c), ErrorCode::INVALID_INPUT, ledger::LEDGER_LEAVES_ORDER, "a leaf that is not canonical");
    }
    {
        // Canonical, ordered leaves holding one atom no deposit created.
        Composed c{parts};
        c.accounts[0].balances[0].available += 1;
        const Result<ledger::LedgerState> richer{from(c)};
        BOOST_CHECK_MESSAGE(!richer.has_value() && richer.error().code == ErrorCode::INVARIANT, "a ledger that does not conserve");
    }
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/amount.h>
#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/fee_math.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/market.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/engine/book.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/test/util/slot_fixtures.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

/** Seeded random slots through ValidateSlotInputs and Engine::ApplySlot
 * (slice R7). After every slot the test checks, independently of the
 * engine's own phase 9:
 * - the next balances equal the parent balances plus the outcomes' deltas,
 *   for every (account, asset);
 * - per asset, the deltas of all outcomes sum to the slot's deposits (fees
 *   are withheld and distributed inside the slot, O-31);
 * - every action has exactly one outcome, with its signer and nonce, and its
 *   nonce is in the next window (DUR-5); rejections move no money;
 * - every fill's quote and seller fee recompute from the market and the
 *   seller's cumulative quote (core/fee_math.h);
 * - every outcome survives Encode/Decode under the profile, and its deltas
 *   are sorted without zero entries; the slot makes at most
 *   max_fills_per_slot fills over all markets;
 * - the same originals in another proposal order give identical outcomes,
 *   roots and state;
 * - the slot-boundary invariants hold.
 * Every outcome code the outline can produce must occur at least once.
 */

using namespace flowmesh2;
using namespace flowmesh2::engine;
using namespace flowmesh2::test;

namespace {

const AssetId B3{AssetId::Native()};
const AssetId RUSD{TestAsset("rUSD")};
const AssetId TOK{TestAsset("TOK")};
MarketDescriptor MarketM3() { return MarketDescriptor{TOK, B3, 1, 1}; }
const std::vector<std::string> SIGNERS{"alice", "bob", "carol", "dave"};

struct Rng {
    uint64_t state;
    uint64_t Next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 33;
    }
    uint64_t Below(uint64_t n) { return Next() % n; }
};

using Key = std::pair<AccountId, AssetId>;

std::map<Key, std::pair<int64_t, int64_t>> Balances(const EngineState& state)
{
    std::map<Key, std::pair<int64_t, int64_t>> out;
    for (const auto& [account, a] : state.ledger.Accounts()) {
        for (const auto& [asset, balance] : a.balances) out[{account, asset}] = {balance.available, balance.reserved};
    }
    return out;
}

//! One random action of `name` with `nonce` (see the cases below).
std::vector<unsigned char> RandomAction(SlotFixture& fx, Rng& rng, const std::string& name, uint64_t nonce,
                                        const std::vector<MarketDescriptor>& markets)
{
    const uint64_t r{rng.Below(100)};
    if (r < 55 || r >= 92) {
        const MarketDescriptor market{rng.Below(25) == 0 ? MarketDescriptor{RUSD, TOK, 3, 3} : markets[rng.Below(markets.size())]};
        const Side side{rng.Below(2) == 0 ? Side::BUY : Side::SELL};
        const uint64_t t{rng.Below(4)};
        const TimeInForce tif{t < 2 ? TimeInForce::GTC : (t == 2 ? TimeInForce::IOC : TimeInForce::POST_ONLY)};
        const uint64_t price{95 + rng.Below(11)};
        uint64_t lots{1 + rng.Below(8)};
        if (rng.Below(30) == 0) lots = 0;           // REJ_BAD_PARAMS
        if (r >= 92) lots = 1'000'000'000;          // far beyond any balance
        return fx.Place(name, nonce, market, side, tif, price, lots);
    }
    if (r < 70) {
        std::vector<OrderId> resting;
        for (const auto& [id, book] : fx.state.books.Books()) {
            for (const RestingOrder& o : book.Orders()) resting.push_back(o.id);
        }
        const OrderId target{resting.empty() || rng.Below(6) == 0 ? OrderId{LabelHash("fm2/test/order/" + std::to_string(rng.Next()))}
                                                                  : resting[rng.Below(resting.size())]};
        return fx.Cancel(name, nonce, target);
    }
    const std::vector<std::string> recipients{"alice", "bob", "carol", "dave", "frank"};
    const AssetId assets[]{B3, RUSD, TOK};
    return fx.Transfer(name, nonce, KeyAccount(recipients[rng.Below(recipients.size())]), assets[rng.Below(3)],
                       static_cast<Atoms>(rng.Below(3'000)));
}

void CheckSlot(const SlotFixture& fx, const SlotInputs& inputs, const SlotResult& result,
               std::map<OutcomeCode, uint64_t>& coverage)
{
    const ProfileParams& profile{fx.profile};
    const Result<ValidatedSlot> slot{fx.Validate(inputs)};
    RequireOk(slot, "re-validate");

    // Exactly one outcome per fact, then one per action in canonical order, then fees.
    const size_t n_facts{slot->Deposits().size()};
    const size_t n_actions{slot->Actions().size()};
    BOOST_REQUIRE_GE(result.outcomes.size(), n_facts + n_actions);
    for (size_t rank{0}; rank < n_actions; ++rank) {
        const SlotAction& a{slot->Actions()[slot->Order()[rank]]};
        const OutcomeV2& o{result.outcomes[n_facts + rank]};
        BOOST_CHECK(o.source == OutcomeSource::ACTION);
        BOOST_CHECK(o.subject == a.action.Id().id);
        BOOST_CHECK(o.account == a.action.Account());
        BOOST_CHECK_EQUAL(o.nonce, a.action.Action().nonce);
        const ledger::AccountState* account{result.state.ledger.Find(a.action.Account())};
        BOOST_REQUIRE(account != nullptr);
        BOOST_CHECK(std::ranges::find(account->nonces.Used(), a.action.Action().nonce) != account->nonces.Used().end());
    }
    for (size_t i{n_facts + n_actions}; i < result.outcomes.size(); ++i) {
        BOOST_CHECK(result.outcomes[i].code == OutcomeCode::FEES_DISTRIBUTED);
    }

    std::map<AssetId, int64_t> deposited;
    for (const DepositFact& d : slot->Deposits()) deposited[d.asset] += d.amount;
    std::map<Key, std::pair<int64_t, int64_t>> expected{Balances(fx.state)};
    std::map<AssetId, int64_t> moved;
    std::map<OrderId, Atoms> seller_quote; // cumulative quote of seller orders seen in this slot
    for (const OutcomeV2& o : result.outcomes) {
        ++coverage[o.code];
        const Result<OutcomeV2> decoded{OutcomeV2::Decode(o.Encode(), profile)};
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK(*decoded == o);
        const bool refused_remainder{o.code == OutcomeCode::REJ_FILL_CAP || o.code == OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED};
        if (IsRejection(o.code) && !refused_remainder) {
            BOOST_CHECK(o.fills.empty() && o.deltas.empty() && o.remaining_lots == 0);
        }
        for (size_t k{0}; k < o.deltas.size(); ++k) {
            const BalanceDelta& d{o.deltas[k]};
            BOOST_CHECK(d.available_delta != 0 || d.reserved_delta != 0);
            if (k > 0) {
                const BalanceDelta& p{o.deltas[k - 1]};
                const bool ascending{std::pair{p.account, p.asset} < std::pair{d.account, d.asset}};
                BOOST_CHECK(ascending);
            }
            auto& e{expected[{d.account, d.asset}]};
            e.first += d.available_delta;
            e.second += d.reserved_delta;
            moved[d.asset] += d.available_delta + d.reserved_delta;
        }
        for (const Fill& f : o.fills) {
            const OrderBook* book{fx.state.books.Find(f.market)};
            BOOST_REQUIRE(book != nullptr);
            BOOST_CHECK_EQUAL(QuoteAtoms(book->Market(), f.lots, f.price_ticks).value(), f.quote_atoms);
            BOOST_CHECK_EQUAL(SellerFeeIncrement(f.seller_quote_before, f.seller_quote_before + f.quote_atoms, profile.fee_ppm).value(),
                              f.seller_fee);
            const OrderId& seller{f.taker_side == Side::SELL ? f.taker : f.maker};
            // The seller's cumulative quote continues across its fills (makers start from the parent's value).
            if (seller_quote.contains(seller)) BOOST_CHECK_EQUAL(seller_quote[seller], f.seller_quote_before);
            seller_quote[seller] = f.seller_quote_before + f.quote_atoms;
        }
    }
    // D6 caps: at most max_fills_per_slot fills in the slot, all markets together.
    uint64_t fills{0};
    for (const OutcomeV2& o : result.outcomes) fills += o.fills.size();
    BOOST_CHECK_LE(fills, profile.max_fills_per_slot);
    std::erase_if(expected, [](const auto& e) { return e.second == std::pair<int64_t, int64_t>{0, 0}; });
    BOOST_CHECK(expected == Balances(result.state));
    std::erase_if(moved, [](const auto& e) { return e.second == 0; });
    BOOST_CHECK(moved == deposited);
    RequireOk(result.state.CheckInvariants(), "slot-boundary invariants");
    for (const auto& [asset, totals] : result.state.ledger.Totals()) BOOST_CHECK_EQUAL(totals.fees_held, 0);
}

} // namespace

BOOST_AUTO_TEST_SUITE(engine_property_tests)

BOOST_AUTO_TEST_CASE(random_slots_conserve_and_report_every_change)
{
    std::map<OutcomeCode, uint64_t> coverage;
    uint64_t slots{0};
    uint64_t actions{0};
    const std::vector<MarketDescriptor> markets{MarketM1(), MarketM2(), MarketM3()};
    for (uint64_t seed{1}; seed <= 24; ++seed) {
        Rng rng{seed * 0x9e3779b97f4a7c15ULL};
        ProfileParams profile{RegtestProfile()};
        if (seed % 2 == 0) {
            profile.max_fills_per_order = 2;
            profile.max_fills_per_slot = 6;
            profile.max_open_orders_per_account = 4;
        }
        SlotFixture fx{profile, markets};
        std::vector<std::vector<unsigned char>> funding;
        for (const std::string& name : SIGNERS) {
            funding.push_back(fx.DepositBytes(KeyAccount(name), B3, 10'000'000'000, "fund-b3-" + name));
            funding.push_back(fx.DepositBytes(KeyAccount(name), RUSD, 20'000, "fund-rusd-" + name));
            funding.push_back(fx.DepositBytes(KeyAccount(name), TOK, 2'000, "fund-tok-" + name));
        }
        fx.Apply(fx.Inputs(funding, {}));
        std::map<std::string, uint64_t> nonce;
        for (uint64_t s{0}; s < 16; ++s) {
            std::vector<std::vector<unsigned char>> facts;
            if (rng.Below(4) == 0) {
                facts.push_back(fx.DepositBytes(KeyAccount(SIGNERS[rng.Below(SIGNERS.size())]), RUSD, 1 + rng.Below(5'000),
                                                "extra-" + std::to_string(seed) + "-" + std::to_string(s)));
            }
            std::vector<std::vector<unsigned char>> originals;
            const uint64_t count{rng.Below(13)};
            for (uint64_t k{0}; k < count; ++k) {
                const std::string& name{SIGNERS[rng.Below(SIGNERS.size())]};
                nonce[name] += rng.Below(10) == 0 ? 2 : 1;
                originals.push_back(RandomAction(fx, rng, name, nonce[name], markets));
            }
            const SlotInputs inputs{fx.Inputs(facts, originals)};
            const Result<SlotResult> result{fx.TryApply(inputs)};
            RequireOk(result, "random slot");
            CheckSlot(fx, inputs, *result, coverage);

            // Another proposal order of the same originals: identical result.
            std::vector<std::vector<unsigned char>> shuffled{originals};
            for (size_t i{shuffled.size()}; i > 1; --i) std::swap(shuffled[i - 1], shuffled[rng.Below(i)]);
            const Result<SlotResult> again{fx.TryApply(fx.Inputs(facts, shuffled))};
            RequireOk(again, "shuffled slot");
            BOOST_CHECK(again->outcomes == result->outcomes);
            BOOST_CHECK(again->roots == result->roots);
            BOOST_CHECK(again->outcomes_root == result->outcomes_root);
            BOOST_CHECK(again->state == result->state);

            fx.Advance(inputs, *result);
            ++slots;
            actions += originals.size();
        }
    }
    BOOST_TEST_MESSAGE("random slots: " << slots << ", actions: " << actions);
    for (const OutcomeCode code :
         {OutcomeCode::DEPOSIT_CREDITED, OutcomeCode::RESTING, OutcomeCode::FILLED, OutcomeCode::PARTIAL_RESTING,
          OutcomeCode::IOC_DONE, OutcomeCode::CANCELLED, OutcomeCode::TRANSFERRED, OutcomeCode::FEES_DISTRIBUTED,
          OutcomeCode::REJ_INSUFFICIENT_AVAILABLE, OutcomeCode::REJ_UNKNOWN_MARKET, OutcomeCode::REJ_UNKNOWN_ORDER,
          OutcomeCode::REJ_NOT_OWN_ORDER, OutcomeCode::REJ_WOULD_CROSS, OutcomeCode::REJ_FILL_CAP,
          OutcomeCode::REJ_OPEN_ORDER_CAP, OutcomeCode::REJ_BAD_PARAMS, OutcomeCode::REJ_NO_RECIPIENT,
          OutcomeCode::REJ_RESIDUAL_CROSS_UNSUPPORTED}) {
        BOOST_CHECK_MESSAGE(coverage[code] > 0, "never produced: " << ToString(code));
        BOOST_TEST_MESSAGE(ToString(code) << ": " << coverage[code]);
    }
}

BOOST_AUTO_TEST_SUITE_END()

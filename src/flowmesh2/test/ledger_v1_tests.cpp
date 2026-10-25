// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath> // first on purpose: macOS <math.h> macros must not break fm2 headers (O-21)

#include <boost/test/unit_test.hpp>

#include <flowmesh2/core/codes.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/core/smt.h>
#include <flowmesh2/core/state_root.h>
#include <flowmesh2/core/state_root_v1.h>
#include <flowmesh2/ledger/ledger.h>
#include <flowmesh2/ledger/smt_tree.h>
#include <flowmesh2/ledger/state_commitment_v1.h>
#include <flowmesh2/test/util/keys.h>
#include <flowmesh2/test/util/ledger_fixtures.h>
#include <flowmesh2/verify/b3verify.h>
#include <uint256.h>
#include <util/strencodings.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace flowmesh2;
using namespace flowmesh2::ledger;
using namespace flowmesh2::test;

namespace {

//! splitmix64.
struct Rng {
    uint64_t state;
    uint64_t Next()
    {
        uint64_t z{state += 0x9e3779b97f4a7c15ULL};
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
};

AccountId Trader(uint64_t n) { return TestAccount("v1/" + std::to_string(n)); }

//! A ledger with `count` funded accounts and two nonces used by the first.
LedgerState Funded(uint64_t count)
{
    LedgerState ledger{EmptyLedger()};
    for (uint64_t i{0}; i < count; ++i) {
        RequireOk(ledger.CreditDeposit(Deposit(Trader(i), TestAsset("rUSD"), 1'000 + static_cast<Atoms>(i),
                                               TestOutpoint("v1", static_cast<uint32_t>(i)))),
                  "fund");
    }
    if (count > 0) {
        RequireOk(ledger.ConsumeNonce(Trader(0), 1), "nonce");
        RequireOk(ledger.ConsumeNonce(Trader(0), 2), "nonce");
    }
    return ledger;
}

std::vector<SmtLeaf> Sorted(std::vector<SmtLeaf> leaves)
{
    std::sort(leaves.begin(), leaves.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return SmtPathLess(a.path, b.path); });
    return leaves;
}

uint256 ComponentRoot(SmtComponent component, const std::vector<std::pair<uint256, uint256>>& keyed)
{
    std::vector<SmtLeaf> leaves;
    leaves.reserve(keyed.size());
    for (const auto& [key, value] : keyed) leaves.push_back(SmtLeaf{SmtKeyPath(component, std::span<const unsigned char>{key.data(), 32}), value});
    return SmtRoot(Sorted(std::move(leaves))).value();
}

} // namespace

BOOST_AUTO_TEST_SUITE(ledger_v1_tests)

BOOST_AUTO_TEST_CASE(an_absent_account_has_a_proof_that_verifies)
{
    // M2-LED-02 failing first: v0 cannot prove an account absent
    // (ProveAccountV0 is INVALID_INPUT); v1 can, and only for an account the
    // ledger lacks.
    const LedgerState ledger{Funded(40)};
    const SmtTree accounts{AccountsTreeV1(ledger).value()};
    RequireError(ProveAccountV0(ledger, Trader(1'000)), ErrorCode::INVALID_INPUT, "v0 absence");

    const AccountProofV1 absent{ProveAccountV1(ledger, accounts, Trader(1'000)).value()};
    BOOST_CHECK(absent.Kind() == AccountProofKind::ABSENT);
    BOOST_CHECK(absent.VerifiesUnder(accounts.Root()));
    BOOST_CHECK(SmtVerifyAbsent(accounts.Root(), AccountPathV1(Trader(1'000)), absent.proof));

    const AccountProofV1 present{ProveAccountV1(ledger, accounts, Trader(0)).value()};
    BOOST_REQUIRE(present.Kind() == AccountProofKind::PRESENT);
    BOOST_CHECK(*present.leaf == ledger.Leaf(Trader(0)).value());
    BOOST_CHECK(present.VerifiesUnder(accounts.Root()));

    // A present account's proof re-labelled ABSENT, or an absent account's
    // proof given a made-up leaf, does not verify.
    AccountProofV1 hidden{present};
    hidden.leaf.reset();
    BOOST_CHECK(!hidden.VerifiesUnder(accounts.Root()));
    AccountProofV1 invented{absent};
    invented.leaf = ledger.Leaf(Trader(0)).value();
    invented.leaf->account = Trader(1'000);
    BOOST_CHECK(!invented.VerifiesUnder(accounts.Root()));
    // The tree of another state.
    BOOST_CHECK(!present.VerifiesUnder(AccountsTreeV1(Funded(41)).value().Root()));
    // A tree that is not the ledger's.
    RequireError(ProveAccountV1(ledger, AccountsTreeV1(Funded(39)).value(), Trader(39)), ErrorCode::INVARIANT, "stale tree");
    RequireError(ProveAccountV1(Funded(39), accounts, Trader(39)), ErrorCode::INVARIANT, "tree of a larger state");
}

BOOST_AUTO_TEST_CASE(each_component_is_the_sparse_tree_of_the_v0_leaf_values)
{
    LedgerState ledger{Funded(12)};
    RequireOk(ledger.CreditDeposit(Deposit(Trader(3), TestAsset("TOK"), 77, TestOutpoint("v1/tok", 0))), "second asset");
    const SmtTree accounts{AccountsTreeV1(ledger).value()};
    const std::vector<MarketDescriptor> markets{MarketM2(), MarketM1()};
    const std::vector<OrderLeafV0> orders{{OrderId{LabelHash("fm2/test/v1/order/b")}, LabelHash("fm2/test/v1/order-leaf/b")},
                                          {OrderId{LabelHash("fm2/test/v1/order/a")}, LabelHash("fm2/test/v1/order-leaf/a")}};
    const StateRootsV1 roots{CommitV1(ledger, accounts, markets, orders).value()};

    std::vector<std::pair<uint256, uint256>> account_leaves;
    for (const auto& entry : ledger.Accounts()) account_leaves.emplace_back(entry.first.id, ledger.Leaf(entry.first).value().LeafValue());
    BOOST_CHECK(roots.accounts == ComponentRoot(SmtComponent::ACCOUNTS, account_leaves));
    std::vector<std::pair<uint256, uint256>> order_leaves;
    for (const OrderLeafV0& order : orders) order_leaves.emplace_back(order.order.id, order.value);
    BOOST_CHECK(roots.orders == ComponentRoot(SmtComponent::ORDERS, order_leaves));
    std::vector<std::pair<uint256, uint256>> market_leaves;
    for (const MarketDescriptor& market : markets) market_leaves.emplace_back(market.Id().id, MarketLeafValue(market));
    BOOST_CHECK(roots.markets == ComponentRoot(SmtComponent::MARKETS, market_leaves));
    std::vector<std::pair<uint256, uint256>> total_leaves;
    for (const auto& [asset, totals] : ledger.Totals()) total_leaves.emplace_back(asset.id, TotalsLeafV0{asset, totals}.LeafValue());
    BOOST_CHECK(roots.totals == ComponentRoot(SmtComponent::TOTALS, total_leaves));
    std::vector<SmtLeaf> nullifiers;
    for (const Outpoint& outpoint : ledger.Nullifiers()) nullifiers.push_back(SmtLeaf{NullifierPathV1(outpoint), NullifierLeafValue(outpoint)});
    BOOST_CHECK(roots.nullifiers == SmtRoot(Sorted(nullifiers)).value());
    BOOST_CHECK(roots.withdrawals == SMT_EMPTY);
    // The nullifier key is the outpoint's 36 serialized bytes: txid || u32 LE vout.
    const Outpoint outpoint{TestOutpoint("v1", 5)};
    std::vector<unsigned char> key(outpoint.txid.begin(), outpoint.txid.end());
    for (unsigned shift : {0u, 8u, 16u, 24u}) key.push_back(static_cast<unsigned char>(outpoint.vout >> shift));
    BOOST_CHECK(NullifierPathV1(outpoint) == SmtKeyPath(SmtComponent::NULLIFIERS, key));

    // The root of roots binds the version byte and all six roots.
    StateRootsV1 pinned;
    pinned.accounts = LabelHash("fm2/test/smt/root/0");
    pinned.orders = LabelHash("fm2/test/smt/root/1");
    pinned.markets = LabelHash("fm2/test/smt/root/2");
    pinned.nullifiers = LabelHash("fm2/test/smt/root/3");
    pinned.totals = LabelHash("fm2/test/smt/root/4");
    pinned.withdrawals = LabelHash("fm2/test/smt/root/5");
    BOOST_CHECK_EQUAL(HexStr(pinned.Root()), "29f4d63615aa0dea61896451d071fcf3820f8083969930f2da6d5f7349089d35");

    // The input order does not matter; the errors are CommitV0's.
    const std::vector<MarketDescriptor> markets_sorted{MarketM1(), MarketM2()};
    const std::vector<OrderLeafV0> orders_sorted{orders[1], orders[0]};
    BOOST_CHECK(CommitV1(ledger, accounts, markets_sorted, orders_sorted).value() == roots);
    RequireError(CommitV1(ledger, accounts, std::vector<MarketDescriptor>{MarketM1(), MarketM1()}, {}), ErrorCode::INVALID_INPUT,
                 "repeated market");
    RequireError(CommitV1(ledger, accounts, markets, std::vector<OrderLeafV0>{orders[0], orders[0]}), ErrorCode::INVALID_INPUT,
                 "repeated order");
    const MarketDescriptor same_assets{TestAsset("rUSD"), TestAsset("rUSD"), 1, 1};
    RequireError(CommitV1(ledger, accounts, std::vector<MarketDescriptor>{same_assets}, {}), ErrorCode::INVALID_INPUT,
                 "invalid market");
}

BOOST_AUTO_TEST_CASE(updating_by_changes_gives_the_built_tree)
{
    // Random deposits, transfers and nonces on copies: the parent's tree
    // updated by the changed accounts equals the child's tree built whole,
    // and the parent's tree is untouched.
    Rng rng{0x1ed6e};
    LedgerState parent{Funded(60)};
    SmtTree tree{AccountsTreeV1(parent).value()};
    uint32_t outpoints{1'000};
    for (int round{0}; round < 40; ++round) {
        LedgerState child{parent};
        const uint64_t ops{rng.Below(round % 5 == 0 ? 30 : 4)};
        for (uint64_t op{0}; op < ops; ++op) {
            const AccountId a{Trader(rng.Below(90))};
            switch (rng.Below(3)) {
            case 0:
                RequireOk(child.CreditDeposit(Deposit(a, TestAsset("rUSD"), 1 + static_cast<Atoms>(rng.Below(100)),
                                                      TestOutpoint("v1/update", outpoints++))),
                          "deposit");
                break;
            case 1: (void)child.Transfer(a, Trader(rng.Below(90)), TestAsset("rUSD"), 1 + static_cast<Atoms>(rng.Below(50))); break;
            default: (void)child.ConsumeNonce(a, 3 + rng.Below(5)); break;
            }
        }
        const uint256 parent_root{tree.Root()};
        SmtTree updated{tree};
        RequireOk(UpdateAccountsTreeV1(updated, parent, child), "update");
        BOOST_REQUIRE(tree.Root() == parent_root);
        BOOST_REQUIRE(updated.Root() == AccountsTreeV1(child).value().Root());
        BOOST_REQUIRE(updated.CheckShape());
        parent = child;
        tree = updated;
    }
    BOOST_CHECK_GT(parent.Accounts().size(), 60U); // some rounds created accounts

    // Between two ledgers built apart, an account the second lacks leaves
    // the tree (the engine never removes an account; the update must).
    const LedgerState more{Funded(70)}, fewer{Funded(60)};
    SmtTree shrunk{AccountsTreeV1(more).value()};
    RequireOk(UpdateAccountsTreeV1(shrunk, more, fewer), "update to fewer accounts");
    BOOST_CHECK(shrunk.Root() == AccountsTreeV1(fewer).value().Root());
    BOOST_CHECK_EQUAL(shrunk.Size(), 60U);
    SmtTree grown{AccountsTreeV1(fewer).value()};
    RequireOk(UpdateAccountsTreeV1(grown, fewer, more), "update to more accounts");
    BOOST_CHECK(grown.Root() == AccountsTreeV1(more).value().Root());
}

BOOST_AUTO_TEST_CASE(an_update_costs_its_changes_not_the_state)
{
    // Ten changed accounts in a state of 10^2 and of 10^4: the update hashes
    // ten paths in both, the build hashes every account.
    const auto time_update = [](uint64_t count) {
        const LedgerState parent{Funded(count)};
        const SmtTree tree{AccountsTreeV1(parent).value()};
        LedgerState child{parent};
        for (uint64_t i{0}; i < 10; ++i) RequireOk(child.ConsumeNonce(Trader(i * (count / 10)), 9), "nonce");
        double best{1e300};
        for (int rep{0}; rep < 7; ++rep) {
            SmtTree updated{tree};
            const auto start{std::chrono::steady_clock::now()};
            RequireOk(UpdateAccountsTreeV1(updated, parent, child), "update");
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return best;
    };
    const double small{time_update(100)};
    const double large{time_update(10'000)};
    BOOST_TEST_MESSAGE("ten changed accounts: " << small << " ms at 10^2, " << large << " ms at 10^4, ratio " << large / small);
    BOOST_CHECK_LT(large / small, 5.0);
}

BOOST_AUTO_TEST_CASE(account_proofs_round_trip_and_the_codec_refuses_the_rest)
{
    const ProfileParams profile{RegtestProfile()};
    const LedgerState ledger{Funded(20)};
    const SmtTree accounts{AccountsTreeV1(ledger).value()};
    const AccountProofV1 present{ProveAccountV1(ledger, accounts, Trader(0)).value()};
    const AccountProofV1 absent{ProveAccountV1(ledger, accounts, Trader(500)).value()};
    for (const AccountProofV1& proof : {present, absent}) {
        const std::vector<unsigned char> bytes{proof.Encode()};
        BOOST_REQUIRE(AccountProofV1::Decode(bytes, profile).value() == proof);
        BOOST_CHECK_LE(bytes.size(), AccountProofV1::MaxEncodedSize(profile));
        std::vector<unsigned char> trailing{bytes};
        trailing.push_back(0);
        RequireError(AccountProofV1::Decode(trailing, profile), ErrorCode::DECODE, "trailing");
        for (size_t cut : {size_t{1}, size_t{33}, bytes.size() / 2}) {
            RequireError(AccountProofV1::Decode(std::span{bytes}.first(bytes.size() - cut), profile), ErrorCode::DECODE, "truncated");
        }
    }
    // Kind byte (offset 32): unknown, or swapped between the two shapes.
    std::vector<unsigned char> unknown{absent.Encode()};
    unknown[32] = 3;
    RequireError(AccountProofV1::Decode(unknown, profile), ErrorCode::DECODE, "unknown kind");
    // ABSENT with the account's own leaf as terminal; PRESENT for another account's leaf.
    AccountProofV1 lying{present};
    lying.leaf.reset();
    RequireError(AccountProofV1::Decode(lying.Encode(), profile), ErrorCode::DECODE, "absent ending at its own leaf");
    AccountProofV1 other{present};
    other.account = Trader(1);
    RequireError(AccountProofV1::Decode(other.Encode(), profile), ErrorCode::DECODE, "leaf of another account");
    AccountProofV1 claimed{absent};
    claimed.leaf = ledger.Leaf(Trader(0)).value();
    claimed.leaf->account = Trader(500);
    RequireError(AccountProofV1::Decode(claimed.Encode(), profile), ErrorCode::DECODE, "present not ending at its leaf");
    RequireError(AccountProofV1::Decode(std::vector<unsigned char>(AccountProofV1::MaxEncodedSize(profile) + 1, 0), profile),
                 ErrorCode::OVERSIZE, "oversize");
}

BOOST_AUTO_TEST_CASE(b3verify_checks_v1_proofs_under_a_v1_header_only)
{
    const ProfileParams profile{RegtestProfile()};
    const LedgerState ledger{Funded(20)};
    const SmtTree accounts{AccountsTreeV1(ledger).value()};
    const StateRootsV1 roots{CommitV1(ledger, accounts, {}, {}).value()};
    // Built in memory: the header codec refuses commitment_version 1 until the switch is ruled.
    SlotHeaderV2 header;
    header.height = 3;
    header.profile_hash = ProfileHash(profile);
    header.commitment_version = COMMITMENT_VERSION_SMT;
    header.state_root = roots.Root();
    const AccountProofV1 present{ProveAccountV1(ledger, accounts, Trader(4)).value()};
    const AccountProofV1 absent{ProveAccountV1(ledger, accounts, Trader(400)).value()};
    RequireOk(verify::VerifyAccountProofV1(header, present, roots, profile), "present");
    RequireOk(verify::VerifyAccountProofV1(header, absent, roots, profile), "absent");

    const auto refused = [&](const SlotHeaderV2& h, const AccountProofV1& proof, const StateRootsV1& r, std::string_view detail) {
        const Result<void> result{verify::VerifyAccountProofV1(h, proof, r, profile)};
        BOOST_REQUIRE(!result.has_value());
        BOOST_CHECK(result.error().code == ErrorCode::VERIFY_FAILED);
        BOOST_CHECK_EQUAL(result.error().detail, detail);
    };
    SlotHeaderV2 v0{header};
    v0.commitment_version = COMMITMENT_VERSION_V0;
    refused(v0, present, roots, verify::V1_NOT_COMMITTED);
    SlotHeaderV2 other_root{header};
    other_root.state_root = LabelHash("fm2/test/v1/other-state");
    refused(other_root, present, roots, verify::LEAF_STATE_ROOTS);
    // The absence proof of an account the state has: built on another state's tree.
    const LedgerState larger{Funded(401)};
    const SmtTree larger_accounts{AccountsTreeV1(larger).value()};
    const StateRootsV1 larger_roots{CommitV1(larger, larger_accounts, {}, {}).value()};
    SlotHeaderV2 larger_header{header};
    larger_header.state_root = larger_roots.Root();
    refused(larger_header, absent, larger_roots, verify::LEAF_PROOF);
    AccountProofV1 zero_entry{present};
    zero_entry.leaf->balances.push_back(AssetBalance{TestAsset("TOK"), 0, 0});
    refused(header, zero_entry, roots, verify::LEAF_NOT_CANONICAL);
    SlotHeaderV2 other_profile{header};
    other_profile.profile_hash = LabelHash("fm2/test/v1/other-profile");
    refused(other_profile, present, roots, verify::PROFILE_MISMATCH);
    // No decoded header can carry version 1 today.
    std::vector<unsigned char> encoded{v0.Encode()};
    encoded.back() = COMMITMENT_VERSION_SMT;
    RequireError(SlotHeaderV2::Decode(encoded), ErrorCode::DECODE, "a v1 header");
}

BOOST_AUTO_TEST_SUITE_END()

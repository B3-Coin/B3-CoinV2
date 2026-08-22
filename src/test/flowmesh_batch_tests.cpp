// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh canonical batch execution: order-independence, per-signer
//! sequencing, same-sequence equivocation rejection, deterministic
//! settlement, request/state roots, and one-time request ids.
//! Execution performs NO authentication (Codex re-audit item 1):
//! credentials are pre-admission evidence, covered by the pool and
//! microblock/evidence suites.

#include <flowmesh/batch.h>
#include <test/util/asset.h>

#include <test/util/flowmesh.h>

#include <flowmesh/clearing.h>
#include <flowmesh/ledger.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

BOOST_AUTO_TEST_SUITE(flowmesh_batch_tests)

namespace {

using Side = flowmesh::ClearingEngine::Side;
using Breakpoint = flowmesh::ClearingEngine::Breakpoint;
using flowmesh::Action;
using flowmesh::ActionType;
using flowmesh::ActionReject;

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
const flowmesh::AccountId ALICE{uint256{"00000000000000000000000000000000000000000000000000000000000000a1"}};
const flowmesh::AccountId BOB{uint256{"00000000000000000000000000000000000000000000000000000000000000b1"}};
const uint256 DEST{uint256{"00000000000000000000000000000000000000000000000000000000000000d1"}};

modern::AssetId BaseX()
{
    return modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}), 0});
}
const modern::AssetId& Quote() { return modern::NativeAsset(); }

std::vector<Breakpoint> Pts(std::vector<std::pair<CAmount, CAmount>> raw)
{
    std::vector<Breakpoint> out;
    for (const auto& [p, q] : raw) out.push_back({p, q});
    return out;
}

Action Bid(const flowmesh::AccountId& signer, uint64_t seq, std::vector<Breakpoint> curve)
{
    Action a;
    a.signer = signer;
    a.sequence = seq;
    a.type = static_cast<uint8_t>(ActionType::SUBMIT_BID);
    a.curve = std::move(curve);
    return a;
}

Action Ask(const flowmesh::AccountId& signer, uint64_t seq, std::vector<Breakpoint> curve)
{
    Action a;
    a.signer = signer;
    a.sequence = seq;
    a.type = static_cast<uint8_t>(ActionType::SUBMIT_ASK);
    a.curve = std::move(curve);
    return a;
}

Action Withdraw(const flowmesh::AccountId& signer, uint64_t seq, const modern::AssetId& asset,
                CAmount amount, const uint256& dest)
{
    Action a;
    a.signer = signer;
    a.sequence = seq;
    a.type = static_cast<uint8_t>(ActionType::WITHDRAW);
    a.asset = asset;
    a.amount = amount;
    a.destination = dest;
    return a;
}

//! Test funding shortcut over the test-only bridge.
inline bool Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account,
                 const modern::AssetId& asset, const CAmount amount)
{
    return flowmesh::test_only::StateFunding::Fund(state, account, asset, amount);
}

struct Fixture {
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{state.LedgerView()};
    flowmesh::BatchExecutor exec{state};
    Fixture()
    {
        Fund(state, ALICE, Quote(), 2400); // covers the standard bid's 2300 bound
        Fund(state, BOB, BaseX(), 80);
    }
};


} // namespace

BOOST_AUTO_TEST_CASE(execution_is_independent_of_arrival_order)
{
    std::vector<Action> actions{
        Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}})),
        Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))};

    // A permutation of the same set yields a byte-identical result.
    uint256 canonical_state, canonical_receipt;
    CAmount price{0}, volume{0};
    std::vector<uint256> applied;
    std::mt19937 rng{12345};
    for (int i{0}; i < 6; ++i) {
        std::vector<Action> shuffled{actions};
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        Fixture f;
        const auto result{*f.exec.ExecuteSlot(shuffled)};
        if (i == 0) {
            canonical_state = result.state_root;
            canonical_receipt = result.request_root;
            price = result.clearing.price;
            volume = result.clearing.volume;
            applied = result.applied;
        } else {
            BOOST_CHECK_EQUAL(result.state_root.GetHex(), canonical_state.GetHex());
            BOOST_CHECK_EQUAL(result.request_root.GetHex(), canonical_receipt.GetHex());
            BOOST_CHECK_EQUAL(result.clearing.price, price);
            BOOST_CHECK_EQUAL(result.clearing.volume, volume);
            BOOST_CHECK(result.applied == applied);
        }
    }
    BOOST_CHECK_EQUAL(price, 20);
    BOOST_CHECK_EQUAL(volume, 40);
    BOOST_CHECK_EQUAL(applied.size(), 2U);
}

BOOST_AUTO_TEST_CASE(same_sequence_equivocation_is_rejected_deterministically)
{
    Fixture f;
    // Two DIFFERENT actions from ALICE at sequence 0.
    const Action a{Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))};
    const Action b{Ask(ALICE, 0, Pts({{10, 20}, {20, 80}}))}; // conflicting seq 0
    const auto result{*f.exec.ExecuteSlot({a, b, Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))})};

    // Both equivocating actions are rejected; ALICE's sequence does not
    // advance, so a later slot can still use sequence 0.
    const std::set<uint256> rejected_ids{[&] {
        std::set<uint256> ids;
        for (const auto& [id, reason] : result.rejected) {
            if (reason == ActionReject::EQUIVOCATION) ids.insert(id);
        }
        return ids;
    }()};
    BOOST_CHECK(rejected_ids.count(a.Id()) == 1);
    BOOST_CHECK(rejected_ids.count(b.Id()) == 1);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 0);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(BOB), 1); // Bob's ask applied

    // Identical duplicates (same id) are NOT equivocation: they collapse.
    Fixture g;
    const auto dup{*g.exec.ExecuteSlot({a, a, Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))})};
    for (const auto& [id, reason] : dup.rejected) {
        BOOST_CHECK(reason != ActionReject::EQUIVOCATION);
    }
    BOOST_CHECK_EQUAL(g.exec.NextSequence(ALICE), 1);
}

BOOST_AUTO_TEST_CASE(per_signer_sequencing_is_enforced)
{
    Fixture f;
    // Wrong starting sequence is rejected and does not advance.
    const auto r1{*f.exec.ExecuteSlot({Bid(ALICE, 1, Pts({{10, 100}, {20, 40}, {30, 0}}))})};
    BOOST_REQUIRE_EQUAL(r1.rejected.size(), 1U);
    BOOST_CHECK(r1.rejected.front().second == ActionReject::BAD_SEQUENCE);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 0);

    // Correct sequence applies and advances.
    const auto r2{*f.exec.ExecuteSlot({Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))})};
    BOOST_CHECK_EQUAL(r2.applied.size(), 1U);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 1);
}

BOOST_AUTO_TEST_CASE(withdrawals_create_one_time_receipts_committed_by_root)
{
    Fixture f;
    // ALICE deposits more native and withdraws twice in one slot.
    Fund(f.state, ALICE, Quote(), 1000);
    const auto result{*f.exec.ExecuteSlot({
        Withdraw(ALICE, 0, Quote(), 300, DEST),
        Withdraw(ALICE, 1, Quote(), 200, DEST)})};

    BOOST_REQUIRE_EQUAL(result.withdrawal_requests.size(), 2U);
    // Request ids are unique and one-time; these are REQUESTS, not
    // redeemable receipts (B3 authorization is an owner decision).
    BOOST_CHECK(result.withdrawal_requests[0].receipt_id != result.withdrawal_requests[1].receipt_id);
    for (const auto& receipt : result.withdrawal_requests) {
        BOOST_CHECK(f.ledger.GetRequest(receipt.receipt_id).has_value());
        BOOST_CHECK(receipt.vault_commitment == VAULT);
    }
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 2);

    // The receipt root commits to exactly these receipts; a slot with no
    // withdrawals has a different (empty) receipt root.
    Fixture g;
    const auto empty{*g.exec.ExecuteSlot({})};
    BOOST_CHECK(empty.withdrawal_requests.empty());
    BOOST_CHECK(empty.request_root != result.request_root);

    // Solvency is preserved across the batch.
    BOOST_CHECK(f.ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(state_rejected_actions_still_consume_their_sequence)
{
    Fixture f;
    // A withdrawal ALICE cannot afford is rejected by state, but the
    // sequence advances so the same nonce cannot be replayed.
    const auto result{*f.exec.ExecuteSlot({Withdraw(ALICE, 0, BaseX(), 10, DEST)})};
    BOOST_REQUIRE_EQUAL(result.rejected.size(), 1U);
    BOOST_CHECK(result.rejected.front().second == ActionReject::REJECTED_BY_STATE);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 1);
    BOOST_CHECK(result.withdrawal_requests.empty());
    BOOST_CHECK(f.ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(distinct_slots_advance_and_produce_distinct_roots)
{
    Fixture f;
    const auto s0{*f.exec.ExecuteSlot({Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))})};
    BOOST_CHECK_EQUAL(s0.slot, 0U);
    const auto s1{*f.exec.ExecuteSlot({Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))})};
    BOOST_CHECK_EQUAL(s1.slot, 1U);
    // The book crossing clears in slot 1.
    BOOST_CHECK(s1.clearing.cleared);
    BOOST_CHECK_EQUAL(s1.clearing.price, 20);
    BOOST_CHECK(s0.state_root != s1.state_root);
}




BOOST_AUTO_TEST_SUITE_END()

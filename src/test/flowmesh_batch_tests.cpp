// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh canonical batch execution: order-independence, per-signer
//! sequencing, same-sequence equivocation rejection, deterministic
//! settlement, receipt/state roots, and one-time receipt ids. The
//! certificate cryptography stays behind an opaque authenticator.

#include <flowmesh/batch.h>

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
    return modern::IssuanceAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}), 0});
}
const modern::AssetId& Quote() { return modern::NativeAsset(); }

std::vector<Breakpoint> Pts(std::vector<std::pair<CAmount, CAmount>> raw)
{
    std::vector<Breakpoint> out;
    for (const auto& [p, q] : raw) out.push_back({p, q});
    return out;
}

//! Accept every credential except the sentinel byte 0x00.
class MockAuth final : public flowmesh::ActionAuthenticator
{
public:
    bool Authenticate(const Action& action) const override
    {
        return !action.credential.empty() && action.credential[0] != 0x00;
    }
};

Action Bid(const flowmesh::AccountId& signer, uint64_t seq, std::vector<Breakpoint> curve)
{
    Action a;
    a.signer = signer;
    a.sequence = seq;
    a.type = static_cast<uint8_t>(ActionType::SUBMIT_BID);
    a.curve = std::move(curve);
    a.credential = {0x01};
    return a;
}

Action Ask(const flowmesh::AccountId& signer, uint64_t seq, std::vector<Breakpoint> curve)
{
    Action a;
    a.signer = signer;
    a.sequence = seq;
    a.type = static_cast<uint8_t>(ActionType::SUBMIT_ASK);
    a.curve = std::move(curve);
    a.credential = {0x01};
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
    a.credential = {0x01};
    return a;
}

struct Fixture {
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    flowmesh::Ledger& ledger{state.ledger};
    flowmesh::ClearingEngine& engine{state.book};
    MockAuth auth;
    flowmesh::BatchExecutor exec{state, auth};
    Fixture()
    {
        ledger.Deposit(ALICE, Quote(), 2400); // staircase reservation bound of the standard bid
        ledger.Deposit(BOB, BaseX(), 80);
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
        const auto result{f.exec.ExecuteSlot(shuffled)};
        if (i == 0) {
            canonical_state = result.state_root;
            canonical_receipt = result.receipt_root;
            price = result.clearing.price;
            volume = result.clearing.volume;
            applied = result.applied;
        } else {
            BOOST_CHECK_EQUAL(result.state_root.GetHex(), canonical_state.GetHex());
            BOOST_CHECK_EQUAL(result.receipt_root.GetHex(), canonical_receipt.GetHex());
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
    const auto result{f.exec.ExecuteSlot({a, b, Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))})};

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
    const auto dup{g.exec.ExecuteSlot({a, a, Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))})};
    for (const auto& [id, reason] : dup.rejected) {
        BOOST_CHECK(reason != ActionReject::EQUIVOCATION);
    }
    BOOST_CHECK_EQUAL(g.exec.NextSequence(ALICE), 1);
}

BOOST_AUTO_TEST_CASE(per_signer_sequencing_is_enforced)
{
    Fixture f;
    // Wrong starting sequence is rejected and does not advance.
    const auto r1{f.exec.ExecuteSlot({Bid(ALICE, 1, Pts({{10, 100}, {20, 40}, {30, 0}}))})};
    BOOST_REQUIRE_EQUAL(r1.rejected.size(), 1U);
    BOOST_CHECK(r1.rejected.front().second == ActionReject::BAD_SEQUENCE);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 0);

    // Correct sequence applies and advances.
    const auto r2{f.exec.ExecuteSlot({Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))})};
    BOOST_CHECK_EQUAL(r2.applied.size(), 1U);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 1);

    // Unauthenticated actions never consume a sequence.
    Action bad{Bid(BOB, 0, Pts({{10, 20}, {20, 80}}))};
    bad.type = static_cast<uint8_t>(ActionType::SUBMIT_ASK);
    bad.credential = {0x00}; // rejected by the authenticator
    const auto r3{f.exec.ExecuteSlot({bad})};
    BOOST_CHECK(r3.rejected.front().second == ActionReject::UNAUTHENTICATED);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(BOB), 0);
}

BOOST_AUTO_TEST_CASE(withdrawals_create_one_time_receipts_committed_by_root)
{
    Fixture f;
    // ALICE deposits more native and withdraws twice in one slot.
    f.ledger.Deposit(ALICE, Quote(), 1000);
    const auto result{f.exec.ExecuteSlot({
        Withdraw(ALICE, 0, Quote(), 300, DEST),
        Withdraw(ALICE, 1, Quote(), 200, DEST)})};

    BOOST_REQUIRE_EQUAL(result.receipts.size(), 2U);
    // Receipt ids are unique and one-time.
    BOOST_CHECK(result.receipts[0].receipt_id != result.receipts[1].receipt_id);
    for (const auto& receipt : result.receipts) {
        BOOST_CHECK(f.ledger.GetFinalized(receipt.receipt_id).has_value());
        BOOST_CHECK(receipt.vault_commitment == VAULT);
    }
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 2);

    // The receipt root commits to exactly these receipts; a slot with no
    // withdrawals has a different (empty) receipt root.
    Fixture g;
    const auto empty{g.exec.ExecuteSlot({})};
    BOOST_CHECK(empty.receipts.empty());
    BOOST_CHECK(empty.receipt_root != result.receipt_root);

    // Solvency is preserved across the batch.
    BOOST_CHECK(f.ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(state_rejected_actions_still_consume_their_sequence)
{
    Fixture f;
    // A withdrawal ALICE cannot afford is rejected by state, but the
    // sequence advances so the same nonce cannot be replayed.
    const auto result{f.exec.ExecuteSlot({Withdraw(ALICE, 0, BaseX(), 10, DEST)})};
    BOOST_REQUIRE_EQUAL(result.rejected.size(), 1U);
    BOOST_CHECK(result.rejected.front().second == ActionReject::REJECTED_BY_STATE);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 1);
    BOOST_CHECK(result.receipts.empty());
    BOOST_CHECK(f.ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(distinct_slots_advance_and_produce_distinct_roots)
{
    Fixture f;
    const auto s0{f.exec.ExecuteSlot({Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))})};
    BOOST_CHECK_EQUAL(s0.slot, 0U);
    const auto s1{f.exec.ExecuteSlot({Ask(BOB, 0, Pts({{10, 20}, {20, 80}}))})};
    BOOST_CHECK_EQUAL(s1.slot, 1U);
    // The book crossing clears in slot 1.
    BOOST_CHECK(s1.clearing.cleared);
    BOOST_CHECK_EQUAL(s1.clearing.price, 20);
    BOOST_CHECK(s0.state_root != s1.state_root);
}

//! D5 fix: dedup is credential-aware and arrival-order independent. One
//! id may arrive with several credentials; it authenticates iff ANY of
//! them does — a junk-credential copy can neither shadow a valid
//! submission nor make the outcome depend on which copy arrived first.
BOOST_AUTO_TEST_CASE(credential_variants_are_order_independent)
{
    Action good{Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))};
    Action junk{good};
    junk.credential = {0x00}; // same id, failing credential
    BOOST_CHECK(good.Id() == junk.Id());

    uint256 root_junk_first, root_good_first;
    {
        Fixture f;
        const auto r{f.exec.ExecuteSlot({junk, good})};
        BOOST_REQUIRE_EQUAL(r.applied.size(), 1U);
        BOOST_CHECK(r.rejected.empty());
        root_junk_first = r.state_root;
    }
    {
        Fixture f;
        const auto r{f.exec.ExecuteSlot({good, junk})};
        BOOST_REQUIRE_EQUAL(r.applied.size(), 1U);
        BOOST_CHECK(r.rejected.empty());
        root_good_first = r.state_root;
    }
    BOOST_CHECK_EQUAL(root_junk_first.GetHex(), root_good_first.GetHex());

    // The junk-credential copy ALONE still rejects, without consuming
    // the sequence.
    Fixture f;
    const auto r{f.exec.ExecuteSlot({junk})};
    BOOST_CHECK(r.applied.empty());
    BOOST_REQUIRE_EQUAL(r.rejected.size(), 1U);
    BOOST_CHECK(r.rejected.front().second == ActionReject::UNAUTHENTICATED);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 0);
}

//! D5 fix: authentication runs before equivocation grouping, so a
//! forged (unauthenticated) action at an honest signer's (signer,
//! sequence) can no longer manufacture an equivocation and kill the
//! honest submission.
BOOST_AUTO_TEST_CASE(forgery_cannot_manufacture_equivocation)
{
    Fixture f;
    const Action honest{Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))};
    Action forged{Bid(ALICE, 0, Pts({{10, 1}, {20, 0}}))}; // different id, same (signer, seq)
    forged.credential = {0x00};
    BOOST_CHECK(honest.Id() != forged.Id());

    const auto r{f.exec.ExecuteSlot({forged, honest})};
    BOOST_REQUIRE_EQUAL(r.applied.size(), 1U);
    BOOST_CHECK(r.applied.front() == honest.Id());
    BOOST_REQUIRE_EQUAL(r.rejected.size(), 1U);
    BOOST_CHECK(r.rejected.front().first == forged.Id());
    BOOST_CHECK(r.rejected.front().second == ActionReject::UNAUTHENTICATED);
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 1); // honest action advanced
}

//! Pass-2 fix: credential variants are CANONICALIZED (lexicographically
//! sorted, exact duplicates removed) before authentication, so every
//! node performs the same authentication calls in the same order no
//! matter how the network delivered the set.
BOOST_AUTO_TEST_CASE(credential_checks_are_canonical)
{
    //! Records every credential it is asked about, in call order;
    //! accepts only the sentinel {0x02}.
    class CountingAuth final : public flowmesh::ActionAuthenticator
    {
    public:
        mutable std::vector<std::vector<unsigned char>> calls;
        bool Authenticate(const Action& action) const override
        {
            calls.push_back(action.credential);
            return action.credential == std::vector<unsigned char>{0x02};
        }
    };

    const Action base{Bid(ALICE, 0, Pts({{10, 100}, {20, 40}, {30, 0}}))};
    const auto with_credential{[&](const uint8_t byte) {
        Action a{base};
        a.credential = {byte};
        return a;
    }};
    // Same credential SET in two arrival orders, with an exact duplicate.
    const std::vector<Action> order_a{with_credential(0x03), with_credential(0x01),
                                      with_credential(0x02), with_credential(0x01)};
    const std::vector<Action> order_b{with_credential(0x01), with_credential(0x02),
                                      with_credential(0x03), with_credential(0x01)};

    std::vector<std::vector<unsigned char>> calls_a, calls_b;
    uint256 root_a, root_b;
    {
        flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
        CountingAuth auth;
        flowmesh::BatchExecutor exec{state, auth};
        state.ledger.Deposit(ALICE, Quote(), 2400);
        root_a = exec.ExecuteSlot(order_a).state_root;
        calls_a = auth.calls;
    }
    {
        flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
        CountingAuth auth;
        flowmesh::BatchExecutor exec{state, auth};
        state.ledger.Deposit(ALICE, Quote(), 2400);
        root_b = exec.ExecuteSlot(order_b).state_root;
        calls_b = auth.calls;
    }
    // Identical call count AND order: canonical {0x01}, then {0x02}
    // (accepted — {0x03} is never reached), duplicates checked once.
    const std::vector<std::vector<unsigned char>> expected{{0x01}, {0x02}};
    BOOST_CHECK(calls_a == expected);
    BOOST_CHECK(calls_b == expected);
    BOOST_CHECK_EQUAL(root_a.GetHex(), root_b.GetHex());
}

BOOST_AUTO_TEST_SUITE_END()

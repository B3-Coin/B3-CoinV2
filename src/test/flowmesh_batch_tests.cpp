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
#include <array>
#include <map>
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
const flowmesh::AnchorRef A2_ANCHOR{130, uint256::ONE};

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

std::array<unsigned char, bls::PUBKEY_SIZE> BlsKey(const unsigned char id)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = id + i + 1;
    const auto secret{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(secret.has_value());
    return secret->GetPublicKey().Compressed();
}

flowmesh::FlowMeshFeeContext FeeContext()
{
    flowmesh::FlowMeshFeeContext fees;
    fees.market_id = uint256::ONE;
    fees.epoch = 7;
    fees.treasury_owner_commitment = DEST;
    for (unsigned char i{1}; i <= 4; ++i) {
        flowmesh::FlowMeshFeeSeat seat;
        seat.seat_id = uint256{i};
        seat.bls_pubkey = BlsKey(i);
        fees.seats.push_back(seat);
    }
    BOOST_REQUIRE(flowmesh::FlowMeshFeeContextIsCanonical(fees));
    return fees;
}

std::vector<Action> FeeBearingTrade()
{
    return {
        Bid(ALICE, 0, Pts({{1'000, 100}, {1'001, 0}})),
        Ask(BOB, 0, Pts({{1'000, 100}})),
    };
}

//! Test funding shortcut over the test-only bridge.
inline bool Fund(flowmesh::FlowMeshState& state, const flowmesh::AccountId& account,
                 const modern::AssetId& asset, const CAmount amount)
{
    return flowmesh::test_only::StateFunding::Fund(state, account, asset, amount);
}

class TestChainFacts final : public flowmesh::DepositVerifier
{
public:
    std::optional<flowmesh::DepositInfo> GetDeposit(
        const COutPoint&, const flowmesh::AnchorRef&) const override
    {
        return std::nullopt;
    }

    std::optional<CAmount> GetWithdrawalCapacity(
        const modern::AssetId& asset,
        const flowmesh::AnchorRef&) const override
    {
        ++capacity_calls[asset];
        const auto it{capacities.find(asset)};
        return it == capacities.end() ? default_capacity : it->second;
    }

    std::optional<std::vector<flowmesh::WithdrawalSettlementFactV1>>
    GetWithdrawalSettlements(
        const std::optional<flowmesh::AnchorRef>&,
        const flowmesh::AnchorRef&) const override
    {
        return std::vector<flowmesh::WithdrawalSettlementFactV1>{};
    }

    std::optional<CAmount> default_capacity{MAX_MONEY};
    std::map<modern::AssetId, std::optional<CAmount>> capacities;
    mutable std::map<modern::AssetId, size_t> capacity_calls;
};

struct Fixture {
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{state.LedgerView()};
    TestChainFacts chain_facts;
    flowmesh::BatchExecutor exec{state, &chain_facts};
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
    // Request ids are unique and one-time. They become spend-authorizing only
    // through the connected type-8 checkpoint and type-9 proof path.
    BOOST_CHECK(result.withdrawal_requests[0].receipt_id != result.withdrawal_requests[1].receipt_id);
    for (const auto& receipt : result.withdrawal_requests) {
        BOOST_CHECK(f.ledger.GetRequest(receipt.receipt_id).has_value());
        BOOST_CHECK(receipt.vault_commitment == VAULT);
    }
    BOOST_CHECK_EQUAL(f.exec.NextSequence(ALICE), 2);
    BOOST_CHECK_EQUAL(f.chain_facts.capacity_calls[Quote()], 1U);

    // The receipt root commits to exactly these receipts; a slot with no
    // withdrawals has a different (empty) receipt root.
    Fixture g;
    const auto empty{*g.exec.ExecuteSlot({})};
    BOOST_CHECK(empty.withdrawal_requests.empty());
    BOOST_CHECK(empty.request_root != result.request_root);

    // Solvency is preserved across the batch.
    BOOST_CHECK(f.ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(withdrawal_capacity_rejects_before_debit)
{
    Fixture f;
    f.chain_facts.capacities[Quote()] = 64;
    const CAmount before{f.ledger.Available(ALICE, Quote())};
    const auto result{
        *f.exec.ExecuteSlot({Withdraw(ALICE, 0, Quote(), 65, DEST)})};
    BOOST_REQUIRE_EQUAL(result.rejected.size(), 1U);
    BOOST_CHECK(result.withdrawal_requests.empty());
    BOOST_CHECK_EQUAL(f.ledger.Available(ALICE, Quote()), before);
    BOOST_CHECK_EQUAL(f.ledger.PendingWithdrawals(Quote()), 0);
    BOOST_CHECK_EQUAL(f.chain_facts.capacity_calls[Quote()], 1U);
}

BOOST_AUTO_TEST_CASE(treasury_fee_defers_at_a3_until_anchored_capacity_exists)
{
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{state.LedgerView()};
    TestChainFacts chain_facts;
    chain_facts.capacities[Quote()] = 0;
    const flowmesh::FlowMeshFeeContext fees{FeeContext()};
    const flowmesh::AccountId treasury{
        flowmesh::FlowMeshTreasuryFeeAccount(fees)};
    flowmesh::BatchExecutor exec{state, &chain_facts, &fees};
    BOOST_REQUIRE(Fund(state, ALICE, Quote(), 100'000));
    BOOST_REQUIRE(Fund(state, BOB, BaseX(), 100));

    // The first A3 slot still anchors into A2. With no anchored pool-change
    // output, trading and fee allocation succeed while the treasury receipt
    // is safely deferred in its fixed internal account.
    const auto first{exec.ExecuteSlot(FeeBearingTrade(), A2_ANCHOR)};
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE(first->clearing.cleared);
    BOOST_CHECK_EQUAL(first->clearing.fees.treasury_fee, 2);
    BOOST_CHECK(first->withdrawal_requests.empty());
    BOOST_CHECK_EQUAL(ledger.Available(treasury, Quote()), 2);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 0);
    BOOST_CHECK_EQUAL(chain_facts.capacity_calls[Quote()], 1U);
    BOOST_CHECK(ledger.SolvencyHolds());

    // Once capacity appears, the next ordinary slot emits exactly one receipt
    // for the complete accrued balance. A separate user withdrawal proves
    // retry is not tied to another fee-bearing trade.
    chain_facts.capacities[Quote()] = 2;
    const auto second{exec.ExecuteSlot(
        {Withdraw(ALICE, 1, BaseX(), 1, DEST)}, A2_ANCHOR)};
    BOOST_REQUIRE(second.has_value());
    BOOST_REQUIRE_EQUAL(second->withdrawal_requests.size(), 2U);
    const auto treasury_request{std::find_if(
        second->account_withdrawal_requests.begin(),
        second->account_withdrawal_requests.end(), [&](const auto& request) {
            return request.account == treasury;
        })};
    BOOST_REQUIRE(treasury_request !=
                  second->account_withdrawal_requests.end());
    BOOST_CHECK_EQUAL(treasury_request->request.amount, 2);
    BOOST_CHECK(treasury_request->request.asset == Quote());
    BOOST_CHECK(treasury_request->request.destination == DEST);
    BOOST_CHECK_EQUAL(ledger.Available(treasury, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 2);
    BOOST_CHECK_EQUAL(chain_facts.capacity_calls[Quote()], 2U);

    // Later slots cannot debit the already-empty treasury account or create a
    // duplicate receipt while the first one remains pending.
    const auto third{exec.ExecuteSlot({}, A2_ANCHOR)};
    BOOST_REQUIRE(third.has_value());
    BOOST_CHECK(third->withdrawal_requests.empty());
    BOOST_CHECK_EQUAL(ledger.Available(treasury, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 2);
    BOOST_CHECK_EQUAL(chain_facts.capacity_calls[Quote()], 2U);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(treasury_fee_with_immediate_capacity_still_emits_once)
{
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{state.LedgerView()};
    TestChainFacts chain_facts;
    chain_facts.capacities[Quote()] = MAX_MONEY;
    const flowmesh::FlowMeshFeeContext fees{FeeContext()};
    const flowmesh::AccountId treasury{
        flowmesh::FlowMeshTreasuryFeeAccount(fees)};
    flowmesh::BatchExecutor exec{state, &chain_facts, &fees};
    BOOST_REQUIRE(Fund(state, ALICE, Quote(), 100'000));
    BOOST_REQUIRE(Fund(state, BOB, BaseX(), 100));

    const auto result{exec.ExecuteSlot(FeeBearingTrade(), A2_ANCHOR)};
    BOOST_REQUIRE(result.has_value());
    BOOST_REQUIRE_EQUAL(result->withdrawal_requests.size(), 1U);
    BOOST_REQUIRE_EQUAL(result->account_withdrawal_requests.size(), 1U);
    BOOST_CHECK(result->account_withdrawal_requests[0].account == treasury);
    BOOST_CHECK_EQUAL(result->withdrawal_requests[0].amount, 2);
    BOOST_CHECK(result->withdrawal_requests[0].destination == DEST);
    BOOST_CHECK_EQUAL(ledger.Available(treasury, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 2);
    BOOST_CHECK_EQUAL(chain_facts.capacity_calls[Quote()], 1U);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(fragmented_treasury_capacity_flushes_maximal_then_remainder)
{
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    const flowmesh::Ledger& ledger{state.LedgerView()};
    TestChainFacts chain_facts;
    chain_facts.capacities[Quote()] = 64;
    const flowmesh::FlowMeshFeeContext fees{FeeContext()};
    const flowmesh::AccountId treasury{
        flowmesh::FlowMeshTreasuryFeeAccount(fees)};
    flowmesh::BatchExecutor exec{state, &chain_facts, &fees};
    BOOST_REQUIRE(Fund(state, treasury, Quote(), 65));

    // Model 65 one-unit pool outputs. The first slot emits the maximal safe
    // receipt (top-64 capacity), leaving one unit available rather than
    // permanently waiting for capacity 65 that fragmentation cannot expose.
    const auto first{exec.ExecuteSlot({}, A2_ANCHOR)};
    BOOST_REQUIRE(first.has_value());
    BOOST_REQUIRE_EQUAL(first->withdrawal_requests.size(), 1U);
    const modern::WithdrawalReceipt first_request{
        first->withdrawal_requests.front()};
    BOOST_CHECK_EQUAL(first_request.amount, 64);
    BOOST_CHECK_EQUAL(ledger.Available(treasury, Quote()), 1);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 64);

    flowmesh::WithdrawalSettlementFactV1 settlement;
    settlement.receipt.receipt_id = first_request.receipt_id;
    settlement.receipt.market_id = fees.market_id;
    settlement.receipt.epoch = fees.epoch;
    settlement.receipt.sequence = 0;
    settlement.receipt.account = treasury;
    settlement.receipt.asset = Quote();
    settlement.receipt.amount = first_request.amount;
    settlement.receipt.destination_owner_commitment = DEST;
    settlement.receipt.vault_id = VAULT;
    settlement.receipt.deterministic_change_shard = 1;
    settlement.checkpoint_id = uint256::ONE;
    settlement.transaction_id = Txid::FromUint256(ALICE);
    settlement.connected_height = 131;
    settlement.connected_block = BOB;
    const std::vector<flowmesh::WithdrawalSettlementFactV1> settlements{
        settlement};
    const auto retired{exec.ExecuteSlot({}, A2_ANCHOR, settlements)};
    BOOST_REQUIRE(retired.has_value());
    BOOST_REQUIRE_EQUAL(retired->settled_withdrawals.size(), 1U);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.Custody(Quote()), 1);

    // After the first payout/settlement leaves one one-unit pool output, the
    // next ordinary slot emits the exact remainder once, with no lost or
    // duplicate debit.
    chain_facts.capacities[Quote()] = 1;
    const auto remainder{exec.ExecuteSlot({}, A2_ANCHOR)};
    BOOST_REQUIRE(remainder.has_value());
    BOOST_REQUIRE_EQUAL(remainder->withdrawal_requests.size(), 1U);
    BOOST_CHECK_EQUAL(remainder->withdrawal_requests.front().amount, 1);
    BOOST_CHECK(remainder->withdrawal_requests.front().receipt_id !=
                first_request.receipt_id);
    BOOST_CHECK_EQUAL(ledger.Available(treasury, Quote()), 0);
    BOOST_CHECK_EQUAL(ledger.PendingWithdrawals(Quote()), 1);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(missing_withdrawal_capacity_fails_closed)
{
    flowmesh::FlowMeshState state{VAULT, BaseX(), Quote()};
    BOOST_REQUIRE(Fund(state, ALICE, Quote(), 100));
    const CAmount before{state.LedgerView().Available(ALICE, Quote())};
    flowmesh::BatchExecutor exec{state, /*deposits=*/nullptr};
    const auto result{
        *exec.ExecuteSlot({Withdraw(ALICE, 0, Quote(), 1, DEST)})};
    BOOST_REQUIRE_EQUAL(result.rejected.size(), 1U);
    BOOST_CHECK(result.withdrawal_requests.empty());
    BOOST_CHECK_EQUAL(state.LedgerView().Available(ALICE, Quote()), before);
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

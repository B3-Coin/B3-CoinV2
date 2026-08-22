// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! FlowMesh internal asset ledger: deterministic per-account/per-asset
//! balances and the vault solvency invariant (custody == liabilities per
//! asset), with adversarial overflow/underflow tests. No matching.

#include <flowmesh/ledger.h>
#include <test/util/asset.h>

#include <hash.h>
#include <modern/policy.h>
#include <modern/vault.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <optional>

BOOST_AUTO_TEST_SUITE(flowmesh_ledger_tests)

namespace {

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
const flowmesh::AccountId ALICE{uint256{"00000000000000000000000000000000000000000000000000000000000000a1"}};
const flowmesh::AccountId BOB{uint256{"00000000000000000000000000000000000000000000000000000000000000b2"}};
const uint256 DEST{uint256{"00000000000000000000000000000000000000000000000000000000000000d1"}};

modern::AssetId AssetX()
{
    return modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000011"}), 0});
}
modern::AssetId AssetY()
{
    return modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"0000000000000000000000000000000000000000000000000000000000000022"}), 0});
}

} // namespace

BOOST_AUTO_TEST_CASE(deposits_credit_and_preserve_solvency)
{
    flowmesh::Ledger ledger{VAULT};
    BOOST_CHECK(ledger.SolvencyHolds());

    BOOST_CHECK(ledger.Deposit(ALICE, AssetX(), 1000));
    BOOST_CHECK(ledger.Deposit(BOB, AssetX(), 500));
    BOOST_CHECK(ledger.Deposit(ALICE, AssetY(), 7));

    BOOST_CHECK_EQUAL(ledger.Available(ALICE, AssetX()), 1000);
    BOOST_CHECK_EQUAL(ledger.Available(BOB, AssetX()), 500);
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), 1500);
    BOOST_CHECK_EQUAL(ledger.Liabilities(AssetX()), 1500);
    BOOST_CHECK_EQUAL(ledger.Custody(AssetY()), 7);
    BOOST_CHECK(ledger.SolvencyHolds());

    // Non-positive deposits are rejected and change nothing.
    const uint256 root{ledger.StateRoot()};
    BOOST_CHECK(!ledger.Deposit(ALICE, AssetX(), 0));
    BOOST_CHECK(!ledger.Deposit(ALICE, AssetX(), -5));
    BOOST_CHECK_EQUAL(ledger.StateRoot().GetHex(), root.GetHex());
}

BOOST_AUTO_TEST_CASE(reservations_and_fees_are_internal_moves)
{
    flowmesh::Ledger ledger{VAULT};
    BOOST_CHECK(ledger.Deposit(ALICE, AssetX(), 1000));

    // Reserve moves available -> reserved; liabilities unchanged.
    BOOST_CHECK(ledger.Reserve(ALICE, AssetX(), 400));
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, AssetX()), 600);
    BOOST_CHECK_EQUAL(ledger.Reserved(ALICE, AssetX()), 400);
    BOOST_CHECK_EQUAL(ledger.Liabilities(AssetX()), 1000);
    BOOST_CHECK(ledger.SolvencyHolds());

    // Cannot reserve more than available.
    BOOST_CHECK(!ledger.Reserve(ALICE, AssetX(), 601));
    BOOST_CHECK(ledger.Release(ALICE, AssetX(), 400));
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, AssetX()), 1000);

    // Fee: internal transfer to the fee account; custody and total
    // liabilities are unchanged.
    BOOST_CHECK(ledger.ChargeFee(ALICE, AssetX(), 30));
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, AssetX()), 970);
    BOOST_CHECK_EQUAL(ledger.Available(flowmesh::FeeAccount(), AssetX()), 30);
    BOOST_CHECK_EQUAL(ledger.Liabilities(AssetX()), 1000);
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), 1000);
    BOOST_CHECK(ledger.SolvencyHolds());

    BOOST_CHECK(!ledger.ChargeFee(ALICE, AssetX(), 10'000)); // insufficient
    BOOST_CHECK(!ledger.ChargeFee(flowmesh::FeeAccount(), AssetX(), 1)); // fee acct cannot pay
}

BOOST_AUTO_TEST_CASE(withdrawal_lifecycle_preserves_the_invariant)
{
    flowmesh::Ledger ledger{VAULT};
    BOOST_CHECK(ledger.Deposit(ALICE, AssetX(), 1000));

    // REQUESTED: liability moves from balance to a pending REQUEST;
    // custody is untouched, so the invariant still holds. Nothing here
    // is B3-redeemable — that stage is gated on an owner decision.
    const auto receipt{ledger.RequestWithdrawal(ALICE, AssetX(), 400, DEST)};
    BOOST_REQUIRE(receipt.has_value());
    BOOST_CHECK_EQUAL(receipt->amount, 400);
    BOOST_CHECK(receipt->vault_commitment == VAULT);
    BOOST_CHECK_EQUAL(ledger.Available(ALICE, AssetX()), 600);
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), 1000);
    BOOST_CHECK_EQUAL(ledger.Liabilities(AssetX()), 1000); // 600 + 400 pending
    BOOST_CHECK(ledger.SolvencyHolds());

    // The pending request is visible as a REQUEST — the ledger exposes
    // no redeemable/finalized view at all.
    BOOST_CHECK(ledger.GetRequest(receipt->receipt_id).has_value());

    // Consume (the on-chain spend connected): custody and receipt-liability
    // leave together.
    BOOST_CHECK(ledger.ConsumeRequest(receipt->receipt_id));
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), 600);
    BOOST_CHECK_EQUAL(ledger.Liabilities(AssetX()), 600);
    BOOST_CHECK(ledger.SolvencyHolds());

    // A receipt is consumed exactly once, and cannot be over-withdrawn.
    BOOST_CHECK(!ledger.ConsumeRequest(receipt->receipt_id));
    BOOST_CHECK(!ledger.GetRequest(receipt->receipt_id).has_value());
    BOOST_CHECK(!ledger.RequestWithdrawal(ALICE, AssetX(), 601, DEST).has_value());
}

BOOST_AUTO_TEST_CASE(end_to_end_solvency_against_the_vault_checker)
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = 1001;
    params.test_only_asset_policies_active = true;

    flowmesh::Ledger ledger{VAULT};
    BOOST_CHECK(ledger.Deposit(ALICE, AssetX(), 1000));
    const auto receipt{ledger.RequestWithdrawal(ALICE, AssetX(), 400, DEST)};
    BOOST_REQUIRE(receipt.has_value());

    // TEST-ONLY adapter SIMULATING the future owner-approved redeemable
    // view: it presents pending requests to the vault checker AS IF the
    // trustless B3 authorization existed. Production deliberately has
    // no such adapter — the ledger itself no longer implements
    // FinalizedReceiptView, so FlowMesh certification alone can never
    // pretend a request is an authorized B3 spend.
    class SimulatedRedeemableView final : public modern::FinalizedReceiptView
    {
    public:
        explicit SimulatedRedeemableView(const flowmesh::Ledger& ledger) : m_ledger{ledger} {}
        std::optional<modern::WithdrawalReceipt> GetFinalized(
            const uint256& receipt_id) const override
        {
            return m_ledger.GetRequest(receipt_id);
        }

    private:
        const flowmesh::Ledger& m_ledger;
    };
    const SimulatedRedeemableView redeemable{ledger};
    modern::ModernOutput vault_prev;
    vault_prev.asset = AssetX();
    vault_prev.amount = 1000;
    vault_prev.policy_type = static_cast<uint16_t>(modern::PolicyType::DEX_VAULT);
    vault_prev.policy_version = modern::DEX_VAULT_POLICY_VERSION_V2;
    vault_prev.policy_commitment = VAULT;
    vault_prev.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_POOL_CHANGE, 0);

    modern::ModernOutput payout;
    payout.asset = AssetX();
    payout.amount = 400;
    payout.policy_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
    payout.policy_version = modern::POLICY_VERSION_V1;
    payout.policy_commitment = DEST;

    modern::ModernOutput change{vault_prev};
    change.amount = 600;
    change.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_POOL_CHANGE, 1);

    modern::ModernTransition t;
    t.inputs.resize(1);
    t.inputs[0].prevout = COutPoint{Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000e0"}), 0};
    modern::TransitionProof proof;
    proof.proof_type = static_cast<uint16_t>(modern::PolicyType::DEX_VAULT);
    proof.proof_version = modern::DEX_VAULT_POLICY_VERSION_V2; // proofs pair with the v2 policy
    VectorWriter writer{proof.payload, 0};
    writer << std::vector<uint256>{receipt->receipt_id};
    t.proofs.push_back(proof);
    t.outputs = {payout, change};

    std::vector<uint256> consumed;
    BOOST_CHECK(modern::CheckVaultWithdrawal(std::vector<modern::ModernOutput>{vault_prev}, t,
                                             redeemable, /*height=*/1001, params, &consumed) ==
                modern::VaultCheck::OK);
    BOOST_REQUIRE_EQUAL(consumed.size(), 1U);

    // Applying the reported consumption keeps the ledger solvent and in
    // step with on-chain custody.
    BOOST_CHECK(ledger.ConsumeRequest(consumed[0]));
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), 600);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_CASE(state_root_is_deterministic_and_path_independent)
{
    flowmesh::Ledger a{VAULT};
    flowmesh::Ledger b{VAULT};

    // Same net operations reached by different intermediate paths must
    // produce the identical canonical root.
    a.Deposit(ALICE, AssetX(), 1000);
    a.Deposit(BOB, AssetX(), 500);
    a.Reserve(ALICE, AssetX(), 200);
    a.Release(ALICE, AssetX(), 200);

    b.Deposit(BOB, AssetX(), 500);
    b.Deposit(ALICE, AssetX(), 400);
    b.Deposit(ALICE, AssetX(), 600);
    BOOST_CHECK_EQUAL(a.StateRoot().GetHex(), b.StateRoot().GetHex());

    // Slot advance changes the root deterministically.
    const uint256 before{a.StateRoot()};
    a.AdvanceSlot();
    BOOST_CHECK(a.StateRoot() != before);
    b.AdvanceSlot();
    BOOST_CHECK_EQUAL(a.StateRoot().GetHex(), b.StateRoot().GetHex());

    // A REJECTED fee (zero amount) is a pure no-op: byte-identical root.
    flowmesh::Ledger c{VAULT};
    c.Deposit(ALICE, AssetX(), 1000);
    BOOST_CHECK(!c.ChargeFee(ALICE, AssetX(), 0)); // rejected, no-op
    flowmesh::Ledger d{VAULT};
    d.Deposit(ALICE, AssetX(), 1000);
    BOOST_CHECK_EQUAL(c.StateRoot().GetHex(), d.StateRoot().GetHex());
    // A REAL fee leaves a persistent fee-account entry: the roots must
    // differ (canonical pruning removes only truly empty entries).
    BOOST_CHECK(c.ChargeFee(ALICE, AssetX(), 1));
    BOOST_CHECK(c.StateRoot() != d.StateRoot());
}

//! Pass-3 fix: the ledger state root is canonically framed (v2 domain).
//! Each variable-length collection — balances, custody, pending
//! receipts — is preceded by its entry count, so the boundaries between
//! collections are part of the preimage: different collection layouts
//! can never rely on unframed boundaries (incidental byte alignment)
//! to distinguish themselves. The empty-ledger root is pinned
//! byte-exactly, and the framed preimage is reconstructed field by
//! field: with the counts it reproduces the root exactly; without
//! them it does not — the counts are load-bearing, not decorative.
BOOST_AUTO_TEST_CASE(state_root_v2_is_canonically_framed)
{
    // Pinned empty-ledger v2 vector (VAULT, slot 0, seq 0, all
    // collections empty) — filled from the first computed value, frozen
    // since; changes only with a reviewed format bump.
    BOOST_CHECK_EQUAL(
        flowmesh::Ledger{VAULT}.StateRoot().GetHex(),
        "ce335b8fce42d09636c1d5c6fd5a2159d307ca0629f452c44a624e237fb77222");

    // Populate all three collections: two balance entries, one custody
    // entry, one pending receipt.
    flowmesh::Ledger ledger{VAULT};
    BOOST_REQUIRE(ledger.Deposit(ALICE, AssetX(), 1000));
    BOOST_REQUIRE(ledger.Deposit(BOB, AssetX(), 500));
    BOOST_REQUIRE(ledger.Reserve(ALICE, AssetX(), 200));
    const auto receipt{ledger.RequestWithdrawal(BOB, AssetX(), 100, DEST)};
    BOOST_REQUIRE(receipt.has_value());

    // Byte-exact reconstruction of the v2 preimage. Balances iterate in
    // (account, asset) map order: ALICE (…a1) before BOB (…b2).
    const auto reconstruct{[&](const bool framed) {
        HashWriter h;
        h << std::string{"b3/flowmesh/state/v2"} << VAULT << uint64_t{0} /*slot*/
          << uint64_t{1} /*next receipt seq*/;
        if (framed) h << uint64_t{2}; // balance count
        h << ALICE << AssetX() << CAmount{800} << CAmount{200};
        h << BOB << AssetX() << CAmount{400} << CAmount{0};
        if (framed) h << uint64_t{1}; // custody count
        h << AssetX() << CAmount{1500};
        if (framed) h << uint64_t{1}; // pending receipt count
        h << *receipt;
        return h.GetHash();
    }};
    BOOST_CHECK_EQUAL(reconstruct(true).GetHex(), ledger.StateRoot().GetHex());
    // The same field bytes WITHOUT the counts hash differently: the
    // collection boundaries genuinely live in the preimage.
    BOOST_CHECK(reconstruct(false) != ledger.StateRoot());
}

BOOST_AUTO_TEST_CASE(adversarial_overflow_and_underflow)
{
    flowmesh::Ledger ledger{VAULT};

    // Deposit near the cap, then a second deposit that would overflow the
    // account balance or custody is rejected and changes nothing.
    BOOST_CHECK(ledger.Deposit(ALICE, AssetX(), MAX_MONEY));
    const uint256 root{ledger.StateRoot()};
    BOOST_CHECK(!ledger.Deposit(ALICE, AssetX(), 1));
    BOOST_CHECK(!ledger.Deposit(BOB, AssetX(), 1)); // custody would overflow
    BOOST_CHECK_EQUAL(ledger.StateRoot().GetHex(), root.GetHex());
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), MAX_MONEY);
    BOOST_CHECK(ledger.SolvencyHolds());

    // Underflow guards: cannot reserve/release/withdraw/consume more than
    // present, and cannot withdraw non-positive.
    BOOST_CHECK(!ledger.Release(ALICE, AssetX(), 1));            // nothing reserved
    BOOST_CHECK(!ledger.Reserve(BOB, AssetX(), 1));              // no balance
    BOOST_CHECK(!ledger.RequestWithdrawal(ALICE, AssetX(), MAX_MONEY + 1, DEST).has_value());
    BOOST_CHECK(!ledger.RequestWithdrawal(ALICE, AssetX(), 0, DEST).has_value());
    BOOST_CHECK(!ledger.RequestWithdrawal(ALICE, AssetX(), -1, DEST).has_value());
    BOOST_CHECK(!ledger.ConsumeRequest(uint256{"00000000000000000000000000000000000000000000000000000000000000ff"}));
    BOOST_CHECK_EQUAL(ledger.StateRoot().GetHex(), root.GetHex());
    BOOST_CHECK(ledger.SolvencyHolds());

    // Rejected deposits of a NEW asset leave every persistent field
    // unchanged (range refusals; the overflow-before-insert bug class
    // itself is pinned by the fresh-balance-key custody-overflow case
    // above, where a rejection genuinely follows the map lookups).
    BOOST_CHECK(!ledger.Deposit(ALICE, AssetY(), MAX_MONEY + 1)); // out of range
    BOOST_CHECK(!ledger.Deposit(ALICE, AssetY(), 0));
    BOOST_CHECK(!ledger.Deposit(ALICE, AssetY(), -1));
    BOOST_CHECK_EQUAL(ledger.StateRoot().GetHex(), root.GetHex());
    BOOST_CHECK_EQUAL(ledger.Custody(AssetY()), 0);
    BOOST_CHECK(ledger.SolvencyHolds());

    // A maximal withdrawal then consumption drains to exactly zero without
    // wrap-around.
    const auto receipt{ledger.RequestWithdrawal(ALICE, AssetX(), MAX_MONEY, DEST)};
    BOOST_REQUIRE(receipt.has_value());
    BOOST_CHECK(ledger.ConsumeRequest(receipt->receipt_id));
    BOOST_CHECK_EQUAL(ledger.Custody(AssetX()), 0);
    BOOST_CHECK_EQUAL(ledger.Liabilities(AssetX()), 0);
    BOOST_CHECK(ledger.SolvencyHolds());
}

BOOST_AUTO_TEST_SUITE_END()

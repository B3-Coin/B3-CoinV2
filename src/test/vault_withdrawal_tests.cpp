// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! DEX_VAULT withdrawal policy: keyless custody, receipt-authorized
//! spends, forced change, batching, sharding, once-only consumption and
//! the impossibility of redirection. Custody only — no matching.

#include <modern/vault.h>
#include <test/util/asset.h>

#include <modern/asset.h>
#include <modern/policy.h>
#include <modern/proof.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

BOOST_AUTO_TEST_SUITE(vault_withdrawal_tests)

namespace {

constexpr int SYNTHETIC_H{1000};
constexpr int MODERN_HEIGHT{SYNTHETIC_H + 1};

Consensus::Params B3Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    return params;
}

// The asset policy set (BURN / DEX_VAULT / conservation) activated for a test,
// via the per-instance Params field that replaces the former global switch.
Consensus::Params B3ParamsActive()
{
    Consensus::Params params{B3Params()};
    params.test_only_asset_policies_active = true;
    return params;
}

//! Bounded mock of FlowMesh finalized-receipt state, with consumption.
class MockReceipts final : public modern::FinalizedReceiptView
{
public:
    std::map<uint256, modern::WithdrawalReceipt> m_finalized;
    std::set<uint256> m_consumed;

    std::optional<modern::WithdrawalReceipt> GetFinalized(const uint256& id) const override
    {
        if (m_consumed.count(id)) return std::nullopt;
        const auto it{m_finalized.find(id)};
        if (it == m_finalized.end()) return std::nullopt;
        return it->second;
    }

    void Consume(const std::vector<uint256>& ids)
    {
        for (const uint256& id : ids) m_consumed.insert(id);
    }
};

const uint256 VAULT{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
const uint256 DEST_A{uint256{"00000000000000000000000000000000000000000000000000000000000000d1"}};
const uint256 DEST_B{uint256{"00000000000000000000000000000000000000000000000000000000000000d2"}};

modern::AssetId Asset()
{
    return modern::test_only::SyntheticAssetId(COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000b3"}), 0});
}

modern::ModernOutput VaultOut(const modern::AssetId& asset, const CAmount amount,
                              const uint16_t shard = 0, const uint256& commitment = VAULT)
{
    modern::ModernOutput out;
    out.asset = asset;
    out.amount = amount;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::DEX_VAULT);
    out.policy_version = modern::POLICY_VERSION_V1;
    out.policy_commitment = commitment;
    out.policy_params = {static_cast<unsigned char>(shard & 0xff),
                         static_cast<unsigned char>(shard >> 8)};
    return out;
}

modern::ModernOutput OwnerOut(const modern::AssetId& asset, const CAmount amount,
                              const uint256& commitment)
{
    modern::ModernOutput out;
    out.asset = asset;
    out.amount = amount;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
    out.policy_version = modern::POLICY_VERSION_V1;
    out.policy_commitment = commitment;
    return out;
}

modern::WithdrawalReceipt Receipt(const uint256& id, const modern::AssetId& asset,
                                  const CAmount amount, const uint256& destination)
{
    modern::WithdrawalReceipt receipt;
    receipt.receipt_id = id;
    receipt.asset = asset;
    receipt.amount = amount;
    receipt.destination = destination;
    receipt.finalized_slot = 42;
    receipt.vault_commitment = VAULT;
    return receipt;
}

modern::TransitionProof VaultProof(std::vector<uint256> ids)
{
    std::sort(ids.begin(), ids.end());
    modern::TransitionProof proof;
    proof.proof_type = static_cast<uint16_t>(modern::PolicyType::DEX_VAULT);
    proof.proof_version = modern::POLICY_VERSION_V1;
    VectorWriter writer{proof.payload, 0};
    writer << ids;
    return proof;
}

//! A withdrawal transition over `vault_prevs`, claiming `ids` on every
//! vault input.
modern::ModernTransition Withdrawal(const std::vector<modern::ModernOutput>& prevs,
                                    const std::vector<uint256>& ids,
                                    std::vector<modern::ModernOutput> outputs)
{
    modern::ModernTransition t;
    t.inputs.resize(prevs.size());
    for (size_t i{0}; i < prevs.size(); ++i) {
        t.inputs[i].prevout = COutPoint{
            Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000e0"}),
            static_cast<uint32_t>(i)};
        t.inputs[i].proof_index = static_cast<uint32_t>(i);
        if (prevs[i].policy_type == static_cast<uint16_t>(modern::PolicyType::DEX_VAULT)) {
            t.proofs.push_back(VaultProof(ids));
        } else {
            modern::TransitionProof owner;
            owner.proof_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
            owner.proof_version = modern::POLICY_VERSION_V1;
            owner.payload = {0x01};
            t.proofs.push_back(owner);
        }
    }
    t.outputs = std::move(outputs);
    return t;
}

const uint256 ID1{uint256{"0000000000000000000000000000000000000000000000000000000000000101"}};
const uint256 ID2{uint256{"0000000000000000000000000000000000000000000000000000000000000102"}};

} // namespace

BOOST_AUTO_TEST_CASE(keyless_receipt_authorized_partial_withdrawal)
{
    const Consensus::Params params{B3ParamsActive()};
    MockReceipts receipts;
    receipts.m_finalized[ID1] = Receipt(ID1, Asset(), 400, DEST_A);

    const std::vector<modern::ModernOutput> prevs{VaultOut(Asset(), 1000)};
    const modern::ModernTransition t{Withdrawal(
        prevs, {ID1}, {OwnerOut(Asset(), 400, DEST_A), VaultOut(Asset(), 600, /*shard=*/1)})};

    // The vault spend carries no key material: its proof is only the
    // receipt list, structurally valid under the generic dispatcher.
    BOOST_CHECK(modern::VerifyTransitionProofs(prevs, t, /*assets_active=*/true) == modern::ProofCheck::OK);

    std::vector<uint256> consumed;
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params,
                                             &consumed) == modern::VaultCheck::OK);
    BOOST_CHECK_EQUAL(consumed.size(), 1U);
    BOOST_CHECK(consumed[0] == ID1);

    // Anyone may relay: the authorized transition is fully determined, so
    // an independent relayer builds the identical bytes.
    const modern::ModernTransition rebuilt{Withdrawal(
        prevs, {ID1}, {OwnerOut(Asset(), 400, DEST_A), VaultOut(Asset(), 600, /*shard=*/1)})};
    BOOST_CHECK_EQUAL(modern::FullTransitionId(t).GetHex(),
                      modern::FullTransitionId(rebuilt).GetHex());
}

BOOST_AUTO_TEST_CASE(remainder_must_return_to_the_approved_vault)
{
    const Consensus::Params params{B3ParamsActive()};
    MockReceipts receipts;
    receipts.m_finalized[ID1] = Receipt(ID1, Asset(), 400, DEST_A);
    const std::vector<modern::ModernOutput> prevs{VaultOut(Asset(), 1000)};

    // Short change: custody value vanished.
    BOOST_CHECK(modern::CheckVaultWithdrawal(
                    prevs,
                    Withdrawal(prevs, {ID1},
                               {OwnerOut(Asset(), 400, DEST_A), VaultOut(Asset(), 599),
                                OwnerOut(Asset(), 1, DEST_B)}),
                    receipts, MODERN_HEIGHT, params) == modern::VaultCheck::CHANGE_MISMATCH);

    // Change escaping to an OWNER output instead of the vault.
    BOOST_CHECK(modern::CheckVaultWithdrawal(
                    prevs,
                    Withdrawal(prevs, {ID1},
                               {OwnerOut(Asset(), 400, DEST_A), OwnerOut(Asset(), 600, DEST_B)}),
                    receipts, MODERN_HEIGHT, params) == modern::VaultCheck::CHANGE_MISMATCH);

    // Change redirected to a different vault commitment.
    const uint256 other_vault{
        uint256{"00000000000000000000000000000000000000000000000000000000000000f2"}};
    BOOST_CHECK(modern::CheckVaultWithdrawal(
                    prevs,
                    Withdrawal(prevs, {ID1},
                               {OwnerOut(Asset(), 400, DEST_A),
                                VaultOut(Asset(), 600, 0, other_vault)}),
                    receipts, MODERN_HEIGHT, params) == modern::VaultCheck::CHANGE_MISMATCH);
}

BOOST_AUTO_TEST_CASE(redirection_is_impossible)
{
    const Consensus::Params params{B3ParamsActive()};
    MockReceipts receipts;
    receipts.m_finalized[ID1] = Receipt(ID1, Asset(), 400, DEST_A);
    const std::vector<modern::ModernOutput> prevs{VaultOut(Asset(), 1000)};

    // Paying the wrong destination.
    BOOST_CHECK(modern::CheckVaultWithdrawal(
                    prevs,
                    Withdrawal(prevs, {ID1},
                               {OwnerOut(Asset(), 400, DEST_B), VaultOut(Asset(), 600)}),
                    receipts, MODERN_HEIGHT, params) == modern::VaultCheck::DESTINATION_MISMATCH);

    // Underpaying the committed destination.
    BOOST_CHECK(modern::CheckVaultWithdrawal(
                    prevs,
                    Withdrawal(prevs, {ID1},
                               {OwnerOut(Asset(), 399, DEST_A), OwnerOut(Asset(), 1, DEST_B),
                                VaultOut(Asset(), 600)}),
                    receipts, MODERN_HEIGHT, params) == modern::VaultCheck::DESTINATION_MISMATCH);
}

BOOST_AUTO_TEST_CASE(receipts_are_finalized_and_consumed_once)
{
    const Consensus::Params params{B3ParamsActive()};
    MockReceipts receipts;
    const std::vector<modern::ModernOutput> prevs{VaultOut(Asset(), 1000)};
    const modern::ModernTransition t{Withdrawal(
        prevs, {ID1}, {OwnerOut(Asset(), 400, DEST_A), VaultOut(Asset(), 600)})};

    // Unknown (never finalized) receipt.
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params) ==
                modern::VaultCheck::RECEIPT_UNKNOWN);

    // Finalized: authorizes exactly once.
    receipts.m_finalized[ID1] = Receipt(ID1, Asset(), 400, DEST_A);
    std::vector<uint256> consumed;
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params,
                                             &consumed) == modern::VaultCheck::OK);
    receipts.Consume(consumed);
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params) ==
                modern::VaultCheck::RECEIPT_UNKNOWN);

    // A receipt finalized for a different vault cannot spend this one.
    receipts.m_consumed.clear();
    receipts.m_finalized[ID1].vault_commitment =
        uint256{"00000000000000000000000000000000000000000000000000000000000000f2"};
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params) ==
                modern::VaultCheck::RECEIPT_WRONG_VAULT);
}

BOOST_AUTO_TEST_CASE(batched_withdrawals_across_shards)
{
    const Consensus::Params params{B3ParamsActive()};
    MockReceipts receipts;
    receipts.m_finalized[ID1] = Receipt(ID1, Asset(), 400, DEST_A);
    receipts.m_finalized[ID2] = Receipt(ID2, modern::NativeAsset(), 50, DEST_B);

    // Two shards of the same approved vault fund two receipts in one
    // transition; remainder returns split across shards.
    const std::vector<modern::ModernOutput> prevs{VaultOut(Asset(), 1000, /*shard=*/0),
                                                  VaultOut(modern::NativeAsset(), 80, /*shard=*/1)};
    const modern::ModernTransition t{Withdrawal(
        prevs, {ID1, ID2},
        {OwnerOut(Asset(), 400, DEST_A), OwnerOut(modern::NativeAsset(), 50, DEST_B),
         VaultOut(Asset(), 350, /*shard=*/2), VaultOut(Asset(), 250, /*shard=*/3),
         VaultOut(modern::NativeAsset(), 30, /*shard=*/1)})};

    std::vector<uint256> consumed;
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params,
                                             &consumed) == modern::VaultCheck::OK);
    BOOST_CHECK_EQUAL(consumed.size(), 2U);

    // Vault inputs claiming different receipt lists are malformed.
    modern::ModernTransition skewed{t};
    skewed.proofs[1] = VaultProof({ID1});
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, skewed, receipts, MODERN_HEIGHT, params) ==
                modern::VaultCheck::BAD_PROOF);

    // Duplicate ids cannot be encoded: the canonical list is strictly
    // ascending, so a duplicated claim is malformed at the proof layer.
    modern::TransitionProof duplicated{VaultProof({ID1})};
    {
        VectorWriter writer{duplicated.payload, 0};
        writer << std::vector<uint256>{ID1, ID1};
    }
    BOOST_CHECK(!modern::ParseVaultReceiptIds(duplicated.payload));
}

BOOST_AUTO_TEST_CASE(vault_is_fail_closed_and_burn_is_unspendable)
{
    const Consensus::Params params{B3Params()};
    MockReceipts receipts;
    const std::vector<modern::ModernOutput> prevs{VaultOut(Asset(), 1000)};
    const modern::ModernTransition t{Withdrawal(
        prevs, {ID1}, {OwnerOut(Asset(), 400, DEST_A), VaultOut(Asset(), 600)})};

    // Inactive by default: DEX_VAULT is part of the test-only asset set.
    BOOST_CHECK(modern::CheckVaultWithdrawal(prevs, t, receipts, MODERN_HEIGHT, params) ==
                modern::VaultCheck::NOT_ACTIVE);
    BOOST_CHECK(!modern::IsActivatedPolicy(
        static_cast<uint16_t>(modern::PolicyType::DEX_VAULT), modern::POLICY_VERSION_V1));

    // With the set active, a burned coin still can never be spent.
    const Consensus::Params active_params{B3ParamsActive()};
    modern::ModernOutput burned;
    burned.asset = Asset();
    burned.amount = 5;
    burned.policy_type = static_cast<uint16_t>(modern::PolicyType::BURN);
    burned.policy_version = modern::POLICY_VERSION_V1;
    modern::TransitionProof any;
    any.proof_type = static_cast<uint16_t>(modern::PolicyType::BURN);
    any.proof_version = modern::POLICY_VERSION_V1;
    any.payload = {0x00};
    BOOST_CHECK(modern::VerifyTransitionProof(burned, any, /*assets_active=*/true) == modern::ProofCheck::UNSPENDABLE);

    // A withdrawal with no vault input is not a vault spend.
    const std::vector<modern::ModernOutput> owner_prevs{OwnerOut(Asset(), 10, DEST_B)};
    BOOST_CHECK(modern::CheckVaultWithdrawal(
                    owner_prevs, Withdrawal(owner_prevs, {ID1}, {OwnerOut(Asset(), 10, DEST_B)}),
                    receipts, MODERN_HEIGHT, active_params) == modern::VaultCheck::NO_VAULT_INPUT);
}

BOOST_AUTO_TEST_SUITE_END()

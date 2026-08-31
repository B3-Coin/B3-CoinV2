// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/era.h>
#include <consensus/params.h>
#include <flowmesh/market.h>
#include <modern/flowmesh_checkpoint.h>
#include <modern/flowmesh_vault_proof.h>
#include <modern/mpa.h>
#include <modern/payload_cost.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr int H{100};
constexpr int A1{120};
constexpr int A2{130};
constexpr int A3{A2 + Consensus::FLOWMESH_ANCHOR_DEPTH};

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

Consensus::Params Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = H + 1;
    params.transition_pow_length = 10;
    params.legacy_final_hash = Filled(0x01);
    params.modern_pos.emplace();
    params.fn_genesis_rights_root = Filled(0x02);
    Consensus::FnGenesisRight right;
    right.pod_id = Filled(0x03);
    right.recipient_key_hash.fill(0x04);
    params.fn_genesis_manifest.push_back(right);
    params.fn_pod_activation_height = A1;
    params.asset_activation_height = A2;
    params.flowmesh_activation_height = A3;
    return params;
}

struct Ids {
    uint256 domain{Filled(0x11)};
    modern::AssetId asset{Filled(0x12)};
    flowmesh::MarketId market;
    flowmesh::VaultId vault;

    Ids()
    {
        const auto m{flowmesh::ComputeFlowMeshMarketId(domain, asset)};
        BOOST_REQUIRE(m.has_value());
        market = *m;
        const auto v{flowmesh::ComputeFlowMeshVaultId(domain, market)};
        BOOST_REQUIRE(v.has_value());
        vault = *v;
    }
};

modern::FlowMeshDepositAcceptanceV1 Deposit(const Ids& ids,
                                            const unsigned char salt = 1,
                                            const uint64_t sequence = 7)
{
    modern::FlowMeshDepositAcceptanceV1 out;
    out.acceptance_id = Filled(static_cast<unsigned char>(0x20 + salt));
    out.market_id = ids.market;
    out.epoch = 3;
    out.sequence = sequence;
    out.deposit_outpoint = COutPoint{
        Txid::FromUint256(Filled(static_cast<unsigned char>(0x30 + salt))),
        static_cast<uint32_t>(salt)};
    out.account = Filled(static_cast<unsigned char>(0x40 + salt));
    out.asset = ids.asset;
    out.amount = 1000 + salt;
    out.vault_id = ids.vault;
    out.shard = salt;
    return out;
}

modern::FlowMeshWithdrawalReceiptV1 Withdrawal(const Ids& ids,
                                                const unsigned char salt = 1,
                                                const uint64_t sequence = 8)
{
    modern::FlowMeshWithdrawalReceiptV1 out;
    out.receipt_id = Filled(static_cast<unsigned char>(0x50 + salt));
    out.market_id = ids.market;
    out.epoch = 3;
    out.sequence = sequence;
    out.account = Filled(static_cast<unsigned char>(0x60 + salt));
    out.asset = ids.asset;
    out.amount = 500 + salt;
    out.destination_owner_commitment =
        Filled(static_cast<unsigned char>(0x70 + salt));
    out.vault_id = ids.vault;
    out.deterministic_change_shard = static_cast<uint16_t>(200 + salt);
    return out;
}

std::vector<modern::FlowMeshEffectV1> Effects(const Ids& ids)
{
    return {Deposit(ids, 1, 7), Withdrawal(ids, 2, 8), Deposit(ids, 3, 9)};
}

void BindProductionIdentity(modern::FlowMeshCheckpointCoreV1& core)
{
    const auto identity{modern::FlowMeshCheckpointProductionIdentityV1(core)};
    BOOST_REQUIRE(identity.has_value());
    core.microblock_hash = *identity;
}

modern::FlowMeshCheckpointCoreV1 ExecutionCore(
    const Ids& ids, const std::span<const modern::FlowMeshEffectV1> effects,
    const uint64_t effect_start = 44)
{
    const auto root{modern::ComputeFlowMeshEffectRoot(effect_start, effects)};
    BOOST_REQUIRE(root.has_value());
    modern::FlowMeshCheckpointCoreV1 core;
    core.domain = ids.domain;
    core.market_id = ids.market;
    core.epoch = 3;
    core.sequence = 9;
    core.previous_checkpoint_id = Filled(0x82);
    core.anchor = {500, Filled(0x83)};
    core.seat_set_hash = Filled(0x84);
    core.production_anchor = {510, Filled(0x86)};
    core.parent_hash = Filled(0x87);
    core.previous_state_root = Filled(0x88);
    core.actions_root = Filled(0x89);
    core.result_root = Filled(0x8a);
    core.state_root = Filled(0x85);
    core.effect_start = effect_start;
    core.effect_count = static_cast<uint32_t>(effects.size());
    core.effect_root = *root;
    BindProductionIdentity(core);
    return core;
}

modern::FlowMeshCheckpointRecordV1 Record(
    const modern::FlowMeshCheckpointCoreV1& core, const unsigned char bitmap = 0x07)
{
    modern::FlowMeshCheckpointRecordV1 out;
    out.core = core;
    out.certificate.seat_epoch = core.epoch;
    out.certificate.sequence = core.sequence;
    out.certificate.microblock_hash = core.microblock_hash;
    out.certificate.signer_bitmap = {bitmap};
    out.certificate.aggregate_signature.fill(0x91);
    return out;
}

CMpaRecord MpaRecord(const uint16_t type, std::vector<unsigned char> payload)
{
    return CMpaRecord{type, modern::MPA_VERSION_V1, std::move(payload)};
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_checkpoint_codec_tests)

BOOST_AUTO_TEST_CASE(typed_effect_codecs_are_exact_canonical_and_bounded)
{
    const Ids ids;
    const modern::FlowMeshEffectV1 deposit{Deposit(ids)};
    const modern::FlowMeshEffectV1 withdrawal{Withdrawal(ids)};
    const auto deposit_bytes{modern::EncodeFlowMeshEffectV1(deposit)};
    const auto withdrawal_bytes{modern::EncodeFlowMeshEffectV1(withdrawal)};
    BOOST_REQUIRE(deposit_bytes.has_value());
    BOOST_REQUIRE(withdrawal_bytes.has_value());
    BOOST_CHECK_EQUAL(deposit_bytes->size(), 223U);
    BOOST_CHECK_EQUAL(withdrawal_bytes->size(), 219U);
    BOOST_CHECK_EQUAL((*deposit_bytes)[0], 1);
    BOOST_CHECK_EQUAL((*withdrawal_bytes)[0], 2);
    // Epoch and sequence are fixed-width big-endian after kind + two ids.
    BOOST_CHECK_EQUAL((*deposit_bytes)[65], 0x00);
    BOOST_CHECK_EQUAL((*deposit_bytes)[72], 0x03);
    BOOST_CHECK_EQUAL((*deposit_bytes)[80], 0x07);
    BOOST_CHECK(modern::DecodeFlowMeshEffectV1(*deposit_bytes) == deposit);
    BOOST_CHECK(modern::DecodeFlowMeshEffectV1(*withdrawal_bytes) == withdrawal);

    for (const size_t bad_size : {size_t{0}, size_t{222}, size_t{224}}) {
        auto malformed{*deposit_bytes};
        malformed.resize(bad_size);
        BOOST_CHECK(!modern::DecodeFlowMeshEffectV1(malformed));
    }
    auto bad_kind{*deposit_bytes};
    bad_kind[0] = 0xff;
    BOOST_CHECK(!modern::DecodeFlowMeshEffectV1(bad_kind));
    auto trailing{*withdrawal_bytes};
    trailing.push_back(0);
    BOOST_CHECK(!modern::DecodeFlowMeshEffectV1(trailing));

    auto bad_deposit{Deposit(ids)};
    bad_deposit.account.SetNull();
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_deposit}));
    bad_deposit = Deposit(ids);
    bad_deposit.amount = 0;
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_deposit}));
    bad_deposit = Deposit(ids);
    bad_deposit.shard = 256;
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_deposit}));
    bad_deposit = Deposit(ids);
    bad_deposit.deposit_outpoint.n = COutPoint::NULL_INDEX;
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_deposit}));

    auto bad_receipt{Withdrawal(ids)};
    bad_receipt.account.SetNull();
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_receipt}));
    bad_receipt = Withdrawal(ids);
    bad_receipt.amount = MAX_MONEY + 1;
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_receipt}));
    bad_receipt = Withdrawal(ids);
    bad_receipt.destination_owner_commitment.SetNull();
    BOOST_CHECK(!modern::EncodeFlowMeshEffectV1(
        modern::FlowMeshEffectV1{bad_receipt}));
}

BOOST_AUTO_TEST_CASE(padded_merkle_root_and_branches_commit_start_count_and_position)
{
    const Ids ids;
    const std::vector<modern::FlowMeshEffectV1> effects{Effects(ids)};
    constexpr uint64_t start{44};
    const auto root{modern::ComputeFlowMeshEffectRoot(start, effects)};
    BOOST_REQUIRE(root.has_value());
    BOOST_CHECK_EQUAL(root->GetHex(),
                      "ef561afbd0268e485a7224a67b6820be3caa462407382e63666ce1f948af8f54");
    BOOST_CHECK_EQUAL(modern::FlowMeshEffectTreeDepth(0), 0U);
    BOOST_CHECK_EQUAL(modern::FlowMeshEffectTreeDepth(1), 0U);
    BOOST_CHECK_EQUAL(modern::FlowMeshEffectTreeDepth(3), 2U);
    BOOST_CHECK_EQUAL(modern::FlowMeshEffectTreeDepth(4096), 12U);
    BOOST_CHECK(modern::ComputeFlowMeshEffectRoot(start, {}) ==
                modern::EmptyFlowMeshEffectRoot(start));

    for (uint32_t i{0}; i < effects.size(); ++i) {
        const auto branch{modern::BuildFlowMeshEffectBranch(start, effects, i)};
        BOOST_REQUIRE(branch.has_value());
        BOOST_CHECK_EQUAL(branch->size(), 2U);
        BOOST_CHECK(modern::VerifyFlowMeshEffectInclusion(
            start, effects.size(), *root, effects[i], i, *branch));
        BOOST_CHECK(!modern::VerifyFlowMeshEffectInclusion(
            start + 1, effects.size(), *root, effects[i], i, *branch));
    }

    const auto branch{modern::BuildFlowMeshEffectBranch(start, effects, 1)};
    BOOST_REQUIRE(branch.has_value());
    auto wrong_branch{*branch};
    wrong_branch[0] = Filled(0xee);
    BOOST_CHECK(!modern::VerifyFlowMeshEffectInclusion(
        start, effects.size(), *root, effects[1], 1, wrong_branch));
    BOOST_CHECK(!modern::VerifyFlowMeshEffectInclusion(
        start, effects.size(), *root, effects[1], 0, *branch));
    BOOST_CHECK(!modern::VerifyFlowMeshEffectInclusion(
        start, effects.size() - 1, *root, effects[1], 1, *branch));

    auto four{effects};
    four.push_back(effects.back());
    const auto four_root{modern::ComputeFlowMeshEffectRoot(start, four)};
    BOOST_REQUIRE(four_root.has_value());
    BOOST_CHECK(*four_root != *root); // pad is not duplicate-last, count is bound

    std::vector<modern::FlowMeshEffectV1> maximum(4096, effects.front());
    const auto max_root{modern::ComputeFlowMeshEffectRoot(1000, maximum)};
    const auto max_branch{modern::BuildFlowMeshEffectBranch(1000, maximum, 4095)};
    BOOST_REQUIRE(max_root.has_value());
    BOOST_REQUIRE(max_branch.has_value());
    BOOST_CHECK_EQUAL(max_branch->size(), 12U);
    BOOST_CHECK(modern::VerifyFlowMeshEffectInclusion(
        1000, 4096, *max_root, maximum.back(), 4095, *max_branch));
    maximum.push_back(effects.front());
    BOOST_CHECK(!modern::ComputeFlowMeshEffectRoot(1000, maximum));
    BOOST_CHECK(!modern::ComputeFlowMeshEffectRoot(
        std::numeric_limits<uint64_t>::max() - 1, effects));
}

BOOST_AUTO_TEST_CASE(checkpoint_core_and_certificate_framing_are_exact)
{
    const Ids ids;
    const auto effects{Effects(ids)};
    const auto execution{ExecutionCore(ids, effects)};
    const auto execution_bytes{modern::EncodeFlowMeshCheckpointCoreV1(execution)};
    BOOST_REQUIRE(execution_bytes.has_value());
    BOOST_CHECK_EQUAL(execution_bytes->size(), 463U);
    BOOST_CHECK_EQUAL((*execution_bytes)[0], 0x00);
    BOOST_CHECK_EQUAL((*execution_bytes)[1], 0x01);
    BOOST_CHECK_EQUAL((*execution_bytes)[2], 0x01);
    BOOST_CHECK(modern::DecodeFlowMeshCheckpointCoreV1(*execution_bytes) == execution);
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(execution)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    BOOST_CHECK_EQUAL(checkpoint_id->GetHex(),
                      "26eb4df9b3195395d3e757bd37f48e2e7930cc7989134d5b19491d1230afd57a");

    auto handoff{ExecutionCore(ids, {})};
    handoff.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    handoff.effect_count = 0;
    handoff.effect_root = modern::EmptyFlowMeshEffectRoot(handoff.effect_start);
    handoff.handoff = modern::FlowMeshCheckpointHandoffV1{
        4, {531, Filled(0x92)}, Filled(0x93)};
    BindProductionIdentity(handoff);
    const auto handoff_bytes{modern::EncodeFlowMeshCheckpointCoreV1(handoff)};
    BOOST_REQUIRE(handoff_bytes.has_value());
    BOOST_CHECK_EQUAL(handoff_bytes->size(), 543U);
    BOOST_CHECK_EQUAL((*handoff_bytes)[2], 0x02);
    BOOST_CHECK(modern::DecodeFlowMeshCheckpointCoreV1(*handoff_bytes) == handoff);

    const auto record{Record(execution)};
    const auto bls_context{modern::FlowMeshCheckpointBlsContextV1(execution)};
    BOOST_CHECK(bls_context.domain == execution.domain);
    BOOST_CHECK(bls_context.market_id == execution.market_id);
    BOOST_CHECK_EQUAL(bls_context.seat_epoch, execution.epoch);
    BOOST_CHECK(bls_context.seat_set_hash == execution.seat_set_hash);
    BOOST_CHECK_EQUAL(bls_context.sequence, execution.sequence);
    BOOST_CHECK(bls_context.microblock_hash == execution.microblock_hash);
    const auto encoded{modern::EncodeFlowMeshCheckpointRecordV1(record, 4)};
    BOOST_REQUIRE(encoded.has_value());
    BOOST_CHECK_EQUAL(encoded->size(), 560U);
    BOOST_CHECK(modern::DecodeFlowMeshCheckpointEnvelopeV1(*encoded) == record);
    BOOST_CHECK(modern::DecodeFlowMeshCheckpointRecordV1(*encoded, 4) == record);
    BOOST_CHECK(!modern::DecodeFlowMeshCheckpointRecordV1(*encoded, 9));

    auto bad_context{record};
    ++bad_context.certificate.sequence;
    BOOST_CHECK(!modern::EncodeFlowMeshCheckpointRecordV1(bad_context, 4));
    auto bad_high_bits{*encoded};
    bad_high_bits[execution_bytes->size()] = 0x87;
    BOOST_CHECK(modern::DecodeFlowMeshCheckpointEnvelopeV1(bad_high_bits));
    BOOST_CHECK(!modern::DecodeFlowMeshCheckpointRecordV1(bad_high_bits, 4));
    auto trailing{*encoded};
    trailing.push_back(0);
    BOOST_CHECK(!modern::DecodeFlowMeshCheckpointRecordV1(trailing, 4));

    auto malformed_core{*execution_bytes};
    malformed_core[0] = 0x01;
    BOOST_CHECK(!modern::DecodeFlowMeshCheckpointCoreV1(malformed_core));
    malformed_core = *execution_bytes;
    malformed_core[2] = 0xff;
    BOOST_CHECK(!modern::DecodeFlowMeshCheckpointCoreV1(malformed_core));
    malformed_core = *execution_bytes;
    malformed_core.push_back(0);
    BOOST_CHECK(!modern::DecodeFlowMeshCheckpointCoreV1(malformed_core));

    auto bad_handoff{handoff};
    bad_handoff.handoff->next_epoch = 9;
    BOOST_CHECK(!modern::EncodeFlowMeshCheckpointCoreV1(bad_handoff));
    bad_handoff = handoff;
    bad_handoff.handoff->next_anchor.height = handoff.anchor.height;
    BOOST_CHECK(!modern::EncodeFlowMeshCheckpointCoreV1(bad_handoff));
    bad_handoff = handoff;
    bad_handoff.effect_count = 1;
    bad_handoff.effect_root = Filled(0xa0);
    BOOST_CHECK(!modern::EncodeFlowMeshCheckpointCoreV1(bad_handoff));

    auto max_record{Record(handoff)};
    max_record.certificate.signer_bitmap.assign(625, 0);
    max_record.certificate.signer_bitmap.back() = 0x80;
    const auto maximum{
        modern::EncodeFlowMeshCheckpointRecordV1(max_record, 5000)};
    BOOST_REQUIRE(maximum.has_value());
    BOOST_CHECK_EQUAL(maximum->size(), 1264U);
    BOOST_CHECK(modern::DecodeFlowMeshCheckpointRecordV1(*maximum, 5000) ==
                max_record);
}

BOOST_AUTO_TEST_CASE(production_identity_binds_every_committee_authored_field)
{
    const Ids ids;
    const auto effects{Effects(ids)};
    const auto execution{ExecutionCore(ids, effects)};
    const auto original{
        modern::FlowMeshCheckpointProductionIdentityV1(execution)};
    BOOST_REQUIRE(original.has_value());

    const auto expect_changed = [&](auto mutate) {
        auto changed{execution};
        mutate(changed);
        const auto identity{
            modern::FlowMeshCheckpointProductionIdentityV1(changed)};
        BOOST_REQUIRE(identity.has_value());
        BOOST_CHECK(*identity != *original);
        // Retaining the old signed digest makes the core non-canonical.
        BOOST_CHECK(!modern::EncodeFlowMeshCheckpointCoreV1(changed));
    };

    expect_changed([](auto& core) { core.domain = Filled(0xa1); });
    expect_changed([](auto& core) { core.market_id = Filled(0xa2); });
    expect_changed([](auto& core) { ++core.epoch; });
    expect_changed([](auto& core) { core.seat_set_hash = Filled(0xa3); });
    expect_changed([](auto& core) { ++core.sequence; });
    expect_changed([](auto& core) { core.parent_hash = Filled(0xa4); });
    expect_changed([](auto& core) { ++core.production_anchor.height; });
    expect_changed([](auto& core) {
        core.production_anchor.block_hash = Filled(0xa5);
    });
    expect_changed([](auto& core) {
        core.previous_state_root = Filled(0xa6);
    });
    expect_changed([](auto& core) { core.actions_root = Filled(0xa7); });
    expect_changed([](auto& core) { core.result_root = Filled(0xa8); });
    expect_changed([](auto& core) { core.state_root = Filled(0xa9); });
    expect_changed([](auto& core) { ++core.effect_start; });
    expect_changed([](auto& core) { ++core.effect_count; });
    expect_changed([](auto& core) { core.effect_root = Filled(0xaa); });

    auto handoff{ExecutionCore(ids, {})};
    handoff.kind = modern::FlowMeshCheckpointKind::EPOCH_HANDOFF;
    handoff.handoff = modern::FlowMeshCheckpointHandoffV1{
        handoff.epoch + 1, {531, Filled(0xab)}, Filled(0xac)};
    BindProductionIdentity(handoff);
    const auto handoff_identity{
        modern::FlowMeshCheckpointProductionIdentityV1(handoff)};
    BOOST_REQUIRE(handoff_identity.has_value());
    for (const int field : {0, 1, 2, 3}) {
        auto changed{handoff};
        if (field == 0) ++changed.handoff->next_epoch;
        if (field == 1) ++changed.handoff->next_anchor.height;
        if (field == 2) changed.handoff->next_anchor.block_hash = Filled(0xad);
        if (field == 3) changed.handoff->next_seat_set_hash = Filled(0xae);
        const auto identity{
            modern::FlowMeshCheckpointProductionIdentityV1(changed)};
        if (field == 0) {
            // A non-consecutive epoch is rejected before hashing.
            BOOST_CHECK(!identity);
        } else {
            BOOST_REQUIRE(identity.has_value());
            BOOST_CHECK(*identity != *handoff_identity);
        }
    }

    // The B3 publication link is intentionally outside the committee's
    // one-round production signature, but remains inside CheckpointId.
    auto relinked{execution};
    relinked.previous_checkpoint_id = Filled(0xaf);
    BOOST_CHECK(modern::FlowMeshCheckpointProductionIdentityV1(relinked) ==
                original);
    BOOST_CHECK(modern::FlowMeshCheckpointIdV1(relinked) !=
                modern::FlowMeshCheckpointIdV1(execution));
}

BOOST_AUTO_TEST_CASE(vault_proof_is_one_effect_and_verifies_checkpoint_context)
{
    const Ids ids;
    const auto effects{Effects(ids)};
    const auto core{ExecutionCore(ids, effects)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());

    modern::FlowMeshVaultProofV1 sweep;
    sweep.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    sweep.checkpoint_id = *checkpoint_id;
    sweep.effect = effects[0];
    sweep.leaf_index = 0;
    sweep.branch = *modern::BuildFlowMeshEffectBranch(
        core.effect_start, effects, sweep.leaf_index);
    const auto sweep_bytes{modern::EncodeFlowMeshVaultProofV1(sweep)};
    BOOST_REQUIRE(sweep_bytes.has_value());
    BOOST_CHECK_EQUAL(sweep_bytes->size(), 325U);
    BOOST_CHECK(modern::DecodeFlowMeshVaultProofV1(*sweep_bytes) == sweep);
    BOOST_CHECK(modern::VerifyFlowMeshVaultProofV1(sweep, core));

    auto maximum_shape{sweep};
    maximum_shape.branch.assign(modern::FLOWMESH_MAX_EFFECT_BRANCH_DEPTH,
                                Filled(0xb0));
    const auto maximum_bytes{modern::EncodeFlowMeshVaultProofV1(maximum_shape)};
    BOOST_REQUIRE(maximum_bytes.has_value());
    BOOST_CHECK_EQUAL(maximum_bytes->size(), 645U);
    BOOST_CHECK(modern::DecodeFlowMeshVaultProofV1(*maximum_bytes) ==
                maximum_shape);
    maximum_shape.branch.push_back(Filled(0xb1));
    BOOST_CHECK(!modern::EncodeFlowMeshVaultProofV1(maximum_shape));

    modern::FlowMeshVaultProofV1 withdrawal;
    withdrawal.kind = modern::FlowMeshVaultProofKind::WITHDRAWAL;
    withdrawal.checkpoint_id = *checkpoint_id;
    withdrawal.effect = effects[1];
    withdrawal.leaf_index = 1;
    withdrawal.branch = *modern::BuildFlowMeshEffectBranch(
        core.effect_start, effects, withdrawal.leaf_index);
    const auto withdrawal_bytes{
        modern::EncodeFlowMeshVaultProofV1(withdrawal)};
    BOOST_REQUIRE(withdrawal_bytes.has_value());
    BOOST_CHECK_EQUAL(withdrawal_bytes->size(), 321U);
    BOOST_CHECK(modern::DecodeFlowMeshVaultProofV1(*withdrawal_bytes) == withdrawal);
    BOOST_CHECK(modern::VerifyFlowMeshVaultProofV1(withdrawal, core));

    // A one-effect withdrawal has a canonical zero-depth branch and is the
    // smallest valid type-9 record (257 bytes).
    const std::vector<modern::FlowMeshEffectV1> one_effect{effects[1]};
    const auto one_core{ExecutionCore(ids, one_effect)};
    const auto one_id{modern::FlowMeshCheckpointIdV1(one_core)};
    BOOST_REQUIRE(one_id.has_value());
    auto one_withdrawal{withdrawal};
    one_withdrawal.checkpoint_id = *one_id;
    one_withdrawal.leaf_index = 0;
    one_withdrawal.branch.clear();
    const auto one_bytes{modern::EncodeFlowMeshVaultProofV1(one_withdrawal)};
    BOOST_REQUIRE(one_bytes.has_value());
    BOOST_CHECK_EQUAL(one_bytes->size(), 257U);
    BOOST_CHECK(modern::DecodeFlowMeshVaultProofV1(*one_bytes) == one_withdrawal);
    BOOST_CHECK(modern::VerifyFlowMeshVaultProofV1(one_withdrawal, one_core));

    auto wrong{withdrawal};
    wrong.checkpoint_id = Filled(0xa1);
    BOOST_CHECK(!modern::VerifyFlowMeshVaultProofV1(wrong, core));
    wrong = withdrawal;
    std::get<modern::FlowMeshWithdrawalReceiptV1>(wrong.effect)
        .destination_owner_commitment = Filled(0xa2);
    BOOST_CHECK(!modern::VerifyFlowMeshVaultProofV1(wrong, core));
    wrong = withdrawal;
    std::get<modern::FlowMeshWithdrawalReceiptV1>(wrong.effect).vault_id =
        Filled(0xa3);
    BOOST_CHECK(!modern::VerifyFlowMeshVaultProofV1(wrong, core));
    wrong = withdrawal;
    wrong.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    BOOST_CHECK(!modern::EncodeFlowMeshVaultProofV1(wrong));

    auto malformed{*sweep_bytes};
    malformed[0] = 0xff;
    BOOST_CHECK(!modern::DecodeFlowMeshVaultProofV1(malformed));
    malformed = *sweep_bytes;
    malformed.push_back(0);
    BOOST_CHECK(!modern::DecodeFlowMeshVaultProofV1(malformed));
    malformed = *sweep_bytes;
    const size_t branch_count_offset{
        modern::FLOWMESH_VAULT_PROOF_PREFIX_SIZE +
        modern::FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE + 4};
    malformed[branch_count_offset] = 13;
    BOOST_CHECK(!modern::DecodeFlowMeshVaultProofV1(malformed));
    malformed.assign(modern::FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE + 1, 0);
    BOOST_CHECK(!modern::DecodeFlowMeshVaultProofV1(malformed));
    malformed.assign(modern::FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE, 0);
    BOOST_CHECK(!modern::DecodeFlowMeshVaultProofV1(malformed));
}

BOOST_AUTO_TEST_CASE(mpa_type_gates_costs_grammar_and_single_proof_rule)
{
    const Consensus::Params params{Params()};
    const Ids ids;
    const auto effects{Effects(ids)};
    const auto core{ExecutionCore(ids, effects)};
    const auto checkpoint_bytes{
        modern::EncodeFlowMeshCheckpointRecordV1(Record(core), 4)};
    BOOST_REQUIRE(checkpoint_bytes.has_value());
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(core)};
    BOOST_REQUIRE(checkpoint_id.has_value());
    modern::FlowMeshVaultProofV1 proof;
    proof.kind = modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP;
    proof.checkpoint_id = *checkpoint_id;
    proof.effect = effects[0];
    proof.leaf_index = 0;
    proof.branch = *modern::BuildFlowMeshEffectBranch(core.effect_start, effects, 0);
    const auto proof_bytes{modern::EncodeFlowMeshVaultProofV1(proof)};
    BOOST_REQUIRE(proof_bytes.has_value());

    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::MPA_TYPE_FLOWMESH_CHECKPOINT, 1, params, A3 - 1) ==
                modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::MPA_TYPE_FLOWMESH_CHECKPOINT, 1, params, A3) ==
                modern::PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, 1, params, A3) ==
                modern::PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, 2, params, A3) ==
                modern::PayloadTypeStatus::UNKNOWN);
    Consensus::Params incomplete{params};
    incomplete.flowmesh_activation_height.reset();
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::MPA_TYPE_FLOWMESH_CHECKPOINT, 1, incomplete, A3) ==
                modern::PayloadTypeStatus::INACTIVE);
    incomplete = params;
    incomplete.flowmesh_activation_height =
        A2 + Consensus::FLOWMESH_ANCHOR_DEPTH - 1;
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, 1, incomplete, A3) ==
                modern::PayloadTypeStatus::INACTIVE);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(
                          modern::MPA_TYPE_FLOWMESH_CHECKPOINT, 1),
                      6000);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(
                          modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, 1),
                      500);
    BOOST_CHECK(modern::PayloadSizeAllowed(
        modern::MPA_TYPE_FLOWMESH_CHECKPOINT,
        modern::FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE));
    BOOST_CHECK(!modern::PayloadSizeAllowed(
        modern::MPA_TYPE_FLOWMESH_CHECKPOINT,
        modern::FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE + 1));
    BOOST_CHECK(modern::PayloadSizeAllowed(
        modern::MPA_TYPE_FLOWMESH_VAULT_PROOF,
        modern::FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE));
    BOOST_CHECK(!modern::PayloadSizeAllowed(
        modern::MPA_TYPE_FLOWMESH_VAULT_PROOF,
        modern::FLOWMESH_VAULT_PROOF_RECORD_MAX_SIZE + 1));

    std::string error;
    CMutableTransaction checkpoint_tx;
    checkpoint_tx.mpa.push_back(MpaRecord(
        modern::MPA_TYPE_FLOWMESH_CHECKPOINT, *checkpoint_bytes));
    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{checkpoint_tx}, params, A3 - 1, error));
    BOOST_CHECK_EQUAL(error, "mpa-inactive-type");
    BOOST_CHECK(modern::CheckTransactionMpa(
        CTransaction{checkpoint_tx}, params, A3, error));

    CMutableTransaction malformed_checkpoint{checkpoint_tx};
    malformed_checkpoint.mpa[0].payload[0] = 0xff; // explicit core version
    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{malformed_checkpoint}, params, A3, error));
    BOOST_CHECK_EQUAL(error, "mpa-malformed-flowmesh-checkpoint");

    CMutableTransaction proof_tx;
    proof_tx.mpa.push_back(MpaRecord(
        modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, *proof_bytes));
    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{proof_tx}, params, A3 - 1, error));
    BOOST_CHECK_EQUAL(error, "mpa-inactive-type");
    BOOST_CHECK(modern::CheckTransactionMpa(
        CTransaction{proof_tx}, params, A3, error));

    CMutableTransaction malformed{proof_tx};
    malformed.mpa[0].payload.push_back(0);
    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{malformed}, params, A3, error));
    BOOST_CHECK_EQUAL(error, "mpa-malformed-flowmesh-vault-proof");

    // Even two distinct, canonically sorted type-9 records are forbidden:
    // v1 transaction semantics carry exactly one effect proof.
    CMutableTransaction duplicate{proof_tx};
    auto second{proof};
    second.leaf_index = 2;
    second.effect = effects[2];
    second.branch = *modern::BuildFlowMeshEffectBranch(
        core.effect_start, effects, second.leaf_index);
    const auto second_bytes{modern::EncodeFlowMeshVaultProofV1(second)};
    BOOST_REQUIRE(second_bytes.has_value());
    duplicate.mpa.push_back(MpaRecord(
        modern::MPA_TYPE_FLOWMESH_VAULT_PROOF, *second_bytes));
    std::sort(duplicate.mpa.begin(), duplicate.mpa.end(), modern::MpaRecordLess);
    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{duplicate}, params, A3, error));
    BOOST_CHECK_EQUAL(error, "mpa-multiple-flowmesh-vault-proofs");

    CMutableTransaction two_checkpoints;
    two_checkpoints.mpa.push_back(checkpoint_tx.mpa.front());
    auto second_checkpoint{checkpoint_tx.mpa.front()};
    second_checkpoint.payload[2 + 32 + 32 + 8 + 7] ^= 1; // sequence low byte
    two_checkpoints.mpa.push_back(std::move(second_checkpoint));
    BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(CTransaction{two_checkpoints}),
                      MAX_TX_PAYLOAD_COST);
    BOOST_CHECK(modern::CheckTransactionPayloadCost(
        CTransaction{two_checkpoints}, error));
    two_checkpoints.mpa.push_back(two_checkpoints.mpa.back());
    BOOST_CHECK(!modern::CheckTransactionPayloadCost(
        CTransaction{two_checkpoints}, error));
    BOOST_CHECK_EQUAL(error, "bad-payload-cost");

    CBlock block;
    for (size_t i{0}; i < 20; ++i) {
        block.vtx.push_back(MakeTransactionRef(checkpoint_tx));
    }
    BOOST_CHECK_EQUAL(modern::BlockPayloadVerifyCost(block),
                      MAX_BLOCK_PAYLOAD_COST);
    BOOST_CHECK(modern::CheckBlockPayloadCost(block, error));
    block.vtx.push_back(MakeTransactionRef(checkpoint_tx));
    BOOST_CHECK(!modern::CheckBlockPayloadCost(block, error));
    BOOST_CHECK_EQUAL(error, "bad-block-payload-cost");
}

BOOST_AUTO_TEST_SUITE_END()

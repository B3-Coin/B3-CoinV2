// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/bridge_state.h>

#include <bridge/proof.h>
#include <bridge/rlp.h>
#include <crypto/common.h>
#include <crypto/keccak256.h>
#include <modern/asset_output.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <primitives/transaction.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int BRIDGE_HEIGHT{100};

constexpr uint256 TestHash(const uint8_t value) { return uint256{value}; }

uint256 TestBlockHash(const uint32_t value)
{
    uint256 hash;
    WriteBE32(hash.begin() + 28, value + 1);
    return hash;
}

Consensus::Params BridgeParams()
{
    Consensus::Params params;
    params.hashGenesisBlock = TestHash(1);
    params.legacy_final_hash = TestHash(2);

    Consensus::BridgeAssetParams busd;
    busd.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
    busd.implementation_or_adapter = TestHash(3);
    busd.adapter_version = 1;
    busd.recipient_encoding_version =
        Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
    busd.activation_height = BRIDGE_HEIGHT;
    busd.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = 100,
        .max_per_epoch = 150,
        .epoch_length_blocks = 10,
    };
    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = TestHash(4);
    light.trusted_checkpoint_slot = 32;
    light.genesis_validators_root = TestHash(5);
    light.fork_schedule = {{0, {0, 0, 0, 0}}};
    light.fork_schedule_valid_through_epoch = 100;
    light.min_sync_committee_participants =
        Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 10;
    busd.light_client = light;
    busd.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins managed;
    managed.authority_address.fill(0x11);
    managed.vault_runtime_code_hash = TestHash(6);
    managed.withdrawal_rules_version =
        Consensus::MANAGED_WITHDRAWAL_RULES_VERSION_V1;
    managed.withdrawal_rules_commitment = TestHash(7);
    busd.managed_withdrawal = managed;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(busd));
    params.busd_bridge = std::move(busd);
    return params;
}

Consensus::Params DecentralizedBridgeParams()
{
    Consensus::Params params{BridgeParams()};
    Consensus::BridgeAssetParams& busd{*params.busd_bridge};
    busd.withdrawal_mode =
        Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1;
    busd.managed_withdrawal.reset();
    Consensus::BridgeDecentralizedWithdrawalPins decentralized;
    decentralized.ethereum_verifier_address.fill(0x33);
    decentralized.ethereum_verifier_code_hash = TestHash(31);
    decentralized.b3_genesis_validator_set_root = TestHash(32);
    decentralized.withdrawal_rules_version =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    decentralized.withdrawal_rules_commitment = TestHash(33);
    decentralized.min_b3_validator_stake = 1;
    decentralized.max_epoch_lag = 10;
    busd.decentralized_withdrawal = decentralized;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(busd));
    return params;
}

Consensus::Params FullyActiveBridgeParams(const bool decentralized)
{
    Consensus::Params params{
        decentralized ? DecentralizedBridgeParams() : BridgeParams()};
    params.legacy_b3coin = true;
    params.hard_fork_height = 10; // H=9
    params.transition_pow_length = 10; // M=20
    params.modern_pos.emplace();
    params.fn_pod_activation_height = 21; // A1 > M
    params.asset_activation_height = 30; // A2
    params.flowmesh_activation_height =
        30 + Consensus::FLOWMESH_ANCHOR_DEPTH; // A3
    BOOST_REQUIRE(Consensus::BridgeRulesActive(BRIDGE_HEIGHT, params));
    return params;
}

uint256 Keccak(const std::vector<unsigned char>& bytes)
{
    uint256 hash;
    Keccak256().Write(bytes).Finalize(hash);
    return hash;
}

std::vector<unsigned char> EncodedBytes(
    const std::span<const unsigned char> bytes)
{
    return bridge::RlpEncodeBytes(bytes);
}

struct DepositSpec {
    uint64_t id{0};
    uint64_t amount{0};
    unsigned char recipient_fill{0};
};

struct ReceiptFixture {
    uint64_t block_number{700};
    uint64_t timestamp{1'000};
    uint256 receipts_root{};
    uint256 block_hash{};
    std::vector<unsigned char> header{};
    std::vector<unsigned char> mpt_node{};
    std::vector<CScript> recipient_scripts{};
};

ReceiptFixture MakeReceiptFixture(
    const Consensus::Params& params,
    const std::vector<DepositSpec>& deposits)
{
    std::vector<std::vector<unsigned char>> encoded_logs;
    std::vector<CScript> recipient_scripts;
    for (const DepositSpec& deposit : deposits) {
        std::array<unsigned char, 32> deposit_topic{};
        WriteBE64(deposit_topic.data() + 24, deposit.id);
        std::array<unsigned char, 32> token_topic{};
        std::copy(params.busd_bridge->asset.token_address.begin(),
                  params.busd_bridge->asset.token_address.end(),
                  token_topic.begin() + 12);
        std::vector<std::vector<unsigned char>> topics{
            EncodedBytes(std::span<const unsigned char>{
                bridge::DepositTopic().begin(), 32}),
            EncodedBytes(deposit_topic), EncodedBytes(token_topic)};

        bridge::RecipientV1 recipient;
        recipient.pubkey_hash.fill(deposit.recipient_fill);
        recipient_scripts.push_back(bridge::RecipientV1Script(recipient));
        const auto encoded_recipient{bridge::EncodeRecipientV1(recipient)};
        std::array<unsigned char, 64> data{};
        WriteBE64(data.data() + 24, deposit.amount);
        std::copy(encoded_recipient.begin(), encoded_recipient.end(),
                  data.begin() + 32);
        const std::vector<std::vector<unsigned char>> log_fields{
            EncodedBytes(params.busd_bridge->asset.vault_address),
            bridge::RlpEncodeList(topics), EncodedBytes(data)};
        encoded_logs.push_back(bridge::RlpEncodeList(log_fields));
    }

    std::array<unsigned char, 256> bloom{};
    const std::vector<std::vector<unsigned char>> receipt_fields{
        bridge::RlpEncodeUint64(1), bridge::RlpEncodeUint64(1),
        EncodedBytes(bloom), bridge::RlpEncodeList(encoded_logs)};
    const std::vector<unsigned char> receipt{
        bridge::RlpEncodeList(receipt_fields)};

    // A one-item receipts trie: key rlp(0) = 0x80, nibbles [8,0], encoded as
    // an even leaf path 0x20 || 0x80.
    const std::array<unsigned char, 2> leaf_path{0x20, 0x80};
    const std::vector<unsigned char> leaf{bridge::RlpEncodeList(
        {EncodedBytes(leaf_path), EncodedBytes(receipt)})};

    ReceiptFixture out;
    out.receipts_root = Keccak(leaf);
    out.mpt_node = leaf;
    out.recipient_scripts = std::move(recipient_scripts);

    std::array<unsigned char, 32> parent{};
    parent[0] = 0x55;
    const std::vector<unsigned char> empty{
        bridge::RlpEncodeBytes(std::span<const unsigned char>{})};
    std::vector<std::vector<unsigned char>> header_fields(16, empty);
    header_fields[0] = EncodedBytes(parent);
    header_fields[5] = EncodedBytes(
        std::span<const unsigned char>{out.receipts_root.begin(), 32});
    header_fields[8] = bridge::RlpEncodeUint64(out.block_number);
    header_fields[11] = bridge::RlpEncodeUint64(out.timestamp);
    out.header = bridge::RlpEncodeList(header_fields);
    out.block_hash = Keccak(out.header);
    return out;
}

void SeedVerifiedAnchor(node::BridgeStateIndex& index,
                        const ReceiptFixture& fixture,
                        const uint64_t beacon_slot = 32,
                        const uint64_t finalized_timestamp = 1'000)
{
    bridge::LightClientStore store;
    store.finalized_header.beacon.slot = beacon_slot;
    store.finalized_header.execution.block_number = fixture.block_number;
    store.finalized_header.execution.block_hash = fixture.block_hash;
    store.finalized_header.execution.receipts_root = fixture.receipts_root;
    store.finalized_header.execution.timestamp = finalized_timestamp;
    store.period = bridge::PeriodAtSlot(beacon_slot);
    store.current.pubkeys.resize(bridge::ssz::SYNC_COMMITTEE_SIZE);

    node::BridgeBlockDelta seed;
    seed.height = BRIDGE_HEIGHT - 1;
    seed.block_hash = TestHash(8);
    seed.light_client_after = store;
    seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        fixture.block_number, fixture.block_hash, fixture.receipts_root,
        beacon_slot, fixture.timestamp, seed.height, seed.block_hash});
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(seed, error), error);
}

CTransactionRef MintTransactionWithAmount(
    const Consensus::Params& params, const ReceiptFixture& fixture,
    const uint32_t log_index, const CAmount amount,
    const CScript& recipient_override = {})
{
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);
    BOOST_REQUIRE(log_index < fixture.recipient_scripts.size());
    const CScript& recipient{recipient_override.empty()
                                 ? fixture.recipient_scripts[log_index]
                                 : recipient_override};
    bridge::BridgeMintV1 mint{*registry, 0, fixture.block_hash,
                              fixture.block_number, 0, log_index,
                              {fixture.header}, {fixture.mpt_node}};
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::MINT, mint})};
    const auto owner{modern::MakeAssetOwnerOutput(*asset, amount, recipient)};
    BOOST_REQUIRE(record);
    BOOST_REQUIRE(owner);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout.push_back(*owner);
    tx.vout.push_back(*binding);
    tx.mpa.push_back(*record);
    return MakeTransactionRef(std::move(tx));
}

CBlock Block(const uint32_t time,
             std::vector<CTransactionRef> transactions)
{
    CBlock block;
    block.nTime = time;
    block.vtx = std::move(transactions);
    return block;
}

} // namespace

BOOST_AUTO_TEST_SUITE(bridge_state_tests)

BOOST_AUTO_TEST_CASE(managed_withdrawal_requires_the_exact_named_burn)
{
    const Consensus::Params params{BridgeParams()};
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);

    bridge::BridgeManagedWithdrawalV1 withdrawal;
    withdrawal.registry_id = *registry;
    withdrawal.burn_output_index = 0;
    withdrawal.raw_amount = 75;
    withdrawal.ethereum_recipient.fill(0x22);
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL, withdrawal})};
    const auto burn{modern::MakeAssetBurnOutput(*asset, 75)};
    BOOST_REQUIRE(record);
    BOOST_REQUIRE(burn);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);

    CMutableTransaction mutable_tx;
    mutable_tx.vin.resize(1);
    mutable_tx.vin[0].prevout = COutPoint{
        Txid::FromUint256(TestHash(20)), 0};
    mutable_tx.vout.push_back(*burn);
    mutable_tx.vout.push_back(*binding);
    mutable_tx.mpa.push_back(*record);
    const auto tx{MakeTransactionRef(std::move(mutable_tx))};
    const CBlock block{Block(1'000, {tx})};
    node::BridgeStateIndex index;
    node::BridgeBlockDelta delta;
    std::string error;
    BOOST_CHECK(!index.VerifyBlock(block, BRIDGE_HEIGHT - 1, TestHash(8),
                                   params, delta, error));
    BOOST_CHECK(error.find("not active") != std::string::npos);
    error.clear();
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(block, BRIDGE_HEIGHT, TestHash(9),
                                            params, delta, error),
                          error);
    BOOST_CHECK(delta.mint_authorizations.empty());
    BOOST_REQUIRE_EQUAL(delta.withdrawals_added.size(), 1U);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_CHECK_EQUAL(index.WithdrawalCount(), 1U);
    BOOST_CHECK(index.Withdrawal(
        node::BridgeWithdrawalId{tx->GetHash(), 0}).has_value());
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(BRIDGE_HEIGHT, TestHash(9), error),
                          error);
    BOOST_CHECK_EQUAL(index.WithdrawalCount(), 0U);

    CMutableTransaction resigned{*tx};
    resigned.vin[0].scriptWitness.stack.push_back({0xaa, 0xbb});
    const CTransactionRef resigned_tx{
        MakeTransactionRef(std::move(resigned))};
    BOOST_CHECK(resigned_tx->GetHash() == tx->GetHash());
    BOOST_CHECK(resigned_tx->GetPtxid() != tx->GetPtxid());
    node::BridgeBlockDelta resigned_delta;
    error.clear();
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
        Block(1'000, {resigned_tx}), BRIDGE_HEIGHT, TestHash(19), params,
        resigned_delta, error),
                          error);
    BOOST_REQUIRE_EQUAL(resigned_delta.withdrawals_added.size(), 1U);
    BOOST_CHECK(resigned_delta.withdrawals_added.front().transaction_id ==
                tx->GetHash());

    node::BridgeBlockDelta ignored;
    CMutableTransaction missing_binding{*tx};
    missing_binding.vout.pop_back();
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(missing_binding))}),
        BRIDGE_HEIGHT, TestHash(10), params, ignored, error));
    BOOST_CHECK_EQUAL(error, "bridge-binding-missing");

    CMutableTransaction mismatched_binding{*tx};
    mismatched_binding.mpa[0].payload.back() ^= 0x01;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(mismatched_binding))}),
        BRIDGE_HEIGHT, TestHash(11), params, ignored, error));
    BOOST_CHECK_EQUAL(error, "bridge-binding-mismatch");

    CMutableTransaction duplicate_binding{*tx};
    duplicate_binding.vout.push_back(*binding);
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(duplicate_binding))}),
        BRIDGE_HEIGHT, TestHash(12), params, ignored, error));
    BOOST_CHECK_EQUAL(error, "bridge-binding-multiple");

    CMutableTransaction orphan_binding{*tx};
    orphan_binding.mpa.clear();
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(orphan_binding))}),
        BRIDGE_HEIGHT, TestHash(13), params, ignored, error));
    BOOST_CHECK(error.find("orphan bridge binding") != std::string::npos);

    CMutableTransaction redirected{*tx};
    redirected.vout[0] = *modern::MakeAssetBurnOutput(*asset, 74);
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(redirected))}),
        BRIDGE_HEIGHT, TestHash(10), params, ignored, error));
    BOOST_CHECK(error.find("exact bUSD BURN") != std::string::npos);

    CMutableTransaction duplicate_record{*tx};
    duplicate_record.mpa.push_back(*record);
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(duplicate_record))}),
        BRIDGE_HEIGHT, TestHash(11), params, ignored, error));
    BOOST_CHECK(error.find("more than one bridge record") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(incomplete_bridge_parameters_fail_without_dereference)
{
    Consensus::Params params;
    params.busd_bridge.emplace();
    node::BridgeStateIndex index;
    node::BridgeTxAuthorization authorization;
    std::string error;
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(TestHash(90)), 0};
    const CTransactionRef candidate{MakeTransactionRef(std::move(tx))};
    BOOST_CHECK(!index.VerifyTransaction(*candidate, BRIDGE_HEIGHT, 1'000,
                                         params, authorization, error));
    BOOST_CHECK_EQUAL(error, "bridge consensus parameters are incomplete");
}

BOOST_AUTO_TEST_CASE(decentralized_burn_ids_roots_and_undo_are_deterministic)
{
    // Cross-language vector independently checked with Foundry `cast keccak`.
    modern::BridgeWithdrawalV1 vector;
    vector.origin_chain_id = 1;
    for (size_t i{0}; i < 32; ++i) {
        vector.asset_id.begin()[i] = static_cast<unsigned char>(i);
    }
    for (size_t i{0}; i < 20; ++i) {
        vector.origin_token[i] = static_cast<unsigned char>(0x20 + i);
        vector.recipient[i] = static_cast<unsigned char>(0x40 + i);
    }
    vector.amount = 1'000'000;
    vector.b3_height = 815'000;
    const auto vector_preimage{modern::EncodeBridgeWithdrawalV1(vector)};
    const auto vector_leaf{modern::BridgeWithdrawalLeafV1(vector)};
    BOOST_REQUIRE(vector_preimage);
    BOOST_REQUIRE(vector_leaf);
    BOOST_CHECK_EQUAL(vector_preimage->size(), 128U);
    BOOST_CHECK_EQUAL(
        HexStr(*vector_leaf),
        "f96ee37321b191d9ba3e573fd7739ab8a163033824a1c534045bd168c3c88b44");

    const Consensus::Params params{FullyActiveBridgeParams(true)};
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);

    const auto make_burn = [&](const uint8_t source_tag,
                               const CAmount amount,
                               const uint8_t recipient_tag) {
        bridge::BridgeBurnV1 burn_record;
        burn_record.registry_id = *registry;
        burn_record.burn_output_index = 0;
        burn_record.raw_amount = static_cast<uint64_t>(amount);
        burn_record.ethereum_recipient.fill(recipient_tag);
        const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
            bridge::BridgeRecordKindV1::BRIDGE_BURN, burn_record})};
        const auto burn{modern::MakeAssetBurnOutput(*asset, amount)};
        BOOST_REQUIRE(record);
        BOOST_REQUIRE(burn);
        const auto binding{modern::MakeBridgeBindingOutput(*record)};
        BOOST_REQUIRE(binding);
        CMutableTransaction tx;
        tx.vin.resize(1);
        tx.vin[0].prevout =
            COutPoint{Txid::FromUint256(TestHash(source_tag)), 0};
        tx.vout = {*burn, *binding};
        tx.mpa = {*record};
        return MakeTransactionRef(std::move(tx));
    };

    const CTransactionRef first{make_burn(40, 11, 0x41)};
    const CTransactionRef second{make_burn(41, 22, 0x42)};
    const uint256 block_hash{TestHash(42)};
    node::BridgeStateIndex index;
    node::BridgeBlockDelta delta;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
                              Block(1'000, {first, second}), BRIDGE_HEIGHT,
                              block_hash, params, delta, error),
                          error);
    BOOST_REQUIRE_EQUAL(delta.decentralized_withdrawals_added.size(), 2U);
    BOOST_CHECK_EQUAL(
        delta.decentralized_withdrawals_added[0].withdrawal.withdrawal_id,
        0U);
    BOOST_CHECK_EQUAL(
        delta.decentralized_withdrawals_added[1].withdrawal.withdrawal_id,
        1U);
    BOOST_CHECK_EQUAL(
        modern::EncodeBridgeWithdrawalV1(
            delta.decentralized_withdrawals_added[0].withdrawal)
            ->size(),
        128U);
    modern::WithdrawalTreeState expected;
    BOOST_REQUIRE(modern::AppendBridgeWithdrawal(
        expected, delta.decentralized_withdrawals_added[0].withdrawal));
    BOOST_REQUIRE(modern::AppendBridgeWithdrawal(
        expected, delta.decentralized_withdrawals_added[1].withdrawal));
    BOOST_CHECK(delta.withdrawal_tree_after == expected);

    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_CHECK_EQUAL(index.DecentralizedWithdrawalCount(), 2U);
    BOOST_CHECK(index.DecentralizedWithdrawal(0).has_value());
    BOOST_CHECK(index.WithdrawalRootAtHeight(BRIDGE_HEIGHT) ==
                std::optional<uint256>{expected.root});

    // The finality field is zero before bridge activation and in managed
    // mode. Once decentralized withdrawals are active it must come from the
    // exact active-chain index; an unavailable index fails closed.
    BOOST_CHECK(node::FinalityWithdrawalRoot(BRIDGE_HEIGHT - 1, params,
                                              nullptr) ==
                std::optional<uint256>{uint256{}});
    BOOST_CHECK(node::FinalityWithdrawalRoot(BRIDGE_HEIGHT, params, &index) ==
                std::optional<uint256>{expected.root});
    BOOST_CHECK(!node::FinalityWithdrawalRoot(BRIDGE_HEIGHT, params, nullptr));
    const Consensus::Params managed_params{FullyActiveBridgeParams(false)};
    BOOST_CHECK(node::FinalityWithdrawalRoot(BRIDGE_HEIGHT, managed_params,
                                              nullptr) ==
                std::optional<uint256>{uint256{}});

    node::BridgeBlockDelta empty_delta;
    const uint256 empty_hash{TestHash(43)};
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
                              Block(1'001, {}), BRIDGE_HEIGHT + 1,
                              empty_hash, params, empty_delta, error),
                          error);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(empty_delta, error), error);
    BOOST_CHECK(empty_delta.withdrawal_tree_before ==
                empty_delta.withdrawal_tree_after);
    BOOST_CHECK(index.WithdrawalRootAtHeight(BRIDGE_HEIGHT + 1) ==
                std::optional<uint256>{expected.root});
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(BRIDGE_HEIGHT + 1,
                                                empty_hash, error),
                          error);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(BRIDGE_HEIGHT, block_hash,
                                                error),
                          error);
    BOOST_CHECK_EQUAL(index.DecentralizedWithdrawalCount(), 0U);
    BOOST_CHECK_EQUAL(index.WithdrawalTree().count, 0U);
    BOOST_CHECK(index.WithdrawalTree().root ==
                modern::WithdrawalZeroHashes()[modern::WITHDRAWAL_TREE_DEPTH]);
    BOOST_CHECK(!index.WithdrawalRootAtHeight(BRIDGE_HEIGHT));

    node::BridgeBlockDelta replay;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
                              Block(1'000, {first, second}), BRIDGE_HEIGHT,
                              block_hash, params, replay, error),
                          error);
    BOOST_CHECK(replay.withdrawal_tree_after == delta.withdrawal_tree_after);
}

BOOST_AUTO_TEST_CASE(mint_checks_nullifier_caps_exact_output_and_reorg)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{MakeReceiptFixture(
        params, {{1, 60, 0x31}, {2, 60, 0x32}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);

    const CTransactionRef tx0{
        MintTransactionWithAmount(params, fixture, 0, 60)};
    node::BridgeBlockDelta delta;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
        Block(1'060, {tx0}), BRIDGE_HEIGHT, TestHash(10), params, delta, error),
                          error);
    BOOST_REQUIRE_EQUAL(delta.mint_authorizations.size(), 1U);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_CHECK(index.IsNullified(
        delta.mint_authorizations.front().authorization.nullifier));
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    BOOST_REQUIRE(asset);
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset, 0), 60);

    node::BridgeBlockDelta duplicate;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(Block(1'061, {tx0}), BRIDGE_HEIGHT + 1,
                                   TestHash(11), params, duplicate, error));
    BOOST_CHECK(error.find("already nullified") != std::string::npos);

    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(BRIDGE_HEIGHT, TestHash(10), error),
                          error);
    BOOST_CHECK(!index.IsNullified(
        delta.mint_authorizations.front().authorization.nullifier));
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset, 0), 0);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset, 0), 60);

    const CScript wrong_recipient{CScript{} << OP_TRUE};
    const CTransactionRef redirected{MintTransactionWithAmount(
        params, fixture, 1, 60, wrong_recipient)};
    node::BridgeBlockDelta redirected_delta;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(Block(1'062, {redirected}),
                                   BRIDGE_HEIGHT + 1, TestHash(12), params,
                                   redirected_delta, error));
    BOOST_CHECK(error.find("exact authorized OWNER") != std::string::npos);

    node::BridgeStateIndex block_cap_index;
    SeedVerifiedAnchor(block_cap_index, fixture);
    node::BridgeBlockDelta cap_delta;
    error.clear();
    BOOST_CHECK(!block_cap_index.VerifyBlock(
        Block(1'060,
              {MintTransactionWithAmount(params, fixture, 0, 60),
               MintTransactionWithAmount(params, fixture, 1, 60)}),
        BRIDGE_HEIGHT, TestHash(13), params, cap_delta, error));
    BOOST_CHECK(error.find("cap admission") != std::string::npos);

    const ReceiptFixture epoch_fixture{MakeReceiptFixture(
        params, {{3, 100, 0x33}, {4, 60, 0x34}})};
    node::BridgeStateIndex epoch_cap_index;
    SeedVerifiedAnchor(epoch_cap_index, epoch_fixture);
    node::BridgeBlockDelta first_epoch;
    error.clear();
    BOOST_REQUIRE_MESSAGE(epoch_cap_index.VerifyBlock(
        Block(1'060,
              {MintTransactionWithAmount(params, epoch_fixture, 0, 100)}),
        BRIDGE_HEIGHT, TestHash(14), params, first_epoch, error),
                          error);
    BOOST_REQUIRE_MESSAGE(epoch_cap_index.ConnectBlock(first_epoch, error),
                          error);
    node::BridgeBlockDelta over_epoch;
    error.clear();
    BOOST_CHECK(!epoch_cap_index.VerifyBlock(
        Block(1'061,
              {MintTransactionWithAmount(params, epoch_fixture, 1, 60)}),
        BRIDGE_HEIGHT + 1, TestHash(15), params, over_epoch, error));
    BOOST_CHECK(error.find("cap admission") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(block_preview_keeps_prefix_and_rolls_back_rejected_chunk)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{MakeReceiptFixture(
        params, {{1, 60, 0x61}, {2, 60, 0x62}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);

    const CTransactionRef first{
        MintTransactionWithAmount(params, fixture, 0, 60)};
    const CTransactionRef over_block_cap{
        MintTransactionWithAmount(params, fixture, 1, 60)};

    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);
    bridge::BridgeManagedWithdrawalV1 withdrawal;
    withdrawal.registry_id = *registry;
    withdrawal.burn_output_index = 0;
    withdrawal.raw_amount = 25;
    withdrawal.ethereum_recipient.fill(0x63);
    const auto withdrawal_record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{
            bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL, withdrawal})};
    const auto burn{modern::MakeAssetBurnOutput(*asset, 25)};
    BOOST_REQUIRE(withdrawal_record);
    BOOST_REQUIRE(burn);
    const auto binding{modern::MakeBridgeBindingOutput(*withdrawal_record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction mutable_withdrawal;
    mutable_withdrawal.vout = {*burn, *binding};
    mutable_withdrawal.mpa = {*withdrawal_record};
    const CTransactionRef valid_after_rejection{
        MakeTransactionRef(std::move(mutable_withdrawal))};

    std::string error;
    auto preview{index.BeginBlockPreview(BRIDGE_HEIGHT, 1'060,
                                         TestHash(20), params, error)};
    BOOST_REQUIRE_MESSAGE(preview != nullptr, error);

    const std::array<CTransactionRef, 1> prefix{first};
    BOOST_REQUIRE_MESSAGE(preview->TryAppend(prefix, error), error);

    // The first transaction in this trial mutates withdrawal state, then the
    // second exceeds the cumulative block cap. The whole chunk must roll back.
    const std::array<CTransactionRef, 2> rejected{
        valid_after_rejection, over_block_cap};
    BOOST_CHECK(!preview->TryAppend(rejected, error));
    BOOST_CHECK(error.find("cap admission") != std::string::npos);

    // This succeeds only if the rejected chunk did not retain its first
    // transaction. The earlier mint prefix must nevertheless remain active.
    const std::array<CTransactionRef, 1> later{valid_after_rejection};
    BOOST_REQUIRE_MESSAGE(preview->TryAppend(later, error), error);

    // The incremental accepted sequence remains equivalent to ordinary full
    // block verification, which is still the final block-template check.
    node::BridgeBlockDelta full_delta;
    error.clear();
    BOOST_CHECK_MESSAGE(index.VerifyBlock(
                            Block(1'060, {first, valid_after_rejection}),
                            BRIDGE_HEIGHT, TestHash(21), params, full_delta,
                            error),
                        error);
}

BOOST_AUTO_TEST_CASE(mint_rejects_stale_and_unknown_fork_heads)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{9, 60, 0x44}})};
    const CTransactionRef mint{
        MintTransactionWithAmount(params, fixture, 0, 60)};
    std::string error;
    node::BridgeBlockDelta ignored;

    node::BridgeStateIndex stale;
    SeedVerifiedAnchor(stale, fixture, 32, 1'000);
    BOOST_CHECK(!stale.VerifyBlock(Block(1'121, {mint}), BRIDGE_HEIGHT,
                                   TestHash(14), params, ignored, error));
    BOOST_CHECK(error.find("stale") != std::string::npos);

    node::BridgeStateIndex unknown;
    SeedVerifiedAnchor(unknown, fixture, 101 * bridge::SLOTS_PER_EPOCH,
                       1'000);
    error.clear();
    BOOST_CHECK(!unknown.VerifyBlock(Block(1'001, {mint}), BRIDGE_HEIGHT,
                                     TestHash(15), params, ignored, error));
    BOOST_CHECK(error.find("fork schedule") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(light_client_operations_require_order_and_non_coinbase)
{
    const Consensus::Params params{BridgeParams()};
    bridge::LightClientUpdate update;
    update.attested.execution_branch.resize(
        bridge::BRIDGE_EXECUTION_BRANCH_NODES);
    update.finalized.execution_branch.resize(
        bridge::BRIDGE_EXECUTION_BRANCH_NODES);
    update.finality_branch.resize(1);
    update.signature_slot = 1;
    const auto update_record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::UPDATE,
                               bridge::BridgeUpdateV1{update}})};
    BOOST_REQUIRE(update_record);
    const auto update_binding{modern::MakeBridgeBindingOutput(*update_record)};
    BOOST_REQUIRE(update_binding);
    CMutableTransaction update_tx;
    update_tx.vout.push_back(*update_binding);
    update_tx.mpa.push_back(*update_record);

    node::BridgeStateIndex empty;
    node::BridgeBlockDelta ignored;
    std::string error;
    BOOST_CHECK(!empty.VerifyBlock(
        Block(1'000, {MakeTransactionRef(update_tx)}), BRIDGE_HEIGHT,
        TestHash(16), params, ignored, error));
    BOOST_CHECK(error.find("precedes bootstrap") != std::string::npos);

    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{20, 60, 0x55}})};
    node::BridgeStateIndex bootstrapped;
    SeedVerifiedAnchor(bootstrapped, fixture);
    bridge::BridgeBootstrapV1 bootstrap;
    bootstrap.header.execution_branch.resize(
        bridge::BRIDGE_EXECUTION_BRANCH_NODES);
    bootstrap.current_committee.pubkeys.resize(
        bridge::ssz::SYNC_COMMITTEE_SIZE);
    bootstrap.current_committee_branch.resize(1);
    const auto bootstrap_record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::BOOTSTRAP,
                               bootstrap})};
    BOOST_REQUIRE(bootstrap_record);
    const auto bootstrap_binding{
        modern::MakeBridgeBindingOutput(*bootstrap_record)};
    BOOST_REQUIRE(bootstrap_binding);
    CMutableTransaction bootstrap_tx;
    bootstrap_tx.vout.push_back(*bootstrap_binding);
    bootstrap_tx.mpa.push_back(*bootstrap_record);
    error.clear();
    BOOST_CHECK(!bootstrapped.VerifyBlock(
        Block(1'000, {MakeTransactionRef(bootstrap_tx)}), BRIDGE_HEIGHT,
        TestHash(17), params, ignored, error));
    BOOST_CHECK(error.find("already bootstrapped") != std::string::npos);

    CMutableTransaction coinbase{update_tx};
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    error.clear();
    BOOST_CHECK(!empty.VerifyBlock(
        Block(1'000, {MakeTransactionRef(std::move(coinbase))}),
        BRIDGE_HEIGHT, TestHash(18), params, ignored, error));
    BOOST_CHECK(error.find("coinbase") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(connect_is_atomic_and_recent_undo_is_bounded)
{
    node::BridgeStateIndex index;
    node::BridgeBlockDelta malformed;
    malformed.height = BRIDGE_HEIGHT;
    malformed.block_hash = TestBlockHash(BRIDGE_HEIGHT);
    malformed.anchors_added.push_back(node::BridgeExecutionAnchor{
        700, TestBlockHash(700), TestBlockHash(701), 32, 1'000,
        malformed.height, malformed.block_hash});
    bridge::BridgeDepositKey unmatched_nullifier;
    unmatched_nullifier.origin_chain_id = 1;
    unmatched_nullifier.vault_address.fill(0x21);
    unmatched_nullifier.deposit_id = 1;
    malformed.nullifiers_added.push_back(unmatched_nullifier);

    std::string error;
    BOOST_CHECK(!index.ConnectBlock(malformed, error));
    BOOST_CHECK(error.find("do not match nullifiers") != std::string::npos);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 0U);
    BOOST_CHECK_EQUAL(index.NullifierCount(), 0U);
    BOOST_CHECK_EQUAL(index.ConnectedHeight(), -1);
    BOOST_CHECK(index.ConnectedHash().IsNull());
    BOOST_CHECK(index.History().empty());

    const int total_blocks{
        static_cast<int>(node::BRIDGE_STATE_UNDO_BLOCKS) + 2};
    for (int offset{0}; offset < total_blocks; ++offset) {
        node::BridgeBlockDelta delta;
        delta.height = BRIDGE_HEIGHT + offset;
        delta.block_hash = TestBlockHash(delta.height);
        delta.previous_height = index.ConnectedHeight();
        delta.previous_block_hash = index.ConnectedHash();
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    }

    const int last_height{BRIDGE_HEIGHT + total_blocks - 1};
    const int oldest_retained{last_height -
                              static_cast<int>(node::BRIDGE_STATE_UNDO_BLOCKS) +
                              1};
    BOOST_CHECK_EQUAL(index.History().size(),
                      node::BRIDGE_STATE_UNDO_BLOCKS);
    BOOST_CHECK_EQUAL(index.History().front().height, oldest_retained);
    BOOST_CHECK_EQUAL(index.ConnectedHeight(), last_height);
    BOOST_CHECK(index.ConnectedHash() == TestBlockHash(last_height));

    error.clear();
    BOOST_CHECK(!index.DisconnectBlock(last_height,
                                       TestBlockHash(last_height + 1),
                                       error));
    BOOST_CHECK_EQUAL(index.ConnectedHeight(), last_height);
    BOOST_CHECK_EQUAL(index.History().size(),
                      node::BRIDGE_STATE_UNDO_BLOCKS);

    for (int height{last_height}; height >= oldest_retained; --height) {
        BOOST_REQUIRE_MESSAGE(
            index.DisconnectBlock(height, TestBlockHash(height), error),
            error);
    }
    const int pruned_height{oldest_retained - 1};
    BOOST_CHECK(index.History().empty());
    BOOST_CHECK_EQUAL(index.ConnectedHeight(), pruned_height);
    BOOST_CHECK(index.ConnectedHash() == TestBlockHash(pruned_height));

    error.clear();
    BOOST_CHECK(!index.DisconnectBlock(
        pruned_height, TestBlockHash(pruned_height), error));
    BOOST_CHECK(error.find("no longer retained") != std::string::npos);
    BOOST_CHECK_EQUAL(index.ConnectedHeight(), pruned_height);
    BOOST_CHECK(index.ConnectedHash() == TestBlockHash(pruned_height));
}

BOOST_AUTO_TEST_SUITE_END()

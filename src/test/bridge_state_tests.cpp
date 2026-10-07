// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/bridge_state.h>

#include <bridge/burn_gate_v2.h>
#include <bridge/proof.h>
#include <bridge/reserve_v2.h>
#include <bridge/rlp.h>
#include <bridge/state_observation.h>
#include <bridge/withdrawal_v2_calldata.h>
#include <chainparams.h>
#include <common/args.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/keccak256.h>
#include <modern/asset_output.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_v2.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <modern/mpa.h>
#include <modern/withdrawal_v2.h>
#include <node/finality_binding_index.h>
#include <node/finality_fresh_lineage.h>
#include <node/validator_set.h>
#include <primitives/transaction.h>
#include <test/data/eth_lc_fixture.h>
#include <test/util/bridge_observation_world.h>
#include <test/util/bridge_state_access.h>
#include <util/chaintype.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace ethfx = eth_lc_fixture;

constexpr int BRIDGE_HEIGHT{100};

constexpr uint256 TestHash(const uint8_t value) { return uint256{value}; }

uint256 TestBlockHash(const uint32_t value)
{
    uint256 hash;
    WriteBE32(hash.begin() + 28, value + 1);
    return hash;
}

std::vector<unsigned char> FixtureHex(const std::string& hex)
{
    const auto value{TryParseHex<unsigned char>(hex)};
    BOOST_REQUIRE_MESSAGE(value.has_value(), "invalid light-client fixture hex");
    return *value;
}

uint256 FixtureHex32(const std::string& hex)
{
    const auto value{FixtureHex(hex)};
    BOOST_REQUIRE_EQUAL(value.size(), 32U);
    return uint256{std::span<const unsigned char>{value}};
}

template <size_t N>
std::array<unsigned char, N> FixtureHexArray(const std::string& hex)
{
    const auto value{FixtureHex(hex)};
    BOOST_REQUIRE_EQUAL(value.size(), N);
    std::array<unsigned char, N> out;
    std::copy(value.begin(), value.end(), out.begin());
    return out;
}

std::vector<uint256> FixtureBranch(const std::vector<std::string>& branch)
{
    std::vector<uint256> out;
    out.reserve(branch.size());
    for (const auto& value : branch) out.push_back(FixtureHex32(value));
    return out;
}

bridge::ssz::BeaconBlockHeader FixtureBeacon(
    const ethfx::BeaconHeaderHex& header)
{
    return {header.slot, header.proposer_index,
            FixtureHex32(header.parent_root), FixtureHex32(header.state_root),
            FixtureHex32(header.body_root)};
}

bridge::ssz::ExecutionPayloadHeader FixtureExecution(
    const ethfx::ExecHeaderHex& execution)
{
    bridge::ssz::ExecutionPayloadHeader out;
    out.parent_hash = FixtureHex32(execution.parent_hash);
    out.fee_recipient = FixtureHexArray<20>(execution.fee_recipient);
    out.state_root = FixtureHex32(execution.state_root);
    out.receipts_root = FixtureHex32(execution.receipts_root);
    out.logs_bloom = FixtureHexArray<256>(execution.logs_bloom);
    out.prev_randao = FixtureHex32(execution.prev_randao);
    out.block_number = execution.block_number;
    out.gas_limit = execution.gas_limit;
    out.gas_used = execution.gas_used;
    out.timestamp = execution.timestamp;
    out.extra_data = FixtureHex(execution.extra_data);
    out.base_fee_per_gas = FixtureHex32(execution.base_fee_le);
    out.block_hash = FixtureHex32(execution.block_hash);
    out.transactions_root = FixtureHex32(execution.transactions_root);
    out.withdrawals_root = FixtureHex32(execution.withdrawals_root);
    out.blob_gas_used = execution.blob_gas_used;
    out.excess_blob_gas = execution.excess_blob_gas;
    return out;
}

bridge::ssz::SyncCommittee FixtureCommittee(const std::string& pubkeys_hex,
                                             const std::string& aggregate_hex)
{
    const auto pubkeys{FixtureHex(pubkeys_hex)};
    BOOST_REQUIRE_EQUAL(
        pubkeys.size(), bridge::ssz::SYNC_COMMITTEE_SIZE * 48);
    bridge::ssz::SyncCommittee out;
    out.pubkeys.resize(bridge::ssz::SYNC_COMMITTEE_SIZE);
    for (size_t i{0}; i < bridge::ssz::SYNC_COMMITTEE_SIZE; ++i) {
        std::copy(pubkeys.begin() + i * 48, pubkeys.begin() + (i + 1) * 48,
                  out.pubkeys[i].begin());
    }
    out.aggregate_pubkey = FixtureHexArray<48>(aggregate_hex);
    return out;
}

bridge::LightClientConfig FixtureLightClientConfig()
{
    bridge::LightClientConfig config;
    config.genesis_validators_root =
        FixtureHex32(ethfx::GENESIS_VALIDATORS_ROOT);
    for (const auto& fixture_fork : ethfx::FORKS) {
        bridge::ForkVersion fork;
        fork.epoch = fixture_fork.epoch;
        fork.version = FixtureHexArray<4>(fixture_fork.version);
        config.forks.push_back(fork);
        if (fork.version[0] == 0x05) config.electra_epoch = fork.epoch;
    }
    config.min_participants =
        Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    return config;
}

bridge::LightClientStore FixtureBootstrapStore()
{
    const bridge::LightClientHeader header{
        FixtureBeacon(ethfx::U1_FINALIZED_BEACON),
        FixtureExecution(ethfx::U1_FINALIZED_EXEC),
        FixtureBranch(ethfx::U1_FINALIZED_EXEC_BRANCH)};
    bridge::LightClientStore store;
    const auto result{bridge::InitStore(
        store, FixtureLightClientConfig(), FixtureHex32(ethfx::BOOTSTRAP_ROOT),
        header,
        FixtureCommittee(ethfx::BOOTSTRAP_COMMITTEE_PUBKEYS,
                         ethfx::BOOTSTRAP_COMMITTEE_AGG),
        FixtureBranch(ethfx::BOOTSTRAP_COMMITTEE_BRANCH))};
    BOOST_REQUIRE(result == bridge::LcResult::OK);
    return store;
}

bridge::LightClientUpdate FixtureCommitteeUpdate()
{
    bridge::LightClientUpdate update;
    update.attested = {
        FixtureBeacon(ethfx::U1_ATTESTED_BEACON),
        FixtureExecution(ethfx::U1_ATTESTED_EXEC),
        FixtureBranch(ethfx::U1_ATTESTED_EXEC_BRANCH)};
    update.finalized = {
        FixtureBeacon(ethfx::U1_FINALIZED_BEACON),
        FixtureExecution(ethfx::U1_FINALIZED_EXEC),
        FixtureBranch(ethfx::U1_FINALIZED_EXEC_BRANCH)};
    update.finality_branch = FixtureBranch(ethfx::U1_FINALITY_BRANCH);
    update.has_next = true;
    update.next_committee = FixtureCommittee(
        ethfx::U1_NEXT_PUBKEYS, ethfx::U1_NEXT_AGG);
    update.next_branch = FixtureBranch(ethfx::U1_NEXT_BRANCH);
    update.sync_aggregate.bits = FixtureHexArray<64>(ethfx::U1_SYNC_BITS);
    update.sync_aggregate.signature = FixtureHexArray<96>(ethfx::U1_SYNC_SIG);
    update.signature_slot = ethfx::U1_SIGNATURE_SLOT;
    return update;
}

Consensus::Params BridgeParams()
{
    Consensus::Params params;
    params.hashGenesisBlock = TestHash(1);
    params.legacy_final_hash = TestHash(2);

    Consensus::BridgeAssetParams busd;
    busd.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
    busd.vault_runtime_code_hash = TestHash(6);
    busd.implementation_or_adapter = TestHash(3);
    busd.adapter_version = 1;
    busd.origin_deployment_block = 1;
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
    light.genesis_validators_root =
        Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    light.fork_schedule = {{0, {0, 0, 0, 0}}};
    light.fork_schedule_valid_through_epoch = 100;
    light.min_sync_committee_participants =
        Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 10;
    busd.light_client = light;
    busd.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins managed;
    managed.authority_address.fill(0x11);
    managed.vault_runtime_code_hash = *busd.vault_runtime_code_hash;
    managed.withdrawal_rules_version =
        Consensus::MANAGED_WITHDRAWAL_RULES_VERSION_V1;
    managed.withdrawal_rules_commitment = TestHash(7);
    busd.managed_withdrawal = managed;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(busd));
    params.busd_bridge = std::move(busd);
    return params;
}

Consensus::Params FixtureBridgeParams()
{
    Consensus::Params params{BridgeParams()};
    auto& pins{*params.busd_bridge->light_client};
    const auto config{FixtureLightClientConfig()};
    pins.trusted_checkpoint_root = FixtureHex32(ethfx::BOOTSTRAP_ROOT);
    pins.trusted_checkpoint_slot = ethfx::U1_FINALIZED_BEACON.slot;
    pins.genesis_validators_root = config.genesis_validators_root;
    pins.fork_schedule.clear();
    for (const auto& fork : config.forks) {
        pins.fork_schedule.push_back({fork.epoch, fork.version});
    }
    pins.fork_schedule_valid_through_epoch =
        bridge::EpochAtSlot(ethfx::U1_SIGNATURE_SLOT) + 1;
    pins.electra_epoch = config.electra_epoch;
    pins.min_sync_committee_participants = config.min_participants;
    pins.max_sync_lag_slots = bridge::SLOTS_PER_PERIOD;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(*params.busd_bridge));
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
    decentralized.bootstrap_validator_set_hash = TestHash(32);
    decentralized.withdrawal_rules_version =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    decentralized.withdrawal_rules_commitment =
        Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1;
    decentralized.min_bridge_validators = 4;
    decentralized.max_bridge_validators = 64;
    decentralized.min_bridge_total_weight = 1;
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
    params.bridge_withdrawal_activation_height = BRIDGE_HEIGHT;
    BOOST_REQUIRE(Consensus::BridgeRulesActive(BRIDGE_HEIGHT, params));
    BOOST_REQUIRE(
        Consensus::BridgeWithdrawalRulesActive(BRIDGE_HEIGHT, params));
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

std::vector<unsigned char> ExecutionHeader(const uint256& parent,
                                           const uint64_t number,
                                           const uint256& receipts_root,
                                           const uint64_t timestamp)
{
    const std::vector<unsigned char> empty{
        bridge::RlpEncodeBytes(std::span<const unsigned char>{})};
    std::vector<std::vector<unsigned char>> fields(16, empty);
    fields[0] = EncodedBytes(
        std::span<const unsigned char>{parent.begin(), 32});
    fields[5] = EncodedBytes(
        std::span<const unsigned char>{receipts_root.begin(), 32});
    fields[8] = bridge::RlpEncodeUint64(number);
    fields[11] = bridge::RlpEncodeUint64(timestamp);
    return bridge::RlpEncodeList(fields);
}

//! ExecutionHeader with header field [3], the stateRoot, set when given.
std::vector<unsigned char> ExecutionHeaderWithStateRoot(
    const uint256& parent, const uint64_t number, const uint256& receipts_root,
    const uint64_t timestamp, const std::optional<uint256>& state_root)
{
    std::vector<unsigned char> encoded{
        ExecutionHeader(parent, number, receipts_root, timestamp)};
    if (!state_root) return encoded;
    const auto top{bridge::RlpDecode(encoded)};
    BOOST_REQUIRE(top);
    const auto items{bridge::RlpChildren(*top)};
    BOOST_REQUIRE(items);
    std::vector<std::vector<unsigned char>> fields;
    for (const auto& item : *items) {
        fields.push_back(EncodedBytes(item.payload));
    }
    fields[3] = EncodedBytes(
        std::span<const unsigned char>{state_root->begin(), 32});
    return bridge::RlpEncodeList(fields);
}

struct DepositSpec {
    uint64_t id{0};
    uint64_t amount{0};
    unsigned char recipient_fill{0};
    //! The emitting vault when not the fixture's (a B-23 successor's
    //! deposits in the same receipt as the launch vault's).
    std::optional<Consensus::BridgeEthAddress> vault{};
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

/**
 * One-receipt trie holding `deposits` as Deposit logs of `vault` for
 * `token`. `receipt_type` != 0 wraps the receipt in that EIP-2718 envelope;
 * `padding_log_bytes` > 0 appends one non-vault log with that much data after
 * the deposits (their log indices are unchanged), enlarging the terminal
 * receipt node.
 */
ReceiptFixture MakeReceiptFixture(
    const Consensus::BridgeEthAddress& vault,
    const Consensus::BridgeEthAddress& token,
    const std::vector<DepositSpec>& deposits,
    const uint8_t receipt_type = 0, const size_t padding_log_bytes = 0)
{
    std::vector<std::vector<unsigned char>> encoded_logs;
    std::vector<CScript> recipient_scripts;
    for (const DepositSpec& deposit : deposits) {
        std::array<unsigned char, 32> deposit_topic{};
        WriteBE64(deposit_topic.data() + 24, deposit.id);
        std::array<unsigned char, 32> token_topic{};
        std::copy(token.begin(), token.end(), token_topic.begin() + 12);
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
            EncodedBytes(deposit.vault.value_or(vault)),
            bridge::RlpEncodeList(topics), EncodedBytes(data)};
        encoded_logs.push_back(bridge::RlpEncodeList(log_fields));
    }
    if (padding_log_bytes > 0) {
        std::array<unsigned char, 20> other{};
        other.fill(0x99);
        const std::vector<unsigned char> padding(padding_log_bytes, 0xab);
        encoded_logs.push_back(bridge::RlpEncodeList(
            {EncodedBytes(other), bridge::RlpEncodeList({}),
             EncodedBytes(padding)}));
    }

    std::array<unsigned char, 256> bloom{};
    const std::vector<std::vector<unsigned char>> receipt_fields{
        bridge::RlpEncodeUint64(1), bridge::RlpEncodeUint64(1),
        EncodedBytes(bloom), bridge::RlpEncodeList(encoded_logs)};
    std::vector<unsigned char> receipt;
    if (receipt_type != 0) receipt.push_back(receipt_type);
    const std::vector<unsigned char> receipt_list{
        bridge::RlpEncodeList(receipt_fields)};
    receipt.insert(receipt.end(), receipt_list.begin(), receipt_list.end());

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

ReceiptFixture MakeReceiptFixture(
    const Consensus::Params& params,
    const std::vector<DepositSpec>& deposits)
{
    return MakeReceiptFixture(params.busd_bridge->asset.vault_address,
                              params.busd_bridge->asset.token_address,
                              deposits);
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
    seed.light_client_connection_after =
        node::BridgeStateConnection{seed.height, seed.block_hash};
    seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        fixture.block_number, fixture.block_hash, fixture.receipts_root,
        beacon_slot, fixture.block_number, fixture.timestamp, seed.height,
        seed.block_hash});
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

CTransactionRef MintTransactionWithAncestry(
    const Consensus::Params& params, const ReceiptFixture& fixture,
    const uint256& source_anchor_hash,
    std::vector<std::vector<unsigned char>> ancestry_headers)
{
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);
    BOOST_REQUIRE(!fixture.recipient_scripts.empty());
    bridge::BridgeMintV1 mint{
        *registry, 0, source_anchor_hash, fixture.block_number, 0, 0,
        std::move(ancestry_headers), {fixture.mpt_node}};
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::MINT, mint})};
    const auto owner{modern::MakeAssetOwnerOutput(
        *asset, 60, fixture.recipient_scripts.front())};
    BOOST_REQUIRE(record);
    BOOST_REQUIRE(owner);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*owner, *binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef BackfillTransaction(
    const uint256& source_anchor_hash, const uint64_t target_block_number,
    std::vector<std::vector<unsigned char>> ancestry_headers)
{
    bridge::BridgeExecutionBackfillV1 backfill{
        source_anchor_hash, target_block_number,
        std::move(ancestry_headers)};
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::EXECUTION_BACKFILL,
        std::move(backfill)})};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*binding};
    tx.mpa = {*record};
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

// -- Bridge V2 fixtures (node changes sections 3-7) -------------------------

constexpr int BRIDGE_V2_HEIGHT{BRIDGE_HEIGHT + 10};

Consensus::BridgeEthAddress FilledAddress(const uint8_t fill)
{
    Consensus::BridgeEthAddress out;
    out.fill(fill);
    return out;
}

/**
 * The V1 test bridge (managed, active from BRIDGE_HEIGHT) plus a complete
 * Ethereum-mainnet V2 envelope from V = BRIDGE_HEIGHT + 10: Vault V2 0x44..,
 * USDT, both OI-3 code hashes (required on chain 1), the eth_lc_fixture
 * light-client pins (mainnet genesis validators root) and caps 100/150 per
 * 10-block epoch.
 */
Consensus::Params V2Params(const int burn_height = BRIDGE_V2_HEIGHT,
                           const uint64_t origin_deployment_block = 1)
{
    Consensus::Params params{FullyActiveBridgeParams(false)};
    Consensus::BridgeV2Params v2;
    v2.activation_height = BRIDGE_V2_HEIGHT;
    v2.burn_height = burn_height;
    v2.light_client = *FixtureBridgeParams().busd_bridge->light_client;
    v2.verifier.address = FilledAddress(0x55);
    v2.verifier.runtime_code_hash = TestHash(51);
    v2.verifier.min_bridge_validators = 4;
    v2.verifier.max_bridge_validators = 64;
    v2.verifier.min_bridge_total_weight = 900;
    v2.verifier.max_epoch_lag_seconds = v2.gate.set_expiry_seconds;
    Consensus::BridgeV2VaultEntry& vault{v2.vault};
    vault.origin_chain_id = Consensus::BUSD_ETHEREUM_CHAIN_ID;
    vault.vault_address = FilledAddress(0x44);
    vault.token_address = Consensus::BUSD_ETHEREUM_USDT;
    vault.vault_runtime_code_hash = TestHash(52);
    vault.token_runtime_code_hash = TestHash(53);
    vault.origin_deployment_block = origin_deployment_block;
    vault.max_deposit_raw = 100;
    vault.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = 100,
        .max_per_epoch = 150,
        .epoch_length_blocks = 10,
    };
    BOOST_REQUIRE(v2.Valid());
    BOOST_REQUIRE(vault.vault_address != params.busd_bridge->asset.vault_address);
    params.bridge_v2 = std::move(v2);
    BOOST_REQUIRE(params.BridgeV2RulesActive(BRIDGE_V2_HEIGHT));
    BOOST_REQUIRE(!params.BridgeV2RulesActive(BRIDGE_V2_HEIGHT - 1));
    return params;
}

ReceiptFixture MakeV2ReceiptFixture(const Consensus::Params& params,
                                    const std::vector<DepositSpec>& deposits,
                                    const uint8_t receipt_type = 0,
                                    const size_t padding_log_bytes = 0)
{
    return MakeReceiptFixture(params.bridge_v2->vault.vault_address,
                              params.bridge_v2->vault.token_address, deposits,
                              receipt_type, padding_log_bytes);
}

uint256 V2TestBlockHash(const int height)
{
    return TestBlockHash(static_cast<uint32_t>(50'000 + height));
}

/**
 * Connect a hand-built delta at `height` extending the tip that seeds one
 * verified light-client store and anchor into the empty V1 or V2 store (the
 * V2 seed below V is a fixture shortcut, as the V1 seed below A3 is).
 */
void SeedVerifiedAnchorAt(node::BridgeStateIndex& index,
                          const ReceiptFixture& fixture, const int height,
                          const bool v2_store,
                          const uint64_t beacon_slot = 32,
                          const uint64_t finalized_timestamp = 1'000)
{
    BOOST_REQUIRE(!(v2_store ? index.LightClientV2() : index.LightClient()));
    bridge::LightClientStore store;
    store.finalized_header.beacon.slot = beacon_slot;
    store.finalized_header.execution.block_number = fixture.block_number;
    store.finalized_header.execution.block_hash = fixture.block_hash;
    store.finalized_header.execution.receipts_root = fixture.receipts_root;
    store.finalized_header.execution.timestamp = finalized_timestamp;
    store.period = bridge::PeriodAtSlot(beacon_slot);
    store.current.pubkeys.resize(bridge::ssz::SYNC_COMMITTEE_SIZE);

    node::BridgeBlockDelta seed;
    seed.height = height;
    seed.block_hash = V2TestBlockHash(height);
    seed.previous_height = index.ConnectedHeight();
    seed.previous_block_hash = index.ConnectedHash();
    seed.v2_store = v2_store;
    seed.light_client_after = store;
    seed.light_client_connection_after =
        node::BridgeStateConnection{seed.height, seed.block_hash};
    seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        fixture.block_number, fixture.block_hash, fixture.receipts_root,
        beacon_slot, fixture.block_number, fixture.timestamp, seed.height,
        seed.block_hash});
    seed.withdrawal_tree_before = index.WithdrawalTree();
    seed.withdrawal_tree_after = index.WithdrawalTree();
    seed.withdrawal_tree_v2_before = index.WithdrawalTreeV2();
    seed.withdrawal_tree_v2_after = index.WithdrawalTreeV2();
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(seed, error), error);
}

//! VerifyBlock + ConnectBlock of one candidate at the next height (the
//! bridge activation height on an empty index).
node::BridgeBlockDelta ConnectNext(
    node::BridgeStateIndex& index, const Consensus::Params& params,
    std::vector<CTransactionRef> transactions = {},
    const uint32_t time = 1'060,
    const node::BridgeV2LineageContext* lineage = nullptr)
{
    const int height{index.ConnectedHeight() < 0
                         ? BRIDGE_HEIGHT
                         : index.ConnectedHeight() + 1};
    node::BridgeBlockDelta delta;
    std::string error;
    BOOST_REQUIRE_MESSAGE(
        index.VerifyBlock(Block(time, std::move(transactions)), height,
                          V2TestBlockHash(height), params, delta, error,
                          lineage),
        error);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    return delta;
}

void ConnectEmptyThrough(node::BridgeStateIndex& index,
                         const Consensus::Params& params, const int height)
{
    while (index.ConnectedHeight() < height) ConnectNext(index, params);
}

//! A kind 3 MINT naming the V2 vault entry `role` (default: the launch
//! entry), encoded under the V2 limits.
CTransactionRef MintTransactionV2(
    const Consensus::Params& params, const ReceiptFixture& fixture,
    const uint32_t log_index, const CAmount amount,
    const Consensus::BridgeV2EntryRole role = Consensus::BridgeV2EntryRole::LAUNCH)
{
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    const auto entry{modern::ConfiguredBridgeV2EntryId(params, role)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(entry);
    BOOST_REQUIRE(log_index < fixture.recipient_scripts.size());
    bridge::BridgeMintV1 mint{*entry, 0, fixture.block_hash,
                              fixture.block_number, 0, log_index,
                              {fixture.header}, {fixture.mpt_node}};
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::MINT, mint},
        bridge::BRIDGE_MPT_LIMITS_V2)};
    const auto owner{modern::MakeAssetOwnerOutput(
        *asset, amount, fixture.recipient_scripts[log_index])};
    BOOST_REQUIRE(record);
    BOOST_REQUIRE(owner);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*owner, *binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

//! A kind 6 burn of `amount` bUSD V2 naming `registry` (default: the V2
//! vault entry id) and an Ethereum recipient filled with `recipient_tag`.
CTransactionRef BurnTransactionV2(
    const Consensus::Params& params, const uint8_t source_tag,
    const CAmount amount, const uint8_t recipient_tag,
    const std::optional<uint256>& registry = std::nullopt,
    const std::optional<modern::AssetId>& burned_asset = std::nullopt)
{
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    const auto entry{modern::ConfiguredBridgeV2EntryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(entry);
    bridge::BridgeBurnV1 burn_record;
    burn_record.registry_id = registry.value_or(*entry);
    burn_record.burn_output_index = 0;
    burn_record.raw_amount = static_cast<uint64_t>(amount);
    burn_record.ethereum_recipient.fill(recipient_tag);
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::BRIDGE_BURN, burn_record})};
    const auto burn{modern::MakeAssetBurnOutput(
        burned_asset.value_or(*asset), amount)};
    BOOST_REQUIRE(record);
    BOOST_REQUIRE(burn);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(TestHash(source_tag)), 0};
    tx.vout = {*burn, *binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

//! A kind 5 managed withdrawal of `amount` V1 bUSD.
CTransactionRef ManagedWithdrawalTransaction(const Consensus::Params& params,
                                             const CAmount amount)
{
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);
    bridge::BridgeManagedWithdrawalV1 withdrawal;
    withdrawal.registry_id = *registry;
    withdrawal.burn_output_index = 0;
    withdrawal.raw_amount = static_cast<uint64_t>(amount);
    withdrawal.ethereum_recipient.fill(0x66);
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL, withdrawal})};
    const auto burn{modern::MakeAssetBurnOutput(*asset, amount)};
    BOOST_REQUIRE(record);
    BOOST_REQUIRE(burn);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*burn, *binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

//! A kind 1 BOOTSTRAP from the eth_lc_fixture checkpoint.
CTransactionRef FixtureBootstrapTransaction()
{
    bridge::BridgeBootstrapV1 bootstrap;
    bootstrap.header = bridge::LightClientHeader{
        FixtureBeacon(ethfx::U1_FINALIZED_BEACON),
        FixtureExecution(ethfx::U1_FINALIZED_EXEC),
        FixtureBranch(ethfx::U1_FINALIZED_EXEC_BRANCH)};
    bootstrap.current_committee = FixtureCommittee(
        ethfx::BOOTSTRAP_COMMITTEE_PUBKEYS, ethfx::BOOTSTRAP_COMMITTEE_AGG);
    bootstrap.current_committee_branch =
        FixtureBranch(ethfx::BOOTSTRAP_COMMITTEE_BRANCH);
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::BOOTSTRAP, bootstrap})};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

//! A kind 2 UPDATE carrying the eth_lc_fixture committee update.
CTransactionRef FixtureUpdateTransaction()
{
    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::UPDATE,
        bridge::BridgeUpdateV1{FixtureCommitteeUpdate()}})};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

bool VerifyFails(const node::BridgeStateIndex& index, const CBlock& block,
                 const int height, const Consensus::Params& params,
                 const std::string& expected,
                 const node::BridgeV2LineageContext* lineage = nullptr)
{
    node::BridgeBlockDelta ignored;
    std::string error;
    if (index.VerifyBlock(block, height, V2TestBlockHash(height), params,
                          ignored, error, lineage)) {
        BOOST_ERROR("bridge block unexpectedly verified at height " << height);
        return false;
    }
    BOOST_CHECK_MESSAGE(error.find(expected) != std::string::npos,
                        "error '" << error << "' lacks '" << expected << "'");
    return error.find(expected) != std::string::npos;
}

// -- Bridge V2 accounting fixtures (kind 9, reserve ledger, gate) ----------

constexpr uint8_t SUBJECT_VERIFIER{bridge::ETH_OBSERVATION_SUBJECT_VERIFIER};
constexpr uint8_t SUBJECT_RESERVE{bridge::ETH_OBSERVATION_SUBJECT_RESERVE};
constexpr uint8_t SUBJECT_TOKEN{bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS};
constexpr uint8_t VERDICT_SETS{bridge::ETH_OBSERVATION_VERDICT_SETS_MISMATCH};
constexpr uint8_t VERDICT_CERT{bridge::ETH_OBSERVATION_VERDICT_CERT_CONFLICT};
//! The B-23 successor's RESERVE_E and TOKEN_FLAGS (0x12 and 0x13).
constexpr uint8_t SUBJECT_RESERVE_NEXT{bridge::ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR};
constexpr uint8_t SUBJECT_TOKEN_NEXT{bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR};
constexpr Consensus::BridgeV2EntryRole ROLE_LAUNCH{Consensus::BridgeV2EntryRole::LAUNCH};
constexpr Consensus::BridgeV2EntryRole ROLE_SUCCESSOR{Consensus::BridgeV2EntryRole::SUCCESSOR};

//! A distinct Ethereum block hash for execution block `number`.
uint256 AnchorHash(const uint64_t number)
{
    return TestBlockHash(static_cast<uint32_t>(70'000 + number));
}

//! Fill every field of a crafted delta that a seed does not change from the
//! connected state, so ConnectBlock's before-state checks pass.
void PassThrough(const node::BridgeStateIndex& index,
                 node::BridgeBlockDelta& delta)
{
    delta.previous_height = index.ConnectedHeight();
    delta.previous_block_hash = index.ConnectedHash();
    delta.withdrawal_tree_before = index.WithdrawalTree();
    delta.withdrawal_tree_after = index.WithdrawalTree();
    delta.withdrawal_tree_v2_before = index.WithdrawalTreeV2();
    delta.withdrawal_tree_v2_after = index.WithdrawalTreeV2();
    delta.observations_v2_before = index.ObservationsV2();
    delta.reserve_v2_before = index.ReserveLedgerV2();
    delta.reserve_v2_after = index.ReserveLedgerV2();
    delta.conflict_v2_before = index.ConflictV2();
    delta.conflict_v2_after = index.ConflictV2();
    delta.reserve_v2_successor_before =
        index.ReserveLedgerV2(Consensus::BridgeV2EntryRole::SUCCESSOR);
    delta.reserve_v2_successor_after = delta.reserve_v2_successor_before;
}

//! One execution block retained as a V2 anchor by a fixture seed.
struct StateAnchorSpec {
    uint64_t block_number{0};
    uint256 block_hash{};
    uint64_t timestamp{0};
    uint256 state_root{};
    uint256 receipts_root{TestHash(0x7e)};
};

StateAnchorSpec StateAnchor(const uint64_t number, const uint64_t timestamp,
                            const uint256& state_root)
{
    return {number, AnchorHash(number), timestamp, state_root};
}

/**
 * Connect a crafted V2-store delta at the next height that retains
 * `anchors` and, when `head` is given, makes it the V2 light client's
 * finalized execution header (a fixture shortcut, like SeedVerifiedAnchorAt).
 */
node::BridgeBlockDelta SeedStateAnchorsV2(
    node::BridgeStateIndex& index, const std::vector<StateAnchorSpec>& anchors,
    const std::optional<StateAnchorSpec>& head = std::nullopt)
{
    const int height{index.ConnectedHeight() + 1};
    node::BridgeBlockDelta seed;
    seed.height = height;
    seed.block_hash = V2TestBlockHash(height);
    seed.v2_store = true;
    PassThrough(index, seed);
    if (head) {
        bridge::LightClientStore store;
        store.finalized_header.beacon.slot = 32;
        store.finalized_header.execution.block_number = head->block_number;
        store.finalized_header.execution.block_hash = head->block_hash;
        store.finalized_header.execution.receipts_root = head->receipts_root;
        store.finalized_header.execution.state_root = head->state_root;
        store.finalized_header.execution.timestamp = head->timestamp;
        store.period = bridge::PeriodAtSlot(32);
        store.current.pubkeys.resize(bridge::ssz::SYNC_COMMITTEE_SIZE);
        seed.light_client_before = index.LightClientV2();
        seed.light_client_after = store;
        seed.light_client_connection_before = index.LightClientV2LastConnected();
        seed.light_client_connection_after =
            node::BridgeStateConnection{height, seed.block_hash};
    }
    for (const StateAnchorSpec& spec : anchors) {
        seed.anchors_added.push_back(node::BridgeExecutionAnchor{
            spec.block_number, spec.block_hash, spec.receipts_root, 32,
            spec.block_number, spec.timestamp, height, seed.block_hash,
            spec.state_root});
    }
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(seed, error), error);
    return seed;
}

//! A kind 9 carrier, encoded under the V2 record set.
CTransactionRef ObservationTransaction(
    const bridge::BridgeEthStateObservationV2& observation)
{
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::ETH_STATE_OBSERVATION,
                               observation},
        bridge::BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef Observe(const bridge_test::ObservationWorld& world,
                        const uint8_t subject, const uint256& anchor_hash,
                        const uint8_t verdict = 0)
{
    return ObservationTransaction(world.Observe(subject, anchor_hash, verdict));
}

//! A token that pays and a vault whose USDT balance covers `locked`.
bridge_test::ObservationWorld HealthyWorld(const Consensus::Params& params,
                                           const uint64_t next_deposit_id = 0,
                                           const CAmount locked = 0)
{
    bridge_test::ObservationWorld world{
        bridge::ObservationPinsFromParams(*params.bridge_v2)};
    world.SetTokenFlags(false, false, false);
    world.SetReserve(next_deposit_id, locked, 0, locked);
    return world;
}

/**
 * A fresh finality lineage at epoch `epoch` whose committees meet
 * V2Params' Verifier V2 bounds (four members of weight 300, each epoch's set
 * the same members stamped with its epoch), genesis G = 89 (R_f = 90) on the
 * default checkpoint interval 10 (checkpoints 99, 109, 119, ... at indices
 * 1, 2, 3, ...), finalized at 109 in epoch `epoch`, and a hash_at matching
 * V2TestBlockHash, the block hashes ConnectNext connects.
 */
//! The chain domain of every fixture (BridgeParams' genesis and X hashes).
uint256 FixtureChainDomain()
{
    const auto domain{modern::ModernChainDomain(TestHash(1), TestHash(2))};
    BOOST_REQUIRE(domain);
    return *domain;
}

//! Four BLS keys from IKM byte 0 = 1..4 and byte 31 = `tag` (0x5b: the
//! LineageFixture members; any other tag: keys no committee holds).
std::vector<bls::SecretKey> FixtureBlsKeys(const unsigned char tag)
{
    std::vector<bls::SecretKey> keys;
    for (unsigned char i{0}; i < 4; ++i) {
        std::array<unsigned char, 32> ikm{};
        ikm[0] = static_cast<unsigned char>(i + 1);
        ikm[31] = tag;
        const auto key{bls::SecretKey::FromIKM(ikm)};
        BOOST_REQUIRE(key);
        keys.push_back(*key);
    }
    return keys;
}

struct LineageFixture {
    static constexpr int GENESIS{89};
    //! LineageParams' checkpoint interval: checkpoints 99, 109, 119, ...
    static constexpr int INTERVAL{10};
    //! Every committee is the same four members.
    static constexpr uint16_t MEMBERS{4};

    std::vector<std::shared_ptr<const node::FreshLineageSet>> sets;
    node::BridgeV2LineageContext context;
    //! keys[i] is member i's BLS key (members sort by validator key, and
    //! member i's key byte 0 is i + 1).
    std::vector<bls::SecretKey> keys{FixtureBlsKeys(0x5b)};

    explicit LineageFixture(const uint64_t epoch = 1)
    {
        node::FinalityBindingIndex bindings;
        std::map<node::ValidatorKey, CAmount> weights;
        std::vector<node::FinalityBindingIndex::Transition> transitions;
        for (unsigned char i{0}; i < MEMBERS; ++i) {
            modern::ValidatorKeyBytes validator{};
            validator[0] = static_cast<unsigned char>(i + 1);
            validator[31] = 0x5c;
            transitions.push_back(
                {validator, {keys[i].GetPublicKey().Compressed(), 0, 1}});
            weights[validator] = 300 * modern::FINALITY_WEIGHT_UNIT;
        }
        bindings.ConnectBlock(1, transitions);
        const auto snapshot{node::ValidatorSetSnapshot::Build(0, weights, bindings)};
        BOOST_REQUIRE(snapshot);
        const auto members{
            std::make_shared<const node::ValidatorSetSnapshot>(*snapshot)};
        for (uint64_t e{0}; e <= epoch + 1; ++e) {
            sets.push_back(node::MakeFreshLineageSet(members, e));
        }

        node::FreshLineageState fresh;
        fresh.bootstrapped = true;
        fresh.genesis_height = GENESIS;
        fresh.genesis_block_hash = V2TestBlockHash(GENESIS);
        fresh.lineage_id = TestHash(0x1d);
        fresh.epoch = epoch;
        for (uint64_t e{0}; e <= epoch; ++e) {
            fresh.epoch_starts.push_back(GENESIS + 1 + static_cast<int>(e) * 10);
        }
        fresh.genesis = sets[0];
        fresh.previous2 = epoch >= 2 ? sets[epoch - 2] : nullptr;
        fresh.previous = epoch >= 1 ? sets[epoch - 1] : nullptr;
        fresh.current = sets[epoch];
        fresh.next = sets[epoch + 1];
        fresh.handover_certified = true;
        fresh.finalized = node::FreshFinalizedCheckpoint{
            .height = 109, .block_hash = V2TestBlockHash(109), .epoch = epoch,
            .checkpoint_index = 2, .history_root = {}, .cert_id = {},
            .certified_at = 110};
        context.fresh = std::move(fresh);
        context.hash_at = [](const int h) -> std::optional<uint256> {
            if (h < 0) return std::nullopt;
            return V2TestBlockHash(h);
        };
        // The finalized checkpoint records B3's history root at its index,
        // as ApplyFreshCertificate does (JudgeFinalityConflictV2 reads it).
        const auto finalized_root{node::FreshHistoryRootAt(
            *context.fresh, context.fresh->finalized->checkpoint_index,
            context.hash_at, INTERVAL)};
        BOOST_REQUIRE(finalized_root);
        context.fresh->finalized->history_root = *finalized_root;
    }

    node::FreshLineageState& Fresh() { return *context.fresh; }

    //! B3's canonical epoch-`e` statement at checkpoint `height`.
    modern::FinalizedBlockV2 Statement(const uint64_t e, const int height,
                                       const Consensus::Params& params,
                                       const node::BridgeStateIndex& index) const
    {
        const auto fb{node::ExpectedFinalizedBlockV2(
            e, height, *context.fresh, context.hash_at, params, &index)};
        BOOST_REQUIRE(fb);
        return *fb;
    }

    //! A LIVE verifier at epoch `e` that accepted B3's statement at
    //! checkpoint `height` and rotated at `rotation_time`.
    bridge::VerifierStateV2 Canonical(const uint64_t e, const int height,
                                      const Consensus::Params& params,
                                      const node::BridgeStateIndex& index,
                                      const int64_t rotation_time = 1'000) const
    {
        const modern::FinalizedBlockV2 fb{Statement(e, height, params, index)};
        bridge::VerifierStateV2 v;
        v.current_epoch = e;
        v.last_rotation_time = rotation_time;
        v.last_certificate_time = rotation_time;
        v.status = bridge::VERIFIER_STATUS_LIVE;
        v.current_set_hash = sets.at(e)->set_hash;
        v.next_set_hash = sets.at(e + 1)->set_hash;
        v.latest_height = static_cast<uint64_t>(height);
        v.latest_index = fb.checkpoint_index;
        v.latest_init_time = 900;
        v.latest_cert_id = modern::FinalityCertIdV2(fb);
        return v;
    }

    //! A digest-V2 certificate over `fb` for a committee of MEMBERS: member
    //! i of `signers` signs with signing_keys[i] (the fixture's own keys
    //! sign as the committee, other keys forge).
    static modern::FinalityCertificate Certificate(
        const modern::FinalizedBlockV2& fb, const std::vector<uint32_t>& signers,
        const std::vector<bls::SecretKey>& signing_keys)
    {
        const uint256 digest{modern::FinalityDigestV2(FixtureChainDomain(), fb)};
        modern::FinalityCertificate cert;
        cert.signer_bitmap.assign(modern::SignerBitmapBytes(MEMBERS), 0);
        std::vector<bls::Signature> signatures;
        for (const uint32_t i : signers) {
            BOOST_REQUIRE(i < MEMBERS);
            cert.signer_bitmap[i / 8] |= static_cast<unsigned char>(1U << (i % 8));
            signatures.push_back(signing_keys.at(i).Sign(
                std::span<const unsigned char>(digest.begin(), 32)));
        }
        const auto aggregate{bls::AggregateSignatures(signatures)};
        BOOST_REQUIRE(aggregate);
        std::copy(aggregate->Compressed().begin(), aggregate->Compressed().end(),
                  cert.aggregate_sig.begin());
        return cert;
    }

    //! Certificate() by all four members.
    modern::FinalityCertificate Signed(const modern::FinalizedBlockV2& fb) const
    {
        return Certificate(fb, {0, 1, 2, 3}, keys);
    }
};

//! A kind 13 body: `fb` and `cert` for a committee of `validator_count`.
bridge::BridgeFinalityConflictV2 ConflictRecord(
    const modern::FinalizedBlockV2& fb, const modern::FinalityCertificate& cert,
    const uint16_t validator_count = LineageFixture::MEMBERS)
{
    return {validator_count, modern::EncodeCertificatePayloadV2(fb, cert)};
}

//! A kind 13 carrier, encoded under the V2 record set.
CTransactionRef ConflictTransaction(
    const bridge::BridgeFinalityConflictV2& conflict)
{
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::FINALITY_CONFLICT,
                               conflict},
        bridge::BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout = {*binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

//! B3's epoch-`epoch` statement at checkpoint `height` with another block
//! hash (a statement of another branch), as `lineage` would sign it.
modern::FinalizedBlockV2 OtherBranch(const LineageFixture& lineage,
                                     const uint64_t epoch, const int height,
                                     const Consensus::Params& params,
                                     const node::BridgeStateIndex& index,
                                     const uint8_t tag = 0xbb)
{
    modern::FinalizedBlockV2 fb{lineage.Statement(epoch, height, params, index)};
    fb.block_hash = TestHash(tag);
    return fb;
}

//! V2Params plus a fresh-lineage configuration matching LineageFixture.
Consensus::Params LineageParams()
{
    Consensus::Params params{V2Params()};
    params.fresh_lineage = Consensus::FreshLineageParams{
        .activation_height = LineageFixture::GENESIS + 1,
        .max_epoch_extension = 1'000,
        .staging_signer = false};
    return params;
}

/**
 * An index through V - 1 whose V2 light client (finalized at execution block
 * 800, timestamp 1,000) retains anchor 800 carrying `state_root`.
 */
node::BridgeStateIndex ObservationIndex(const Consensus::Params& params,
                                        const uint256& state_root)
{
    node::BridgeStateIndex index;
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 2);
    const StateAnchorSpec anchor{StateAnchor(800, 1'000, state_root)};
    SeedStateAnchorsV2(index, {anchor}, anchor);
    return index;
}

//! Receipt fixture of the funded scenarios: deposits 0, 1, 2 of 60, 40, 30.
ReceiptFixture FundingReceipts(const Consensus::Params& params)
{
    return MakeV2ReceiptFixture(params,
                                {{0, 60, 0x31}, {1, 40, 0x32}, {2, 30, 0x33}});
}

/**
 * An index through V - 1 whose V2 light client is finalized at the funding
 * receipts' block (700, timestamp 1,000), retained as an anchor that also
 * carries `state_root` for kind 9.
 */
node::BridgeStateIndex FundingIndex(const Consensus::Params& params,
                                    const ReceiptFixture& receipts,
                                    const uint256& state_root)
{
    node::BridgeStateIndex index;
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 2);
    const StateAnchorSpec anchor{receipts.block_number, receipts.block_hash,
                                 receipts.timestamp, state_root,
                                 receipts.receipts_root};
    SeedStateAnchorsV2(index, {anchor}, anchor);
    return index;
}

//! The bridge V2 accounting of two indexes, field for field.
void CheckSameAccounting(const node::BridgeStateIndex& a,
                         const node::BridgeStateIndex& b)
{
    BOOST_CHECK(a.ObservationsV2() == b.ObservationsV2());
    BOOST_CHECK(a.ReserveLedgerV2() == b.ReserveLedgerV2());
    BOOST_CHECK(a.ProvenDepositsV2() == b.ProvenDepositsV2());
    BOOST_CHECK_EQUAL(a.ProvenDepositsTotalV2(), b.ProvenDepositsTotalV2());
    // The running total is the map's sum.
    CAmount total{0};
    for (const auto& [id, amount] : a.ProvenDepositsV2()) total += amount;
    BOOST_CHECK_EQUAL(a.ProvenDepositsTotalV2(), total);
    BOOST_CHECK(a.ConflictV2() == b.ConflictV2());
    BOOST_CHECK_EQUAL(a.ConnectedHeight(), b.ConnectedHeight());
    BOOST_CHECK(a.ConnectedHash() == b.ConnectedHash());
    BOOST_CHECK(a.WithdrawalTreeV2() == b.WithdrawalTreeV2());
    // The B-23 successor's ledger and deposits (node changes section 7.6).
    constexpr auto SUCCESSOR{Consensus::BridgeV2EntryRole::SUCCESSOR};
    BOOST_CHECK(a.ReserveLedgerV2(SUCCESSOR) == b.ReserveLedgerV2(SUCCESSOR));
    BOOST_CHECK(a.ProvenDepositsV2(SUCCESSOR) == b.ProvenDepositsV2(SUCCESSOR));
    BOOST_CHECK_EQUAL(a.ProvenDepositsTotalV2(SUCCESSOR),
                      b.ProvenDepositsTotalV2(SUCCESSOR));
    CAmount successor_total{0};
    for (const auto& [id, amount] : a.ProvenDepositsV2(SUCCESSOR)) {
        successor_total += amount;
    }
    BOOST_CHECK_EQUAL(a.ProvenDepositsTotalV2(SUCCESSOR), successor_total);
}

void CheckSameV2Delta(const node::BridgeBlockDelta& a,
                      const node::BridgeBlockDelta& b)
{
    BOOST_CHECK(a.observations_v2_before == b.observations_v2_before);
    BOOST_CHECK(a.observations_v2_added == b.observations_v2_added);
    BOOST_CHECK(a.reserve_v2_before == b.reserve_v2_before);
    BOOST_CHECK(a.reserve_v2_after == b.reserve_v2_after);
    BOOST_CHECK(a.deposits_v2_added == b.deposits_v2_added);
    BOOST_CHECK(a.conflict_v2_before == b.conflict_v2_before);
    BOOST_CHECK(a.conflict_v2_after == b.conflict_v2_after);
    BOOST_CHECK(a.finality_conflicts_v2_added == b.finality_conflicts_v2_added);
    BOOST_CHECK(a.withdrawals_v2_added == b.withdrawals_v2_added);
    BOOST_CHECK(a.reserve_v2_successor_before == b.reserve_v2_successor_before);
    BOOST_CHECK(a.reserve_v2_successor_after == b.reserve_v2_successor_after);
    BOOST_CHECK(a.deposits_v2_successor_added == b.deposits_v2_successor_added);
}

void Disconnect(node::BridgeStateIndex& index)
{
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(index.ConnectedHeight(),
                                                index.ConnectedHash(), error),
                          error);
}

// -- B-23 migration fixtures (node changes section 7.6) ---------------------

//! The migration height of the migration fixtures: V + 5, inside the
//! LineageFixture's C3 window (finalized at 109, a lag of at most 90).
constexpr int MIGRATION_HEIGHT{BRIDGE_V2_HEIGHT + 5};

//! The successor vault v' of the migration fixtures.
Consensus::BridgeEthAddress SuccessorVault() { return FilledAddress(0x46); }

/**
 * `params` with a B-23 successor v' (node changes section 7.6): vault
 * 0x46.. on the launch entry's chain, token and caps, with a code hash of
 * its own (required on chain 1), predecessor v, v's bUSD V2 AssetId under
 * the fixture chain domain and `migration_height`. The shared per-epoch cap
 * is raised to 1,000 so the scenarios' mints of one epoch fit.
 */
Consensus::Params WithSuccessor(Consensus::Params params,
                                const int migration_height = MIGRATION_HEIGHT)
{
    Consensus::BridgeV2Params& v2{*params.bridge_v2};
    v2.vault.mint_caps.max_per_epoch = 1'000;
    Consensus::BridgeV2SuccessorEntry successor;
    successor.entry = v2.vault;
    successor.entry.vault_address = SuccessorVault();
    successor.entry.vault_runtime_code_hash = TestHash(54);
    successor.predecessor = v2.vault.vault_address;
    successor.asset_id = modern::BridgeAssetIdV2(FixtureChainDomain(), v2.vault);
    successor.migration_height = migration_height;
    v2.successor = successor;
    BOOST_REQUIRE(v2.Valid());
    BOOST_REQUIRE(modern::ConfiguredBridgeV2AssetId(params));
    BOOST_REQUIRE(modern::ConfiguredBridgeV2EntryId(params, ROLE_SUCCESSOR));
    BOOST_REQUIRE(!params.BridgeV2MigrationActive(migration_height - 1));
    BOOST_REQUIRE(params.BridgeV2MigrationActive(migration_height));
    return params;
}

//! LineageParams with the successor.
Consensus::Params MigrationParams() { return WithSuccessor(LineageParams()); }

//! One receipt (block 700) holding v's deposits 0, 1, 2 of 60, 40, 30
//! (logs 0-2) and v''s deposits 0, 1 of 50, 20 (logs 3-4).
ReceiptFixture MigrationReceipts(const Consensus::Params& params)
{
    const Consensus::BridgeV2Params& v2{*params.bridge_v2};
    const Consensus::BridgeEthAddress& next{v2.successor->entry.vault_address};
    return MakeReceiptFixture(v2.vault.vault_address, v2.vault.token_address,
                              {{0, 60, 0x31}, {1, 40, 0x32}, {2, 30, 0x33},
                               {0, 50, 0x34, next}, {1, 20, 0x35, next}});
}

/**
 * The migration scenarios' Ethereum state at anchor 700: v after deposits 0
 * and 1 (cut 2, locked and balance 100), v' holding nothing and not
 * blacklisted, a token that pays, and a LIVE verifier that accepted B3's
 * statement at checkpoint 109.
 */
bridge_test::ObservationWorld MigrationWorld(const Consensus::Params& params,
                                             const ReceiptFixture& receipts,
                                             const LineageFixture& lineage)
{
    bridge_test::ObservationWorld world{HealthyWorld(params, 2, 100)};
    world.AddSuccessorVault(SuccessorVault(), TestHash(54));
    world.SetSuccessorReserve(0, 0, 0, 0);
    world.SetSuccessorBlacklisted(false);
    const node::BridgeStateIndex probe{
        FundingIndex(params, receipts, world.StateRoot())};
    world.SetVerifier(lineage.Canonical(1, 109, params, probe));
    return world;
}

/**
 * FundingIndex on `world`, then block V: v's mints of deposits 0 (60) and 1
 * (40) and subjects 3, 2 and 1 observed at anchor 700, judged by `lineage`.
 */
node::BridgeStateIndex MigrationIndex(const Consensus::Params& params,
                                      const ReceiptFixture& receipts,
                                      const bridge_test::ObservationWorld& world,
                                      const LineageFixture& lineage)
{
    node::BridgeStateIndex index{FundingIndex(params, receipts, world.StateRoot())};
    ConnectNext(index, params,
                {MintTransactionV2(params, receipts, 0, 60),
                 MintTransactionV2(params, receipts, 1, 40),
                 Observe(world, SUBJECT_TOKEN, receipts.block_hash),
                 Observe(world, SUBJECT_RESERVE, receipts.block_hash),
                 Observe(world, SUBJECT_VERIFIER, receipts.block_hash)},
                1'060, &lineage.context);
    BOOST_REQUIRE_EQUAL(index.ConnectedHeight(), BRIDGE_V2_HEIGHT);
    BOOST_REQUIRE_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 100);
    return index;
}

/**
 * A kind 16 VAULT_SWEEP carrier naming `registry` (default: the launch
 * entry id), shaped as submitbridgecarrier builds one: a payment at output
 * 0 and the binding at output 1. `source_tag` makes its txid distinct.
 */
CTransactionRef SweepTransactionV2(const Consensus::Params& params,
                                   const uint8_t source_tag,
                                   const std::optional<uint256>& registry = std::nullopt)
{
    const auto entry{modern::ConfiguredBridgeV2EntryId(params, ROLE_LAUNCH)};
    BOOST_REQUIRE(entry);
    const auto record{bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::VAULT_SWEEP,
                               bridge::BridgeVaultSweepV2{registry.value_or(*entry)}},
        bridge::BRIDGE_MPT_LIMITS_V2)};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(TestHash(source_tag)), 0};
    tx.vout = {CTxOut{1'000, CScript{} << OP_TRUE}, *binding};
    tx.mpa = {*record};
    return MakeTransactionRef(std::move(tx));
}

//! The sweep leaf consensus derives: v pays v' `amount` as leaf `id` at
//! `height` (modern::MakeVaultSweepWithdrawalV2).
modern::BridgeWithdrawalV2 ExpectedSweep(const Consensus::Params& params,
                                         const uint64_t id, const CAmount amount,
                                         const int height)
{
    const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
    BOOST_REQUIRE(asset);
    return modern::MakeVaultSweepWithdrawalV2(params.bridge_v2->vault, *asset,
                                              SuccessorVault(), id, amount,
                                              static_cast<uint64_t>(height));
}

//! A kind 6 burn of `amount` naming the successor entry.
CTransactionRef SuccessorBurn(const Consensus::Params& params,
                              const uint8_t source_tag, const CAmount amount,
                              const uint8_t recipient_tag)
{
    const auto entry{modern::ConfiguredBridgeV2EntryId(params, ROLE_SUCCESSOR)};
    BOOST_REQUIRE(entry);
    return BurnTransactionV2(params, source_tag, amount, recipient_tag, *entry);
}

//! VerifyBlock of `transactions` at the index's next height, which must
//! pass; the index is unchanged.
node::BridgeBlockDelta Verifies(const node::BridgeStateIndex& index,
                                const Consensus::Params& params,
                                std::vector<CTransactionRef> transactions,
                                const uint32_t time,
                                const node::BridgeV2LineageContext* lineage)
{
    const int height{index.ConnectedHeight() + 1};
    node::BridgeBlockDelta delta;
    std::string error;
    BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(time, std::move(transactions)),
                                          height, V2TestBlockHash(height), params,
                                          delta, error, lineage),
                        error);
    return delta;
}

} // namespace

BOOST_AUTO_TEST_SUITE(bridge_state_tests)

BOOST_AUTO_TEST_CASE(managed_withdrawal_requires_the_exact_named_burn)
{
    Consensus::Params params{FullyActiveBridgeParams(false)};
    params.bridge_withdrawal_activation_height.reset();
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
    BOOST_CHECK(!index.VerifyBlock(block, BRIDGE_HEIGHT, TestHash(9), params,
                                   delta, error));
    BOOST_CHECK(error.find("withdrawal rules are not active") !=
                std::string::npos);
    BOOST_CHECK(Consensus::BridgeMintParamsReady(*params.busd_bridge));
    BOOST_CHECK(!params.bridge_withdrawal_activation_height);
    const auto registry_before_activation{
        modern::ConfiguredBridgeRegistryId(params)};
    params.bridge_withdrawal_activation_height = BRIDGE_HEIGHT;
    BOOST_CHECK(modern::ConfiguredBridgeRegistryId(params) ==
                registry_before_activation);
    delta = {};
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
    BOOST_CHECK(*vector_leaf ==
                Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1);

    const Consensus::Params params{FullyActiveBridgeParams(true)};
    const auto asset{modern::ConfiguredBridgeAssetId(params)};
    const auto registry{modern::ConfiguredBridgeRegistryId(params)};
    BOOST_REQUIRE(asset);
    BOOST_REQUIRE(registry);

    // B starts the canonical cumulative root even when W is unset. Ethereum
    // requires a nonzero root in post-B certificates before it will accept
    // deposits; the empty root is safe because no withdrawal leaf exists.
    Consensus::Params inbound_only{params};
    inbound_only.bridge_withdrawal_activation_height.reset();
    node::BridgeStateIndex inbound_index;
    node::BridgeBlockDelta inbound_delta;
    std::string inbound_error;
    BOOST_REQUIRE_MESSAGE(inbound_index.VerifyBlock(
                              Block(1'000, {}), BRIDGE_HEIGHT, TestHash(39),
                              inbound_only, inbound_delta, inbound_error),
                          inbound_error);
    BOOST_REQUIRE_MESSAGE(inbound_index.ConnectBlock(inbound_delta,
                                                     inbound_error),
                          inbound_error);
    const uint256 canonical_empty_root{
        modern::WithdrawalZeroHashes()[modern::WITHDRAWAL_TREE_DEPTH]};
    BOOST_CHECK(!canonical_empty_root.IsNull());
    BOOST_CHECK(node::FinalityWithdrawalRoot(
                    BRIDGE_HEIGHT, inbound_only, &inbound_index) ==
                std::optional<uint256>{canonical_empty_root});

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
    BOOST_CHECK(node::HasDecentralizedBridgeBurn(*first));
    BOOST_CHECK(node::HasDecentralizedBridgeBurn(
        Block(1'000, {first})));
    BOOST_CHECK(!node::CheckBridgeBurnReadiness(
        *first, node::BridgeBurnReadiness::NOT_READY, error));
    BOOST_CHECK(error.find("current and next validator sets") !=
                std::string::npos);
    error.clear();
    BOOST_CHECK(!node::CheckBridgeBurnReadiness(
        Block(1'000, {first}),
        node::BridgeBurnReadiness::UNAVAILABLE, error));
    BOOST_CHECK(error.find("unavailable") != std::string::npos);
    error.clear();
    BOOST_CHECK(node::CheckBridgeBurnReadiness(
        *first, node::BridgeBurnReadiness::READY, error));
    CMutableTransaction ordinary;
    ordinary.vout.emplace_back(1, CScript{} << OP_TRUE);
    BOOST_CHECK(node::CheckBridgeBurnReadiness(
        CTransaction{ordinary}, node::BridgeBurnReadiness::NOT_READY,
        error));
    inbound_only = params;
    inbound_only.bridge_withdrawal_activation_height.reset();
    node::BridgeBlockDelta disabled_delta;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'000, {first}), BRIDGE_HEIGHT, block_hash, inbound_only,
        disabled_delta, error));
    BOOST_CHECK(error.find("withdrawal rules are not active") !=
                std::string::npos);
    error.clear();
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
    const node::BridgeWithdrawalId first_source{first->GetHash(), 0};
    const node::BridgeWithdrawalId second_source{second->GetHash(), 0};
    const auto first_by_source{
        index.DecentralizedWithdrawal(first_source)};
    BOOST_REQUIRE(first_by_source);
    BOOST_CHECK_EQUAL(first_by_source->withdrawal.withdrawal_id, 0U);
    BOOST_CHECK(first_by_source->leaf ==
                delta.decentralized_withdrawals_added[0].leaf);
    const auto second_by_source{
        index.DecentralizedWithdrawal(second_source)};
    BOOST_REQUIRE(second_by_source);
    BOOST_CHECK_EQUAL(second_by_source->withdrawal.withdrawal_id, 1U);
    BOOST_CHECK(!index.DecentralizedWithdrawal(
        node::BridgeWithdrawalId{first->GetHash(), 1}));
    BOOST_CHECK(index.WithdrawalRootAtHeight(BRIDGE_HEIGHT) ==
                std::optional<uint256>{expected.root});
    const auto finalized_prefix{
        index.DecentralizedWithdrawalsThrough(BRIDGE_HEIGHT)};
    BOOST_REQUIRE(finalized_prefix);
    BOOST_REQUIRE_EQUAL(finalized_prefix->size(), 2U);
    BOOST_CHECK(finalized_prefix->at(0) ==
                delta.decentralized_withdrawals_added[0]);
    BOOST_CHECK(!index.DecentralizedWithdrawalsThrough(BRIDGE_HEIGHT + 1));

    // The finality field is zero before inbound bridge activation and in
    // managed mode. From B onward, even before W, it comes from the exact
    // active-chain index; an unavailable index fails closed.
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
    BOOST_REQUIRE(index.DecentralizedWithdrawalsThrough(BRIDGE_HEIGHT + 1));
    BOOST_CHECK_EQUAL(
        index.DecentralizedWithdrawalsThrough(BRIDGE_HEIGHT + 1)->size(),
        2U);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(BRIDGE_HEIGHT + 1,
                                                empty_hash, error),
                          error);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(BRIDGE_HEIGHT, block_hash,
                                                error),
                          error);
    BOOST_CHECK_EQUAL(index.DecentralizedWithdrawalCount(), 0U);
    BOOST_CHECK(!index.DecentralizedWithdrawal(first_source));
    BOOST_CHECK(!index.DecentralizedWithdrawal(second_source));
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
    const auto& nullifier{
        delta.mint_authorizations.front().authorization.nullifier};
    BOOST_CHECK(index.IsNullified(nullifier));
    const auto first_connection{index.NullifierConnection(nullifier)};
    BOOST_REQUIRE(first_connection);
    BOOST_CHECK_EQUAL(first_connection->height, BRIDGE_HEIGHT);
    BOOST_CHECK(first_connection->block_hash == TestHash(10));
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
    BOOST_CHECK(!index.IsNullified(nullifier));
    BOOST_CHECK(!index.NullifierConnection(nullifier));
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset, 0), 0);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset, 0), 60);
    const auto reconnected{index.NullifierConnection(nullifier)};
    BOOST_REQUIRE(reconnected);
    BOOST_CHECK_EQUAL(reconnected->height, BRIDGE_HEIGHT);
    BOOST_CHECK(reconnected->block_hash == TestHash(10));

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
    const Consensus::Params params{FullyActiveBridgeParams(false)};
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

BOOST_AUTO_TEST_CASE(mint_rejects_predeployment_execution_target)
{
    Consensus::Params params{BridgeParams()};
    params.busd_bridge->origin_deployment_block = 701;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(*params.busd_bridge));
    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{10, 60, 0x45}})};
    BOOST_REQUIRE_EQUAL(fixture.block_number + 1,
                        *params.busd_bridge->origin_deployment_block);
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);
    const CTransactionRef mint{
        MintTransactionWithAmount(params, fixture, 0, 60)};
    node::BridgeBlockDelta ignored;
    std::string error;
    BOOST_CHECK(!index.VerifyBlock(Block(1'001, {mint}), BRIDGE_HEIGHT,
                                   TestHash(65), params, ignored, error));
    BOOST_CHECK(error.find("deployment") != std::string::npos);
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

BOOST_AUTO_TEST_CASE(older_committee_update_changes_only_the_proven_committee)
{
    const Consensus::Params params{FixtureBridgeParams()};
    const bridge::LightClientUpdate update{FixtureCommitteeUpdate()};
    bridge::LightClientStore initial{FixtureBootstrapStore()};

    // The trusted store is newer than the canonical committee update but is
    // still in that update's period, matching the production checkpoint.
    initial.finalized_header.beacon.slot = update.attested.beacon.slot + 1;
    BOOST_REQUIRE_EQUAL(
        bridge::PeriodAtSlot(initial.finalized_header.beacon.slot),
        initial.period);
    BOOST_REQUIRE(!initial.next);

    node::BridgeStateIndex index;
    node::BridgeBlockDelta seed;
    seed.height = BRIDGE_HEIGHT - 1;
    seed.block_hash = TestHash(80);
    seed.light_client_after = initial;
    seed.light_client_connection_after =
        node::BridgeStateConnection{seed.height, seed.block_hash};
    seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        initial.finalized_header.execution.block_number,
        initial.finalized_header.execution.block_hash,
        initial.finalized_header.execution.receipts_root,
        initial.finalized_header.beacon.slot,
        initial.finalized_header.execution.block_number,
        initial.finalized_header.execution.timestamp, seed.height,
        seed.block_hash});
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(seed, error), error);
    BOOST_REQUIRE_EQUAL(index.AnchorCount(), 1U);

    const auto record{bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
        bridge::BridgeRecordKindV1::UPDATE,
        bridge::BridgeUpdateV1{update}})};
    BOOST_REQUIRE(record);
    const auto binding{modern::MakeBridgeBindingOutput(*record)};
    BOOST_REQUIRE(binding);
    CMutableTransaction mutable_tx;
    mutable_tx.vout.push_back(*binding);
    mutable_tx.mpa.push_back(*record);
    const CTransactionRef transaction{
        MakeTransactionRef(std::move(mutable_tx))};

    const uint256 update_block{TestHash(81)};
    node::BridgeBlockDelta delta;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
                              Block(1'000, {transaction}), BRIDGE_HEIGHT,
                              update_block, params, delta, error),
                          error);
    BOOST_REQUIRE(delta.light_client_before);
    BOOST_REQUIRE(delta.light_client_after);
    BOOST_CHECK(delta.light_client_before->finalized_header ==
                initial.finalized_header);
    BOOST_CHECK(delta.light_client_after->finalized_header ==
                initial.finalized_header);
    BOOST_CHECK_EQUAL(delta.light_client_after->period, initial.period);
    BOOST_CHECK(delta.light_client_after->current.HashTreeRoot() ==
                initial.current.HashTreeRoot());
    BOOST_REQUIRE(delta.light_client_after->next);
    BOOST_CHECK(delta.light_client_after->next->HashTreeRoot() ==
                update.next_committee.HashTreeRoot());
    BOOST_CHECK(delta.anchors_added.empty());

    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_REQUIRE(index.LightClient());
    BOOST_REQUIRE(index.LightClient()->next);
    BOOST_CHECK(index.LightClient()->finalized_header ==
                initial.finalized_header);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);

    node::BridgeBlockDelta replay;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'001, {transaction}), BRIDGE_HEIGHT + 1, TestHash(82), params,
        replay, error));
    BOOST_CHECK(error.find("light-client update is invalid") !=
                std::string::npos);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(index.LightClient()->finalized_header ==
                initial.finalized_header);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);

    BOOST_REQUIRE_MESSAGE(
        index.DisconnectBlock(BRIDGE_HEIGHT, update_block, error), error);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(*index.LightClient() == initial);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);
}

BOOST_AUTO_TEST_CASE(light_client_connection_metadata_tracks_store_changes_and_undo)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{21, 60, 0x56}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);
    const auto original_connection{index.LightClientLastConnected()};
    BOOST_REQUIRE(original_connection);
    BOOST_CHECK_EQUAL(original_connection->height, BRIDGE_HEIGHT - 1);
    BOOST_CHECK(original_connection->block_hash == TestHash(8));
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(!index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 2));
    const auto original_snapshot{
        index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 1)};
    BOOST_REQUIRE(original_snapshot);
    BOOST_CHECK(original_snapshot->connection == *original_connection);
    BOOST_CHECK(!original_snapshot->store.next);

    // This represents an accepted next-committee-only update: the finalized
    // head is unchanged, but the persistent light-client store changed.
    bridge::LightClientStore before{*index.LightClient()};
    bridge::LightClientStore after{before};
    after.next = after.current;
    const uint256 update_block{TestHash(57)};
    node::BridgeBlockDelta update;
    update.height = BRIDGE_HEIGHT;
    update.block_hash = update_block;
    update.previous_height = index.ConnectedHeight();
    update.previous_block_hash = index.ConnectedHash();
    update.light_client_before = before;
    update.light_client_after = after;

    std::string error;
    BOOST_CHECK(!index.ConnectBlock(update, error));
    BOOST_CHECK(error.find("connection metadata") != std::string::npos);
    BOOST_CHECK(index.LightClientLastConnected() == original_connection);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(!index.LightClient()->next);

    update.light_client_connection_before = original_connection;
    update.light_client_connection_after =
        node::BridgeStateConnection{update.height, update.block_hash};
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(update, error), error);
    const auto connected{index.LightClientLastConnected()};
    BOOST_REQUIRE(connected);
    BOOST_CHECK_EQUAL(connected->height, BRIDGE_HEIGHT);
    BOOST_CHECK(connected->block_hash == update_block);
    BOOST_REQUIRE(index.LightClient());
    BOOST_REQUIRE(index.LightClient()->next);
    BOOST_CHECK(index.LightClient()->finalized_header ==
                before.finalized_header);

    // The latest update is not yet covered at H-1, but the previous exact
    // store remains exportable instead of making the RPC unavailable.
    const auto finalized_before_update{
        index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 1)};
    BOOST_REQUIRE(finalized_before_update);
    BOOST_CHECK(finalized_before_update->connection == *original_connection);
    BOOST_CHECK(!finalized_before_update->store.next);
    const auto finalized_through_update{
        index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT)};
    BOOST_REQUIRE(finalized_through_update);
    BOOST_CHECK(finalized_through_update->connection == *connected);
    BOOST_REQUIRE(finalized_through_update->store.next);

    BOOST_REQUIRE_MESSAGE(
        index.DisconnectBlock(update.height, update.block_hash, error), error);
    BOOST_CHECK(index.LightClientLastConnected() == original_connection);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(!index.LightClient()->next);
    const auto after_undo{
        index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT)};
    BOOST_REQUIRE(after_undo);
    BOOST_CHECK(after_undo->connection == *original_connection);
    BOOST_CHECK(!after_undo->store.next);
}

BOOST_AUTO_TEST_CASE(finalized_light_client_snapshot_survives_continuous_updates)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{22, 60, 0x57}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);

    std::string error;
    const int update_count{
        static_cast<int>(node::BRIDGE_STATE_UNDO_BLOCKS) + 8};
    for (int i{0}; i < update_count; ++i) {
        BOOST_REQUIRE(index.LightClient());
        BOOST_REQUIRE(index.LightClientLastConnected());
        bridge::LightClientStore before{*index.LightClient()};
        bridge::LightClientStore after{before};
        if (!after.next) {
            after.next = after.current;
        } else {
            after.next->aggregate_pubkey[0] =
                static_cast<unsigned char>(i);
        }

        node::BridgeBlockDelta delta;
        delta.height = index.ConnectedHeight() + 1;
        delta.block_hash = TestBlockHash(
            static_cast<uint32_t>(10'000 + delta.height));
        delta.previous_height = index.ConnectedHeight();
        delta.previous_block_hash = index.ConnectedHash();
        delta.light_client_before = std::move(before);
        delta.light_client_after = std::move(after);
        delta.light_client_connection_before =
            index.LightClientLastConnected();
        delta.light_client_connection_after =
            node::BridgeStateConnection{delta.height, delta.block_hash};
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    }

    BOOST_CHECK_EQUAL(index.History().size(),
                      node::BRIDGE_STATE_UNDO_BLOCKS);
    // Every recent update is newer than this finalized height. The retained
    // floor still yields the exact first store, so continuous updates cannot
    // starve snapshot export after bounded undo history rolls over.
    const auto floor{
        index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 1)};
    BOOST_REQUIRE(floor);
    BOOST_CHECK_EQUAL(floor->connection.height, BRIDGE_HEIGHT - 1);
    BOOST_CHECK(floor->connection.block_hash == TestHash(8));
    BOOST_CHECK(!floor->store.next);

    const int recent_finalized{index.ConnectedHeight() - 5};
    const auto recent{
        index.FinalizedLightClientSnapshot(recent_finalized)};
    BOOST_REQUIRE(recent);
    BOOST_CHECK_EQUAL(recent->connection.height, recent_finalized);

    const int disconnected_height{index.ConnectedHeight()};
    const uint256 disconnected_hash{index.ConnectedHash()};
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              disconnected_height, disconnected_hash, error),
                          error);
    const auto after_undo{
        index.FinalizedLightClientSnapshot(disconnected_height)};
    BOOST_REQUIRE(after_undo);
    BOOST_CHECK_EQUAL(after_undo->connection.height,
                      disconnected_height - 1);

    // Full replay starts from Clear() and deterministically recreates the
    // floor when the bootstrap block is connected again.
    index.Clear();
    BOOST_CHECK(!index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 1));
    SeedVerifiedAnchor(index, fixture);
    const auto replayed{
        index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 1)};
    BOOST_REQUIRE(replayed);
    BOOST_CHECK(replayed->connection.block_hash == TestHash(8));
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              BRIDGE_HEIGHT - 1, TestHash(8), error),
                          error);
    BOOST_CHECK(!index.LightClient());
    BOOST_CHECK(!index.LightClientLastConnected());
    BOOST_CHECK(!index.FinalizedLightClientSnapshot(BRIDGE_HEIGHT - 1));
}

BOOST_AUTO_TEST_CASE(light_client_execution_jump_cannot_strand_deposits)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{23, 60, 0x58}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);
    BOOST_REQUIRE(index.LightClient());
    BOOST_REQUIRE(index.LightClientLastConnected());

    bridge::LightClientStore before{*index.LightClient()};
    bridge::LightClientStore after{before};
    after.finalized_header.beacon.slot += 1;
    after.finalized_header.execution.block_number +=
        bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS + 1;
    after.finalized_header.execution.block_hash = TestHash(66);

    node::BridgeBlockDelta jump;
    jump.height = BRIDGE_HEIGHT;
    jump.block_hash = TestHash(67);
    jump.previous_height = index.ConnectedHeight();
    jump.previous_block_hash = index.ConnectedHash();
    jump.light_client_before = before;
    jump.light_client_after = after;
    jump.light_client_connection_before = index.LightClientLastConnected();
    jump.light_client_connection_after =
        node::BridgeStateConnection{jump.height, jump.block_hash};

    std::string error;
    BOOST_CHECK(!index.ConnectBlock(jump, error));
    BOOST_CHECK(error.find("backfill window") != std::string::npos);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(index.LightClient()->finalized_header ==
                before.finalized_header);
}

BOOST_AUTO_TEST_CASE(cumulative_backfill_limit_blocks_serial_step_bypass)
{
    const Consensus::Params params{BridgeParams()};
    const uint256 receipts_698{TestBlockHash(698)};
    const uint256 receipts_699{TestBlockHash(699)};
    const uint256 receipts_700{TestBlockHash(700)};
    const std::vector<unsigned char> header_698{
        ExecutionHeader(uint256{}, 698, receipts_698, 1'698)};
    const uint256 hash_698{Keccak(header_698)};
    const std::vector<unsigned char> header_699{
        ExecutionHeader(hash_698, 699, receipts_699, 1'699)};
    const uint256 hash_699{Keccak(header_699)};
    const std::vector<unsigned char> header_700{
        ExecutionHeader(hash_699, 700, receipts_700, 1'700)};
    const uint256 hash_700{Keccak(header_700)};

    node::BridgeStateIndex index;
    node::BridgeBlockDelta seed;
    seed.height = BRIDGE_HEIGHT - 1;
    seed.block_hash = TestHash(58);
    seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        700, hash_700, receipts_700, 32,
        699 + bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS, 1'700,
        seed.height, seed.block_hash});
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(seed, error), error);

    // The first one-block step lands exactly 20,000 blocks behind the
    // directly finalized origin and remains valid.
    const CTransactionRef boundary{BackfillTransaction(
        hash_700, 699, {header_700, header_699})};
    node::BridgeBlockDelta boundary_delta;
    const uint256 boundary_block{TestHash(59)};
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(
                              Block(1'700, {boundary}), BRIDGE_HEIGHT,
                              boundary_block, params, boundary_delta, error),
                          error);
    BOOST_REQUIRE_EQUAL(boundary_delta.anchors_added.size(), 1U);
    BOOST_CHECK_EQUAL(
        boundary_delta.anchors_added.front()
            .source_finalized_execution_block,
        699 + bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(boundary_delta, error), error);
    const auto boundary_anchor{index.Anchor(hash_699)};
    BOOST_REQUIRE(boundary_anchor);
    BOOST_CHECK_EQUAL(
        boundary_anchor->source_finalized_execution_block,
        699 + bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS);

    // A second individually-small step would cross the cumulative floor.
    const CTransactionRef bypass{BackfillTransaction(
        hash_699, 698, {header_699, header_698})};
    node::BridgeBlockDelta bypass_delta;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'701, {bypass}), BRIDGE_HEIGHT + 1, TestHash(60), params,
        bypass_delta, error));
    BOOST_CHECK(error.find("cumulative backfill limit") != std::string::npos);
    BOOST_CHECK(!index.Anchor(hash_698));

    BOOST_REQUIRE_MESSAGE(
        index.DisconnectBlock(BRIDGE_HEIGHT, boundary_block, error), error);
    BOOST_CHECK(!index.Anchor(hash_699));
    BOOST_CHECK(index.Anchor(hash_700).has_value());

    node::BridgeStateIndex malformed_index;
    node::BridgeBlockDelta malformed;
    malformed.height = BRIDGE_HEIGHT;
    malformed.block_hash = TestHash(61);
    malformed.anchors_added.push_back(node::BridgeExecutionAnchor{
        700, hash_700, receipts_700, 32,
        700 + bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS + 1, 1'700,
        malformed.height, malformed.block_hash});
    error.clear();
    BOOST_CHECK(!malformed_index.ConnectBlock(malformed, error));
    BOOST_CHECK(error.find("execution anchor") != std::string::npos);
    BOOST_CHECK_EQUAL(malformed_index.AnchorCount(), 0U);
}

BOOST_AUTO_TEST_CASE(anchor_for_target_selects_nearest_b3_finalized_origin)
{
    node::BridgeStateIndex index;
    node::BridgeBlockDelta first;
    first.height = BRIDGE_HEIGHT;
    first.block_hash = TestHash(68);
    first.anchors_added = {
        // This is a valid anchor in isolation, but its directly-finalized
        // origin is one block too far from target 100.
        node::BridgeExecutionAnchor{
            101, TestBlockHash(101), TestBlockHash(201), 32,
            101 + bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS, 1'101,
            first.height, first.block_hash},
        node::BridgeExecutionAnchor{
            102, TestBlockHash(102), TestBlockHash(202), 33, 102, 1'102,
            first.height, first.block_hash},
    };
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(first, error), error);

    node::BridgeBlockDelta second;
    second.height = BRIDGE_HEIGHT + 1;
    second.block_hash = TestHash(69);
    second.previous_height = index.ConnectedHeight();
    second.previous_block_hash = index.ConnectedHash();
    second.anchors_added.push_back(node::BridgeExecutionAnchor{
        100, TestBlockHash(100), TestBlockHash(200), 34, 100, 1'100,
        second.height, second.block_hash});
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(second, error), error);

    BOOST_CHECK(!index.AnchorForTarget(100, -1));
    BOOST_CHECK(!index.AnchorForTarget(100, BRIDGE_HEIGHT - 1));

    // The numerically nearest anchor is not B3-finalized and the next one
    // crosses the cumulative origin window, so selection continues to 102.
    const auto fallback{index.AnchorForTarget(100, BRIDGE_HEIGHT)};
    BOOST_REQUIRE(fallback);
    BOOST_CHECK_EQUAL(fallback->block_number, 102U);
    BOOST_CHECK_EQUAL(fallback->source_finalized_beacon_slot, 33U);
    BOOST_CHECK_EQUAL(fallback->connected_height, BRIDGE_HEIGHT);

    // Once the later B3 connection is finalized, block 100 is nearest.
    const auto nearest{index.AnchorForTarget(100, BRIDGE_HEIGHT + 1)};
    BOOST_REQUIRE(nearest);
    BOOST_CHECK_EQUAL(nearest->block_number, 100U);
    BOOST_CHECK(nearest->connected_block == second.block_hash);

    // Exactly 20,000 blocks from the finalized Ethereum origin is allowed.
    const auto boundary{index.AnchorForTarget(101, BRIDGE_HEIGHT)};
    BOOST_REQUIRE(boundary);
    BOOST_CHECK_EQUAL(boundary->block_number, 101U);
    BOOST_CHECK(!index.AnchorForTarget(103, BRIDGE_HEIGHT + 1));
}

BOOST_AUTO_TEST_CASE(mint_cannot_cross_cumulative_backfill_limit)
{
    const Consensus::Params params{BridgeParams()};
    const ReceiptFixture fixture{
        MakeReceiptFixture(params, {{22, 60, 0x57}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, fixture);

    const uint256 source_receipts{TestHash(62)};
    const std::vector<unsigned char> source_header{ExecutionHeader(
        fixture.block_hash, fixture.block_number + 1, source_receipts,
        fixture.timestamp + 1)};
    const uint256 source_hash{Keccak(source_header)};
    node::BridgeBlockDelta source_delta;
    source_delta.height = BRIDGE_HEIGHT;
    source_delta.block_hash = TestHash(63);
    source_delta.previous_height = index.ConnectedHeight();
    source_delta.previous_block_hash = index.ConnectedHash();
    source_delta.anchors_added.push_back(node::BridgeExecutionAnchor{
        fixture.block_number + 1, source_hash, source_receipts, 32,
        fixture.block_number + 1 +
            bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS,
        fixture.timestamp + 1, source_delta.height, source_delta.block_hash});
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(source_delta, error), error);

    const CTransactionRef mint{MintTransactionWithAncestry(
        params, fixture, source_hash, {source_header, fixture.header})};
    node::BridgeBlockDelta ignored;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(
        Block(1'060, {mint}), BRIDGE_HEIGHT + 1, TestHash(64), params,
        ignored, error));
    BOOST_CHECK(error.find("cumulative backfill limit") != std::string::npos);
    BOOST_CHECK_EQUAL(index.NullifierCount(), 0U);
}

BOOST_AUTO_TEST_CASE(connect_is_atomic_and_recent_undo_is_bounded)
{
    node::BridgeStateIndex index;
    node::BridgeBlockDelta malformed;
    malformed.height = BRIDGE_HEIGHT;
    malformed.block_hash = TestBlockHash(BRIDGE_HEIGHT);
    malformed.anchors_added.push_back(node::BridgeExecutionAnchor{
        700, TestBlockHash(700), TestBlockHash(701), 32, 700, 1'000,
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

// -- Bridge V2 (node changes sections 3-7) ----------------------------------

BOOST_AUTO_TEST_CASE(v2_store_routing)
{
    // Kinds 1 and 2 from V run against the separate V2 store, built here
    // from the real eth_lc_fixture checkpoint and committee update; the V1
    // store, its anchors and its finalized snapshot do not move.
    const uint64_t fixture_block{ethfx::U1_FINALIZED_EXEC.block_number};
    const Consensus::Params params{V2Params(BRIDGE_V2_HEIGHT, fixture_block)};
    const ReceiptFixture v1_fixture{
        MakeReceiptFixture(params, {{1, 60, 0x71}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, v1_fixture);
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 1);
    const auto v1_store{index.LightClient()};
    const auto v1_connection{index.LightClientLastConnected()};
    BOOST_REQUIRE(v1_store);
    BOOST_REQUIRE(v1_connection);
    BOOST_CHECK(!index.LightClientV2());
    BOOST_CHECK_EQUAL(index.AnchorCountV2(), 0U);
    BOOST_CHECK(!index.FinalizedLightClientSnapshotV2(BRIDGE_V2_HEIGHT));

    const CTransactionRef bootstrap{FixtureBootstrapTransaction()};
    const CTransactionRef update{FixtureUpdateTransaction()};
    // The V1 store is bootstrapped, but from V an update needs the V2 one.
    VerifyFails(index, Block(1'060, {update}), BRIDGE_V2_HEIGHT, params,
                "light-client update precedes bootstrap");
    // Below V the same bootstrap is a second V1 bootstrap.
    VerifyFails(index, Block(1'060, {bootstrap}), BRIDGE_V2_HEIGHT - 1,
                params, "already bootstrapped");

    const node::BridgeBlockDelta boot_delta{
        ConnectNext(index, params, {bootstrap})};
    BOOST_CHECK(boot_delta.v2_store);
    BOOST_CHECK(!boot_delta.light_client_before);
    BOOST_REQUIRE(boot_delta.light_client_after);
    BOOST_REQUIRE_EQUAL(boot_delta.anchors_added.size(), 1U);
    const uint256 v2_anchor_hash{
        FixtureHex32(ethfx::U1_FINALIZED_EXEC.block_hash)};
    BOOST_CHECK(boot_delta.anchors_added.front().block_hash == v2_anchor_hash);

    BOOST_REQUIRE(index.LightClientV2());
    BOOST_CHECK(*index.LightClientV2() == FixtureBootstrapStore());
    const node::BridgeStateConnection boot_connection{
        BRIDGE_V2_HEIGHT, V2TestBlockHash(BRIDGE_V2_HEIGHT)};
    BOOST_CHECK(index.LightClientV2LastConnected() ==
                std::optional<node::BridgeStateConnection>{boot_connection});
    BOOST_CHECK(index.AnchorV2(v2_anchor_hash).has_value());
    BOOST_CHECK(!index.Anchor(v2_anchor_hash));
    BOOST_CHECK_EQUAL(index.AnchorCountV2(), 1U);
    BOOST_CHECK(index.AnchorForTargetV2(fixture_block, BRIDGE_V2_HEIGHT)
                    .has_value());
    BOOST_CHECK(!index.AnchorForTargetV2(fixture_block, BRIDGE_V2_HEIGHT - 1));
    // V1 is untouched.
    BOOST_CHECK(index.LightClient() == v1_store);
    BOOST_CHECK(index.LightClientLastConnected() == v1_connection);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);
    BOOST_CHECK(!index.AnchorV2(v1_fixture.block_hash));
    const auto v1_anchor{
        index.AnchorForTarget(v1_fixture.block_number, BRIDGE_V2_HEIGHT)};
    BOOST_REQUIRE(v1_anchor);
    BOOST_CHECK(v1_anchor->block_hash == v1_fixture.block_hash);
    BOOST_CHECK(!index.AnchorForTarget(fixture_block, BRIDGE_V2_HEIGHT));

    // The per-store second-bootstrap guard.
    VerifyFails(index, Block(1'061, {bootstrap}), BRIDGE_V2_HEIGHT + 1,
                params, "already bootstrapped");

    const node::BridgeBlockDelta update_delta{
        ConnectNext(index, params, {update})};
    BOOST_CHECK(update_delta.v2_store);
    BOOST_CHECK(update_delta.anchors_added.empty());
    BOOST_REQUIRE(index.LightClientV2());
    BOOST_CHECK(index.LightClientV2()->next.has_value());
    BOOST_CHECK(!index.LightClient()->next);

    // Snapshots: V1 keeps its seed; V2 follows its own deltas only.
    const auto v1_snapshot{
        index.FinalizedLightClientSnapshot(BRIDGE_V2_HEIGHT + 1)};
    BOOST_REQUIRE(v1_snapshot);
    BOOST_CHECK(v1_snapshot->connection == *v1_connection);
    BOOST_CHECK(v1_snapshot->store == *v1_store);
    BOOST_CHECK(!index.FinalizedLightClientSnapshotV2(BRIDGE_V2_HEIGHT - 1));
    const auto v2_at_boot{
        index.FinalizedLightClientSnapshotV2(BRIDGE_V2_HEIGHT)};
    BOOST_REQUIRE(v2_at_boot);
    BOOST_CHECK_EQUAL(v2_at_boot->connection.height, BRIDGE_V2_HEIGHT);
    BOOST_CHECK(!v2_at_boot->store.next);
    const auto v2_at_update{
        index.FinalizedLightClientSnapshotV2(BRIDGE_V2_HEIGHT + 1)};
    BOOST_REQUIRE(v2_at_update);
    BOOST_CHECK_EQUAL(v2_at_update->connection.height, BRIDGE_V2_HEIGHT + 1);
    BOOST_CHECK(v2_at_update->store.next.has_value());

    // Undo unwinds the V2 store alone.
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              BRIDGE_V2_HEIGHT + 1,
                              V2TestBlockHash(BRIDGE_V2_HEIGHT + 1), error),
                          error);
    BOOST_REQUIRE(index.LightClientV2());
    BOOST_CHECK(!index.LightClientV2()->next);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              BRIDGE_V2_HEIGHT,
                              V2TestBlockHash(BRIDGE_V2_HEIGHT), error),
                          error);
    BOOST_CHECK(!index.LightClientV2());
    BOOST_CHECK(!index.LightClientV2LastConnected());
    BOOST_CHECK_EQUAL(index.AnchorCountV2(), 0U);
    BOOST_CHECK(!index.FinalizedLightClientSnapshotV2(BRIDGE_V2_HEIGHT));
    BOOST_CHECK(index.LightClient() == v1_store);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);

    // Kind 4 from V extends the V2 anchors only; a V1 anchor is unknown to
    // the V2 store. Both stores may hold an anchor at the same number.
    const uint256 receipts_699{TestBlockHash(699)};
    const uint256 receipts_700{TestBlockHash(700)};
    const std::vector<unsigned char> header_699{
        ExecutionHeader(uint256{}, 699, receipts_699, 1'699)};
    const uint256 hash_699{Keccak(header_699)};
    const std::vector<unsigned char> header_700{
        ExecutionHeader(hash_699, 700, receipts_700, 1'700)};
    const uint256 hash_700{Keccak(header_700)};
    const Consensus::Params backfill_params{V2Params()};
    node::BridgeStateIndex backfill_index;
    SeedVerifiedAnchor(backfill_index, v1_fixture);
    ConnectEmptyThrough(backfill_index, backfill_params, BRIDGE_V2_HEIGHT - 2);
    node::BridgeBlockDelta v2_seed;
    v2_seed.height = BRIDGE_V2_HEIGHT - 1;
    v2_seed.block_hash = V2TestBlockHash(v2_seed.height);
    v2_seed.previous_height = backfill_index.ConnectedHeight();
    v2_seed.previous_block_hash = backfill_index.ConnectedHash();
    v2_seed.v2_store = true;
    v2_seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        700, hash_700, receipts_700, 32, 700, 1'700, v2_seed.height,
        v2_seed.block_hash});
    BOOST_REQUIRE_EQUAL(v1_fixture.block_number, 700U);
    BOOST_REQUIRE_MESSAGE(backfill_index.ConnectBlock(v2_seed, error), error);
    BOOST_CHECK(backfill_index.AnchorV2(hash_700).has_value());
    BOOST_CHECK(backfill_index.Anchor(v1_fixture.block_hash).has_value());

    VerifyFails(backfill_index,
                Block(1'060, {BackfillTransaction(v1_fixture.block_hash, 699,
                                                  {v1_fixture.header})}),
                BRIDGE_V2_HEIGHT, backfill_params,
                "backfill names an unretained anchor");
    // Below V the V2 anchor is unknown to the V1 store.
    const CTransactionRef backfill{
        BackfillTransaction(hash_700, 699, {header_700, header_699})};
    VerifyFails(backfill_index, Block(1'060, {backfill}),
                BRIDGE_V2_HEIGHT - 1, backfill_params,
                "backfill names an unretained anchor");
    const node::BridgeBlockDelta backfill_delta{
        ConnectNext(backfill_index, backfill_params, {backfill})};
    BOOST_CHECK(backfill_delta.v2_store);
    BOOST_REQUIRE_EQUAL(backfill_delta.anchors_added.size(), 1U);
    BOOST_CHECK(backfill_index.AnchorV2(hash_699).has_value());
    BOOST_CHECK(!backfill_index.Anchor(hash_699));
    BOOST_CHECK_EQUAL(backfill_index.AnchorCount(), 1U);
    BOOST_CHECK_EQUAL(backfill_index.AnchorCountV2(), 2U);
}

BOOST_AUTO_TEST_CASE(v2_mint_large_typed_receipt)
{
    const Consensus::Params params{V2Params()};
    const auto asset_v2{modern::ConfiguredBridgeV2AssetId(params)};
    BOOST_REQUIRE(asset_v2);
    // A type-0x05 receipt (beyond V1's 1..4) whose terminal node exceeds
    // V1's 1,024-byte limit but fits V2's 8,192.
    const ReceiptFixture fixture{
        MakeV2ReceiptFixture(params, {{1, 60, 0x31}}, 0x05, 1'700)};
    BOOST_CHECK_GT(fixture.mpt_node.size(), bridge::MAX_BRIDGE_RLP_ITEM_SIZE);
    BOOST_CHECK_LE(fixture.mpt_node.size(),
                   bridge::MAX_BRIDGE_TERMINAL_RECEIPT_NODE_SIZE_V2);
    BOOST_CHECK_GT(fixture.mpt_node.size(), 2'000U);

    node::BridgeStateIndex index;
    SeedVerifiedAnchorAt(index, fixture, BRIDGE_V2_HEIGHT - 1,
                         /*v2_store=*/true);
    const CTransactionRef mint{MintTransactionV2(params, fixture, 0, 60)};

    // Below V the record needs limits V1 does not have.
    VerifyFails(index, Block(1'060, {mint}), BRIDGE_V2_HEIGHT - 1, params,
                "bridge record is malformed");
    // The structural MPA rule that mempool admission and block checks run
    // before any bridge state selects the same limits by height.
    {
        std::string mpa_error;
        BOOST_CHECK(!modern::CheckTransactionMpa(*mint, params,
                                                 BRIDGE_V2_HEIGHT - 1,
                                                 mpa_error));
        BOOST_CHECK_EQUAL(mpa_error, "mpa-malformed-bridge-record");
        mpa_error.clear();
        BOOST_CHECK_MESSAGE(modern::CheckTransactionMpa(*mint, params,
                                                        BRIDGE_V2_HEIGHT,
                                                        mpa_error),
                            mpa_error);
        // Without a bridge V2 envelope the V1 limits hold at every height.
        Consensus::Params no_v2{params};
        no_v2.bridge_v2.reset();
        mpa_error.clear();
        BOOST_CHECK(!modern::CheckTransactionMpa(*mint, no_v2,
                                                 BRIDGE_V2_HEIGHT + 100,
                                                 mpa_error));
        BOOST_CHECK_EQUAL(mpa_error, "mpa-malformed-bridge-record");
    }

    node::BridgeTxAuthorization authorization;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.VerifyTransaction(*mint, BRIDGE_V2_HEIGHT,
                                                  1'060, params,
                                                  authorization, error),
                          error);
    BOOST_REQUIRE(authorization.mint);
    BOOST_CHECK(authorization.mint->authorization.asset == *asset_v2);
    BOOST_CHECK(!authorization.HasWithdrawal());

    const node::BridgeBlockDelta delta{ConnectNext(index, params, {mint})};
    BOOST_CHECK(delta.v2_store);
    BOOST_REQUIRE_EQUAL(delta.mint_authorizations.size(), 1U);
    const auto& minted{delta.mint_authorizations.front().authorization};
    BOOST_CHECK(minted.asset == *asset_v2);
    BOOST_CHECK(minted.asset != *modern::ConfiguredBridgeAssetId(params));
    BOOST_CHECK_EQUAL(minted.amount, 60);
    BOOST_CHECK(minted.recipient_script == fixture.recipient_scripts.front());
    BOOST_CHECK_EQUAL(minted.nullifier.nullifier_namespace,
                      bridge::BRIDGE_NULLIFIER_NAMESPACE_V2);

    // From V a V1-registry mint is refused, even for a V1 deposit.
    const ReceiptFixture v1_fixture{
        MakeReceiptFixture(params, {{2, 60, 0x32}})};
    VerifyFails(index,
                Block(1'061, {MintTransactionWithAmount(params, v1_fixture, 0,
                                                        60)}),
                BRIDGE_V2_HEIGHT + 1, params,
                "registry id does not match the V2 vault entry");

    // V1 keeps its receipt types: a small type-0x05 receipt is malformed
    // under the V1 mint rules.
    const Consensus::Params v1_params{BridgeParams()};
    const ReceiptFixture typed_v1{MakeReceiptFixture(
        v1_params.busd_bridge->asset.vault_address,
        v1_params.busd_bridge->asset.token_address, {{3, 60, 0x33}}, 0x05)};
    BOOST_REQUIRE_LE(typed_v1.mpt_node.size(), bridge::MAX_BRIDGE_RLP_ITEM_SIZE);
    node::BridgeStateIndex v1_index;
    SeedVerifiedAnchor(v1_index, typed_v1);
    VerifyFails(v1_index,
                Block(1'060, {MintTransactionWithAmount(v1_params, typed_v1, 0,
                                                        60)}),
                BRIDGE_HEIGHT, v1_params, "bridge mint receipt is malformed");
}

BOOST_AUTO_TEST_CASE(v2_mint_replay_and_namespace)
{
    const Consensus::Params params{V2Params()};
    const auto asset_v1{modern::ConfiguredBridgeAssetId(params)};
    const auto asset_v2{modern::ConfiguredBridgeV2AssetId(params)};
    BOOST_REQUIRE(asset_v1);
    BOOST_REQUIRE(asset_v2);
    // The same deposit id on both vaults: one V1 mint before V, one V2 mint
    // from V, kept apart by vault and nullifier namespace.
    const ReceiptFixture v1_fixture{
        MakeReceiptFixture(params, {{7, 60, 0x41}})};
    const ReceiptFixture v2_fixture{MakeV2ReceiptFixture(
        params, {{7, 60, 0x42}, {8, 50, 0x43}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, v1_fixture);
    const node::BridgeBlockDelta v1_mint{ConnectNext(
        index, params, {MintTransactionWithAmount(params, v1_fixture, 0, 60)})};
    BOOST_CHECK(!v1_mint.v2_store);
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 2);
    SeedVerifiedAnchorAt(index, v2_fixture, BRIDGE_V2_HEIGHT - 1,
                         /*v2_store=*/true);

    const CTransactionRef mint{MintTransactionV2(params, v2_fixture, 0, 60)};
    const node::BridgeBlockDelta delta{ConnectNext(index, params, {mint})};
    BOOST_REQUIRE_EQUAL(delta.nullifiers_added.size(), 1U);
    const bridge::BridgeDepositKey v2_key{
        Consensus::BUSD_ETHEREUM_CHAIN_ID,
        params.bridge_v2->vault.vault_address, 7,
        bridge::BRIDGE_NULLIFIER_NAMESPACE_V2};
    BOOST_CHECK(delta.nullifiers_added.front() == v2_key);
    BOOST_CHECK(index.IsNullified(v2_key));
    bridge::BridgeDepositKey v2_vault_in_v1_namespace{v2_key};
    v2_vault_in_v1_namespace.nullifier_namespace =
        bridge::BRIDGE_NULLIFIER_NAMESPACE_V1;
    BOOST_CHECK(!index.IsNullified(v2_vault_in_v1_namespace));
    BOOST_CHECK(index.IsNullified(
        v1_mint.mint_authorizations.front().authorization.nullifier));
    BOOST_CHECK_EQUAL(index.NullifierCount(), 2U);
    BOOST_CHECK_EQUAL(
        index.NullifierCount(bridge::BRIDGE_NULLIFIER_NAMESPACE_V1), 1U);
    BOOST_CHECK_EQUAL(
        index.NullifierCount(bridge::BRIDGE_NULLIFIER_NAMESPACE_V2), 1U);
    const auto connection{index.NullifierConnection(v2_key)};
    BOOST_REQUIRE(connection);
    BOOST_CHECK_EQUAL(connection->height, BRIDGE_V2_HEIGHT);
    // Epochs: V2 counts from V, keyed by the bUSD V2 AssetId.
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset_v2, 0), 60);
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset_v1, 0), 60);

    // Replay at V + 1, alone and together with a fresh deposit.
    VerifyFails(index, Block(1'061, {mint}), BRIDGE_V2_HEIGHT + 1, params,
                "already nullified");
    const CTransactionRef second{MintTransactionV2(params, v2_fixture, 1, 50)};
    VerifyFails(index, Block(1'061, {mint, second}), BRIDGE_V2_HEIGHT + 1,
                params, "already nullified");
    // The V2 epoch counter (cap 150) carries the earlier mint: 60 + 50.
    const node::BridgeBlockDelta second_delta{
        ConnectNext(index, params, {second})};
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset_v2, 0), 110);
    BOOST_REQUIRE_EQUAL(second_delta.epoch_mint_changes.size(), 1U);
    BOOST_CHECK(second_delta.epoch_mint_changes.front().asset == *asset_v2);
    BOOST_CHECK_EQUAL(second_delta.epoch_mint_changes.front().before, 60);

    // Undo returns both V2 nullifiers and the epoch counter.
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              BRIDGE_V2_HEIGHT + 1,
                              V2TestBlockHash(BRIDGE_V2_HEIGHT + 1), error),
                          error);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              BRIDGE_V2_HEIGHT,
                              V2TestBlockHash(BRIDGE_V2_HEIGHT), error),
                          error);
    BOOST_CHECK(!index.IsNullified(v2_key));
    BOOST_CHECK_EQUAL(
        index.NullifierCount(bridge::BRIDGE_NULLIFIER_NAMESPACE_V2), 0U);
    BOOST_CHECK_EQUAL(
        index.NullifierCount(bridge::BRIDGE_NULLIFIER_NAMESPACE_V1), 1U);
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset_v2, 0), 0);
    BOOST_CHECK_EQUAL(index.EpochMinted(*asset_v1, 0), 60);
}

BOOST_AUTO_TEST_CASE(v2_burn_tree_roots_undo)
{
    const Consensus::Params params{V2Params()};
    const auto asset_v2{modern::ConfiguredBridgeV2AssetId(params)};
    const auto entry_id{modern::ConfiguredBridgeV2EntryId(params)};
    BOOST_REQUIRE(asset_v2);
    BOOST_REQUIRE(entry_id);
    const uint256 empty_root{
        modern::WithdrawalZeroHashes()[modern::WITHDRAWAL_TREE_DEPTH]};

    node::BridgeStateIndex index;
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 1);
    // No root V2 is recorded below V (the V2 tree is empty there and
    // certificates sign zero without consulting the index).
    BOOST_CHECK(!index.WithdrawalRootV2AtHeight(BRIDGE_V2_HEIGHT - 1));
    BOOST_CHECK(index.WithdrawalTreeV2().root == empty_root);
    BOOST_CHECK(node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT - 1, params,
                                               &index) ==
                std::optional<uint256>{uint256{}});

    const CTransactionRef first{BurnTransactionV2(params, 90, 11, 0x91)};
    const CTransactionRef second{BurnTransactionV2(params, 91, 22, 0x92)};
    // Below V a kind 6 naming the V2 entry is a V1 burn, refused on this
    // managed V1 bridge.
    VerifyFails(index, Block(1'060, {first}), BRIDGE_V2_HEIGHT - 1, params,
                "decentralized bridge burn is disabled");
    // Kind 5 stays valid below V and closes at V.
    const CTransactionRef managed{ManagedWithdrawalTransaction(params, 25)};
    node::BridgeBlockDelta managed_delta;
    std::string error;
    BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(1'060, {managed}),
                                          BRIDGE_V2_HEIGHT - 1,
                                          V2TestBlockHash(1), params,
                                          managed_delta, error),
                        error);
    VerifyFails(index, Block(1'060, {managed}), BRIDGE_V2_HEIGHT, params,
                "managed bridge withdrawals are closed from the bridge V2 activation height");
    // From V the record must name the V2 entry and burn bUSD V2.
    VerifyFails(index,
                Block(1'060, {BurnTransactionV2(
                                 params, 92, 11, 0x93,
                                 modern::ConfiguredBridgeRegistryId(params))}),
                BRIDGE_V2_HEIGHT, params,
                "bridge burn does not name the V2 vault entry");
    VerifyFails(index,
                Block(1'060, {BurnTransactionV2(
                                 params, 93, 11, 0x94, std::nullopt,
                                 modern::ConfiguredBridgeAssetId(params))}),
                BRIDGE_V2_HEIGHT, params, "exact bUSD V2 BURN output");
    // W2 = V + 5 keeps burns closed at V.
    const Consensus::Params late_burns{V2Params(BRIDGE_V2_HEIGHT + 5)};
    VerifyFails(index, Block(1'060, {first}), BRIDGE_V2_HEIGHT, late_burns,
                "bridge V2 burns are not active");
    BOOST_CHECK(modern::ConfiguredBridgeV2EntryId(late_burns) == entry_id);

    // V: a seed retaining a V2 light client and the funding receipts'
    // block (no V2 record); the certificate root is the empty V2 tree.
    const ReceiptFixture funding{MakeV2ReceiptFixture(params, {{0, 100, 0x31}})};
    const bridge_test::ObservationWorld world{HealthyWorld(params, 1, 100)};
    const StateAnchorSpec funding_anchor{funding.block_number, funding.block_hash,
                                         funding.timestamp, world.StateRoot(),
                                         funding.receipts_root};
    const node::BridgeBlockDelta at_v{
        SeedStateAnchorsV2(index, {funding_anchor}, funding_anchor)};
    BOOST_CHECK(at_v.v2_store);
    BOOST_CHECK(index.WithdrawalRootV2AtHeight(BRIDGE_V2_HEIGHT) ==
                std::optional<uint256>{empty_root});
    BOOST_CHECK(node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT, params,
                                               &index) ==
                std::optional<uint256>{empty_root});
    // A burn needs reserve (check 2) and a RESERVE_E observation (check 3).
    VerifyFails(index, Block(1'061, {first}), BRIDGE_V2_HEIGHT + 1, params,
                "exceeds the vault entry's reserve");
    // V + 1: mint 100 bUSD V2 and observe the vault holding it.
    ConnectNext(index, params,
                {MintTransactionV2(params, funding, 0, 100),
                 Observe(world, SUBJECT_RESERVE, funding.block_hash)},
                1'061);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, 100);

    // The per-transaction preview reports the V2 leaf (and so SIGHASH_ALL).
    const int burn_height{BRIDGE_V2_HEIGHT + 2};
    node::BridgeTxAuthorization authorization;
    BOOST_REQUIRE_MESSAGE(index.VerifyTransaction(*first, burn_height, 1'061,
                                                  params, authorization,
                                                  error),
                          error);
    BOOST_REQUIRE(authorization.withdrawal_v2);
    BOOST_CHECK(authorization.HasWithdrawal());
    BOOST_CHECK(!authorization.decentralized_withdrawal);
    BOOST_CHECK(!authorization.mint);
    auto preview{index.BeginBlockPreview(burn_height, 1'061, TestHash(95),
                                         params, error)};
    BOOST_REQUIRE_MESSAGE(preview != nullptr, error);
    const std::array<CTransactionRef, 1> chunk{first};
    BOOST_REQUIRE_MESSAGE(preview->TryAppend(chunk, error), error);
    BOOST_CHECK(!preview->TryAppend(chunk, error));
    BOOST_CHECK(error.find("duplicated") != std::string::npos);

    // V + 2: two burns take ids 0 and 1 of the V2 tree.
    const node::BridgeBlockDelta delta{
        ConnectNext(index, params, {first, second}, 1'061)};
    BOOST_REQUIRE_EQUAL(delta.withdrawals_v2_added.size(), 2U);
    BOOST_CHECK(delta.decentralized_withdrawals_added.empty());
    BOOST_CHECK(delta.withdrawal_tree_after == delta.withdrawal_tree_before);
    modern::WithdrawalTreeState expected;
    for (size_t i{0}; i < delta.withdrawals_v2_added.size(); ++i) {
        const node::BridgeWithdrawalV2Request& request{
            delta.withdrawals_v2_added[i]};
        const modern::BridgeWithdrawalV2& w{request.withdrawal};
        BOOST_CHECK_EQUAL(w.withdrawal_id, i);
        BOOST_CHECK_EQUAL(w.destination_chain_id,
                          Consensus::BUSD_ETHEREUM_CHAIN_ID);
        BOOST_CHECK(w.asset_id == *asset_v2);
        BOOST_CHECK(w.vault == params.bridge_v2->vault.vault_address);
        BOOST_CHECK(w.token == Consensus::BUSD_ETHEREUM_USDT);
        BOOST_CHECK(w.recipient == FilledAddress(static_cast<uint8_t>(0x91 + i)));
        BOOST_CHECK_EQUAL(w.amount, i == 0 ? 11 : 22);
        BOOST_CHECK_EQUAL(w.b3_height, static_cast<uint64_t>(burn_height));
        BOOST_CHECK_EQUAL(request.connected_height, burn_height);
        BOOST_CHECK(request.connected_block == V2TestBlockHash(burn_height));
        BOOST_CHECK(modern::BridgeWithdrawalLeafV2(w) ==
                    std::optional<uint256>{request.leaf});
        BOOST_CHECK(modern::AppendBridgeWithdrawalV2(expected, w) ==
                    std::optional<uint256>{request.leaf});
    }
    BOOST_CHECK(delta.withdrawal_tree_v2_after == expected);
    BOOST_CHECK(index.WithdrawalTreeV2() == expected);
    BOOST_CHECK(index.WithdrawalRootV2AtHeight(burn_height) ==
                std::optional<uint256>{expected.root});
    BOOST_CHECK(node::FinalityWithdrawalRootV2(burn_height, params, &index) ==
                std::optional<uint256>{expected.root});
    BOOST_CHECK(index.WithdrawalRootV2AtHeight(BRIDGE_V2_HEIGHT) ==
                std::optional<uint256>{empty_root});
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().burned, 33);
    BOOST_CHECK_EQUAL(delta.reserve_v2_before.burned, 0);
    BOOST_CHECK_EQUAL(delta.reserve_v2_after.burned, 33);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 2U);
    BOOST_CHECK_EQUAL(index.DecentralizedWithdrawalCount(), 0U);
    BOOST_CHECK_EQUAL(index.WithdrawalTree().count, 0U);
    const node::BridgeWithdrawalId first_source{first->GetHash(), 0};
    const auto by_source{index.WithdrawalV2(first_source)};
    BOOST_REQUIRE(by_source);
    BOOST_CHECK(*by_source == delta.withdrawals_v2_added[0]);
    BOOST_CHECK(index.WithdrawalV2(uint64_t{1}) ==
                std::optional<node::BridgeWithdrawalV2Request>{
                    delta.withdrawals_v2_added[1]});
    BOOST_CHECK(!index.WithdrawalV2(node::BridgeWithdrawalId{first->GetHash(), 1}));
    BOOST_CHECK(!index.DecentralizedWithdrawal(first_source));
    BOOST_CHECK(!index.WithdrawalsV2Through(burn_height + 1));
    const auto through_v{index.WithdrawalsV2Through(BRIDGE_V2_HEIGHT)};
    BOOST_REQUIRE(through_v);
    BOOST_CHECK(through_v->empty());

    // The S1 relay builder reproduces the indexed root from the prefix and
    // folds each path back to it, as Vault V2 does.
    const auto prefix{index.WithdrawalsV2Through(burn_height)};
    BOOST_REQUIRE(prefix);
    BOOST_REQUIRE_EQUAL(prefix->size(), 2U);
    std::vector<modern::BridgeWithdrawalV2> withdrawals;
    for (const auto& request : *prefix) withdrawals.push_back(request.withdrawal);
    for (uint64_t id{0}; id < withdrawals.size(); ++id) {
        const auto artifacts{bridge::BuildWithdrawalV2RelayArtifacts(
            withdrawals, id, expected.root, error)};
        BOOST_REQUIRE_MESSAGE(artifacts, error);
        BOOST_CHECK(artifacts->leaf == prefix->at(id).leaf);
        BOOST_CHECK_EQUAL(artifacts->release_calldata.size(),
                          bridge::VAULT_V2_RELEASE_CALLDATA_SIZE);
    }

    // A replayed burn source is refused; a new one continues at id 2.
    VerifyFails(index, Block(1'062, {first}), burn_height + 1, params,
                "duplicated");
    const CTransactionRef third{BurnTransactionV2(params, 96, 33, 0x97)};
    const node::BridgeBlockDelta third_delta{
        ConnectNext(index, params, {third}, 1'062)};
    BOOST_REQUIRE_EQUAL(third_delta.withdrawals_v2_added.size(), 1U);
    BOOST_CHECK_EQUAL(
        third_delta.withdrawals_v2_added.front().withdrawal.withdrawal_id, 2U);

    // Undo restores the tree, roots and maps exactly; replay is identical.
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              burn_height + 1,
                              V2TestBlockHash(burn_height + 1), error),
                          error);
    BOOST_CHECK(index.WithdrawalTreeV2() == expected);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(
                              burn_height, V2TestBlockHash(burn_height), error),
                          error);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 0U);
    BOOST_CHECK(!index.WithdrawalV2(first_source));
    BOOST_CHECK(!index.WithdrawalV2(uint64_t{0}));
    BOOST_CHECK_EQUAL(index.WithdrawalTreeV2().count, 0U);
    BOOST_CHECK(index.WithdrawalTreeV2().root == empty_root);
    BOOST_CHECK(!index.WithdrawalRootV2AtHeight(burn_height));
    BOOST_CHECK(index.WithdrawalRootV2AtHeight(BRIDGE_V2_HEIGHT) ==
                std::optional<uint256>{empty_root});
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().burned, 0);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, 100);
    const node::BridgeBlockDelta replay{
        ConnectNext(index, params, {first, second}, 1'061)};
    BOOST_CHECK(replay.withdrawals_v2_added == delta.withdrawals_v2_added);
    BOOST_CHECK(replay.withdrawal_tree_v2_after == delta.withdrawal_tree_v2_after);
}

BOOST_AUTO_TEST_CASE(v2_delta_store_invariants)
{
    // ConnectBlock refuses deltas that mix the V1 and V2 regimes.
    node::BridgeStateIndex index;
    node::BridgeBlockDelta wrong_namespace;
    wrong_namespace.height = BRIDGE_V2_HEIGHT;
    wrong_namespace.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT);
    wrong_namespace.v2_store = true;
    bridge::BridgeDepositKey v1_key;
    v1_key.origin_chain_id = 1;
    v1_key.vault_address.fill(0x21);
    v1_key.deposit_id = 1;
    wrong_namespace.nullifiers_added.push_back(v1_key);
    std::string error;
    BOOST_CHECK(!index.ConnectBlock(wrong_namespace, error));
    BOOST_CHECK(error.find("namespace does not match") != std::string::npos);

    node::BridgeBlockDelta v2_leaf_below_v;
    v2_leaf_below_v.height = BRIDGE_V2_HEIGHT - 1;
    v2_leaf_below_v.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT - 1);
    v2_leaf_below_v.withdrawals_v2_added.emplace_back();
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(v2_leaf_below_v, error));
    BOOST_CHECK(error.find("withdrawals do not match its store") !=
                std::string::npos);

    node::BridgeBlockDelta v1_leaf_from_v;
    v1_leaf_from_v.height = BRIDGE_V2_HEIGHT;
    v1_leaf_from_v.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT);
    v1_leaf_from_v.v2_store = true;
    v1_leaf_from_v.decentralized_withdrawals_added.emplace_back();
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(v1_leaf_from_v, error));
    BOOST_CHECK(error.find("withdrawals do not match its store") !=
                std::string::npos);

    // V2 accounting changes only on the V2 store, from a matching
    // before-state, by the block's own records.
    node::BridgeBlockDelta ledger_below_v;
    ledger_below_v.height = BRIDGE_V2_HEIGHT - 1;
    ledger_below_v.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT - 1);
    ledger_below_v.reserve_v2_after.minted = 5;
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(ledger_below_v, error));
    BOOST_CHECK(error.find("V2 accounting does not match its store") !=
                std::string::npos);

    node::BridgeBlockDelta wrong_before;
    wrong_before.height = BRIDGE_V2_HEIGHT;
    wrong_before.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT);
    wrong_before.v2_store = true;
    wrong_before.reserve_v2_before.minted = 1;
    wrong_before.reserve_v2_after.minted = 1;
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(wrong_before, error));
    BOOST_CHECK(error.find("V2 accounting before-state does not match") !=
                std::string::npos);

    node::BridgeBlockDelta unearned;
    unearned.height = BRIDGE_V2_HEIGHT;
    unearned.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT);
    unearned.v2_store = true;
    unearned.reserve_v2_after.minted = 7; // no mint authorizes it
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(unearned, error));
    BOOST_CHECK(error.find("reserve ledger does not match its effects") !=
                std::string::npos);
    unearned.reserve_v2_after = {};
    unearned.reserve_v2_after.transit = 3; // transit has no source yet
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(unearned, error));
    BOOST_CHECK(error.find("reserve ledger does not match its effects") !=
                std::string::npos);

    node::BridgeBlockDelta conjured;
    conjured.height = BRIDGE_V2_HEIGHT;
    conjured.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT);
    conjured.v2_store = true;
    conjured.conflict_v2_after = node::BridgeV2Conflict{
        bridge::BRIDGE_V2_CONFLICT_E8, BRIDGE_V2_HEIGHT,
        V2TestBlockHash(BRIDGE_V2_HEIGHT), 800};
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(conjured, error));
    BOOST_CHECK(error.find("malformed sticky conflict") != std::string::npos);

    node::BridgeBlockDelta orphan_observation;
    orphan_observation.height = BRIDGE_V2_HEIGHT;
    orphan_observation.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT);
    orphan_observation.v2_store = true;
    node::BridgeObservationRecordV2 record;
    record.subject = SUBJECT_TOKEN;
    record.state = bridge::TokenFlagsV2{};
    record.anchor_hash = AnchorHash(800);
    record.anchor_block = 800;
    record.connected_height = BRIDGE_V2_HEIGHT;
    record.connected_block = orphan_observation.block_hash;
    orphan_observation.observations_v2_added.push_back(record);
    error.clear();
    BOOST_CHECK(!index.ConnectBlock(orphan_observation, error));
    BOOST_CHECK(error.find("malformed observation") != std::string::npos);

    BOOST_CHECK_EQUAL(index.ConnectedHeight(), -1);
    BOOST_CHECK(index.History().empty());
}

BOOST_AUTO_TEST_CASE(v2_burn_gate_from_index)
{
    // EvaluateBridgeV2BurnGate reads the fresh lineage from its context and
    // E1-E8 / C4 from the connected index (node changes section 8.4). The
    // row logic itself is bridge_burn_gate_v2_tests'; this checks the
    // wiring: availability, now_eff, the observations and the conflict.
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    bridge_test::ObservationWorld world{HealthyWorld(params)};
    node::BridgeStateIndex probe{ObservationIndex(params, world.StateRoot())};
    world.SetVerifier(lineage.Canonical(1, 109, params, probe));
    node::BridgeStateIndex index{ObservationIndex(params, world.StateRoot())};
    const int height{BRIDGE_V2_HEIGHT};
    const uint256 anchor{AnchorHash(800)};

    // UNAVAILABLE: no bridge V2 envelope, no context, the replay context,
    // or an index not at the parent.
    {
        Consensus::Params no_v2{params};
        no_v2.bridge_v2.reset();
        const auto result{node::EvaluateBridgeV2BurnGate(
            &lineage.context, index, no_v2, height, 1'060)};
        BOOST_CHECK(result.readiness == node::BridgeBurnReadiness::UNAVAILABLE);
        BOOST_CHECK(result.Describe().find("not configured") != std::string::npos);
    }
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(nullptr, index, params, height,
                                               1'060)
                    .readiness == node::BridgeBurnReadiness::UNAVAILABLE);
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&node::REPLAY_LINEAGE, index,
                                               params, height, 1'060)
                    .readiness == node::BridgeBurnReadiness::UNAVAILABLE);
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params,
                                               height + 1, 1'060)
                    .readiness == node::BridgeBurnReadiness::UNAVAILABLE);

    // No observation yet: E1.
    {
        const auto result{node::EvaluateBridgeV2BurnGate(
            &lineage.context, index, params, height, 1'060)};
        BOOST_CHECK(result.readiness == node::BridgeBurnReadiness::NOT_READY);
        BOOST_CHECK(result.row == bridge::BurnGateRowV2::E1);
        BOOST_CHECK_EQUAL(result.Describe(), "E1: no VERIFIER_E observation");
        BOOST_CHECK_EQUAL(result.now_eff, 1'060);
    }
    // A context without fresh state: C2 (the lineage is configured but
    // was not projected), never READY.
    {
        node::BridgeV2LineageContext empty;
        const auto result{node::EvaluateBridgeV2BurnGate(&empty, index, params,
                                                         height, 1'060)};
        BOOST_CHECK(result.row == bridge::BurnGateRowV2::C2);
    }

    // All three observations at anchor 800 (t_obs 1,000): READY.
    ConnectNext(index, params,
                {Observe(world, SUBJECT_TOKEN, anchor),
                 Observe(world, SUBJECT_RESERVE, anchor),
                 Observe(world, SUBJECT_VERIFIER, anchor)},
                1'060, &lineage.context);
    const Consensus::BridgeV2GateParams& gate{params.bridge_v2->gate};
    {
        const auto result{node::EvaluateBridgeV2BurnGate(
            &lineage.context, index, params, height + 1, 1'061)};
        BOOST_CHECK_MESSAGE(result.readiness == node::BridgeBurnReadiness::READY,
                            result.Describe());
        BOOST_CHECK_EQUAL(result.Describe(), "ready");
        BOOST_CHECK_EQUAL(result.now_eff, 1'061);
        // The observations expire before the rotation age limit.
        BOOST_REQUIRE(result.closes_at);
        BOOST_CHECK_EQUAL(*result.closes_at,
                          1'000 + gate.observation_max_age_seconds + 1);
    }
    // now_eff takes the light client's finalized execution time when the
    // block time lags it.
    BOOST_CHECK_EQUAL(node::EvaluateBridgeV2BurnGate(&lineage.context, index,
                                                     params, height + 1, 500)
                          .now_eff,
                      1'000);
    // Exactly at the age limit the gate is READY; one second later every
    // observation is stale and E1 is the first row to fail.
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(
                    &lineage.context, index, params, height + 1,
                    1'000 + gate.observation_max_age_seconds)
                    .readiness == node::BridgeBurnReadiness::READY);
    {
        const auto result{node::EvaluateBridgeV2BurnGate(
            &lineage.context, index, params, height + 1,
            1'000 + gate.observation_max_age_seconds + 1)};
        BOOST_CHECK(result.readiness == node::BridgeBurnReadiness::NOT_READY);
        BOOST_CHECK_EQUAL(result.Describe(), "E1: VERIFIER_E observation is stale");
    }
    // A broken lineage is C2; a lagging finalized checkpoint C3.
    {
        LineageFixture broken;
        broken.Fresh().lineage_broken = true;
        BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&broken.context, index,
                                                   params, height + 1, 1'061)
                        .row == bridge::BurnGateRowV2::C2);
        LineageFixture lagging;
        lagging.Fresh().finalized->height =
            height + 1 - gate.max_finality_lag_blocks - 1;
        BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&lagging.context, index,
                                                   params, height + 1, 1'061)
                        .row == bridge::BurnGateRowV2::C3);
    }
    // A recorded SETS_MISMATCH verdict closes the gate at E4.
    {
        bridge_test::ObservationWorld mismatched{world};
        bridge::VerifierStateV2 other{lineage.Canonical(1, 109, params, index)};
        other.next_set_hash = TestHash(0x5e);
        mismatched.SetVerifier(other);
        SeedStateAnchorsV2(index, {StateAnchor(801, 1'010, mismatched.StateRoot())});
        ConnectNext(index, params,
                    {Observe(mismatched, SUBJECT_VERIFIER, AnchorHash(801),
                             VERDICT_SETS)},
                    1'062, &lineage.context);
        const auto result{node::EvaluateBridgeV2BurnGate(
            &lineage.context, index, params, index.ConnectedHeight() + 1,
            1'063)};
        BOOST_CHECK(result.row == bridge::BurnGateRowV2::E4);
        // A halted verifier is impaired (C1), whatever its age.
        bridge::VerifierStateV2 halted{other};
        halted.status = bridge::VERIFIER_STATUS_HALTED_CONFLICT;
        mismatched.SetVerifier(halted);
        SeedStateAnchorsV2(index, {StateAnchor(802, 1'020, mismatched.StateRoot())});
        ConnectNext(index, params,
                    {Observe(mismatched, SUBJECT_VERIFIER, AnchorHash(802))},
                    1'064);
        const auto halted_result{node::EvaluateBridgeV2BurnGate(
            &lineage.context, index, params, index.ConnectedHeight() + 1,
            1'065)};
        BOOST_CHECK(halted_result.row == bridge::BurnGateRowV2::C1);
        BOOST_CHECK(halted_result.Describe().find("halted") != std::string::npos);
    }

    // The V2 refusal text no longer names a temporary gate.
    const CTransactionRef burn{BurnTransactionV2(params, 98, 5, 0x99)};
    std::string error;
    BOOST_CHECK(!node::CheckBridgeV2BurnReadiness(
        *burn, node::BridgeBurnReadiness::NOT_READY, error));
    BOOST_CHECK_EQUAL(error, "bridge V2 burn gate is not ready");
    error.clear();
    BOOST_CHECK(!node::CheckBridgeV2BurnReadiness(
        Block(1'000, {burn}), node::BridgeBurnReadiness::UNAVAILABLE, error));
    BOOST_CHECK(error.find("unavailable") != std::string::npos);
    BOOST_CHECK(node::CheckBridgeV2BurnReadiness(
        *burn, node::BridgeBurnReadiness::READY, error));
    const ReceiptFixture fixture{MakeV2ReceiptFixture(params, {{1, 60, 0x31}})};
    BOOST_CHECK(node::CheckBridgeV2BurnReadiness(
        *MintTransactionV2(params, fixture, 0, 60),
        node::BridgeBurnReadiness::NOT_READY, error));
    // Kind 9 carriers are recognised only under the V2 record set.
    const CTransactionRef observation{Observe(world, SUBJECT_TOKEN, anchor)};
    BOOST_CHECK(node::HasBridgeV2LineageRecord(*observation));
    BOOST_CHECK(node::HasBridgeV2LineageRecord(Block(1'000, {burn, observation})));
    BOOST_CHECK(!node::HasBridgeV2LineageRecord(*burn));
    BOOST_CHECK(!node::HasBridgeV2FinalityConflict(*observation));
    BOOST_CHECK(!node::HasDecentralizedBridgeBurn(*observation));
}

BOOST_AUTO_TEST_CASE(finality_withdrawal_root_v2)
{
    const uint256 empty_root{
        modern::WithdrawalZeroHashes()[modern::WITHDRAWAL_TREE_DEPTH]};
    // Without a bridge V2 envelope the root is zero at every height.
    const Consensus::Params v1_only{FullyActiveBridgeParams(true)};
    BOOST_REQUIRE(!v1_only.bridge_v2);
    for (const int height : {0, BRIDGE_HEIGHT, BRIDGE_V2_HEIGHT, 1'000'000}) {
        BOOST_CHECK(node::FinalityWithdrawalRootV2(height, v1_only, nullptr) ==
                    std::optional<uint256>{uint256{}});
    }
    // ... and an index on such a chain records no V2 root at any height
    // (no per-block V2 state on mainnet or testnet).
    {
        node::BridgeStateIndex v1_index;
        ConnectEmptyThrough(v1_index, v1_only, BRIDGE_V2_HEIGHT + 2);
        for (int height{BRIDGE_HEIGHT}; height <= BRIDGE_V2_HEIGHT + 2;
             ++height) {
            BOOST_CHECK(!v1_index.WithdrawalRootV2AtHeight(height));
            BOOST_CHECK(node::FinalityWithdrawalRootV2(height, v1_only,
                                                       &v1_index) ==
                        std::optional<uint256>{uint256{}});
        }
        BOOST_CHECK(std::none_of(v1_index.History().begin(),
                                 v1_index.History().end(),
                                 [](const node::BridgeBlockDelta& delta) {
                                     return delta.v2_store;
                                 }));
        // Undo below V needs no V2 root either.
        std::string error;
        BOOST_CHECK_MESSAGE(v1_index.DisconnectBlock(
                                BRIDGE_V2_HEIGHT + 2,
                                V2TestBlockHash(BRIDGE_V2_HEIGHT + 2), error),
                            error);
    }
    // An incomplete envelope is inactive, hence zero.
    Consensus::Params incomplete{V2Params()};
    incomplete.bridge_v2->vault.token_runtime_code_hash.SetNull();
    BOOST_REQUIRE(!incomplete.bridge_v2->Valid());
    BOOST_CHECK(node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT, incomplete,
                                               nullptr) ==
                std::optional<uint256>{uint256{}});

    const Consensus::Params params{V2Params()};
    BOOST_CHECK(node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT - 1, params,
                                               nullptr) ==
                std::optional<uint256>{uint256{}});
    // From V it fails closed without an index or without the height.
    BOOST_CHECK(!node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT, params,
                                                nullptr));
    node::BridgeStateIndex index;
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT);
    BOOST_CHECK(node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT, params,
                                               &index) ==
                std::optional<uint256>{empty_root});
    BOOST_CHECK(!node::FinalityWithdrawalRootV2(BRIDGE_V2_HEIGHT + 1, params,
                                                &index));
    // The V1 hook is unchanged by the V2 regime (managed V1: zero).
    BOOST_CHECK(node::FinalityWithdrawalRoot(BRIDGE_V2_HEIGHT, params,
                                             &index) ==
                std::optional<uint256>{uint256{}});
}

BOOST_AUTO_TEST_CASE(v2_anchors_record_state_roots)
{
    // Anchors keep their execution header's stateRoot, the root kind 9
    // observations prove storage against. A light-client bootstrap records
    // the SSZ-proven root; a backfill records RLP header field [3]. A header
    // without a 32-byte stateRoot still backfills (V1 parity) and records a
    // null root.
    const uint64_t fixture_block{ethfx::U1_FINALIZED_EXEC.block_number};
    const Consensus::Params params{V2Params(BRIDGE_V2_HEIGHT, fixture_block)};
    const ReceiptFixture v1_fixture{
        MakeReceiptFixture(params, {{1, 60, 0x71}})};
    node::BridgeStateIndex index;
    SeedVerifiedAnchor(index, v1_fixture);
    ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 1);
    const node::BridgeBlockDelta boot_delta{
        ConnectNext(index, params, {FixtureBootstrapTransaction()})};
    BOOST_REQUIRE_EQUAL(boot_delta.anchors_added.size(), 1U);
    const uint256 boot_state{
        FixtureHex32(ethfx::U1_FINALIZED_EXEC.state_root)};
    BOOST_CHECK(!boot_state.IsNull());
    BOOST_CHECK(boot_delta.anchors_added.front().state_root == boot_state);
    const auto boot_anchor{index.AnchorV2(
        FixtureHex32(ethfx::U1_FINALIZED_EXEC.block_hash))};
    BOOST_REQUIRE(boot_anchor);
    BOOST_CHECK(boot_anchor->state_root == boot_state);

    auto header = [](const uint256& parent, const uint64_t number,
                     const uint256& receipts_root, const uint64_t timestamp,
                     const std::optional<uint256>& state_root) {
        std::vector<unsigned char> encoded{ExecutionHeader(
            parent, number, receipts_root, timestamp)};
        if (!state_root) return encoded;
        const auto top{bridge::RlpDecode(encoded)};
        BOOST_REQUIRE(top);
        const auto items{bridge::RlpChildren(*top)};
        BOOST_REQUIRE(items);
        std::vector<std::vector<unsigned char>> fields;
        for (const auto& item : *items) {
            fields.push_back(EncodedBytes(item.payload));
        }
        fields[3] = EncodedBytes(
            std::span<const unsigned char>{state_root->begin(), 32});
        return bridge::RlpEncodeList(fields);
    };
    const uint256 state_699{TestBlockHash(1'699)};
    const uint256 state_700{TestBlockHash(1'700)};
    const std::vector<unsigned char> header_698{header(
        uint256{}, 698, TestBlockHash(698), 1'698, std::nullopt)};
    const uint256 hash_698{Keccak(header_698)};
    const std::vector<unsigned char> header_699{header(
        hash_698, 699, TestBlockHash(699), 1'699, state_699)};
    const uint256 hash_699{Keccak(header_699)};
    const std::vector<unsigned char> header_700{header(
        hash_699, 700, TestBlockHash(700), 1'700, state_700)};
    const uint256 hash_700{Keccak(header_700)};

    const Consensus::Params backfill_params{V2Params()};
    node::BridgeStateIndex backfill_index;
    SeedVerifiedAnchor(backfill_index, v1_fixture);
    ConnectEmptyThrough(backfill_index, backfill_params, BRIDGE_V2_HEIGHT - 2);
    node::BridgeBlockDelta v2_seed;
    v2_seed.height = BRIDGE_V2_HEIGHT - 1;
    v2_seed.block_hash = V2TestBlockHash(v2_seed.height);
    v2_seed.previous_height = backfill_index.ConnectedHeight();
    v2_seed.previous_block_hash = backfill_index.ConnectedHash();
    v2_seed.v2_store = true;
    v2_seed.anchors_added.push_back(node::BridgeExecutionAnchor{
        700, hash_700, TestBlockHash(700), 32, 700, 1'700, v2_seed.height,
        v2_seed.block_hash, state_700});
    std::string error;
    BOOST_REQUIRE_MESSAGE(backfill_index.ConnectBlock(v2_seed, error), error);

    const node::BridgeBlockDelta with_root{ConnectNext(
        backfill_index, backfill_params,
        {BackfillTransaction(hash_700, 699, {header_700, header_699})})};
    BOOST_REQUIRE_EQUAL(with_root.anchors_added.size(), 1U);
    BOOST_CHECK(with_root.anchors_added.front().state_root == state_699);
    const auto anchor_699{backfill_index.AnchorV2(hash_699)};
    BOOST_REQUIRE(anchor_699);
    BOOST_CHECK(anchor_699->state_root == state_699);

    const node::BridgeBlockDelta without_root{ConnectNext(
        backfill_index, backfill_params,
        {BackfillTransaction(hash_699, 698, {header_699, header_698})})};
    BOOST_REQUIRE_EQUAL(without_root.anchors_added.size(), 1U);
    BOOST_CHECK(without_root.anchors_added.front().state_root.IsNull());
    const auto anchor_698{backfill_index.AnchorV2(hash_698)};
    BOOST_REQUIRE(anchor_698);
    BOOST_CHECK(anchor_698->state_root.IsNull());
}

BOOST_AUTO_TEST_CASE(duplicate_anchor_state_root_compared_on_v2_store_only)
{
    // A bootstrap whose finalized execution block is already retained adds
    // no anchor when the retained one has the same number and receipts root.
    // On the V1 store that stays true whatever the retained stateRoot is:
    // comparing it there would be a new refusal, and V1 acceptance must not
    // change. On the V2 store, whose stateRoot kind 9 observations read, a
    // different retained root is a conflict.
    const uint64_t fixture_block{ethfx::U1_FINALIZED_EXEC.block_number};
    const uint256 fixture_hash{
        FixtureHex32(ethfx::U1_FINALIZED_EXEC.block_hash)};
    const uint256 fixture_receipts{
        FixtureHex32(ethfx::U1_FINALIZED_EXEC.receipts_root)};
    const uint256 fixture_state{
        FixtureHex32(ethfx::U1_FINALIZED_EXEC.state_root)};
    const uint256 other_state{TestBlockHash(4'242)};
    BOOST_REQUIRE(!fixture_state.IsNull());
    BOOST_REQUIRE(other_state != fixture_state);

    // The V2 envelope with the V1 pins moved to the same checkpoint, so the
    // fixture bootstrap verifies against either store.
    Consensus::Params params{V2Params(BRIDGE_V2_HEIGHT, fixture_block)};
    params.busd_bridge->light_client = params.bridge_v2->light_client;
    params.busd_bridge->origin_deployment_block = fixture_block;

    // Connect a hand-built delta at the next height that retains the
    // fixture's finalized execution block in the chosen store with
    // `state_root`, without a light client (a fixture shortcut).
    auto seed = [&](node::BridgeStateIndex& index, const int height,
                    const bool v2_store, const uint256& state_root) {
        node::BridgeBlockDelta delta;
        delta.height = height;
        delta.block_hash = V2TestBlockHash(height);
        delta.previous_height = index.ConnectedHeight();
        delta.previous_block_hash = index.ConnectedHash();
        delta.v2_store = v2_store;
        delta.anchors_added.push_back(node::BridgeExecutionAnchor{
            fixture_block, fixture_hash, fixture_receipts,
            ethfx::U1_FINALIZED_BEACON.slot, fixture_block,
            ethfx::U1_FINALIZED_EXEC.timestamp, height, delta.block_hash,
            state_root});
        delta.withdrawal_tree_before = index.WithdrawalTree();
        delta.withdrawal_tree_after = index.WithdrawalTree();
        delta.withdrawal_tree_v2_before = index.WithdrawalTreeV2();
        delta.withdrawal_tree_v2_after = index.WithdrawalTreeV2();
        std::string error;
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    };

    // V1 store, below V: the bootstrap is accepted identically whether the
    // retained root matches, differs or is null, and the retained anchor
    // keeps the root it was connected with.
    std::optional<node::BridgeBlockDelta> v1_reference;
    for (const uint256& retained : {fixture_state, other_state, uint256{}}) {
        node::BridgeStateIndex index;
        seed(index, BRIDGE_HEIGHT - 1, /*v2_store=*/false, retained);
        const node::BridgeBlockDelta delta{
            ConnectNext(index, params, {FixtureBootstrapTransaction()})};
        BOOST_CHECK_EQUAL(delta.height, BRIDGE_HEIGHT);
        BOOST_CHECK(!delta.v2_store);
        BOOST_CHECK(delta.anchors_added.empty());
        BOOST_REQUIRE(delta.light_client_after);
        BOOST_REQUIRE(index.LightClient());
        BOOST_CHECK(*index.LightClient() == FixtureBootstrapStore());
        BOOST_CHECK(!index.LightClientV2());
        BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);
        const auto anchor{index.Anchor(fixture_hash)};
        BOOST_REQUIRE(anchor);
        BOOST_CHECK(anchor->state_root == retained);
        if (!v1_reference) {
            v1_reference = delta;
            continue;
        }
        BOOST_CHECK(delta.light_client_before == v1_reference->light_client_before);
        BOOST_CHECK(delta.light_client_after == v1_reference->light_client_after);
        BOOST_CHECK(delta.light_client_connection_after ==
                    v1_reference->light_client_connection_after);
        BOOST_CHECK(delta.anchors_added == v1_reference->anchors_added);
    }

    // V2 store, at V: the matching root is accepted the same way ...
    {
        node::BridgeStateIndex index;
        ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 2);
        seed(index, BRIDGE_V2_HEIGHT - 1, /*v2_store=*/true, fixture_state);
        const node::BridgeBlockDelta delta{
            ConnectNext(index, params, {FixtureBootstrapTransaction()})};
        BOOST_CHECK_EQUAL(delta.height, BRIDGE_V2_HEIGHT);
        BOOST_CHECK(delta.v2_store);
        BOOST_CHECK(delta.anchors_added.empty());
        BOOST_REQUIRE(index.LightClientV2());
        BOOST_CHECK(*index.LightClientV2() == FixtureBootstrapStore());
        const auto anchor{index.AnchorV2(fixture_hash)};
        BOOST_REQUIRE(anchor);
        BOOST_CHECK(anchor->state_root == fixture_state);
    }
    // ... and a different or null retained root is refused.
    for (const uint256& retained : {other_state, uint256{}}) {
        node::BridgeStateIndex index;
        ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 2);
        seed(index, BRIDGE_V2_HEIGHT - 1, /*v2_store=*/true, retained);
        VerifyFails(index, Block(1'060, {FixtureBootstrapTransaction()}),
                    BRIDGE_V2_HEIGHT, params,
                    "bridge execution anchor hash conflicts with retained state");
        BOOST_CHECK(!index.LightClientV2());
        BOOST_CHECK_EQUAL(index.ConnectedHeight(), BRIDGE_V2_HEIGHT - 1);
    }
}

BOOST_AUTO_TEST_CASE(v2_observations_connect_undo_replay)
{
    // One record per subject at anchor 800, connected, undone, redone, and
    // replayed on a second index with REPLAY_LINEAGE.
    const Consensus::Params params{V2Params()};
    bridge_test::ObservationWorld world{HealthyWorld(params, 3, 500)};
    world.SetReserve(3, 500, 0, 520);
    bridge::VerifierStateV2 halted;
    halted.current_epoch = 4;
    halted.last_rotation_time = 900;
    halted.last_certificate_time = 950;
    halted.latest_bridge_height = 105;
    halted.status = bridge::VERIFIER_STATUS_HALTED_CONFLICT;
    halted.current_set_hash = TestHash(0x41);
    halted.next_set_hash = TestHash(0x42);
    halted.latest_height = 99;
    halted.latest_index = 1;
    halted.latest_init_time = 800;
    halted.latest_cert_id = TestHash(0x43);
    world.SetVerifier(halted);
    const uint256 root{world.StateRoot()};
    const uint256 anchor{AnchorHash(800)};
    const CTransactionRef token{Observe(world, SUBJECT_TOKEN, anchor)};
    const CTransactionRef reserve{Observe(world, SUBJECT_RESERVE, anchor)};
    const CTransactionRef verifier{Observe(world, SUBJECT_VERIFIER, anchor)};

    node::BridgeStateIndex index{ObservationIndex(params, root)};
    // Each record also verifies alone, as the mempool previews it (a
    // verifier that is not LIVE needs no lineage).
    for (const CTransactionRef& tx : {token, reserve, verifier}) {
        node::BridgeTxAuthorization authorization;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyTransaction(*tx, BRIDGE_V2_HEIGHT,
                                                    1'060, params,
                                                    authorization, error),
                            error);
        BOOST_CHECK(!authorization.mint && !authorization.HasWithdrawal());
    }
    const node::BridgeBlockDelta delta{
        ConnectNext(index, params, {token, reserve, verifier})};
    BOOST_CHECK(delta.v2_store);
    BOOST_REQUIRE_EQUAL(delta.observations_v2_added.size(), 3U);
    for (const uint8_t subject : {SUBJECT_VERIFIER, SUBJECT_RESERVE, SUBJECT_TOKEN}) {
        const auto latest{index.LatestObservationV2(subject)};
        BOOST_REQUIRE(latest);
        BOOST_CHECK_EQUAL(latest->subject, subject);
        BOOST_CHECK(latest->anchor_hash == anchor);
        BOOST_CHECK_EQUAL(latest->anchor_block, 800U);
        BOOST_CHECK_EQUAL(latest->t_obs, 1'000);
        BOOST_CHECK_EQUAL(latest->verdict, 0);
        BOOST_CHECK_EQUAL(latest->connected_height, BRIDGE_V2_HEIGHT);
        BOOST_CHECK(latest->connected_block == V2TestBlockHash(BRIDGE_V2_HEIGHT));
        BOOST_CHECK_EQUAL(latest->state.index(), size_t{subject} - 1);
    }
    BOOST_CHECK(!index.LatestObservationV2(0));
    BOOST_CHECK(!index.LatestObservationV2(4));
    {
        bridge::VerifierStateV2 expected{halted};
        expected.verifier_code_hash = *params.bridge_v2->verifier.runtime_code_hash;
        BOOST_CHECK(std::get<bridge::VerifierStateV2>(
                        index.LatestObservationV2(SUBJECT_VERIFIER)->state) ==
                    expected);
        const auto& r{std::get<bridge::ReserveStateV2>(
            index.LatestObservationV2(SUBJECT_RESERVE)->state)};
        BOOST_CHECK_EQUAL(r.next_deposit_id, 3U);
        BOOST_CHECK_EQUAL(r.locked, 500);
        BOOST_CHECK_EQUAL(r.balance, 520);
        BOOST_CHECK_EQUAL(r.absorbed_total, 0);
        BOOST_CHECK(r.vault_code_hash == *params.bridge_v2->vault.vault_runtime_code_hash);
        BOOST_CHECK(r.token_code_hash == params.bridge_v2->vault.token_runtime_code_hash);
        const auto& t{std::get<bridge::TokenFlagsV2>(
            index.LatestObservationV2(SUBJECT_TOKEN)->state)};
        BOOST_CHECK(!t.paused && !t.deprecated && !t.vault_blacklisted);
    }
    // Nothing minted or burned: the ledger is empty and invariant R holds.
    BOOST_CHECK(index.ReserveLedgerV2() == bridge::ReserveLedgerV2{});
    BOOST_CHECK(!index.ConflictV2());
    BOOST_CHECK(delta.observations_v2_before == node::BridgeObservationSlotsV2{});

    // Undo empties the slots; redo is identical.
    Disconnect(index);
    BOOST_CHECK(index.ObservationsV2() == node::BridgeObservationSlotsV2{});
    const node::BridgeBlockDelta redo{
        ConnectNext(index, params, {token, reserve, verifier})};
    CheckSameV2Delta(delta, redo);

    // Replay from scratch (BridgeStateTracker's path) is field-for-field
    // the incremental index.
    node::BridgeStateIndex replay{ObservationIndex(params, root)};
    node::BridgeBlockDelta replayed;
    std::string error;
    BOOST_REQUIRE_MESSAGE(replay.VerifyBlock(Block(1'060, {token, reserve, verifier}),
                                             BRIDGE_V2_HEIGHT,
                                             V2TestBlockHash(BRIDGE_V2_HEIGHT),
                                             params, replayed, error,
                                             &node::REPLAY_LINEAGE),
                          error);
    BOOST_REQUIRE_MESSAGE(replay.ConnectBlock(replayed, error), error);
    CheckSameV2Delta(delta, replayed);
    CheckSameAccounting(index, replay);
}

BOOST_AUTO_TEST_CASE(v2_observation_refusals)
{
    const Consensus::Params params{V2Params()};
    const bridge_test::ObservationWorld world{HealthyWorld(params)};
    const uint256 root{world.StateRoot()};
    const uint256 anchor{AnchorHash(800)};
    const CTransactionRef token{Observe(world, SUBJECT_TOKEN, anchor)};
    node::BridgeStateIndex index{ObservationIndex(params, root)};

    // Below V, and on every chain without a bridge V2 envelope, kind 9 is
    // not a record at all: the codec refuses it as before (as the MPA
    // structure rule does too).
    VerifyFails(index, Block(1'060, {token}), BRIDGE_V2_HEIGHT - 1, params,
                "bridge record is malformed");
    {
        std::string mpa_error;
        BOOST_CHECK(!modern::CheckTransactionMpa(*token, params,
                                                 BRIDGE_V2_HEIGHT - 1,
                                                 mpa_error));
        BOOST_CHECK_EQUAL(mpa_error, "mpa-malformed-bridge-record");
        mpa_error.clear();
        BOOST_CHECK_MESSAGE(modern::CheckTransactionMpa(*token, params,
                                                        BRIDGE_V2_HEIGHT,
                                                        mpa_error),
                            mpa_error);
        Consensus::Params no_v2{params};
        no_v2.bridge_v2.reset();
        VerifyFails(index, Block(1'060, {token}), BRIDGE_V2_HEIGHT + 5, no_v2,
                    "bridge record is malformed");
        mpa_error.clear();
        BOOST_CHECK(!modern::CheckTransactionMpa(*token, no_v2,
                                                 BRIDGE_V2_HEIGHT + 5,
                                                 mpa_error));
    }
    // An unknown anchor; no V2 light client; a stale light client.
    VerifyFails(index, Block(1'060, {Observe(world, SUBJECT_TOKEN, TestHash(0x99))}),
                BRIDGE_V2_HEIGHT, params, "names an unretained execution anchor");
    {
        node::BridgeStateIndex no_light_client;
        ConnectEmptyThrough(no_light_client, params, BRIDGE_V2_HEIGHT - 2);
        SeedStateAnchorsV2(no_light_client, {StateAnchor(800, 1'000, root)});
        VerifyFails(no_light_client, Block(1'060, {token}), BRIDGE_V2_HEIGHT,
                    params, "precedes light-client bootstrap");
    }
    const uint64_t lag_seconds{params.bridge_v2->light_client.max_sync_lag_slots * 12};
    VerifyFails(index,
                Block(static_cast<uint32_t>(1'000 + lag_seconds + 1), {token}),
                BRIDGE_V2_HEIGHT, params, "light-client head is stale");
    {
        node::BridgeBlockDelta at_limit;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyBlock(
                                Block(static_cast<uint32_t>(1'000 + lag_seconds),
                                      {token}),
                                BRIDGE_V2_HEIGHT, V2TestBlockHash(BRIDGE_V2_HEIGHT),
                                params, at_limit, error),
                            error);
    }

    // An anchor without a state root (a header lacking a 32-byte stateRoot
    // still backfills) proves nothing.
    {
        node::BridgeStateIndex rootless{ObservationIndex(params, root)};
        SeedStateAnchorsV2(rootless, {StateAnchor(801, 1'010, uint256{})});
        VerifyFails(rootless,
                    Block(1'060, {Observe(world, SUBJECT_TOKEN, AnchorHash(801))}),
                    BRIDGE_V2_HEIGHT + 1, params, "anchor has no state root");
    }

    // An anchor added by the same block cannot be read; from the next block
    // it can.
    {
        const uint256 receipts_799{TestBlockHash(799)};
        const uint256 receipts_800{TestBlockHash(800)};
        const std::vector<unsigned char> header_799{ExecutionHeaderWithStateRoot(
            uint256{}, 799, receipts_799, 990, root)};
        const uint256 hash_799{Keccak(header_799)};
        const std::vector<unsigned char> header_800{ExecutionHeaderWithStateRoot(
            hash_799, 800, receipts_800, 1'000, root)};
        const uint256 hash_800{Keccak(header_800)};
        node::BridgeStateIndex chain;
        ConnectEmptyThrough(chain, params, BRIDGE_V2_HEIGHT - 2);
        const StateAnchorSpec head{800, hash_800, 1'000, root, receipts_800};
        SeedStateAnchorsV2(chain, {head}, head);
        const CTransactionRef backfill{
            BackfillTransaction(hash_800, 799, {header_800, header_799})};
        const CTransactionRef reads_799{Observe(world, SUBJECT_TOKEN, hash_799)};
        VerifyFails(chain, Block(1'060, {backfill, reads_799}), BRIDGE_V2_HEIGHT,
                    params, "names an anchor connected by the same block");
        ConnectNext(chain, params, {backfill});
        const auto backfilled{chain.AnchorV2(hash_799)};
        BOOST_REQUIRE(backfilled);
        BOOST_CHECK(backfilled->state_root == root);
        ConnectNext(chain, params, {reads_799});
        BOOST_CHECK_EQUAL(chain.LatestObservationV2(SUBJECT_TOKEN)->anchor_block, 799U);
        BOOST_CHECK_EQUAL(chain.LatestObservationV2(SUBJECT_TOKEN)->t_obs, 990);
    }

    // Anchors increase per subject, within one block too.
    ConnectNext(index, params, {token});
    VerifyFails(index, Block(1'061, {token}), BRIDGE_V2_HEIGHT + 1, params,
                "not newer than the subject's latest observation");
    SeedStateAnchorsV2(index, {StateAnchor(799, 990, root),
                               StateAnchor(801, 1'010, root),
                               StateAnchor(802, 1'020, root)});
    const CTransactionRef at_799{Observe(world, SUBJECT_TOKEN, AnchorHash(799))};
    const CTransactionRef at_801{Observe(world, SUBJECT_TOKEN, AnchorHash(801))};
    const CTransactionRef at_802{Observe(world, SUBJECT_TOKEN, AnchorHash(802))};
    const int next{index.ConnectedHeight() + 1};
    VerifyFails(index, Block(1'062, {at_799}), next, params,
                "not newer than the subject's latest observation");
    VerifyFails(index, Block(1'062, {at_802, at_801}), next, params,
                "not newer than the subject's latest observation");
    VerifyFails(index, Block(1'062, {at_801, at_801}), next, params,
                "not newer than the subject's latest observation");
    // Another subject keeps its own sequence: a reserve record at anchor 800
    // follows token records at 801 and 802.
    ConnectNext(index, params,
                {at_801, at_802, Observe(world, SUBJECT_RESERVE, anchor)}, 1'062);
    BOOST_CHECK_EQUAL(index.LatestObservationV2(SUBJECT_TOKEN)->anchor_block, 802U);
    BOOST_CHECK_EQUAL(index.LatestObservationV2(SUBJECT_TOKEN)->t_obs, 1'020);
    BOOST_CHECK_EQUAL(index.LatestObservationV2(SUBJECT_RESERVE)->anchor_block, 800U);

    // Pinned code hashes, and the OI-3 unpinned rule on 31337 only.
    {
        bridge_test::ObservationWorld wrong_code{world};
        wrong_code.code[params.bridge_v2->vault.vault_address] = TestHash(0x77);
        node::BridgeStateIndex other{ObservationIndex(params, wrong_code.StateRoot())};
        VerifyFails(other,
                    Block(1'060, {Observe(wrong_code, SUBJECT_RESERVE, anchor)}),
                    BRIDGE_V2_HEIGHT, params,
                    "bridge observation is invalid: observation vault code hash does not match its pin");
    }
    {
        Consensus::Params anvil{V2Params()};
        anvil.bridge_v2->vault.origin_chain_id = bridge::OBSERVATION_UNPINNED_CODE_CHAIN_ID;
        anvil.bridge_v2->vault.vault_runtime_code_hash.reset();
        anvil.bridge_v2->verifier.runtime_code_hash.reset();
        BOOST_REQUIRE(anvil.bridge_v2->Valid());
        bridge_test::ObservationWorld empty_vault{HealthyWorld(anvil)};
        empty_vault.code[anvil.bridge_v2->vault.vault_address] = bridge::EMPTY_CODE_HASH;
        node::BridgeStateIndex unpinned{ObservationIndex(anvil, empty_vault.StateRoot())};
        VerifyFails(unpinned,
                    Block(1'060, {Observe(empty_vault, SUBJECT_RESERVE, anchor)}),
                    BRIDGE_V2_HEIGHT, anvil,
                    "bridge observation is invalid: observation vault has no code");
        const bridge_test::ObservationWorld with_code{HealthyWorld(anvil)};
        node::BridgeStateIndex accepted{ObservationIndex(anvil, with_code.StateRoot())};
        ConnectNext(accepted, anvil, {Observe(with_code, SUBJECT_RESERVE, anchor)});
        BOOST_CHECK(accepted.LatestObservationV2(SUBJECT_RESERVE));
        // The same unpinned configuration is refused off 31337.
        Consensus::Params unpinned_mainnet_like{anvil};
        unpinned_mainnet_like.bridge_v2->vault.origin_chain_id = 5;
        node::BridgeStateIndex refused{ObservationIndex(unpinned_mainnet_like,
                                                        with_code.StateRoot())};
        VerifyFails(refused,
                    Block(1'060, {Observe(with_code, SUBJECT_RESERVE, anchor)}),
                    BRIDGE_V2_HEIGHT, unpinned_mainnet_like,
                    "code hash is unpinned off the regtest chain");
    }
    // A proof against another state root.
    {
        bridge_test::ObservationWorld changed{world};
        changed.SetReserve(9, 1, 0, 1);
        node::BridgeStateIndex other{ObservationIndex(params, root)};
        VerifyFails(other,
                    Block(1'060, {Observe(changed, SUBJECT_RESERVE, anchor)}),
                    BRIDGE_V2_HEIGHT, params, "bridge observation is invalid");
    }
}

BOOST_AUTO_TEST_CASE(v2_verifier_verdicts)
{
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    bridge_test::ObservationWorld world{HealthyWorld(params)};
    node::BridgeStateIndex index{ObservationIndex(params, uint256{uint8_t{1}})};
    const int height{BRIDGE_V2_HEIGHT};
    const bridge::VerifierStateV2 canonical{lineage.Canonical(1, 109, params, index)};
    const auto verdict = [&](const bridge::VerifierStateV2& v,
                             const node::BridgeV2LineageContext& context,
                             const int at, std::string& error) {
        return node::VerifierVerdictV2(v, context, params, index, at, error);
    };
    std::string error;
    BOOST_CHECK(verdict(canonical, lineage.context, height, error) ==
                std::optional<uint8_t>{0});

    // E4: either pinned set differs.
    bridge::VerifierStateV2 v{canonical};
    v.next_set_hash = TestHash(0x5e);
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_SETS});
    v = canonical;
    v.current_set_hash = lineage.sets[0]->set_hash;
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_SETS});
    // E5: another certId, an off-grid height, an index whose grid height
    // differs, index 0.
    v = canonical;
    v.latest_cert_id = TestHash(0x5f);
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_CERT});
    v = canonical;
    v.latest_height = 108;
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_CERT});
    v = canonical;
    v.latest_index = 1;
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_CERT});
    v = canonical;
    v.latest_index = 0;
    v.latest_height = LineageFixture::GENESIS;
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_CERT});
    // B3's statement at another epoch is not the one the verifier signed.
    v = canonical;
    v.latest_cert_id =
        modern::FinalityCertIdV2(lineage.Statement(0, 109, params, index));
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_CERT});
    v.next_set_hash = TestHash(0x5e);
    BOOST_CHECK(verdict(v, lineage.context, height, error) ==
                std::optional<uint8_t>{VERDICT_SETS | VERDICT_CERT});
    // CertIdAt(index) is B3's statement in the epoch of the checkpoint's
    // height only (as kind 13's check 9): a Latest whose epoch is another
    // one conflicts even when every other field is B3's. An epoch-0 verifier
    // (Set'_0 / Set'_1 pinned, E4 clean) at index 2 (H = 109, epoch 1)
    // holding B3's epoch-0 rebuild there, and an epoch-1 verifier at index 1
    // (H = 99, epoch 0) holding B3's epoch-1 rebuild there.
    {
        bridge::VerifierStateV2 wrong_epoch{canonical};
        wrong_epoch.current_epoch = 0;
        wrong_epoch.current_set_hash = lineage.sets[0]->set_hash;
        wrong_epoch.next_set_hash = lineage.sets[1]->set_hash;
        wrong_epoch.latest_cert_id =
            modern::FinalityCertIdV2(lineage.Statement(0, 109, params, index));
        BOOST_CHECK(verdict(wrong_epoch, lineage.context, height, error) ==
                    std::optional<uint8_t>{VERDICT_CERT});
        wrong_epoch = canonical;
        wrong_epoch.latest_height = 99;
        wrong_epoch.latest_index = 1;
        wrong_epoch.latest_cert_id =
            modern::FinalityCertIdV2(lineage.Statement(1, 99, params, index));
        BOOST_CHECK(verdict(wrong_epoch, lineage.context, height, error) ==
                    std::optional<uint8_t>{VERDICT_CERT});
        // The same checkpoint in its own epoch is B3's certificate.
        wrong_epoch.latest_cert_id =
            modern::FinalityCertIdV2(lineage.Statement(0, 99, params, index));
        wrong_epoch.current_epoch = 0;
        wrong_epoch.current_set_hash = lineage.sets[0]->set_hash;
        wrong_epoch.next_set_hash = lineage.sets[1]->set_hash;
        BOOST_CHECK(wrong_epoch == lineage.Canonical(0, 99, params, index));
        BOOST_CHECK(verdict(wrong_epoch, lineage.context, height, error) ==
                    std::optional<uint8_t>{0});
    }
    // A verifier one epoch behind is judged against Set'_0 / Set'_1.
    const bridge::VerifierStateV2 behind{lineage.Canonical(0, 99, params, index)};
    BOOST_CHECK(behind.current_set_hash == lineage.sets[0]->set_hash);
    BOOST_CHECK(verdict(behind, lineage.context, height, error) ==
                std::optional<uint8_t>{0});
    // Two epochs behind (E3's max_epoch_gap = 2, section 8.6): the lineage
    // retains Set'_{e-2}, so the epoch-1 verifier is judged at e_B3 = 3
    // against Set'_1 / Set'_2 and B3's canonical epoch-1 statement.
    {
        const LineageFixture later{3};
        BOOST_REQUIRE(later.context.fresh->previous2);
        BOOST_CHECK(later.context.fresh->previous2->set_hash == lineage.sets[1]->set_hash);
        BOOST_CHECK(verdict(canonical, later.context, height, error) ==
                    std::optional<uint8_t>{0});
        BOOST_CHECK(later.Canonical(1, 109, params, index) == canonical);
        bridge::VerifierStateV2 gap2{canonical};
        gap2.next_set_hash = TestHash(0x5e);
        BOOST_CHECK(verdict(gap2, later.context, height, error) ==
                    std::optional<uint8_t>{VERDICT_SETS});
        // The pair one epoch on (Set'_2 / Set'_3) is not the epoch-1 pair.
        gap2 = canonical;
        gap2.current_set_hash = later.sets[2]->set_hash;
        gap2.next_set_hash = later.sets[3]->set_hash;
        BOOST_CHECK(verdict(gap2, later.context, height, error) ==
                    std::optional<uint8_t>{VERDICT_SETS});
        // B3's epoch-2 statement at the same checkpoint is another certId.
        gap2 = canonical;
        gap2.latest_cert_id =
            modern::FinalityCertIdV2(later.Statement(2, 109, params, index));
        BOOST_CHECK(verdict(gap2, later.context, height, error) ==
                    std::optional<uint8_t>{VERDICT_CERT});
        // Gap 2 down to the genesis committee: e_B3 = 2, e_E = 0.
        const LineageFixture second{2};
        const bridge::VerifierStateV2 genesis_verifier{second.Canonical(0, 99, params, index)};
        BOOST_CHECK(genesis_verifier == behind);
        BOOST_CHECK(verdict(genesis_verifier, second.context, height, error) ==
                    std::optional<uint8_t>{0});
    }

    // Records that cannot be judged are invalid.
    const auto refused = [&](const bridge::VerifierStateV2& state,
                             const node::BridgeV2LineageContext& context,
                             const int at, const std::string& expected) {
        std::string why;
        BOOST_CHECK(!verdict(state, context, at, why));
        BOOST_CHECK_MESSAGE(why.find(expected) != std::string::npos,
                            "'" << why << "' lacks '" << expected << "'");
    };
    v = canonical;
    v.current_epoch = 2;
    refused(v, lineage.context, height,
            "bridge observation verifier epoch is outside the lineage's retained epochs (e_B3 - 2 .. e_B3)");
    {
        // Gap 3: Set'_1 is no longer retained at e_B3 = 4, whatever the
        // gate's max_epoch_gap.
        const LineageFixture gap3{4};
        BOOST_CHECK(!gap3.context.fresh->SetHashForEpoch(1));
        refused(canonical, gap3.context, height,
                "bridge observation verifier epoch is outside the lineage's retained epochs (e_B3 - 2 .. e_B3)");
        Consensus::Params wide{params};
        wide.bridge_v2->gate.max_epoch_gap = 3;
        std::string why;
        BOOST_CHECK(!node::VerifierVerdictV2(canonical, gap3.context, wide, index, height, why));
        BOOST_CHECK(why.find("outside the lineage's retained epochs") != std::string::npos);
    }
    refused(canonical, lineage.context, 109, "above the parent block");
    {
        LineageFixture broken;
        broken.Fresh().lineage_broken = true;
        refused(canonical, broken.context, height, "broken fresh lineage");
        LineageFixture unbootstrapped;
        unbootstrapped.Fresh().bootstrapped = false;
        refused(canonical, unbootstrapped.context, height, "bootstrapped fresh lineage");
        refused(canonical, node::BridgeV2LineageContext{}, height,
                "bootstrapped fresh lineage");
    }
    // At or above V the statement carries the indexed withdrawal root, so a
    // checkpoint the index has not connected cannot be judged.
    {
        bridge::VerifierStateV2 future{canonical};
        future.latest_height = 119;
        future.latest_index = 3;
        refused(future, lineage.context, 125,
                "B3 cannot form its statement at the verifier's latest checkpoint");
    }

    // Through the index: the record's verdict must equal B3's.
    world.SetVerifier(canonical);
    node::BridgeStateIndex judged{ObservationIndex(params, world.StateRoot())};
    const uint256 anchor{AnchorHash(800)};
    VerifyFails(judged, Block(1'060, {Observe(world, SUBJECT_VERIFIER, anchor, VERDICT_SETS)}),
                height, params, "verdict does not match B3's lineage",
                &lineage.context);
    VerifyFails(judged, Block(1'060, {Observe(world, SUBJECT_VERIFIER, anchor)}),
                height, params, "verdict cannot be judged here");
    {
        LineageFixture gap3{4};
        VerifyFails(judged, Block(1'060, {Observe(world, SUBJECT_VERIFIER, anchor)}),
                    height, params, "outside the lineage's retained epochs (e_B3 - 2 .. e_B3)",
                    &gap3.context);
    }
    // Gap 2 connects with verdict 0, and the gate passes E3 (READY with
    // fresh token and reserve observations). Once B3 rotates again the same
    // observation trails by 3 and E3 closes the gate.
    {
        const LineageFixture later{3};
        node::BridgeStateIndex gap2{ObservationIndex(params, world.StateRoot())};
        VerifyFails(gap2, Block(1'060, {Observe(world, SUBJECT_VERIFIER, anchor, VERDICT_SETS)}),
                    height, params, "verdict does not match B3's lineage", &later.context);
        ConnectNext(gap2, params,
                    {Observe(world, SUBJECT_TOKEN, anchor),
                     Observe(world, SUBJECT_RESERVE, anchor),
                     Observe(world, SUBJECT_VERIFIER, anchor)},
                    1'060, &later.context);
        BOOST_CHECK_EQUAL(gap2.LatestObservationV2(SUBJECT_VERIFIER)->verdict, 0);
        BOOST_CHECK(!gap2.ConflictV2());
        const auto ready{node::EvaluateBridgeV2BurnGate(
            &later.context, gap2, params, gap2.ConnectedHeight() + 1, 1'061)};
        BOOST_CHECK_MESSAGE(ready.readiness == node::BridgeBurnReadiness::READY,
                            ready.Describe());
        const LineageFixture rotated{4};
        const auto aged{node::EvaluateBridgeV2BurnGate(
            &rotated.context, gap2, params, gap2.ConnectedHeight() + 1, 1'061)};
        BOOST_CHECK(aged.readiness == node::BridgeBurnReadiness::NOT_READY);
        BOOST_CHECK_EQUAL(aged.Describe(),
                          "E3: verifier epoch trails the lineage epoch by more than the limit");
    }
    ConnectNext(judged, params, {Observe(world, SUBJECT_VERIFIER, anchor)}, 1'060,
                &lineage.context);
    BOOST_CHECK_EQUAL(judged.LatestObservationV2(SUBJECT_VERIFIER)->verdict, 0);
    BOOST_CHECK(!judged.ConflictV2());

    // A recorded SETS_MISMATCH is accepted when B3 computes it (no
    // conflict: E4 closes the gate, nothing sticks).
    bridge_test::ObservationWorld mismatched{world};
    bridge::VerifierStateV2 other{canonical};
    other.next_set_hash = TestHash(0x5e);
    mismatched.SetVerifier(other);
    SeedStateAnchorsV2(judged, {StateAnchor(801, 1'010, mismatched.StateRoot())});
    const int next{judged.ConnectedHeight() + 1};
    VerifyFails(judged, Block(1'061, {Observe(mismatched, SUBJECT_VERIFIER, AnchorHash(801))}),
                next, params, "verdict does not match B3's lineage", &lineage.context);
    ConnectNext(judged, params,
                {Observe(mismatched, SUBJECT_VERIFIER, AnchorHash(801), VERDICT_SETS)},
                1'061, &lineage.context);
    BOOST_CHECK_EQUAL(judged.LatestObservationV2(SUBJECT_VERIFIER)->verdict, VERDICT_SETS);
    BOOST_CHECK(!judged.ConflictV2());

    // A verifier that is not LIVE records verdict 0, judged or not.
    bridge_test::ObservationWorld cancelled{world};
    bridge::VerifierStateV2 gone{canonical};
    gone.status = bridge::VERIFIER_STATUS_CANCELLED;
    cancelled.SetVerifier(gone);
    SeedStateAnchorsV2(judged, {StateAnchor(802, 1'020, cancelled.StateRoot())});
    const int after{judged.ConnectedHeight() + 1};
    VerifyFails(judged, Block(1'062, {Observe(cancelled, SUBJECT_VERIFIER, AnchorHash(802), VERDICT_CERT)}),
                after, params, "verdict must be zero for a verifier that is not LIVE",
                &node::REPLAY_LINEAGE);
    ConnectNext(judged, params, {Observe(cancelled, SUBJECT_VERIFIER, AnchorHash(802))}, 1'062);
    BOOST_CHECK_EQUAL(std::get<bridge::VerifierStateV2>(
                          judged.LatestObservationV2(SUBJECT_VERIFIER)->state)
                          .status,
                      bridge::VERIFIER_STATUS_CANCELLED);
}

BOOST_AUTO_TEST_CASE(v2_replay_trusts_recorded_verdicts)
{
    // A LIVE VERIFIER_E whose latestCertId matches B3 but whose record says
    // CERT_CONFLICT: a full context refuses it, no context refuses it, and
    // only replay (which trusts what ConnectBlock already judged) applies
    // the recorded byte, sticky conflict included.
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    bridge_test::ObservationWorld world{HealthyWorld(params)};
    node::BridgeStateIndex probe{ObservationIndex(params, world.StateRoot())};
    world.SetVerifier(lineage.Canonical(1, 109, params, probe));
    node::BridgeStateIndex index{ObservationIndex(params, world.StateRoot())};
    const CTransactionRef record{
        Observe(world, SUBJECT_VERIFIER, AnchorHash(800), VERDICT_CERT)};
    VerifyFails(index, Block(1'060, {record}), BRIDGE_V2_HEIGHT, params,
                "verdict does not match B3's lineage", &lineage.context);
    VerifyFails(index, Block(1'060, {record}), BRIDGE_V2_HEIGHT, params,
                "verdict cannot be judged here");
    const node::BridgeBlockDelta delta{
        ConnectNext(index, params, {record}, 1'060, &node::REPLAY_LINEAGE)};
    BOOST_CHECK_EQUAL(index.LatestObservationV2(SUBJECT_VERIFIER)->verdict, VERDICT_CERT);
    BOOST_REQUIRE(index.ConflictV2());
    BOOST_CHECK(*index.ConflictV2() ==
                (node::BridgeV2Conflict{bridge::BRIDGE_V2_CONFLICT_E5, BRIDGE_V2_HEIGHT,
                                        V2TestBlockHash(BRIDGE_V2_HEIGHT), 800}));
    BOOST_CHECK(!delta.conflict_v2_before);
    BOOST_CHECK(delta.conflict_v2_after == index.ConflictV2());
}

BOOST_AUTO_TEST_CASE(v2_sticky_conflict)
{
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{FundingReceipts(params)};
    // E5: a LIVE verifier whose latestCertId is not B3's certificate.
    bridge_test::ObservationWorld world{HealthyWorld(params, 2, 100)};
    node::BridgeStateIndex probe{FundingIndex(params, receipts, world.StateRoot())};
    const bridge::VerifierStateV2 canonical{lineage.Canonical(1, 109, params, probe)};
    bridge::VerifierStateV2 forged{canonical};
    forged.latest_cert_id = TestHash(0x66);
    world.SetVerifier(forged);
    node::BridgeStateIndex index{FundingIndex(params, receipts, world.StateRoot())};
    VerifyFails(index, Block(1'060, {Observe(world, SUBJECT_VERIFIER, receipts.block_hash)}),
                BRIDGE_V2_HEIGHT, params, "verdict does not match B3's lineage",
                &lineage.context);
    const node::BridgeBlockDelta setting{ConnectNext(
        index, params, {Observe(world, SUBJECT_VERIFIER, receipts.block_hash, VERDICT_CERT)},
        1'060, &lineage.context)};
    const node::BridgeV2Conflict e5{bridge::BRIDGE_V2_CONFLICT_E5, BRIDGE_V2_HEIGHT,
                                    V2TestBlockHash(BRIDGE_V2_HEIGHT), 700};
    BOOST_REQUIRE(index.ConflictV2());
    BOOST_CHECK(*index.ConflictV2() == e5);
    BOOST_CHECK(setting.conflict_v2_after == std::optional<node::BridgeV2Conflict>{e5});
    // The gate stays closed at C4 whatever follows.
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params,
                                               BRIDGE_V2_HEIGHT + 1, 1'061)
                    .row == bridge::BurnGateRowV2::C4);

    // A later good VERIFIER_E and a later E8 failure leave it as it is.
    bridge_test::ObservationWorld good{world};
    good.SetVerifier(canonical);
    good.SetReserve(2, 0, 0, 0); // locked 0 < 100 proven below the cut
    SeedStateAnchorsV2(index, {StateAnchor(701, 1'010, good.StateRoot())});
    ConnectNext(index, params,
                {MintTransactionV2(params, receipts, 0, 60)}, 1'061);
    ConnectNext(index, params,
                {MintTransactionV2(params, receipts, 1, 40),
                 Observe(good, SUBJECT_VERIFIER, AnchorHash(701)),
                 Observe(good, SUBJECT_RESERVE, AnchorHash(701))},
                1'062, &lineage.context);
    BOOST_CHECK_EQUAL(index.LatestObservationV2(SUBJECT_VERIFIER)->verdict, 0);
    BOOST_CHECK(index.ConflictV2() == std::optional<node::BridgeV2Conflict>{e5});
    const node::BridgeBlockDelta last{index.History().back()};
    BOOST_CHECK(last.conflict_v2_before == last.conflict_v2_after);
    // Undo above the setting block keeps it; undoing that block clears it.
    Disconnect(index);
    Disconnect(index);
    Disconnect(index);
    BOOST_CHECK(index.ConflictV2() == std::optional<node::BridgeV2Conflict>{e5});
    Disconnect(index);
    BOOST_CHECK(!index.ConflictV2());
    BOOST_CHECK(index.ObservationsV2() == node::BridgeObservationSlotsV2{});

    // E8: proven deposits below the observed cut exceed locked. The record
    // still connects (it is a true observation) and its block sets the
    // conflict.
    bridge_test::ObservationWorld drained{HealthyWorld(params, 2, 100)};
    drained.SetReserve(2, 99, 0, 99);
    node::BridgeStateIndex vault{FundingIndex(params, receipts, drained.StateRoot())};
    ConnectNext(vault, params, {MintTransactionV2(params, receipts, 0, 60)});
    ConnectNext(vault, params, {MintTransactionV2(params, receipts, 1, 40)}, 1'061);
    BOOST_CHECK(vault.ProvenDepositsV2() ==
                (bridge::ProvenDepositMapV2{{0, 60}, {1, 40}}));
    const node::BridgeBlockDelta e8_delta{ConnectNext(
        vault, params, {Observe(drained, SUBJECT_RESERVE, receipts.block_hash)}, 1'062)};
    const node::BridgeV2Conflict e8{bridge::BRIDGE_V2_CONFLICT_E8, BRIDGE_V2_HEIGHT + 2,
                                    V2TestBlockHash(BRIDGE_V2_HEIGHT + 2), 700};
    BOOST_CHECK(vault.ConflictV2() == std::optional<node::BridgeV2Conflict>{e8});
    BOOST_CHECK(e8_delta.conflict_v2_after == std::optional<node::BridgeV2Conflict>{e8});
    BOOST_CHECK(vault.LatestObservationV2(SUBJECT_RESERVE));
    Disconnect(vault);
    BOOST_CHECK(!vault.ConflictV2());
    // The honest vault (locked 100) holds invariant R.
    bridge_test::ObservationWorld honest{HealthyWorld(params, 2, 100)};
    node::BridgeStateIndex honest_vault{FundingIndex(params, receipts, honest.StateRoot())};
    ConnectNext(honest_vault, params, {MintTransactionV2(params, receipts, 0, 60)});
    ConnectNext(honest_vault, params, {MintTransactionV2(params, receipts, 1, 40),
                                       Observe(honest, SUBJECT_RESERVE, receipts.block_hash)},
                1'061);
    BOOST_CHECK(!honest_vault.ConflictV2());
}

BOOST_AUTO_TEST_CASE(v2_finality_conflict_judge)
{
    // Kind 13 (node changes section 8.5): a digest-V2 certificate of the
    // lineage's CURRENT or NEXT committee that differs from B3's canonical
    // statement at its checkpoint. LineageFixture is at e_B3 = 1 (Set'_1
    // current, Set'_2 next, the same four members), epoch_starts {90, 100},
    // checkpoints 99 (index 1, epoch 0), 109 (index 2, epoch 1), ...; the
    // index is connected at V - 1 = 109, so a record in the block at V can
    // name index 2 (H = 109) but not index 3 (H = 119).
    const Consensus::Params params{LineageParams()};
    BOOST_REQUIRE(modern::ModernChainDomain(params.hashGenesisBlock,
                                            *params.legacy_final_hash) ==
                  FixtureChainDomain());
    BOOST_REQUIRE_EQUAL(params.modern_pos->checkpoint_interval, LineageFixture::INTERVAL);
    const LineageFixture lineage;
    node::BridgeStateIndex index{ObservationIndex(params, uint256{uint8_t{1}})};
    const int height{BRIDGE_V2_HEIGHT};
    BOOST_REQUIRE_EQUAL(index.ConnectedHeight(), height - 1);
    const auto judge = [&](const bridge::BridgeFinalityConflictV2& record,
                           const node::BridgeV2LineageContext& context,
                           const int at, std::string& error) {
        return node::JudgeFinalityConflictV2(record, context, params, index, at,
                                             error);
    };
    const auto refused = [&](const bridge::BridgeFinalityConflictV2& record,
                             const node::BridgeV2LineageContext& context,
                             const int at, const std::string& expected) {
        std::string why;
        BOOST_CHECK(!judge(record, context, at, why));
        BOOST_CHECK_MESSAGE(why == expected || why.find(expected) != std::string::npos,
                            "'" << why << "' lacks '" << expected << "'");
    };
    const auto signed_record = [&](const modern::FinalizedBlockV2& fb) {
        return ConflictRecord(fb, lineage.Signed(fb));
    };

    // Accepted: the current committee's epoch-1 statement of another branch
    // at index 2, by all four members or by a quorum of three.
    const modern::FinalizedBlockV2 other{OtherBranch(lineage, 1, 109, params, index)};
    std::string error;
    {
        const auto accepted{judge(signed_record(other), lineage.context, height, error)};
        BOOST_REQUIRE_MESSAGE(accepted, error);
        BOOST_CHECK(*accepted == other);
        BOOST_CHECK(judge(ConflictRecord(other, LineageFixture::Certificate(
                                                    other, {0, 1, 3}, lineage.keys)),
                          lineage.context, height, error));
    }
    // Accepted: the next committee (Set'_2) at the same checkpoint. B3 signs
    // index 2 only in epoch 1, so an epoch-2 statement differs whatever its
    // other fields, even the canonical block hash.
    {
        modern::FinalizedBlockV2 next_epoch{lineage.Statement(1, 109, params, index)};
        next_epoch.epoch = 2;
        const auto accepted{judge(signed_record(next_epoch), lineage.context, height, error)};
        BOOST_REQUIRE_MESSAGE(accepted, error);
        BOOST_CHECK_EQUAL(accepted->epoch, 2U);
    }
    // Accepted: the canonical statement with another successor hash (the
    // successor field is deliberately unchecked), and a current-committee
    // statement at an index of epoch 0 (H = 99), which B3 signs in epoch 0.
    {
        modern::FinalizedBlockV2 successor{lineage.Statement(1, 109, params, index)};
        successor.validator_set_hash = TestHash(0x5e);
        BOOST_CHECK_MESSAGE(judge(signed_record(successor), lineage.context, height, error), error);
        modern::FinalizedBlockV2 earlier{lineage.Statement(0, 99, params, index)};
        earlier.epoch = 1;
        BOOST_CHECK_MESSAGE(judge(signed_record(earlier), lineage.context, height, error), error);
    }

    // Refused: B3's canonical statement itself, however aggregated.
    {
        const modern::FinalizedBlockV2 canonical{lineage.Statement(1, 109, params, index)};
        refused(signed_record(canonical), lineage.context, height,
                "bridge finality conflict equals B3's canonical statement");
        refused(ConflictRecord(canonical, LineageFixture::Certificate(
                                              canonical, {1, 2, 3}, lineage.keys)),
                lineage.context, height,
                "bridge finality conflict equals B3's canonical statement");
    }
    // Check 9 field by field. Index 2 is the fixture's finalized index, so
    // B3's history root there is the finalized checkpoint's recorded root;
    // without a finalized checkpoint it is rebuilt (FreshHistoryRootAt),
    // with the same verdicts. Each field B3 forms, changed alone, is a
    // conflict; the canonical statement is refused on both paths.
    {
        const modern::FinalizedBlockV2 canonical{lineage.Statement(1, 109, params, index)};
        BOOST_CHECK(lineage.context.fresh->finalized->checkpoint_index == canonical.checkpoint_index);
        BOOST_CHECK(lineage.context.fresh->finalized->history_root == canonical.checkpoint_history_root);
        LineageFixture unfinalized;
        unfinalized.Fresh().finalized.reset();
        refused(signed_record(canonical), unfinalized.context, height,
                "bridge finality conflict equals B3's canonical statement");
        std::vector<modern::FinalizedBlockV2> changed(5, canonical);
        changed[0].height = 110;
        changed[1].block_hash = TestHash(0xbd);
        changed[2].withdrawal_root = TestHash(0xbe);
        changed[3].bridge_flags = modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED;
        changed[4].checkpoint_history_root = TestHash(0xbf);
        for (const modern::FinalizedBlockV2& fb : changed) {
            for (const node::BridgeV2LineageContext* context :
                 std::initializer_list<const node::BridgeV2LineageContext*>{&lineage.context,
                                                                            &unfinalized.context}) {
                const auto accepted{judge(signed_record(fb), *context, height, error)};
                BOOST_CHECK_MESSAGE(accepted && *accepted == fb, error);
            }
        }
    }
    // Refused: older committees and committees beyond the next one, with a
    // valid quorum of the same members: Set'_0 (e - 1), Set'_3 (e + 2), and
    // at e_B3 = 3 the epoch-1 statement of Set'_1 (e - 2).
    {
        const modern::FinalizedBlockV2 previous{OtherBranch(lineage, 0, 99, params, index)};
        refused(signed_record(previous), lineage.context, height,
                "bridge finality conflict is not signed by the lineage's current or next committee");
        modern::FinalizedBlockV2 beyond{other};
        beyond.epoch = 3;
        refused(signed_record(beyond), lineage.context, height,
                "bridge finality conflict is not signed by the lineage's current or next committee");
        const LineageFixture later{3};
        refused(signed_record(other), later.context, height,
                "bridge finality conflict is not signed by the lineage's current or next committee");
        modern::FinalizedBlockV2 wrapped{other};
        wrapped.epoch = std::numeric_limits<uint64_t>::max();
        refused(signed_record(wrapped), lineage.context, height,
                "bridge finality conflict is not signed by the lineage's current or next committee");
    }
    // Refused: another lineage.
    {
        modern::FinalizedBlockV2 foreign{other};
        foreign.lineage_id = TestHash(0x1e);
        refused(signed_record(foreign), lineage.context, height,
                "bridge finality conflict names another lineage");
    }
    // Refused: a bitmap sized for another committee (n = 5 decodes with the
    // same one-byte bitmap as n = 4).
    refused(ConflictRecord(other, lineage.Signed(other), 5), lineage.context, height,
            "bridge finality conflict bitmap does not match the signing committee");
    // Refused: index 0, index 2^32, an index whose height leaves the int
    // range, and checkpoints at or above the including block.
    {
        modern::FinalizedBlockV2 zero{other};
        zero.checkpoint_index = 0;
        refused(signed_record(zero), lineage.context, height,
                "bridge finality conflict checkpoint index is out of range");
        modern::FinalizedBlockV2 huge{other};
        huge.checkpoint_index = modern::MAX_FINALITY_CHECKPOINT_INDEX;
        refused(signed_record(huge), lineage.context, height,
                "bridge finality conflict checkpoint index is out of range");
        modern::FinalizedBlockV2 beyond_int{other};
        beyond_int.checkpoint_index = 300'000'000; // H = 3,000,000,089
        refused(signed_record(beyond_int), lineage.context, height,
                "bridge finality conflict checkpoint index is out of range");
        // H = 109 in the block at 109 (H = height), and index 3 (H = 119) in
        // the block at V.
        refused(signed_record(other), lineage.context, 109,
                "bridge finality conflict checkpoint is above the parent block");
        modern::FinalizedBlockV2 ahead{other};
        ahead.checkpoint_index = 3;
        ahead.height = 119;
        refused(signed_record(ahead), lineage.context, height,
                "bridge finality conflict checkpoint is above the parent block");
        // An index is valid once its checkpoint is at or below the parent:
        // index 3 of epoch 1 in the block at 125, where B3 cannot form its
        // own statement (the index has not connected 119 >= V), is not
        // judged, while an epoch-2 statement there differs by its epoch.
        refused(signed_record(ahead), lineage.context, 125,
                "bridge finality conflict: B3 cannot form its statement at the evidence checkpoint");
        modern::FinalizedBlockV2 ahead_next{ahead};
        ahead_next.epoch = 2;
        BOOST_CHECK_MESSAGE(judge(signed_record(ahead_next), lineage.context, 125, error), error);
    }
    // Refused by the BLS check: two of four members (no quorum), keys no
    // committee holds, and a statement changed after it was signed.
    {
        std::string why;
        BOOST_CHECK(!judge(ConflictRecord(other, LineageFixture::Certificate(
                                                     other, {0, 1}, lineage.keys)),
                           lineage.context, height, why));
        BOOST_CHECK_MESSAGE(why.starts_with("bridge finality conflict certificate does not verify: insufficient-"),
                            why);
        const std::vector<bls::SecretKey> rogue{FixtureBlsKeys(0x6b)};
        refused(ConflictRecord(other, LineageFixture::Certificate(other, {0, 1, 2, 3}, rogue)),
                lineage.context, height,
                "bridge finality conflict certificate does not verify: bad-signature");
        modern::FinalizedBlockV2 tampered{other};
        tampered.block_hash = TestHash(0xbc);
        refused(ConflictRecord(tampered, lineage.Signed(other)), lineage.context, height,
                "bridge finality conflict certificate does not verify: bad-signature");
        // The cheap checks run first: a forged record naming a checkpoint
        // above the parent is refused for its checkpoint.
        refused(ConflictRecord(other, LineageFixture::Certificate(other, {0, 1, 2, 3}, rogue)),
                lineage.context, 109,
                "bridge finality conflict checkpoint is above the parent block");
    }
    // Refused: a certificate that does not decode for its count (only a
    // record built in memory can carry one; the codec refuses it).
    {
        bridge::BridgeFinalityConflictV2 truncated{signed_record(other)};
        truncated.certificate.pop_back();
        refused(truncated, lineage.context, height,
                "bridge finality conflict certificate is malformed");
    }
    // Refused: no lineage to judge on.
    {
        LineageFixture broken;
        broken.Fresh().lineage_broken = true;
        refused(signed_record(other), broken.context, height,
                "bridge finality conflict cannot be judged on a broken fresh lineage");
        LineageFixture unbootstrapped;
        unbootstrapped.Fresh().bootstrapped = false;
        refused(signed_record(other), unbootstrapped.context, height,
                "bridge finality conflict needs a bootstrapped fresh lineage");
        refused(signed_record(other), node::BridgeV2LineageContext{}, height,
                "bridge finality conflict needs a bootstrapped fresh lineage");
        refused(signed_record(other), node::REPLAY_LINEAGE, height,
                "bridge finality conflict needs a bootstrapped fresh lineage");
    }
    // Refused: no chain domain or no checkpoint grid.
    {
        Consensus::Params no_domain{params};
        no_domain.legacy_final_hash.reset();
        std::string why;
        BOOST_CHECK(!node::JudgeFinalityConflictV2(signed_record(other), lineage.context,
                                                   no_domain, index, height, why));
        BOOST_CHECK_EQUAL(why, "bridge finality conflict needs a pinned chain domain");
        Consensus::Params no_pos{params};
        no_pos.modern_pos.reset();
        BOOST_CHECK(!node::JudgeFinalityConflictV2(signed_record(other), lineage.context,
                                                   no_pos, index, height, why));
        BOOST_CHECK_EQUAL(why, "bridge finality conflict needs modern PoS checkpoint parameters");
    }

    // Through the index: a full context judges the record, no context
    // refuses it, and below V it is not a record at all.
    const CTransactionRef conflict{ConflictTransaction(signed_record(other))};
    BOOST_CHECK(node::HasBridgeV2LineageRecord(*conflict));
    BOOST_CHECK(node::HasBridgeV2LineageRecord(Block(1'060, {conflict})));
    BOOST_CHECK(node::HasBridgeV2FinalityConflict(*conflict));
    BOOST_CHECK(!node::HasDecentralizedBridgeBurn(*conflict));
    BOOST_CHECK(!node::HasBridgeV2LineageRecord(*BurnTransactionV2(params, 1, 10, 0x91)));
    BOOST_CHECK(!node::HasBridgeV2FinalityConflict(*BurnTransactionV2(params, 1, 10, 0x91)));
    {
        bridge_test::ObservationWorld world{HealthyWorld(params)};
        const CTransactionRef observation{Observe(world, SUBJECT_TOKEN, AnchorHash(800))};
        BOOST_CHECK(node::HasBridgeV2LineageRecord(*observation));
        BOOST_CHECK(!node::HasBridgeV2FinalityConflict(*observation));
    }
    VerifyFails(index, Block(1'060, {conflict}), height, params,
                "bridge finality conflict cannot be judged here");
    VerifyFails(index, Block(1'060, {conflict}), height - 1, params,
                "bridge record is malformed", &lineage.context);
    VerifyFails(index, Block(1'060, {ConflictTransaction(signed_record(
                           lineage.Statement(1, 109, params, index)))}),
                height, params, "equals B3's canonical statement", &lineage.context);
    {
        node::BridgeBlockDelta delta;
        BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(1'060, {conflict}), height,
                                              V2TestBlockHash(height), params, delta,
                                              error, &lineage.context),
                            error);
        BOOST_CHECK(delta.finality_conflicts_v2_added ==
                    std::vector<uint256>{modern::FinalityCertIdV2(other)});
    }
}

BOOST_AUTO_TEST_CASE(v2_finality_conflict_sticky)
{
    // Kind 13 through the index: the record sets the sticky conflict, which
    // closes the burn gate at C4 for good (a burn after the record in the
    // same block is refused at its position), refuses every later conflict
    // of any kind, is reproduced by replay and is cleared only by the
    // disconnect of its block.
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{FundingReceipts(params)};
    bridge_test::ObservationWorld world{HealthyWorld(params, 2, 100)};
    node::BridgeStateIndex probe{FundingIndex(params, receipts, world.StateRoot())};
    world.SetVerifier(lineage.Canonical(1, 109, params, probe));
    // Anchor 701: a drained vault (locked 25 < 100 proven below the cut).
    bridge_test::ObservationWorld drained{world};
    drained.SetReserve(2, 25, 0, 25);

    const modern::FinalizedBlockV2 other{OtherBranch(lineage, 1, 109, params, probe)};
    const uint256 cert_id{modern::FinalityCertIdV2(other)};
    const CTransactionRef conflict{
        ConflictTransaction(ConflictRecord(other, lineage.Signed(other)))};
    modern::FinalizedBlockV2 next_epoch{other};
    next_epoch.epoch = 2;
    const CTransactionRef second{
        ConflictTransaction(ConflictRecord(next_epoch, lineage.Signed(next_epoch)))};
    const modern::FinalizedBlockV2 canonical{lineage.Statement(1, 109, params, probe)};
    const CTransactionRef identical{
        ConflictTransaction(ConflictRecord(canonical, lineage.Signed(canonical)))};
    const CTransactionRef burn{BurnTransactionV2(params, 1, 10, 0x91)};
    const CTransactionRef drain{Observe(drained, SUBJECT_RESERVE, AnchorHash(701))};
    const std::string c4{"C4: sticky conflict: finality conflict evidence"};
    const std::string burn_refused{"bridge V2 burn gate is not ready at the burn: " + c4};

    // One E5 conflict first: a later kind 13 is refused, in a later block or
    // in the same one.
    {
        bridge_test::ObservationWorld forged_world{world};
        bridge::VerifierStateV2 forged{lineage.Canonical(1, 109, params, probe)};
        forged.latest_cert_id = TestHash(0x66);
        forged_world.SetVerifier(forged);
        const CTransactionRef e5{
            Observe(forged_world, SUBJECT_VERIFIER, receipts.block_hash, VERDICT_CERT)};
        node::BridgeStateIndex e5_index{FundingIndex(params, receipts, forged_world.StateRoot())};
        VerifyFails(e5_index, Block(1'060, {e5, conflict}), BRIDGE_V2_HEIGHT, params,
                    "bridge V2 sticky conflict is already set", &lineage.context);
        ConnectNext(e5_index, params, {e5}, 1'060, &lineage.context);
        BOOST_REQUIRE(e5_index.ConflictV2());
        BOOST_CHECK_EQUAL(e5_index.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_E5);
        VerifyFails(e5_index, Block(1'061, {conflict}), BRIDGE_V2_HEIGHT + 1, params,
                    "bridge V2 sticky conflict is already set", &lineage.context);
    }

    // `index` judges with the full context, `replayed` replays the same
    // blocks with REPLAY_LINEAGE, as BridgeStateTracker does.
    node::BridgeStateIndex index{FundingIndex(params, receipts, world.StateRoot())};
    node::BridgeStateIndex replayed{FundingIndex(params, receipts, world.StateRoot())};
    const auto connect_both = [&](const std::vector<CTransactionRef>& txs,
                                  const uint32_t time) {
        const node::BridgeBlockDelta a{ConnectNext(index, params, txs, time, &lineage.context)};
        const node::BridgeBlockDelta b{ConnectNext(replayed, params, txs, time, &node::REPLAY_LINEAGE)};
        CheckSameV2Delta(a, b);
        CheckSameAccounting(index, replayed);
        return a;
    };
    connect_both({MintTransactionV2(params, receipts, 0, 60)}, 1'060);
    connect_both({MintTransactionV2(params, receipts, 1, 40),
                  Observe(world, SUBJECT_TOKEN, receipts.block_hash),
                  Observe(world, SUBJECT_RESERVE, receipts.block_hash),
                  Observe(world, SUBJECT_VERIFIER, receipts.block_hash)},
                 1'061);
    SeedStateAnchorsV2(index, {StateAnchor(701, 1'010, drained.StateRoot())});
    SeedStateAnchorsV2(replayed, {StateAnchor(701, 1'010, drained.StateRoot())});
    const int next{index.ConnectedHeight() + 1};
    BOOST_REQUIRE(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next, 1'063)
                      .readiness == node::BridgeBurnReadiness::READY);

    // Without a context the record cannot be judged; the identical
    // statement is refused by the judge, while replay only decodes it.
    VerifyFails(index, Block(1'063, {conflict}), next, params,
                "bridge finality conflict cannot be judged here");
    VerifyFails(index, Block(1'063, {identical}), next, params,
                "bridge finality conflict equals B3's canonical statement", &lineage.context);
    {
        node::BridgeBlockDelta trusted;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(1'063, {identical}), next, V2TestBlockHash(next),
                                              params, trusted, error, &node::REPLAY_LINEAGE),
                            error);
        BOOST_REQUIRE(trusted.conflict_v2_after);
        BOOST_CHECK(trusted.conflict_v2_after->evidence_cert_id ==
                    modern::FinalityCertIdV2(canonical));
    }
    // A burn after the record in the same block is refused at its position;
    // before it the burn passes and the record still sets the conflict. An
    // E8 RESERVE_E after the record leaves the kind 13 conflict in place; a
    // kind 13 after an E8 RESERVE_E is refused.
    VerifyFails(index, Block(1'063, {conflict, burn}), next, params, burn_refused,
                &lineage.context);
    VerifyFails(index, Block(1'063, {drain, conflict}), next, params,
                "bridge V2 sticky conflict is already set", &lineage.context);
    VerifyFails(index, Block(1'063, {conflict, second}), next, params,
                "bridge V2 sticky conflict is already set", &lineage.context);
    {
        node::BridgeBlockDelta delta;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(1'063, {burn, conflict, drain}), next,
                                              V2TestBlockHash(next), params, delta, error,
                                              &lineage.context),
                            error);
        BOOST_CHECK_EQUAL(delta.withdrawals_v2_added.size(), 1U);
        BOOST_CHECK_EQUAL(delta.observations_v2_added.size(), 1U);
        BOOST_REQUIRE(delta.conflict_v2_after);
        BOOST_CHECK_EQUAL(delta.conflict_v2_after->reason, bridge::BRIDGE_V2_CONFLICT_KIND13);
        BOOST_CHECK(delta.finality_conflicts_v2_added == std::vector<uint256>{cert_id});
    }
    // Block assembly: the preview takes the record, then refuses the burn
    // and a second kind 13 after it, so the assembler skips both. Without a
    // context the preview refuses the record itself.
    {
        std::string error;
        auto preview{index.BeginBlockPreview(next, 1'063, TestHash(98), params, error, &lineage.context)};
        BOOST_REQUIRE_MESSAGE(preview, error);
        const std::array<CTransactionRef, 1> record_chunk{conflict};
        const std::array<CTransactionRef, 1> burn_chunk{burn};
        const std::array<CTransactionRef, 1> second_chunk{second};
        BOOST_REQUIRE_MESSAGE(preview->TryAppend(record_chunk, error), error);
        BOOST_CHECK(!preview->TryAppend(burn_chunk, error));
        BOOST_CHECK_MESSAGE(error.find(burn_refused) != std::string::npos, error);
        BOOST_CHECK(!preview->TryAppend(second_chunk, error));
        BOOST_CHECK_EQUAL(error, "bridge V2 sticky conflict is already set");
        auto blind{index.BeginBlockPreview(next, 1'063, TestHash(99), params, error)};
        BOOST_REQUIRE_MESSAGE(blind, error);
        BOOST_CHECK(!blind->TryAppend(record_chunk, error));
        BOOST_CHECK_EQUAL(error, "bridge finality conflict cannot be judged here");
    }
    // Mempool admission and wallet prevalidation: one transaction on the
    // connected state, judged only with the context.
    {
        node::BridgeTxAuthorization authorization;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyTransaction(*conflict, next, 1'063, params, authorization,
                                                    error, &lineage.context),
                            error);
        BOOST_CHECK(!authorization.mint && !authorization.HasWithdrawal());
        BOOST_CHECK(!index.VerifyTransaction(*conflict, next, 1'063, params, authorization, error));
        BOOST_CHECK_EQUAL(error, "bridge finality conflict cannot be judged here");
    }

    // The exact delta, and every malformed variant of it refused atomically.
    node::BridgeBlockDelta good;
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(index.VerifyBlock(Block(1'063, {conflict}), next, V2TestBlockHash(next),
                                                params, good, error, &lineage.context),
                              error);
    }
    const node::BridgeV2Conflict kind13{bridge::BRIDGE_V2_CONFLICT_KIND13, next,
                                        V2TestBlockHash(next), 0, cert_id, 2, 1};
    BOOST_CHECK(!good.conflict_v2_before);
    BOOST_CHECK(good.conflict_v2_after == std::optional<node::BridgeV2Conflict>{kind13});
    BOOST_CHECK(good.finality_conflicts_v2_added == std::vector<uint256>{cert_id});
    BOOST_CHECK(good.observations_v2_added.empty());
    const auto refused_delta = [&](const node::BridgeBlockDelta& delta,
                                   const std::string& expected) {
        std::string error;
        BOOST_CHECK(!index.ConnectBlock(delta, error));
        BOOST_CHECK_MESSAGE(error.find(expected) != std::string::npos,
                            "'" << error << "' lacks '" << expected << "'");
        BOOST_CHECK_EQUAL(index.ConnectedHeight(), next - 1);
        BOOST_CHECK(!index.ConflictV2());
    };
    {
        node::BridgeBlockDelta d{good};
        d.finality_conflicts_v2_added.push_back(TestHash(0x77));
        refused_delta(d, "malformed finality conflict");
        d = good;
        d.finality_conflicts_v2_added = {uint256{}};
        refused_delta(d, "malformed finality conflict");
        // Evidence without a newly set conflict.
        d = good;
        d.conflict_v2_after.reset();
        refused_delta(d, "malformed finality conflict");
        // Evidence on a non-V2 store.
        d.v2_store = false;
        refused_delta(d, "V2 accounting does not match its store");
        // A kind 13 conflict that its evidence does not set.
        d = good;
        d.conflict_v2_after->anchor_block = 800;
        refused_delta(d, "malformed sticky conflict");
        d = good;
        d.conflict_v2_after->height = next + 1;
        refused_delta(d, "malformed sticky conflict");
        d = good;
        d.conflict_v2_after->block_hash = TestHash(0x78);
        refused_delta(d, "malformed sticky conflict");
        d = good;
        d.conflict_v2_after->evidence_cert_id = TestHash(0x77);
        refused_delta(d, "malformed sticky conflict");
        d = good;
        d.conflict_v2_after->evidence_cert_id.SetNull();
        refused_delta(d, "malformed sticky conflict");
        d = good;
        d.conflict_v2_after->evidence_checkpoint_index = 0;
        refused_delta(d, "malformed sticky conflict");
        d = good;
        d.finality_conflicts_v2_added.clear();
        refused_delta(d, "malformed sticky conflict");
        // The evidence cannot be relabelled as an E5 conflict.
        d = good;
        d.conflict_v2_after->reason = bridge::BRIDGE_V2_CONFLICT_E5;
        refused_delta(d, "malformed sticky conflict");
    }
    {
        // An E8 conflict carries no kind 13 evidence.
        node::BridgeBlockDelta e8;
        std::string error;
        BOOST_REQUIRE_MESSAGE(index.VerifyBlock(Block(1'063, {drain}), next, V2TestBlockHash(next),
                                                params, e8, error, &lineage.context),
                              error);
        BOOST_REQUIRE(e8.conflict_v2_after);
        BOOST_REQUIRE_EQUAL(e8.conflict_v2_after->reason, bridge::BRIDGE_V2_CONFLICT_E8);
        node::BridgeBlockDelta d{e8};
        d.finality_conflicts_v2_added = {cert_id};
        refused_delta(d, "malformed finality conflict");
        d = e8;
        d.conflict_v2_after->evidence_cert_id = cert_id;
        refused_delta(d, "malformed sticky conflict");
        d = e8;
        d.conflict_v2_after->evidence_epoch = 1;
        refused_delta(d, "malformed sticky conflict");
    }

    // Connect: the exact conflict on both indexes; the gate is closed at C4.
    const node::BridgeBlockDelta setting{connect_both({conflict}, 1'063)};
    BOOST_CHECK(setting.finality_conflicts_v2_added == std::vector<uint256>{cert_id});
    BOOST_CHECK(index.ConflictV2() == std::optional<node::BridgeV2Conflict>{kind13});
    BOOST_CHECK(replayed.ConflictV2() == index.ConflictV2());
    {
        const auto gate{node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next + 1, 1'064)};
        BOOST_CHECK(gate.readiness == node::BridgeBurnReadiness::NOT_READY);
        BOOST_CHECK(gate.row == bridge::BurnGateRowV2::C4);
        BOOST_CHECK_EQUAL(gate.Describe(), c4);
    }
    // For good: burns, and a second conflict of any kind under any context.
    VerifyFails(index, Block(1'064, {burn}), next + 1, params, burn_refused, &lineage.context);
    for (const node::BridgeV2LineageContext* context :
         {&lineage.context, &node::REPLAY_LINEAGE,
          static_cast<const node::BridgeV2LineageContext*>(nullptr)}) {
        VerifyFails(index, Block(1'064, {second}), next + 1, params,
                    "bridge V2 sticky conflict is already set", context);
    }
    {
        // Evidence after an existing conflict is malformed.
        node::BridgeBlockDelta later;
        std::string error;
        BOOST_REQUIRE_MESSAGE(index.VerifyBlock(Block(1'064, {}), next + 1, V2TestBlockHash(next + 1),
                                                params, later, error, &lineage.context),
                              error);
        BOOST_CHECK(later.conflict_v2_before == later.conflict_v2_after);
        later.finality_conflicts_v2_added = {cert_id};
        BOOST_CHECK(!index.ConnectBlock(later, error));
        BOOST_CHECK_MESSAGE(error.find("malformed finality conflict") != std::string::npos, error);
    }
    // A later E8 RESERVE_E connects and leaves the conflict as it is.
    connect_both({drain}, 1'064);
    BOOST_CHECK(index.ConflictV2() == std::optional<node::BridgeV2Conflict>{kind13});
    BOOST_CHECK_EQUAL(index.LatestObservationV2(SUBJECT_RESERVE)->anchor_block, 701U);
    // Undo above the setting block keeps it; undoing that block clears it,
    // and the same record then sets it again.
    Disconnect(index);
    Disconnect(replayed);
    BOOST_CHECK(index.ConflictV2() == std::optional<node::BridgeV2Conflict>{kind13});
    Disconnect(index);
    Disconnect(replayed);
    BOOST_CHECK(!index.ConflictV2());
    BOOST_CHECK(!replayed.ConflictV2());
    CheckSameAccounting(index, replayed);
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next, 1'063)
                    .readiness == node::BridgeBurnReadiness::READY);
    connect_both({conflict}, 1'063);
    BOOST_CHECK(index.ConflictV2() == std::optional<node::BridgeV2Conflict>{kind13});
}

BOOST_AUTO_TEST_CASE(finality_bridge_flags_v2)
{
    // bridgeFlags of B3's fresh-lineage statement (node changes sections 7.4
    // and 8.5): 0 below V and on every chain without a complete bridge V2
    // envelope; from V it fails closed for a height the index has not
    // connected, is 0 before the sticky conflict and bit 0 from the
    // conflict's block on, and only the disconnect of that block clears it.
    const std::optional<uint8_t> open{uint8_t{0}};
    const std::optional<uint8_t> closed{
        modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED};
    BOOST_REQUIRE_EQUAL(int{modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED}, 1);

    // Mainnet and testnet keep bridge_v2 unset: 0 at every height.
    for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET}) {
        const auto chain_params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& consensus{chain_params->GetConsensus()};
        BOOST_REQUIRE(!consensus.bridge_v2);
        for (const int height : {0, 1'000'000, std::numeric_limits<int>::max()}) {
            BOOST_CHECK(node::FinalityBridgeFlagsV2(height, consensus, nullptr) == open);
            BOOST_CHECK(node::FreshLineageBridgeFlags(height, consensus, nullptr) == open);
        }
    }
    // A V1-only test chain and an incomplete envelope: 0 too, index or not.
    {
        const Consensus::Params v1_only{FullyActiveBridgeParams(true)};
        node::BridgeStateIndex v1_index;
        ConnectEmptyThrough(v1_index, v1_only, BRIDGE_V2_HEIGHT + 2);
        Consensus::Params incomplete{V2Params()};
        incomplete.bridge_v2->vault.token_runtime_code_hash.SetNull();
        for (const int height : {BRIDGE_HEIGHT, BRIDGE_V2_HEIGHT, BRIDGE_V2_HEIGHT + 5}) {
            BOOST_CHECK(node::FinalityBridgeFlagsV2(height, v1_only, nullptr) == open);
            BOOST_CHECK(node::FinalityBridgeFlagsV2(height, v1_only, &v1_index) == open);
            BOOST_CHECK(node::FinalityBridgeFlagsV2(height, incomplete, nullptr) == open);
        }
    }

    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    // Connected at V - 1 = 109.
    node::BridgeStateIndex index{ObservationIndex(params, uint256{uint8_t{1}})};
    BOOST_REQUIRE_EQUAL(index.ConnectedHeight(), BRIDGE_V2_HEIGHT - 1);
    // Below V: 0 with or without the index.
    BOOST_CHECK(node::FinalityBridgeFlagsV2(BRIDGE_V2_HEIGHT - 1, params, nullptr) == open);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(BRIDGE_V2_HEIGHT - 1, params, &index) == open);
    // From V: nullopt without an index or for a height not connected yet.
    BOOST_CHECK(!node::FinalityBridgeFlagsV2(BRIDGE_V2_HEIGHT, params, nullptr));
    BOOST_CHECK(!node::FinalityBridgeFlagsV2(BRIDGE_V2_HEIGHT, params, &index));

    // V .. 125 without a conflict: 0 at every connected height, including
    // the grid checkpoint 119 (index 3, epoch 1).
    const int pre_checkpoint{LineageFixture::GENESIS + 3 * 10};
    const int post_checkpoint{pre_checkpoint + 10};
    ConnectEmptyThrough(index, params, pre_checkpoint + 6);
    for (int height{BRIDGE_V2_HEIGHT}; height <= index.ConnectedHeight(); ++height) {
        BOOST_CHECK(node::FinalityBridgeFlagsV2(height, params, &index) == open);
        BOOST_CHECK_EQUAL(index.BridgeFlagsV2AtHeight(height), 0);
    }
    const modern::FinalizedBlockV2 before{lineage.Statement(1, pre_checkpoint, params, index)};
    BOOST_CHECK_EQUAL(before.bridge_flags, 0);

    // A kind 13 record in block c = 126 sets the sticky conflict.
    const modern::FinalizedBlockV2 other{OtherBranch(lineage, 1, 109, params, index)};
    ConnectNext(index, params, {ConflictTransaction(ConflictRecord(other, lineage.Signed(other)))},
                1'061, &lineage.context);
    const int c{index.ConnectedHeight()};
    BOOST_REQUIRE(index.ConflictV2());
    BOOST_REQUIRE_EQUAL(index.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_KIND13);
    BOOST_REQUIRE_EQUAL(index.ConflictV2()->height, c);
    BOOST_REQUIRE(c < post_checkpoint);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c - 1, params, &index) == open);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c, params, &index) == closed);
    BOOST_CHECK(!node::FinalityBridgeFlagsV2(c + 1, params, &index));
    ConnectEmptyThrough(index, params, std::max(post_checkpoint, c + 5));
    for (int height{BRIDGE_V2_HEIGHT}; height <= index.ConnectedHeight(); ++height) {
        const std::optional<uint8_t> expected{height >= c ? closed : open};
        BOOST_CHECK(node::FinalityBridgeFlagsV2(height, params, &index) == expected);
        BOOST_CHECK(node::FreshLineageBridgeFlags(height, params, &index) == expected);
    }
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c - 1, params, &index) == open);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c, params, &index) == closed);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c + 5, params, &index) == closed);
    BOOST_CHECK(!node::FinalityBridgeFlagsV2(index.ConnectedHeight() + 1, params, &index));

    // B3's statement at the first grid checkpoint at or after c carries bit
    // 0 (byte 184 of the 185-byte FinalizedBlockV2); a delayed statement
    // below c keeps 0.
    const modern::FinalizedBlockV2 flagged{lineage.Statement(1, post_checkpoint, params, index)};
    BOOST_CHECK_EQUAL(flagged.bridge_flags, modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED);
    const auto encoded{flagged.Encode()};
    BOOST_REQUIRE_EQUAL(encoded.size(), 185U);
    BOOST_CHECK_EQUAL(encoded[184], 0x01);
    BOOST_CHECK(modern::FinalityCertIdV2(flagged) ==
                Keccak(std::vector<unsigned char>(encoded.begin(), encoded.end())));
    modern::FinalizedBlockV2 unflagged{flagged};
    unflagged.bridge_flags = 0;
    BOOST_CHECK(modern::FinalityCertIdV2(unflagged) != modern::FinalityCertIdV2(flagged));
    BOOST_CHECK(lineage.Statement(1, pre_checkpoint, params, index) == before);

    // Undo down to c keeps the flag; undoing c clears it, and an empty block
    // at c then signs 0 there.
    while (index.ConnectedHeight() > c) Disconnect(index);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c, params, &index) == closed);
    Disconnect(index);
    BOOST_CHECK(!index.ConflictV2());
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c - 1, params, &index) == open);
    BOOST_CHECK(!node::FinalityBridgeFlagsV2(c, params, &index));
    ConnectNext(index, params, {}, 1'061, &lineage.context);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(c, params, &index) == open);

    // An E8 conflict closes deposits from its block the same way.
    const ReceiptFixture receipts{FundingReceipts(params)};
    bridge_test::ObservationWorld drained{HealthyWorld(params, 2, 100)};
    drained.SetReserve(2, 99, 0, 99);
    node::BridgeStateIndex vault{FundingIndex(params, receipts, drained.StateRoot())};
    ConnectNext(vault, params, {MintTransactionV2(params, receipts, 0, 60)});
    ConnectNext(vault, params, {MintTransactionV2(params, receipts, 1, 40)}, 1'061);
    ConnectNext(vault, params, {Observe(drained, SUBJECT_RESERVE, receipts.block_hash)}, 1'062);
    BOOST_REQUIRE(vault.ConflictV2());
    BOOST_REQUIRE_EQUAL(vault.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_E8);
    const int e8{vault.ConflictV2()->height};
    BOOST_CHECK(node::FinalityBridgeFlagsV2(e8 - 1, params, &vault) == open);
    BOOST_CHECK(node::FinalityBridgeFlagsV2(e8, params, &vault) == closed);
}

BOOST_AUTO_TEST_CASE(v2_deposits_closed_reason)
{
    // node::BridgeV2DepositsClosedReason (node changes sections 7.5 and 8.5,
    // "Deposits stop too"): B3 closes deposits for good on the sticky
    // conflict (row C4), a broken fresh lineage (C2's permanent case), or a
    // latest VERIFIER_E showing an impaired verifier (C1) or Verifier V2
    // depositsClosed, in that order. Nothing else closes them, and only the
    // disconnect of the closing block reopens them.
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    const int64_t expiry{params.bridge_v2->gate.set_expiry_seconds};
    const node::BridgeStateIndex probe{ObservationIndex(params, uint256{uint8_t{1}})};
    const bridge::VerifierStateV2 canonical{lineage.Canonical(1, 109, params, probe)};
    BOOST_REQUIRE_EQUAL(canonical.last_rotation_time, 1'000);
    LineageFixture broken;
    broken.Fresh().lineage_broken = true;

    // An index whose V2 light client retains anchor 800 at `t_obs` for
    // `world`, connected at V - 1.
    const auto index_at{[&](const bridge_test::ObservationWorld& world, const uint64_t t_obs) {
        node::BridgeStateIndex index;
        ConnectEmptyThrough(index, params, BRIDGE_V2_HEIGHT - 2);
        const StateAnchorSpec anchor{StateAnchor(800, t_obs, world.StateRoot())};
        SeedStateAnchorsV2(index, {anchor}, anchor);
        return index;
    }};
    // The reason after the block at V connects a VERIFIER_E showing `v`,
    // proven at anchor 800 (t_obs); the same without a lineage context; and
    // open again once that block is disconnected.
    const auto closed_by_verifier{[&](const bridge::VerifierStateV2& v, const uint64_t t_obs = 1'000,
                                      const uint8_t verdict = 0) {
        bridge_test::ObservationWorld world{HealthyWorld(params)};
        world.SetVerifier(v);
        node::BridgeStateIndex index{index_at(world, t_obs)};
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(index, &lineage.context, params));
        ConnectNext(index, params, {Observe(world, SUBJECT_VERIFIER, AnchorHash(800), verdict)},
                    static_cast<uint32_t>(t_obs + 60), &lineage.context);
        BOOST_REQUIRE(index.LatestObservationV2(SUBJECT_VERIFIER));
        const auto reason{node::BridgeV2DepositsClosedReason(index, &lineage.context, params)};
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, nullptr, params) == reason);
        // Mainnet and testnet keep bridge_v2 unset: never closed.
        for (const ChainType chain : {ChainType::MAIN, ChainType::TESTNET}) {
            const auto chain_params{CreateChainParams(ArgsManager{}, chain)};
            BOOST_CHECK(!node::BridgeV2DepositsClosedReason(index, &lineage.context,
                                                            chain_params->GetConsensus()));
        }
        Disconnect(index);
        BOOST_CHECK(!index.LatestObservationV2(SUBJECT_VERIFIER));
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(index, &lineage.context, params));
        return reason;
    }};
    const std::string seen{"the latest VERIFIER_E (anchor block 800) shows "};

    // Open: no observation, and a LIVE verifier whose deposits are open.
    {
        const node::BridgeStateIndex empty{ObservationIndex(params, uint256{uint8_t{1}})};
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(empty, &lineage.context, params));
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(empty, nullptr, params));
    }
    BOOST_CHECK(!closed_by_verifier(canonical));

    // C1: halted, cancelled, and expired at the observation (one second past
    // set_expiry; exactly at it the set is still live).
    {
        bridge::VerifierStateV2 halted{canonical};
        halted.status = bridge::VERIFIER_STATUS_HALTED_CONFLICT;
        BOOST_CHECK(closed_by_verifier(halted) ==
                    std::optional<std::string>{seen + "the verifier is halted by a conflict"});
        bridge::VerifierStateV2 cancelled{canonical};
        cancelled.status = bridge::VERIFIER_STATUS_CANCELLED;
        // Impairment is reported ahead of depositsClosed.
        cancelled.deposits_closed = true;
        BOOST_CHECK(closed_by_verifier(cancelled) ==
                    std::optional<std::string>{seen + "the verifier is cancelled"});
        BOOST_CHECK(!closed_by_verifier(canonical, static_cast<uint64_t>(1'000 + expiry)));
        BOOST_CHECK(closed_by_verifier(canonical, static_cast<uint64_t>(1'000 + expiry + 1)) ==
                    std::optional<std::string>{seen + "the verifier signing set had expired at the observation"});
    }
    // Ethereum's own close: a LIVE verifier showing depositsClosed.
    {
        bridge::VerifierStateV2 closed{canonical};
        closed.deposits_closed = true;
        BOOST_CHECK(closed_by_verifier(closed) ==
                    std::optional<std::string>{seen + "Verifier V2 depositsClosed"});
    }

    // C2's permanent case: a broken fresh lineage, ahead of the verifier and
    // read only from a lineage context.
    {
        bridge::VerifierStateV2 halted{canonical};
        halted.status = bridge::VERIFIER_STATUS_HALTED_CONFLICT;
        bridge_test::ObservationWorld world{HealthyWorld(params)};
        world.SetVerifier(halted);
        node::BridgeStateIndex index{index_at(world, 1'000)};
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &broken.context, params) ==
                    std::optional<std::string>{"the fresh finality lineage is broken"});
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(index, nullptr, params));
        ConnectNext(index, params, {Observe(world, SUBJECT_VERIFIER, AnchorHash(800))}, 1'060,
                    &lineage.context);
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &broken.context, params) ==
                    std::optional<std::string>{"the fresh finality lineage is broken"});
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, nullptr, params) ==
                    std::optional<std::string>{seen + "the verifier is halted by a conflict"});
    }

    // C4: the sticky conflict, ahead of everything, from E5 ...
    {
        bridge::VerifierStateV2 forged{canonical};
        forged.latest_cert_id = TestHash(0x66);
        const std::string e5{"sticky E5 conflict at height " + std::to_string(BRIDGE_V2_HEIGHT)};
        BOOST_CHECK(closed_by_verifier(forged, 1'000, VERDICT_CERT) == std::optional<std::string>{e5});
        // A conflict outranks a broken lineage and a later VERIFIER_E
        // showing a halted verifier.
        bridge_test::ObservationWorld world{HealthyWorld(params)};
        world.SetVerifier(forged);
        node::BridgeStateIndex index{index_at(world, 1'000)};
        ConnectNext(index, params, {Observe(world, SUBJECT_VERIFIER, AnchorHash(800), VERDICT_CERT)}, 1'060,
                    &lineage.context);
        bridge::VerifierStateV2 halted{canonical};
        halted.status = bridge::VERIFIER_STATUS_HALTED_CONFLICT;
        bridge_test::ObservationWorld halted_world{world};
        halted_world.SetVerifier(halted);
        SeedStateAnchorsV2(index, {StateAnchor(801, 1'010, halted_world.StateRoot())});
        ConnectNext(index, params, {Observe(halted_world, SUBJECT_VERIFIER, AnchorHash(801))}, 1'062,
                    &lineage.context);
        BOOST_REQUIRE_EQUAL(index.LatestObservationV2(SUBJECT_VERIFIER)->anchor_block, 801U);
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &broken.context, params) ==
                    std::optional<std::string>{e5});
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, nullptr, params) ==
                    std::optional<std::string>{e5});
    }
    // ... kind 13 FINALITY_CONFLICT evidence ...
    {
        node::BridgeStateIndex index{ObservationIndex(params, uint256{uint8_t{1}})};
        const modern::FinalizedBlockV2 other{OtherBranch(lineage, 1, 109, params, index)};
        ConnectNext(index, params, {ConflictTransaction(ConflictRecord(other, lineage.Signed(other)))}, 1'061,
                    &lineage.context);
        BOOST_REQUIRE(index.ConflictV2());
        BOOST_REQUIRE_EQUAL(index.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_KIND13);
        const std::optional<std::string> kind13{"sticky kind 13 conflict at height " +
                                                std::to_string(BRIDGE_V2_HEIGHT)};
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &lineage.context, params) == kind13);
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &broken.context, params) == kind13);
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, nullptr, params) == kind13);
        // Later empty blocks keep it; only the disconnect of its block
        // reopens deposits.
        ConnectNext(index, params, {}, 1'062, &lineage.context);
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &lineage.context, params) == kind13);
        Disconnect(index);
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(index, &lineage.context, params) == kind13);
        Disconnect(index);
        BOOST_CHECK(!index.ConflictV2());
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(index, &lineage.context, params));
    }
    // ... and E8 (a RESERVE_E failing invariant R).
    {
        const ReceiptFixture receipts{FundingReceipts(params)};
        bridge_test::ObservationWorld drained{HealthyWorld(params, 2, 100)};
        drained.SetReserve(2, 99, 0, 99);
        node::BridgeStateIndex vault{FundingIndex(params, receipts, drained.StateRoot())};
        ConnectNext(vault, params, {MintTransactionV2(params, receipts, 0, 60)});
        ConnectNext(vault, params, {MintTransactionV2(params, receipts, 1, 40)}, 1'061);
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(vault, &lineage.context, params));
        ConnectNext(vault, params, {Observe(drained, SUBJECT_RESERVE, receipts.block_hash)}, 1'062);
        BOOST_REQUIRE(vault.ConflictV2());
        BOOST_CHECK(node::BridgeV2DepositsClosedReason(vault, &lineage.context, params) ==
                    std::optional<std::string>{"sticky E8 conflict at height " +
                                               std::to_string(vault.ConnectedHeight())});
        Disconnect(vault);
        BOOST_CHECK(!node::BridgeV2DepositsClosedReason(vault, &lineage.context, params));
    }
}

BOOST_AUTO_TEST_CASE(v2_reserve_ledger_mints_and_burns)
{
    // Checks 2 and 3 of node changes section 7.2 through real V2 mints and
    // burns. Deposits 0, 1, 2 of 60, 40, 30 are in one receipt; the first
    // RESERVE_E sees the vault after deposits 0 and 1 (cut 2, locked 100)
    // with a USDT balance of only 80, so margin_v = ceil(80 * 10%) = 8.
    const Consensus::Params params{V2Params()};
    const ReceiptFixture receipts{FundingReceipts(params)};
    bridge_test::ObservationWorld world{HealthyWorld(params, 2, 100)};
    world.SetReserve(2, 100, 0, 80);
    node::BridgeStateIndex index{FundingIndex(params, receipts, world.StateRoot())};

    const node::BridgeBlockDelta first_mint{
        ConnectNext(index, params, {MintTransactionV2(params, receipts, 0, 60)})};
    BOOST_CHECK_EQUAL(first_mint.reserve_v2_before.minted, 0);
    BOOST_CHECK_EQUAL(first_mint.reserve_v2_after.minted, 60);
    BOOST_CHECK(first_mint.deposits_v2_added ==
                (std::vector<node::BridgeProvenDepositV2>{{0, 60}}));
    ConnectNext(index, params, {MintTransactionV2(params, receipts, 1, 40)}, 1'061);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, 100);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 100);
    BOOST_CHECK(index.ProvenDepositsV2() ==
                (bridge::ProvenDepositMapV2{{0, 60}, {1, 40}}));
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(), 100);
    BOOST_CHECK(index.ProvenDepositSumsAtV2(1) ==
                (std::optional<bridge::ProvenDepositSumsV2>{{1, 60, 40}}));

    // Check 3 needs a RESERVE_E observation.
    const auto burn = [&params](const uint8_t source, const CAmount amount) {
        return BurnTransactionV2(params, source, amount, 0x90);
    };
    int next{index.ConnectedHeight() + 1};
    VerifyFails(index, Block(1'062, {burn(1, 10)}), next, params,
                "bridge V2 burn has no reserve observation");
    ConnectNext(index, params, {Observe(world, SUBJECT_RESERVE, receipts.block_hash)}, 1'062);
    BOOST_CHECK(!index.ConflictV2());

    // Headroom = B + P_post - margin - unreleased = 80 + 0 - 8 - 0 = 72.
    next = index.ConnectedHeight() + 1;
    VerifyFails(index, Block(1'063, {burn(2, 101)}), next, params,
                "exceeds the vault entry's reserve");
    VerifyFails(index, Block(1'063, {burn(2, 73)}), next, params,
                "exceeds the wind-down bound");
    {
        node::BridgeTxAuthorization authorization;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyTransaction(*burn(2, 72), next, 1'063,
                                                    params, authorization, error),
                            error);
    }
    // Earlier burns of the block count.
    VerifyFails(index, Block(1'063, {burn(2, 50), burn(3, 23)}), next, params,
                "exceeds the wind-down bound");
    {
        std::string error;
        auto preview{index.BeginBlockPreview(next, 1'063, TestHash(97), params, error)};
        BOOST_REQUIRE_MESSAGE(preview, error);
        const std::array<CTransactionRef, 1> fifty{burn(2, 50)};
        const std::array<CTransactionRef, 1> twenty_three{burn(3, 23)};
        const std::array<CTransactionRef, 1> twenty_two{burn(4, 22)};
        BOOST_REQUIRE_MESSAGE(preview->TryAppend(fifty, error), error);
        BOOST_CHECK(!preview->TryAppend(twenty_three, error));
        BOOST_CHECK(error.find("wind-down") != std::string::npos);
        BOOST_CHECK_MESSAGE(preview->TryAppend(twenty_two, error), error);
    }
    const node::BridgeBlockDelta burns{
        ConnectNext(index, params, {burn(2, 50), burn(4, 22)}, 1'063)};
    BOOST_CHECK_EQUAL(burns.reserve_v2_before.burned, 0);
    BOOST_CHECK_EQUAL(burns.reserve_v2_after.burned, 72);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 28);

    // The vault releases the 72: locked 28, balance 28. R_v = 100 - 28 = 72
    // covers the burns, margin is 0 (B >= L), so the bound is the reserve.
    world.SetReserve(2, 28, 0, 28);
    SeedStateAnchorsV2(index, {StateAnchor(701, 1'010, world.StateRoot())});
    ConnectNext(index, params, {Observe(world, SUBJECT_RESERVE, AnchorHash(701))}, 1'064);
    BOOST_CHECK(!index.ConflictV2());
    next = index.ConnectedHeight() + 1;
    VerifyFails(index, Block(1'065, {burn(5, 29)}), next, params,
                "exceeds the vault entry's reserve");
    // A deposit proven at or above the cut in the same block redeems at
    // once: P_post = 30 lifts both the reserve and the observed balance.
    VerifyFails(index, Block(1'065, {burn(5, 58), MintTransactionV2(params, receipts, 2, 30)}),
                next, params, "exceeds the vault entry's reserve");
    const node::BridgeBlockDelta redeemed{ConnectNext(
        index, params, {MintTransactionV2(params, receipts, 2, 30), burn(5, 58)}, 1'065)};
    BOOST_CHECK(redeemed.deposits_v2_added ==
                (std::vector<node::BridgeProvenDepositV2>{{2, 30}}));
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, 130);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().burned, 130);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 0);
    BOOST_REQUIRE_EQUAL(index.WithdrawalV2Count(), 3U);
    VerifyFails(index, Block(1'066, {burn(6, 1)}), index.ConnectedHeight() + 1, params,
                "exceeds the vault entry's reserve");

    // Undo restores the ledger, the deposit index and its total exactly.
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(), 130);
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, 100);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().burned, 72);
    BOOST_CHECK(!index.ProvenDepositsV2().contains(2));
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(), 100);
    Disconnect(index);
    Disconnect(index);
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().burned, 0);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 0U);
}

BOOST_AUTO_TEST_CASE(v2_transit_credited_on_proven_arrival)
{
    // Section 7.6 accounting on a ledger seeded with 50 in transit (the
    // migration leaf that creates transit is a later slice): nothing is
    // credited before Ethereum's absorbedTotal counts it, the credit never
    // exceeds it or the transit, and invariant R counts only credited
    // amounts.
    const Consensus::Params params{V2Params()};
    bridge_test::ObservationWorld world{HealthyWorld(params, 1, 100)};
    node::BridgeStateIndex index{ObservationIndex(params, world.StateRoot())};
    bridge::ReserveLedgerV2 seeded;
    seeded.minted = 100;
    seeded.transit = 50;
    node::BridgeStateIndexTestAccess::SeedReserve(index, seeded, {{0, 100}});

    // Anchored before absorb(): absorbedTotal 0 credits nothing; R holds
    // (locked 100 >= P 100 + C 0).
    const node::BridgeBlockDelta before_absorb{ConnectNext(
        index, params, {Observe(world, SUBJECT_RESERVE, AnchorHash(800))})};
    BOOST_CHECK(before_absorb.reserve_v2_after == seeded);
    BOOST_CHECK(!index.ConflictV2());

    // absorb() counted 30: 30 moves to credited; locked grew to 130.
    world.SetReserve(1, 130, 30, 130);
    SeedStateAnchorsV2(index, {StateAnchor(801, 1'010, world.StateRoot())});
    ConnectNext(index, params, {Observe(world, SUBJECT_RESERVE, AnchorHash(801))}, 1'061);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().credited, 30);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().transit, 20);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 130);
    BOOST_CHECK(!index.ConflictV2());

    // absorbedTotal 80 (strays absorbed too): only the 20 left in transit
    // moves, so credited stops at 50 < 80.
    world.SetReserve(1, 180, 80, 180);
    SeedStateAnchorsV2(index, {StateAnchor(802, 1'020, world.StateRoot())});
    ConnectNext(index, params, {Observe(world, SUBJECT_RESERVE, AnchorHash(802))}, 1'062);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().credited, 50);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().transit, 0);
    BOOST_CHECK(!index.ConflictV2());

    // locked below P + C - burned (100 + 50 - 0) fails invariant R: E8.
    world.SetReserve(1, 149, 80, 149);
    SeedStateAnchorsV2(index, {StateAnchor(803, 1'030, world.StateRoot())});
    ConnectNext(index, params, {Observe(world, SUBJECT_RESERVE, AnchorHash(803))}, 1'063);
    BOOST_REQUIRE(index.ConflictV2());
    BOOST_CHECK_EQUAL(index.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_E8);
    BOOST_CHECK_EQUAL(index.ConflictV2()->anchor_block, 803U);

    // Undo walks the credits back.
    Disconnect(index);
    Disconnect(index);
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().credited, 30);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().transit, 20);
    Disconnect(index);
    Disconnect(index);
    BOOST_CHECK(index.ReserveLedgerV2() == seeded);
}

BOOST_AUTO_TEST_CASE(v2_burn_gate_per_burn_in_block)
{
    // Check 4 per record (node changes section 7.2): with a full context,
    // block validation and the block preview evaluate the burn gate again at
    // each burn, on the observations and sticky conflict after the block's
    // earlier records. A RESERVE_E failing invariant R or a TOKEN_FLAGS
    // showing a paused token earlier in the block refuses every later burn;
    // the same burn before the record passes.
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{FundingReceipts(params)};
    bridge_test::ObservationWorld world{HealthyWorld(params, 2, 100)};
    node::BridgeStateIndex probe{FundingIndex(params, receipts, world.StateRoot())};
    world.SetVerifier(lineage.Canonical(1, 109, params, probe));
    node::BridgeStateIndex index{FundingIndex(params, receipts, world.StateRoot())};
    ConnectNext(index, params, {MintTransactionV2(params, receipts, 0, 60)});
    ConnectNext(index, params,
                {MintTransactionV2(params, receipts, 1, 40),
                 Observe(world, SUBJECT_TOKEN, receipts.block_hash),
                 Observe(world, SUBJECT_RESERVE, receipts.block_hash),
                 Observe(world, SUBJECT_VERIFIER, receipts.block_hash)},
                1'061, &lineage.context);
    // Anchor 701: a drained vault (locked 25 < 100 proven below the cut).
    // Anchor 702: a paused token.
    bridge_test::ObservationWorld drained{world};
    drained.SetReserve(2, 25, 0, 25);
    bridge_test::ObservationWorld paused{world};
    paused.SetTokenFlags(true, false, false);
    SeedStateAnchorsV2(index, {StateAnchor(701, 1'010, drained.StateRoot()),
                               StateAnchor(702, 1'020, paused.StateRoot())});
    const int next{index.ConnectedHeight() + 1};
    BOOST_REQUIRE(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next, 1'063).readiness ==
                  node::BridgeBurnReadiness::READY);

    const CTransactionRef burn{BurnTransactionV2(params, 1, 10, 0x91)};
    const CTransactionRef drain{Observe(drained, SUBJECT_RESERVE, AnchorHash(701))};
    const CTransactionRef pause{Observe(paused, SUBJECT_TOKEN, AnchorHash(702))};
    const auto verifies{[&](const std::vector<CTransactionRef>& txs,
                            const node::BridgeV2LineageContext* context) {
        node::BridgeBlockDelta delta;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(1'063, txs), next, V2TestBlockHash(next), params, delta,
                                              error, context),
                            error);
        return delta;
    }};
    const std::string c4{"bridge V2 burn gate is not ready at the burn: C4: sticky E8 conflict: the vault failed invariant R"};
    const std::string e7{"bridge V2 burn gate is not ready at the burn: E7: token is paused"};

    // Alone, the burn passes the gate at its position and both bounds.
    BOOST_CHECK_EQUAL(verifies({burn}, &lineage.context).withdrawals_v2_added.size(), 1U);
    // After the record the burn is refused...
    VerifyFails(index, Block(1'063, {drain, burn}), next, params, c4, &lineage.context);
    VerifyFails(index, Block(1'063, {pause, burn}), next, params, e7, &lineage.context);
    VerifyFails(index, Block(1'063, {drain, pause, burn}), next, params, c4, &lineage.context);
    // ... and before it the burn passes, while the record still sets the
    // conflict (its invariant R counts the burn: 25 + 10 < 100).
    {
        const node::BridgeBlockDelta delta{verifies({burn, drain}, &lineage.context)};
        BOOST_CHECK_EQUAL(delta.withdrawals_v2_added.size(), 1U);
        BOOST_REQUIRE(delta.conflict_v2_after);
        BOOST_CHECK_EQUAL(delta.conflict_v2_after->reason, bridge::BRIDGE_V2_CONFLICT_E8);
        BOOST_CHECK_EQUAL(verifies({burn, pause}, &lineage.context).withdrawals_v2_added.size(), 1U);
    }
    // A record that leaves every row passing does not close the gate: a
    // second healthy RESERVE_E at anchor 703.
    {
        bridge_test::ObservationWorld refreshed{world};
        refreshed.SetReserve(2, 100, 0, 100);
        SeedStateAnchorsV2(index, {StateAnchor(703, 1'030, refreshed.StateRoot())});
        const int later{index.ConnectedHeight() + 1};
        node::BridgeBlockDelta delta;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyBlock(Block(1'064, {Observe(refreshed, SUBJECT_RESERVE, AnchorHash(703)), burn}),
                                              later, V2TestBlockHash(later), params, delta, error,
                                              &lineage.context),
                            error);
        BOOST_CHECK(!delta.conflict_v2_after);
        BOOST_CHECK_EQUAL(delta.withdrawals_v2_added.size(), 1U);
        Disconnect(index);
    }

    // Without a context a burn after an observation of its block fails
    // closed; with no earlier observation the caller's block-level gate
    // stands. Replay (REPLAY_LINEAGE) skips the per-burn gate: the block
    // passed it when it first connected.
    VerifyFails(index, Block(1'063, {pause, burn}), next, params,
                "bridge V2 burn after a kind 9 observation of the same block needs the fresh lineage context");
    BOOST_CHECK_EQUAL(verifies({burn}, nullptr).withdrawals_v2_added.size(), 1U);
    BOOST_CHECK_EQUAL(verifies({pause, burn}, &node::REPLAY_LINEAGE).withdrawals_v2_added.size(), 1U);

    // Block assembly: the preview takes the record and refuses the burn
    // after it, so the assembler skips the burn.
    for (const auto& [record, expected] : {std::pair{drain, c4}, std::pair{pause, e7}}) {
        std::string error;
        auto preview{index.BeginBlockPreview(next, 1'063, TestHash(98), params, error, &lineage.context)};
        BOOST_REQUIRE_MESSAGE(preview, error);
        const std::array<CTransactionRef, 1> record_chunk{record};
        const std::array<CTransactionRef, 1> burn_chunk{burn};
        BOOST_REQUIRE_MESSAGE(preview->TryAppend(record_chunk, error), error);
        BOOST_CHECK(!preview->TryAppend(burn_chunk, error));
        BOOST_CHECK_MESSAGE(error.find(expected) != std::string::npos, error);
    }
    // In the other order the preview takes both.
    {
        std::string error;
        auto preview{index.BeginBlockPreview(next, 1'063, TestHash(99), params, error, &lineage.context)};
        BOOST_REQUIRE_MESSAGE(preview, error);
        const std::array<CTransactionRef, 1> burn_chunk{burn};
        const std::array<CTransactionRef, 1> record_chunk{drain};
        BOOST_REQUIRE_MESSAGE(preview->TryAppend(burn_chunk, error), error);
        BOOST_CHECK_MESSAGE(preview->TryAppend(record_chunk, error), error);
    }
    // Mempool admission and wallet prevalidation judge one transaction on
    // the connected state, where the gate is READY.
    {
        node::BridgeTxAuthorization authorization;
        std::string error;
        BOOST_CHECK_MESSAGE(index.VerifyTransaction(*burn, next, 1'063, params, authorization, error,
                                                    &lineage.context),
                            error);
        BOOST_CHECK(authorization.withdrawal_v2);
    }
    // Once the drained RESERVE_E connects, the block-level gate (C4) and
    // the per-burn gate refuse every burn.
    ConnectNext(index, params, {drain}, 1'063, &lineage.context);
    BOOST_REQUIRE(index.ConflictV2());
    BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next + 1, 1'064).row ==
                bridge::BurnGateRowV2::C4);
    VerifyFails(index, Block(1'064, {burn}), next + 1, params, c4, &lineage.context);
}

BOOST_AUTO_TEST_CASE(v2_incremental_equals_rebuild)
{
    // A V2 history with mints, every subject (a LIVE verifier judged by a
    // full context), burns and an E8 conflict, built incrementally and
    // rebuilt from scratch as BridgeStateTracker replays it. Every block's
    // V2 delta and the final accounting agree field for field. A second run
    // adds a kind 13 FINALITY_CONFLICT block (judged by the full context,
    // trusted on replay) after the burns: it sets the sticky conflict, and
    // the RESERVE_E failing invariant R later leaves it in place.
    const Consensus::Params params{LineageParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{FundingReceipts(params)};
    bridge_test::ObservationWorld world{HealthyWorld(params, 2, 100)};
    node::BridgeStateIndex probe{FundingIndex(params, receipts, world.StateRoot())};
    world.SetVerifier(lineage.Canonical(1, 109, params, probe));
    const modern::FinalizedBlockV2 other{OtherBranch(lineage, 1, 109, params, probe)};
    const CTransactionRef conflict{
        ConflictTransaction(ConflictRecord(other, lineage.Signed(other)))};

    // Each step is a block (with the lineage it is judged under) or a seed.
    struct Step {
        std::vector<CTransactionRef> block;
        uint32_t time{0};
        bool judged{false};
        std::vector<StateAnchorSpec> anchors{};
    };
    // The vault then shows locked 25: 25 + 72 burned < 100 proven below the
    // cut, so invariant R fails (E8).
    bridge_test::ObservationWorld later{world};
    later.SetReserve(2, 25, 0, 25);
    for (const bool with_kind13 : {false, true}) {
        BOOST_TEST_MESSAGE("kind 13 block: " << with_kind13);
        node::BridgeStateIndex incremental{
            FundingIndex(params, receipts, world.StateRoot())};
        node::BridgeStateIndex rebuilt{FundingIndex(params, receipts, world.StateRoot())};
        std::vector<Step> steps{
            {{MintTransactionV2(params, receipts, 0, 60)}, 1'060, false},
            {{MintTransactionV2(params, receipts, 1, 40),
              Observe(world, SUBJECT_TOKEN, receipts.block_hash),
              Observe(world, SUBJECT_RESERVE, receipts.block_hash),
              Observe(world, SUBJECT_VERIFIER, receipts.block_hash)},
             1'061, true},
            {{BurnTransactionV2(params, 1, 50, 0x91), BurnTransactionV2(params, 2, 22, 0x92)},
             1'062, false},
            {{}, 0, false, {StateAnchor(701, 1'010, later.StateRoot())}},
            {{Observe(later, SUBJECT_RESERVE, AnchorHash(701)),
              MintTransactionV2(params, receipts, 2, 30)},
             1'063, false},
        };
        if (with_kind13) {
            steps.insert(steps.begin() + 3, Step{{conflict}, 1'062, true});
        }
        for (const Step& step : steps) {
            if (!step.anchors.empty()) {
                const node::BridgeBlockDelta a{SeedStateAnchorsV2(incremental, step.anchors)};
                const node::BridgeBlockDelta b{SeedStateAnchorsV2(rebuilt, step.anchors)};
                CheckSameV2Delta(a, b);
                continue;
            }
            const node::BridgeBlockDelta a{ConnectNext(
                incremental, params, step.block, step.time,
                step.judged ? &lineage.context : nullptr)};
            const node::BridgeBlockDelta b{ConnectNext(
                rebuilt, params, step.block, step.time, &node::REPLAY_LINEAGE)};
            CheckSameV2Delta(a, b);
            CheckSameAccounting(incremental, rebuilt);
        }
        BOOST_REQUIRE(incremental.ConflictV2());
        if (with_kind13) {
            BOOST_CHECK_EQUAL(incremental.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_KIND13);
            BOOST_CHECK_EQUAL(incremental.ConflictV2()->anchor_block, 0U);
            BOOST_CHECK(incremental.ConflictV2()->evidence_cert_id == modern::FinalityCertIdV2(other));
            BOOST_CHECK_EQUAL(incremental.ConflictV2()->evidence_checkpoint_index, 2U);
            BOOST_CHECK_EQUAL(incremental.ConflictV2()->evidence_epoch, 1U);
        } else {
            BOOST_CHECK_EQUAL(incremental.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_E8);
            BOOST_CHECK_EQUAL(incremental.ConflictV2()->anchor_block, 701U);
            BOOST_CHECK(incremental.ConflictV2()->evidence_cert_id.IsNull());
        }
        BOOST_CHECK_EQUAL(incremental.ReserveLedgerV2().minted, 130);
        BOOST_CHECK_EQUAL(incremental.ReserveLedgerV2().burned, 72);
        BOOST_CHECK_EQUAL(incremental.ProvenDepositsV2().size(), 3U);
        BOOST_CHECK(incremental.LatestObservationV2(SUBJECT_VERIFIER));
        BOOST_CHECK(incremental.LatestObservationV2(SUBJECT_TOKEN));
        BOOST_CHECK_EQUAL(incremental.LatestObservationV2(SUBJECT_RESERVE)->anchor_block, 701U);
        CheckSameAccounting(incremental, rebuilt);

        // Undo on both sides keeps them equal at every height.
        while (incremental.ConnectedHeight() >= BRIDGE_V2_HEIGHT) {
            Disconnect(incremental);
            Disconnect(rebuilt);
            CheckSameAccounting(incremental, rebuilt);
        }
        BOOST_CHECK(!incremental.ConflictV2());
        BOOST_CHECK(incremental.ReserveLedgerV2() == bridge::ReserveLedgerV2{});
        BOOST_CHECK(incremental.ProvenDepositsV2().empty());
        BOOST_CHECK(incremental.ObservationsV2() == node::BridgeObservationSlotsV2{});
    }
}

BOOST_AUTO_TEST_CASE(v2_migration_activation_and_retirement)
{
    // B-23 (node changes section 7.6): below h_m no record may name the
    // successor v'. From h_m the launch entry v is retired for burns, with
    // any context, but keeps minting (it cannot refund); v' mints, and the
    // block-level burn gate reads v''s rows (TOKEN_FLAGS_SUCCESSOR and
    // RESERVE_E_SUCCESSOR), whatever v's.
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    // v' after its deposit 0: cut 1, locked and balance 50.
    world.SetSuccessorReserve(1, 50, 0, 50);
    node::BridgeStateIndex index{MigrationIndex(params, receipts, world, lineage)};
    const auto launch_id{modern::ConfiguredBridgeV2EntryId(params, ROLE_LAUNCH)};
    const auto successor_id{modern::ConfiguredBridgeV2EntryId(params, ROLE_SUCCESSOR)};
    BOOST_REQUIRE(launch_id && successor_id);
    BOOST_CHECK(*launch_id != *successor_id);
    const int h_m{MIGRATION_HEIGHT};

    // Below h_m: no successor mint, burn, observation or sweep.
    {
        const int next{index.ConnectedHeight() + 1};
        BOOST_REQUIRE(next < h_m);
        VerifyFails(index, Block(1'061, {MintTransactionV2(params, receipts, 3, 50, ROLE_SUCCESSOR)}), next,
                    params, "bridge mint registry id does not match the V2 vault entry", &lineage.context);
        VerifyFails(index, Block(1'061, {SuccessorBurn(params, 1, 10, 0x91)}), next, params,
                    "bridge burn does not name the V2 vault entry", &lineage.context);
        for (const uint8_t subject : {SUBJECT_RESERVE_NEXT, SUBJECT_TOKEN_NEXT}) {
            VerifyFails(index, Block(1'061, {Observe(world, subject, receipts.block_hash)}), next, params,
                        "bridge V2 successor observations are not active", &lineage.context);
        }
        VerifyFails(index, Block(1'061, {SweepTransactionV2(params, 1)}), next, params,
                    "bridge V2 vault sweeps are not active", &lineage.context);
        // The launch entry takes burns; no sweep gate exists yet.
        const auto gate{node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next, 1'061)};
        BOOST_CHECK_MESSAGE(gate.readiness == node::BridgeBurnReadiness::READY, gate.Describe());
        const auto sweep_gate{node::EvaluateBridgeV2SweepGate(&lineage.context, index, params, next, 1'061)};
        BOOST_CHECK(sweep_gate.readiness == node::BridgeBurnReadiness::UNAVAILABLE);
        BOOST_CHECK(sweep_gate.Describe().find("sweeps are not active") != std::string::npos);
        const node::BridgeBlockDelta launch_burn{
            Verifies(index, params, {BurnTransactionV2(params, 2, 10, 0x92)}, 1'061, &lineage.context)};
        BOOST_REQUIRE_EQUAL(launch_burn.withdrawals_v2_added.size(), 1U);
        BOOST_CHECK(launch_burn.withdrawals_v2_added.front().entry == 0);
        BOOST_CHECK(launch_burn.withdrawals_v2_added.front().withdrawal.vault == params.bridge_v2->vault.vault_address);
    }
    ConnectEmptyThrough(index, params, h_m - 1);

    // From h_m a burn naming v is refused before the gate, deterministically:
    // with the full context, under replay and without a context.
    const CTransactionRef launch_burn{BurnTransactionV2(params, 3, 10, 0x93)};
    for (const node::BridgeV2LineageContext* context :
         {&lineage.context, &node::REPLAY_LINEAGE,
          static_cast<const node::BridgeV2LineageContext*>(nullptr)}) {
        VerifyFails(index, Block(1'070, {launch_burn}), h_m, params,
                    "bridge V2 burn names the retired vault entry", context);
    }
    // v keeps minting; v' mints from h_m, only against its own Deposit log.
    {
        const node::BridgeBlockDelta late{
            Verifies(index, params, {MintTransactionV2(params, receipts, 2, 30)}, 1'070, nullptr)};
        BOOST_CHECK(late.deposits_v2_added == (std::vector<node::BridgeProvenDepositV2>{{2, 30}}));
        BOOST_CHECK(late.deposits_v2_successor_added.empty());
        BOOST_CHECK_EQUAL(late.reserve_v2_after.minted, 130);
        BOOST_CHECK(late.reserve_v2_successor_after == bridge::ReserveLedgerV2{});
        const node::BridgeBlockDelta next_mint{Verifies(
            index, params, {MintTransactionV2(params, receipts, 3, 50, ROLE_SUCCESSOR)}, 1'070, nullptr)};
        BOOST_CHECK(next_mint.deposits_v2_added.empty());
        BOOST_CHECK(next_mint.deposits_v2_successor_added == (std::vector<node::BridgeProvenDepositV2>{{0, 50}}));
        BOOST_CHECK_EQUAL(next_mint.reserve_v2_successor_after.minted, 50);
        BOOST_CHECK_EQUAL(next_mint.reserve_v2_after.minted, 100);
        BOOST_REQUIRE_EQUAL(next_mint.nullifiers_added.size(), 1U);
        BOOST_CHECK(next_mint.nullifiers_added.front().vault_address == SuccessorVault());
        // Log 0 is v's Deposit, which v' cannot claim.
        VerifyFails(index, Block(1'070, {MintTransactionV2(params, receipts, 0, 60, ROLE_SUCCESSOR)}), h_m,
                    params, "does not name one exact configured-vault deposit log");
    }
    // The block-level gate is v''s now: v's TOKEN_FLAGS (3) is fresh, but
    // no TOKEN_FLAGS_SUCCESSOR exists. The sweep gate (v's but for
    // retirement) is READY.
    {
        const auto gate{node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, h_m, 1'070)};
        BOOST_CHECK(gate.row == bridge::BurnGateRowV2::E7);
        BOOST_CHECK_EQUAL(gate.Describe(), "E7: no TOKEN_FLAGS observation");
        const auto sweep_gate{node::EvaluateBridgeV2SweepGate(&lineage.context, index, params, h_m, 1'070)};
        BOOST_CHECK_MESSAGE(sweep_gate.readiness == node::BridgeBurnReadiness::READY, sweep_gate.Describe());
        BOOST_CHECK(node::EvaluateBridgeV2SweepGate(nullptr, index, params, h_m, 1'070).readiness ==
                    node::BridgeBurnReadiness::UNAVAILABLE);
        BOOST_CHECK(node::EvaluateBridgeV2SweepGate(&node::REPLAY_LINEAGE, index, params, h_m, 1'070).readiness ==
                    node::BridgeBurnReadiness::UNAVAILABLE);
    }
    ConnectNext(index, params,
                {MintTransactionV2(params, receipts, 3, 50, ROLE_SUCCESSOR),
                 Observe(world, SUBJECT_TOKEN_NEXT, receipts.block_hash)},
                1'070, &lineage.context);
    BOOST_CHECK(index.LatestObservationV2(SUBJECT_TOKEN_NEXT));
    BOOST_CHECK(!index.LatestObservationV2(SUBJECT_RESERVE_NEXT));
    BOOST_CHECK(index.ProvenDepositsV2(ROLE_SUCCESSOR) == (bridge::ProvenDepositMapV2{{0, 50}}));
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(ROLE_SUCCESSOR), 50);
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(ROLE_LAUNCH), 100);
    BOOST_CHECK_EQUAL(
        node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, h_m + 1, 1'071).Describe(),
        "E8: no RESERVE_E observation");
    VerifyFails(index, Block(1'071, {SuccessorBurn(params, 4, 10, 0x94)}), h_m + 1, params,
                "bridge V2 burn gate is not ready at the burn: E8: no RESERVE_E observation", &lineage.context);
    ConnectNext(index, params, {Observe(world, SUBJECT_RESERVE_NEXT, receipts.block_hash)}, 1'071,
                &lineage.context);
    BOOST_CHECK(!index.ConflictV2());
    {
        const auto gate{node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, h_m + 2, 1'072)};
        BOOST_CHECK_MESSAGE(gate.readiness == node::BridgeBurnReadiness::READY, gate.Describe());
    }
    // A burn naming v' passes; its leaf names v' with the shared asset, and
    // v''s ledger counts it.
    VerifyFails(index, Block(1'072, {SuccessorBurn(params, 4, 51, 0x94)}), h_m + 2, params,
                "bridge V2 burn exceeds the vault entry's reserve", &lineage.context);
    const node::BridgeBlockDelta burned{
        ConnectNext(index, params, {SuccessorBurn(params, 4, 10, 0x94)}, 1'072, &lineage.context)};
    BOOST_REQUIRE_EQUAL(burned.withdrawals_v2_added.size(), 1U);
    const node::BridgeWithdrawalV2Request& leaf{burned.withdrawals_v2_added.front()};
    BOOST_CHECK(leaf.entry == 1);
    BOOST_CHECK(!leaf.sweep);
    BOOST_CHECK(leaf.withdrawal.vault == SuccessorVault());
    BOOST_CHECK(leaf.withdrawal.token == params.bridge_v2->vault.token_address);
    BOOST_CHECK(leaf.withdrawal.asset_id == *modern::ConfiguredBridgeV2AssetId(params));
    BOOST_CHECK_EQUAL(leaf.withdrawal.destination_chain_id, params.bridge_v2->vault.origin_chain_id);
    BOOST_CHECK(leaf.leaf == *modern::BridgeWithdrawalLeafV2(leaf.withdrawal));
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).burned, 10);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).Reserve().value_or(-1), 40);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_LAUNCH).burned, 0);

    // Undo restores both ledgers and v''s deposits.
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).burned, 0);
    Disconnect(index);
    Disconnect(index);
    BOOST_CHECK(index.ReserveLedgerV2(ROLE_SUCCESSOR) == bridge::ReserveLedgerV2{});
    BOOST_CHECK(index.ProvenDepositsV2(ROLE_SUCCESSOR).empty());
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(ROLE_SUCCESSOR), 0);
    BOOST_CHECK(!index.LatestObservationV2(SUBJECT_TOKEN_NEXT));
}

BOOST_AUTO_TEST_CASE(v2_migration_sweep_leaf_and_transit)
{
    // Kind 16 (node changes section 7.6): from h_m, with v's gate READY but
    // for retirement, the block appends the migration leaf naming v and
    // paying v' reserve_v, which empties reserve_v and adds the amount to
    // transit_v'. Nothing is credited to v' yet.
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    const bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    node::BridgeStateIndex index{MigrationIndex(params, receipts, world, lineage)};
    // Anchors: 701 a paused token, 704 the same healthy world as the light
    // client's new head (t 100,000), 705 a drained v (E8), 706 a v balance
    // of 80 short of locked 100 and 707 a v balance of 0 (all seized, so
    // invariant R still holds). VERIFIER_E and RESERVE_E are then observed
    // again at 704, so TOKEN_FLAGS (t 1,000) goes stale first.
    bridge_test::ObservationWorld paused{world};
    paused.SetTokenFlags(true, false, false);
    bridge_test::ObservationWorld drained{world};
    drained.SetReserve(2, 25, 0, 25);
    bridge_test::ObservationWorld short_balance{world};
    short_balance.SetReserve(2, 100, 0, 80);
    bridge_test::ObservationWorld seized{world};
    seized.SetReserve(2, 100, 0, 0);
    const StateAnchorSpec head{StateAnchor(704, 100'000, world.StateRoot())};
    SeedStateAnchorsV2(index,
                       {StateAnchor(701, 1'010, paused.StateRoot()), head,
                        StateAnchor(705, 1'020, drained.StateRoot()),
                        StateAnchor(706, 1'030, short_balance.StateRoot()),
                        StateAnchor(707, 1'040, seized.StateRoot())},
                       head);
    ConnectNext(index, params,
                {Observe(world, SUBJECT_VERIFIER, AnchorHash(704)),
                 Observe(world, SUBJECT_RESERVE, AnchorHash(704))},
                100'060, &lineage.context);
    ConnectEmptyThrough(index, params, MIGRATION_HEIGHT - 1);
    const int h_m{MIGRATION_HEIGHT};
    constexpr uint32_t T{100'100};
    const CTransactionRef sweep{SweepTransactionV2(params, 1)};

    // Refused without a context, naming another entry, while v's gate is
    // closed (a stale TOKEN_FLAGS, a paused token or a sticky conflict
    // earlier in the block), and with no wind-down headroom (a balance of
    // 0).
    VerifyFails(index, Block(T, {sweep}), h_m, params, "bridge V2 vault sweep cannot be judged here");
    VerifyFails(index, Block(T, {SweepTransactionV2(params, 2, *modern::ConfiguredBridgeV2EntryId(params, ROLE_SUCCESSOR))}),
                h_m, params, "bridge V2 vault sweep does not name the retired vault entry", &lineage.context);
    VerifyFails(index, Block(T, {SweepTransactionV2(params, 2, TestHash(0x77))}), h_m, params,
                "bridge V2 vault sweep does not name the retired vault entry", &lineage.context);
    {
        const uint32_t stale{static_cast<uint32_t>(1'000 + params.bridge_v2->gate.observation_max_age_seconds + 1)};
        VerifyFails(index, Block(stale, {sweep}), h_m, params,
                    "bridge V2 vault sweep gate is not ready: E7: TOKEN_FLAGS observation is stale", &lineage.context);
        BOOST_CHECK_EQUAL(node::EvaluateBridgeV2SweepGate(&lineage.context, index, params, h_m, stale).Describe(),
                          "E7: TOKEN_FLAGS observation is stale");
    }
    VerifyFails(index, Block(T, {Observe(paused, SUBJECT_TOKEN, AnchorHash(701)), sweep}), h_m, params,
                "bridge V2 vault sweep gate is not ready: E7: token is paused", &lineage.context);
    VerifyFails(index, Block(T, {Observe(drained, SUBJECT_RESERVE, AnchorHash(705)), sweep}), h_m, params,
                "bridge V2 vault sweep gate is not ready: C4: sticky E8 conflict", &lineage.context);
    VerifyFails(index, Block(T, {Observe(seized, SUBJECT_RESERVE, AnchorHash(707)), sweep}), h_m, params,
                "bridge V2 vault sweep has no wind-down headroom at the latest reserve observation",
                &lineage.context);
    // Replay trusts the gate (judged when the block first connected); the
    // deterministic checks still run.
    BOOST_CHECK_EQUAL(Verifies(index, params, {Observe(paused, SUBJECT_TOKEN, AnchorHash(701)), sweep}, T,
                               &node::REPLAY_LINEAGE)
                          .withdrawals_v2_added.size(),
                      1U);
    VerifyFails(index, Block(T, {Observe(seized, SUBJECT_RESERVE, AnchorHash(707)), sweep}), h_m, params,
                "has no wind-down headroom", &node::REPLAY_LINEAGE);
    // A balance of 80 short of locked 100 raises margin_v to 8, so the
    // wind-down headroom is 72, below reserve_v (100): the sweep moves 72
    // (a leaf v can pay) and leaves the 28 short in reserve_v, which a later
    // sweep moves as far as a later observation allows. A second sweep in
    // the same block finds no headroom left.
    {
        const node::BridgeBlockDelta partial{Verifies(
            index, params, {Observe(short_balance, SUBJECT_RESERVE, AnchorHash(706)), sweep}, T,
            &lineage.context)};
        BOOST_REQUIRE_EQUAL(partial.withdrawals_v2_added.size(), 1U);
        BOOST_CHECK(partial.withdrawals_v2_added.front().withdrawal == ExpectedSweep(params, 0, 72, h_m));
        BOOST_CHECK_EQUAL(partial.reserve_v2_after.burned, 72);
        BOOST_CHECK_EQUAL(partial.reserve_v2_after.Reserve().value_or(-1), 28);
        BOOST_CHECK_EQUAL(partial.reserve_v2_successor_after.transit, 72);
        BOOST_CHECK(!partial.conflict_v2_after);
        VerifyFails(index,
                    Block(T, {Observe(short_balance, SUBJECT_RESERVE, AnchorHash(706)), sweep,
                              SweepTransactionV2(params, 2)}),
                    h_m, params, "bridge V2 vault sweep has no wind-down headroom at the latest reserve observation",
                    &lineage.context);
    }

    // With the context: the migration leaf. It names v, pays v' reserve_v
    // (100) as the next V2 id at this height; its source is the carrier's
    // binding output (1).
    const modern::BridgeWithdrawalV2 expected{ExpectedSweep(params, 0, 100, h_m)};
    BOOST_CHECK(expected.recipient == SuccessorVault());
    BOOST_CHECK(expected.vault == params.bridge_v2->vault.vault_address);
    const node::BridgeBlockDelta swept{Verifies(index, params, {sweep}, T, &lineage.context)};
    BOOST_REQUIRE_EQUAL(swept.withdrawals_v2_added.size(), 1U);
    {
        const node::BridgeWithdrawalV2Request& request{swept.withdrawals_v2_added.front()};
        BOOST_CHECK(request.withdrawal == expected);
        BOOST_CHECK(request.leaf == *modern::BridgeWithdrawalLeafV2(expected));
        BOOST_CHECK(request.transaction_id == sweep->GetHash());
        BOOST_CHECK_EQUAL(request.burn_output_index, 1U);
        BOOST_CHECK(sweep->vout[1] == *modern::MakeBridgeBindingOutput(sweep->mpa.front()));
        BOOST_CHECK(request.sweep);
        BOOST_CHECK(request.entry == 0);
        BOOST_CHECK_EQUAL(request.connected_height, h_m);
    }
    BOOST_CHECK_EQUAL(swept.reserve_v2_after.burned, 100);
    BOOST_CHECK_EQUAL(swept.reserve_v2_after.Reserve().value_or(-1), 0);
    BOOST_CHECK_EQUAL(swept.reserve_v2_successor_after.transit, 100);
    BOOST_CHECK_EQUAL(swept.reserve_v2_successor_after.credited, 0);
    BOOST_CHECK_EQUAL(swept.reserve_v2_successor_after.Reserve().value_or(-1), 0);
    BOOST_CHECK(swept.mint_authorizations.empty());
    // A second sweep in the same block has nothing to sweep.
    VerifyFails(index, Block(T, {sweep, SweepTransactionV2(params, 2)}), h_m, params,
                "bridge V2 vault sweep has nothing to sweep", &lineage.context);
    const node::BridgeBlockDelta connected{ConnectNext(index, params, {sweep}, T, &lineage.context)};
    CheckSameV2Delta(swept, connected);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_LAUNCH).Reserve().value_or(-1), 0);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 100);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 1U);
    {
        const auto stored{index.WithdrawalV2(node::BridgeWithdrawalId{sweep->GetHash(), 1})};
        BOOST_REQUIRE(stored);
        BOOST_CHECK(stored->sweep);
        BOOST_CHECK(stored->withdrawal == expected);
        BOOST_REQUIRE(index.WithdrawalsV2Through(h_m));
        BOOST_CHECK_EQUAL(index.WithdrawalsV2Through(h_m)->size(), 1U);
    }
    // ... and so does one in a later block.
    VerifyFails(index, Block(T + 1, {SweepTransactionV2(params, 2)}), h_m + 1, params,
                "bridge V2 vault sweep has nothing to sweep", &lineage.context);

    // Undo restores reserve_v and drops the transit and the leaf.
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_LAUNCH).Reserve().value_or(-1), 100);
    BOOST_CHECK(index.ReserveLedgerV2(ROLE_SUCCESSOR) == bridge::ReserveLedgerV2{});
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 0U);
    BOOST_CHECK(!index.WithdrawalV2(node::BridgeWithdrawalId{sweep->GetHash(), 1}));
}

BOOST_AUTO_TEST_CASE(v2_migration_credit_only_after_absorb)
{
    // Credit on proven arrival (node changes section 7.6) for v': a
    // RESERVE_E_SUCCESSOR anchored after the release but before absorb()
    // credits nothing and cannot fail invariant R; absorb() credits at most
    // what it counted and at most the transit; v' burns need the credit
    // (check 2); a forged drain of v' fails invariant R (E8).
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    const bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    node::BridgeStateIndex index{MigrationIndex(params, receipts, world, lineage)};
    ConnectEmptyThrough(index, params, MIGRATION_HEIGHT - 1);
    // h_m: the migration leaf (transit_v' 100) and v''s TOKEN_FLAGS.
    ConnectNext(index, params,
                {SweepTransactionV2(params, 1), Observe(world, SUBJECT_TOKEN_NEXT, receipts.block_hash)},
                1'070, &lineage.context);
    BOOST_REQUIRE_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 100);

    // v released the leaf: v holds nothing, v' holds 100 uncounted (701).
    // A partial absorb() of 99, one unit lost to a USDT fee (702); a later
    // absorb() of 31 more, strays included (703); a forged drain of v' (704).
    bridge_test::ObservationWorld released{world};
    released.SetReserve(2, 0, 0, 0);
    released.SetSuccessorReserve(0, 0, 0, 100);
    bridge_test::ObservationWorld partial{released};
    partial.SetSuccessorReserve(0, 99, 99, 99);
    bridge_test::ObservationWorld absorbed{released};
    absorbed.SetSuccessorReserve(0, 130, 130, 130);
    bridge_test::ObservationWorld forged{absorbed};
    forged.SetSuccessorReserve(0, 50, 130, 50);
    SeedStateAnchorsV2(index, {StateAnchor(701, 1'010, released.StateRoot()),
                               StateAnchor(702, 1'020, partial.StateRoot()),
                               StateAnchor(703, 1'030, absorbed.StateRoot()),
                               StateAnchor(704, 1'040, forged.StateRoot())});

    // Before absorb(): absorbedTotal 0 credits nothing; no conflict. v's
    // RESERVE_E shows the release, and invariant R holds for v across the
    // migration (locked 0 + burned 100 >= P 100 + C 0).
    const node::BridgeBlockDelta before_absorb{ConnectNext(
        index, params,
        {Observe(released, SUBJECT_RESERVE_NEXT, AnchorHash(701)), Observe(released, SUBJECT_RESERVE, AnchorHash(701))},
        1'071, &lineage.context)};
    BOOST_CHECK(before_absorb.reserve_v2_successor_after == before_absorb.reserve_v2_successor_before);
    BOOST_CHECK(before_absorb.reserve_v2_after == before_absorb.reserve_v2_before);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 100);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).credited, 0);
    BOOST_CHECK(!index.ConflictV2());
    BOOST_CHECK_EQUAL(bridge::InvariantRMarginV2(0, 0, index.ReserveLedgerV2(ROLE_SUCCESSOR)).value_or(-1), 0);
    BOOST_CHECK_EQUAL(bridge::InvariantRMarginV2(0, 100, index.ReserveLedgerV2(ROLE_LAUNCH)).value_or(-1), 0);
    // v''s gate is READY, but v' has no reserve before the credit: check 2.
    {
        const int next{index.ConnectedHeight() + 1};
        const auto gate{node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next, 1'072)};
        BOOST_CHECK_MESSAGE(gate.readiness == node::BridgeBurnReadiness::READY, gate.Describe());
        VerifyFails(index, Block(1'072, {SuccessorBurn(params, 1, 10, 0x91)}), next, params,
                    "bridge V2 burn exceeds the vault entry's reserve", &lineage.context);
    }

    // A partial absorb() credits 99 and leaves 1 in transit.
    ConnectNext(index, params, {Observe(partial, SUBJECT_RESERVE_NEXT, AnchorHash(702))}, 1'072, &lineage.context);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).credited, 99);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 1);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).Reserve().value_or(-1), 99);
    BOOST_CHECK(!index.ConflictV2());
    // Now a v' burn passes checks 2 and 3, up to the credit.
    VerifyFails(index, Block(1'073, {SuccessorBurn(params, 1, 100, 0x91)}), index.ConnectedHeight() + 1, params,
                "bridge V2 burn exceeds the vault entry's reserve", &lineage.context);
    ConnectNext(index, params, {SuccessorBurn(params, 1, 10, 0x91)}, 1'073, &lineage.context);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).burned, 10);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 2U);

    // absorb() counted 130 in all (31 more, strays included): only the 1
    // left in transit moves, so C_v' stops at 100 < 130.
    ConnectNext(index, params, {Observe(absorbed, SUBJECT_RESERVE_NEXT, AnchorHash(703))}, 1'074, &lineage.context);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).credited, 100);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 0);
    BOOST_CHECK(!index.ConflictV2());
    // Supply (100 minted - 10 burned to a leaf) equals the sum over both
    // entries of reserve + transit.
    {
        const bridge::ReserveLedgerV2& v{index.ReserveLedgerV2(ROLE_LAUNCH)};
        const bridge::ReserveLedgerV2& next{index.ReserveLedgerV2(ROLE_SUCCESSOR)};
        BOOST_CHECK_EQUAL(*v.Reserve() + v.transit + *next.Reserve() + next.transit, 100 - 10);
    }

    // A forged drain of v' (locked 50 + burned 10 < C_v' 100) fails
    // invariant R: the sticky E8 conflict, which closes both gates.
    ConnectNext(index, params, {Observe(forged, SUBJECT_RESERVE_NEXT, AnchorHash(704))}, 1'075, &lineage.context);
    BOOST_REQUIRE(index.ConflictV2());
    BOOST_CHECK_EQUAL(index.ConflictV2()->reason, bridge::BRIDGE_V2_CONFLICT_E8);
    BOOST_CHECK_EQUAL(index.ConflictV2()->anchor_block, 704U);
    {
        const int next{index.ConnectedHeight() + 1};
        BOOST_CHECK(node::EvaluateBridgeV2BurnGate(&lineage.context, index, params, next, 1'076).row ==
                    bridge::BurnGateRowV2::C4);
        BOOST_CHECK(node::EvaluateBridgeV2SweepGate(&lineage.context, index, params, next, 1'076).row ==
                    bridge::BurnGateRowV2::C4);
    }

    // Undo walks the credits back.
    Disconnect(index);
    BOOST_CHECK(!index.ConflictV2());
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).credited, 99);
    Disconnect(index);
    Disconnect(index);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).credited, 0);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 100);
}

BOOST_AUTO_TEST_CASE(v2_migration_late_deposit_swept)
{
    // A retired v keeps taking deposits while its verifier lives (node
    // changes section 7.6): B3 mints them and a later sweep moves exactly
    // the amount proven since the last one to v'. Invariant R keeps judging
    // v: a forged drain below the unswept deposits fails it.
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    const bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    node::BridgeStateIndex index{MigrationIndex(params, receipts, world, lineage)};
    ConnectEmptyThrough(index, params, MIGRATION_HEIGHT - 1);
    const int h_m{MIGRATION_HEIGHT};
    ConnectNext(index, params, {SweepTransactionV2(params, 1)}, 1'070, &lineage.context);
    BOOST_REQUIRE_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 0);
    VerifyFails(index, Block(1'071, {SweepTransactionV2(params, 2)}), h_m + 1, params,
                "bridge V2 vault sweep has nothing to sweep", &lineage.context);

    // Deposit 2 (30) into v after h_m: minted, and swept in the same block
    // when the sweep follows it.
    {
        const node::BridgeBlockDelta both{Verifies(
            index, params, {MintTransactionV2(params, receipts, 2, 30), SweepTransactionV2(params, 2)}, 1'071,
            &lineage.context)};
        BOOST_REQUIRE_EQUAL(both.withdrawals_v2_added.size(), 1U);
        BOOST_CHECK(both.withdrawals_v2_added.front().withdrawal == ExpectedSweep(params, 1, 30, h_m + 1));
    }
    ConnectNext(index, params, {MintTransactionV2(params, receipts, 2, 30)}, 1'071, &lineage.context);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().minted, 130);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 30);
    BOOST_CHECK(index.ProvenDepositsV2().contains(2));

    // Invariant R still judges v after the migration: anchor 701 shows v
    // holding the 30 (R holds exactly), anchor 702 a forged drain to 20
    // (20 + burned 100 < P 130): E8.
    bridge_test::ObservationWorld holding{world};
    holding.SetReserve(3, 30, 0, 30);
    bridge_test::ObservationWorld drained{world};
    drained.SetReserve(3, 20, 0, 20);
    SeedStateAnchorsV2(index, {StateAnchor(701, 1'010, holding.StateRoot()),
                               StateAnchor(702, 1'020, drained.StateRoot())});
    {
        const node::BridgeBlockDelta forged{
            Verifies(index, params, {Observe(drained, SUBJECT_RESERVE, AnchorHash(702))}, 1'072, &lineage.context)};
        BOOST_REQUIRE(forged.conflict_v2_after);
        BOOST_CHECK_EQUAL(forged.conflict_v2_after->reason, bridge::BRIDGE_V2_CONFLICT_E8);
    }
    ConnectNext(index, params, {Observe(holding, SUBJECT_RESERVE, AnchorHash(701))}, 1'072, &lineage.context);
    BOOST_CHECK(!index.ConflictV2());

    // The sweep moves exactly the late 30, as leaf 1 (wind-down headroom:
    // B 30 + P_post 0 - margin 0 - unreleased 0 = 30).
    const node::BridgeBlockDelta late{
        ConnectNext(index, params, {SweepTransactionV2(params, 3)}, 1'073, &lineage.context)};
    BOOST_REQUIRE_EQUAL(late.withdrawals_v2_added.size(), 1U);
    const modern::BridgeWithdrawalV2 expected{ExpectedSweep(params, 1, 30, index.ConnectedHeight())};
    BOOST_CHECK(late.withdrawals_v2_added.front().withdrawal == expected);
    BOOST_CHECK(late.withdrawals_v2_added.front().sweep);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().burned, 130);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2().Reserve().value_or(-1), 0);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 130);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 2U);
    // Supply (130 minted, nothing burned to a recipient) is all in transit.
    BOOST_CHECK_EQUAL(*index.ReserveLedgerV2().Reserve() + index.ReserveLedgerV2().transit +
                          *index.ReserveLedgerV2(ROLE_SUCCESSOR).Reserve() +
                          index.ReserveLedgerV2(ROLE_SUCCESSOR).transit,
                      130);
    VerifyFails(index, Block(1'074, {SweepTransactionV2(params, 4)}), index.ConnectedHeight() + 1, params,
                "bridge V2 vault sweep has nothing to sweep", &lineage.context);
}

BOOST_AUTO_TEST_CASE(v2_migration_delta_invariants)
{
    // ConnectBlock's B-23 accounting rules (node changes sections 7.1 and
    // 7.6) need no parameters: each entry's ledger moves only by its own
    // mints and leaves, a sweep names the launch entry and is the only
    // source of the successor's transit, the launch transit never rises,
    // and below V the successor fields pass through unchanged and empty.
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    const bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    node::BridgeStateIndex index{MigrationIndex(params, receipts, world, lineage)};
    ConnectEmptyThrough(index, params, MIGRATION_HEIGHT - 1);
    // A real h_m block: a v' mint (deposit 0, 50) and the migration leaf.
    const node::BridgeBlockDelta real{Verifies(
        index, params,
        {MintTransactionV2(params, receipts, 3, 50, ROLE_SUCCESSOR), SweepTransactionV2(params, 1)}, 1'070,
        &lineage.context)};
    BOOST_REQUIRE_EQUAL(real.withdrawals_v2_added.size(), 1U);
    BOOST_REQUIRE_EQUAL(real.deposits_v2_successor_added.size(), 1U);

    const auto refused{[](node::BridgeStateIndex& target, const node::BridgeBlockDelta& delta,
                          const std::string& expected) {
        const int height{target.ConnectedHeight()};
        std::string error;
        BOOST_CHECK(!target.ConnectBlock(delta, error));
        BOOST_CHECK_MESSAGE(error.find(expected) != std::string::npos,
                            "error '" << error << "' lacks '" << expected << "'");
        BOOST_CHECK_EQUAL(target.ConnectedHeight(), height);
    }};
    const std::string effects{"bridge delta reserve ledger does not match its effects"};
    {
        node::BridgeBlockDelta bad{real};
        bad.withdrawals_v2_added.front().entry = 1; // a sweep on the successor
        refused(index, bad, "bridge delta contains an invalid V2 withdrawal");
        bad.withdrawals_v2_added.front().entry = 2; // no such entry
        refused(index, bad, "bridge delta contains an invalid V2 withdrawal");
    }
    {
        // Successor transit without a sweep.
        node::BridgeBlockDelta bad{real};
        bad.withdrawals_v2_added.front().sweep = false;
        refused(index, bad, effects);
    }
    {
        // A launch transit increase: transit only ever reaches v'.
        node::BridgeBlockDelta bad{real};
        bad.reserve_v2_after.transit = 3;
        refused(index, bad, effects);
    }
    {
        node::BridgeBlockDelta bad{real};
        bad.reserve_v2_successor_after.transit += 1;
        refused(index, bad, effects);
        bad = real;
        bad.reserve_v2_successor_after.credited += 1;
        refused(index, bad, effects);
        bad = real;
        bad.reserve_v2_successor_after.minted -= 1;
        refused(index, bad, effects);
        bad = real;
        bad.reserve_v2_after.burned -= 1; // the sweep counts in burned_v
        refused(index, bad, effects);
    }
    {
        // The v' deposit listed as v's: id 0 is already v's own deposit,
        // and under a fresh id v's ledger does not count it.
        node::BridgeBlockDelta bad{real};
        std::swap(bad.deposits_v2_added, bad.deposits_v2_successor_added);
        refused(index, bad, "bridge delta contains an invalid proven deposit");
        bad.deposits_v2_added.front().deposit_id = 5;
        refused(index, bad, effects);
        // Listed twice.
        bad = real;
        bad.deposits_v2_successor_added.push_back(bad.deposits_v2_successor_added.front());
        refused(index, bad, "bridge delta contains an invalid proven deposit");
    }
    {
        node::BridgeBlockDelta bad{real};
        bad.reserve_v2_successor_before.minted = 1;
        refused(index, bad, "bridge delta V2 accounting before-state does not match");
    }

    // The real delta connects; undo restores the successor ledger and
    // deposits.
    {
        std::string error;
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(real, error), error);
    }
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).minted, 50);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_SUCCESSOR).transit, 100);
    BOOST_CHECK(index.ProvenDepositsV2(ROLE_SUCCESSOR) == (bridge::ProvenDepositMapV2{{0, 50}}));
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(ROLE_SUCCESSOR), 50);
    BOOST_CHECK(index.ProvenDepositSumsAtV2(ROLE_SUCCESSOR, 1) ==
                (std::optional<bridge::ProvenDepositSumsV2>{{1, 50, 0}}));
    // Each entry keeps its own ids: v's deposit 0 and v''s are distinct.
    BOOST_CHECK(index.ProvenDepositsV2(ROLE_LAUNCH) == (bridge::ProvenDepositMapV2{{0, 60}, {1, 40}}));
    Disconnect(index);
    BOOST_CHECK(index.ReserveLedgerV2(ROLE_SUCCESSOR) == bridge::ReserveLedgerV2{});
    BOOST_CHECK(index.ProvenDepositsV2(ROLE_SUCCESSOR).empty());
    BOOST_CHECK_EQUAL(index.ProvenDepositsTotalV2(ROLE_SUCCESSOR), 0);
    BOOST_CHECK_EQUAL(index.ReserveLedgerV2(ROLE_LAUNCH).Reserve().value_or(-1), 100);
    BOOST_CHECK_EQUAL(index.WithdrawalV2Count(), 0U);

    // Below V the successor fields stay unchanged and empty.
    node::BridgeStateIndex empty;
    node::BridgeBlockDelta below;
    below.height = BRIDGE_V2_HEIGHT - 1;
    below.block_hash = V2TestBlockHash(BRIDGE_V2_HEIGHT - 1);
    below.reserve_v2_successor_after.minted = 5;
    refused(empty, below, "bridge delta V2 accounting does not match its store");
    below.reserve_v2_successor_after = {};
    below.deposits_v2_successor_added.push_back({0, 5});
    refused(empty, below, "bridge delta V2 accounting does not match its store");
    below.deposits_v2_successor_added.clear();
    below.reserve_v2_successor_before.transit = 1;
    below.reserve_v2_successor_after.transit = 1;
    refused(empty, below, "bridge delta V2 accounting before-state does not match");
    BOOST_CHECK(empty.History().empty());
}

BOOST_AUTO_TEST_CASE(v2_migration_incremental_equals_rebuild)
{
    // A B-23 history (v mints, v' mints, the migration leaf, observations of
    // both entries crediting v''s transit, a v' burn, a late v deposit and
    // its sweep), built incrementally with the full context and rebuilt as
    // BridgeStateTracker replays it (REPLAY_LINEAGE): every block's V2
    // delta and the accounting of both entries agree field for field, and
    // undo keeps them equal.
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    // v' after its deposit 0 (cut 1, locked and balance 50).
    world.SetSuccessorReserve(1, 50, 0, 50);
    // After v released the migration leaf and v' absorbed it: v holds
    // nothing, v' locked 150 (deposit 0 and the 100 absorbed).
    bridge_test::ObservationWorld after{world};
    after.SetReserve(2, 0, 0, 0);
    after.SetSuccessorReserve(1, 150, 100, 150);

    struct Step {
        std::vector<CTransactionRef> block;
        uint32_t time{0};
        bool judged{false};
        std::vector<StateAnchorSpec> anchors{};
    };
    std::vector<Step> steps{
        {{MintTransactionV2(params, receipts, 0, 60), MintTransactionV2(params, receipts, 1, 40),
          Observe(world, SUBJECT_TOKEN, receipts.block_hash), Observe(world, SUBJECT_RESERVE, receipts.block_hash),
          Observe(world, SUBJECT_VERIFIER, receipts.block_hash)},
         1'060, true},
    };
    for (int h{BRIDGE_V2_HEIGHT + 1}; h < MIGRATION_HEIGHT; ++h) steps.push_back({{}, 1'060, false});
    steps.push_back({{MintTransactionV2(params, receipts, 3, 50, ROLE_SUCCESSOR),
                      Observe(world, SUBJECT_TOKEN_NEXT, receipts.block_hash),
                      Observe(world, SUBJECT_RESERVE_NEXT, receipts.block_hash), SweepTransactionV2(params, 1)},
                     1'070, true});
    steps.push_back({{}, 0, false, {StateAnchor(701, 1'010, after.StateRoot())}});
    steps.push_back({{Observe(after, SUBJECT_RESERVE_NEXT, AnchorHash(701)),
                      Observe(after, SUBJECT_RESERVE, AnchorHash(701))},
                     1'071, true});
    steps.push_back({{SuccessorBurn(params, 1, 20, 0x91)}, 1'072, true});
    steps.push_back({{MintTransactionV2(params, receipts, 2, 30), MintTransactionV2(params, receipts, 4, 20, ROLE_SUCCESSOR),
                      SweepTransactionV2(params, 2)},
                     1'073, true});

    node::BridgeStateIndex incremental{FundingIndex(params, receipts, world.StateRoot())};
    node::BridgeStateIndex rebuilt{FundingIndex(params, receipts, world.StateRoot())};
    for (const Step& step : steps) {
        if (!step.anchors.empty()) {
            const node::BridgeBlockDelta a{SeedStateAnchorsV2(incremental, step.anchors)};
            const node::BridgeBlockDelta b{SeedStateAnchorsV2(rebuilt, step.anchors)};
            CheckSameV2Delta(a, b);
            continue;
        }
        const node::BridgeBlockDelta a{ConnectNext(incremental, params, step.block, step.time,
                                                   step.judged ? &lineage.context : nullptr)};
        const node::BridgeBlockDelta b{
            ConnectNext(rebuilt, params, step.block, step.time, &node::REPLAY_LINEAGE)};
        CheckSameV2Delta(a, b);
        CheckSameAccounting(incremental, rebuilt);
    }
    BOOST_CHECK(!incremental.ConflictV2());
    const bridge::ReserveLedgerV2& v{incremental.ReserveLedgerV2(ROLE_LAUNCH)};
    const bridge::ReserveLedgerV2& next{incremental.ReserveLedgerV2(ROLE_SUCCESSOR)};
    BOOST_CHECK_EQUAL(v.minted, 130);
    BOOST_CHECK_EQUAL(v.burned, 130);
    BOOST_CHECK_EQUAL(v.transit, 0);
    BOOST_CHECK_EQUAL(next.minted, 70);
    BOOST_CHECK_EQUAL(next.burned, 20);
    BOOST_CHECK_EQUAL(next.credited, 100);
    BOOST_CHECK_EQUAL(next.transit, 30);
    // Supply: 200 minted, 20 burned to a recipient.
    BOOST_CHECK_EQUAL(*v.Reserve() + v.transit + *next.Reserve() + next.transit, 200 - 20);
    BOOST_CHECK_EQUAL(incremental.WithdrawalV2Count(), 3U);
    BOOST_CHECK(incremental.LatestObservationV2(SUBJECT_RESERVE_NEXT));
    BOOST_CHECK(incremental.LatestObservationV2(SUBJECT_TOKEN_NEXT));
    BOOST_CHECK_EQUAL(incremental.ProvenDepositsV2(ROLE_SUCCESSOR).size(), 2U);
    CheckSameAccounting(incremental, rebuilt);

    while (incremental.ConnectedHeight() >= BRIDGE_V2_HEIGHT) {
        Disconnect(incremental);
        Disconnect(rebuilt);
        CheckSameAccounting(incremental, rebuilt);
    }
    BOOST_CHECK(incremental.ReserveLedgerV2(ROLE_SUCCESSOR) == bridge::ReserveLedgerV2{});
    BOOST_CHECK(incremental.ProvenDepositsV2(ROLE_SUCCESSOR).empty());
    BOOST_CHECK(incremental.ObservationsV2() == node::BridgeObservationSlotsV2{});
}

BOOST_AUTO_TEST_CASE(v2_migration_preview_and_mempool)
{
    // A kind 16 record is a lineage record (mempool admission, block
    // connection, assembly and wallet prevalidation build the context for
    // it) but no burn. The preview takes it with a context and refuses it
    // without one; VerifyTransaction yields its leaf as withdrawal_v2.
    const Consensus::Params params{MigrationParams()};
    const LineageFixture lineage;
    const ReceiptFixture receipts{MigrationReceipts(params)};
    const bridge_test::ObservationWorld world{MigrationWorld(params, receipts, lineage)};
    node::BridgeStateIndex index{MigrationIndex(params, receipts, world, lineage)};
    ConnectEmptyThrough(index, params, MIGRATION_HEIGHT - 1);
    const int h_m{MIGRATION_HEIGHT};
    const CTransactionRef sweep{SweepTransactionV2(params, 1)};
    const CTransactionRef burn{BurnTransactionV2(params, 9, 5, 0x99)};

    BOOST_CHECK(node::HasBridgeV2LineageRecord(*sweep));
    BOOST_CHECK(node::HasBridgeV2LineageRecord(Block(1'070, {burn, sweep})));
    BOOST_CHECK(node::HasBridgeV2VaultSweep(*sweep));
    BOOST_CHECK(!node::HasBridgeV2VaultSweep(*burn));
    BOOST_CHECK(!node::HasBridgeV2FinalityConflict(*sweep));
    BOOST_CHECK(!node::HasDecentralizedBridgeBurn(*sweep));
    // The mempool's retirement eviction (ConnectTip at h_m - 1) matches a
    // kind 6 burn naming the launch entry, only where the migration is
    // active: never a v' burn, a sweep or a mint, and never on a chain
    // without a successor.
    {
        const CTransactionRef successor_burn{SuccessorBurn(params, 8, 5, 0x98)};
        const CTransactionRef mint{MintTransactionV2(params, receipts, 2, 30)};
        BOOST_CHECK(node::HasBridgeV2RetiredEntryBurn(*burn, params, h_m));
        BOOST_CHECK(node::HasBridgeV2RetiredEntryBurn(*burn, params, h_m + 100));
        BOOST_CHECK(!node::HasBridgeV2RetiredEntryBurn(*burn, params, h_m - 1));
        BOOST_CHECK(!node::HasBridgeV2RetiredEntryBurn(*successor_burn, params, h_m));
        BOOST_CHECK(!node::HasBridgeV2RetiredEntryBurn(*sweep, params, h_m));
        BOOST_CHECK(!node::HasBridgeV2RetiredEntryBurn(*mint, params, h_m));
        BOOST_CHECK(!node::HasBridgeV2RetiredEntryBurn(*burn, LineageParams(), h_m));
        Consensus::Params no_v2{params};
        no_v2.bridge_v2.reset();
        BOOST_CHECK(!node::HasBridgeV2RetiredEntryBurn(*burn, no_v2, h_m));
    }
    {
        std::string error;
        BOOST_CHECK(node::CheckBridgeV2BurnReadiness(*sweep, node::BridgeBurnReadiness::NOT_READY, error));
    }
    // Under the V1 record set kind 16 is malformed, so it is no lineage
    // record there.
    {
        const auto entry{modern::ConfiguredBridgeV2EntryId(params)};
        BOOST_CHECK(!bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::VAULT_SWEEP,
                                                                        bridge::BridgeVaultSweepV2{*entry}}));
    }

    const std::array<CTransactionRef, 1> chunk{sweep};
    {
        std::string error;
        auto preview{index.BeginBlockPreview(h_m, 1'070, TestHash(97), params, error, &lineage.context)};
        BOOST_REQUIRE_MESSAGE(preview, error);
        BOOST_CHECK_MESSAGE(preview->TryAppend(chunk, error), error);
        const std::array<CTransactionRef, 1> second{SweepTransactionV2(params, 2)};
        BOOST_CHECK(!preview->TryAppend(second, error));
        BOOST_CHECK_MESSAGE(error.find("nothing to sweep") != std::string::npos, error);
    }
    {
        std::string error;
        auto preview{index.BeginBlockPreview(h_m, 1'070, TestHash(98), params, error)};
        BOOST_REQUIRE_MESSAGE(preview, error);
        BOOST_CHECK(!preview->TryAppend(chunk, error));
        BOOST_CHECK_MESSAGE(error.find("bridge V2 vault sweep cannot be judged here") != std::string::npos, error);
    }
    // Mempool admission and wallet prevalidation judge one transaction on
    // the connected state.
    {
        node::BridgeTxAuthorization authorization;
        std::string error;
        BOOST_CHECK_MESSAGE(
            index.VerifyTransaction(*sweep, h_m, 1'070, params, authorization, error, &lineage.context), error);
        BOOST_REQUIRE(authorization.withdrawal_v2);
        BOOST_CHECK(authorization.withdrawal_v2->sweep);
        BOOST_CHECK(authorization.withdrawal_v2->entry == 0);
        BOOST_CHECK_EQUAL(authorization.withdrawal_v2->burn_output_index, 1U);
        BOOST_CHECK(authorization.withdrawal_v2->withdrawal == ExpectedSweep(params, 0, 100, h_m));
        BOOST_CHECK(authorization.HasWithdrawal());
        BOOST_CHECK(!authorization.mint);
        BOOST_CHECK(!authorization.withdrawal);
        BOOST_CHECK(!authorization.decentralized_withdrawal);
        BOOST_CHECK(!index.VerifyTransaction(*sweep, h_m, 1'070, params, authorization, error));
        BOOST_CHECK_MESSAGE(error.find("cannot be judged here") != std::string::npos, error);
    }
    // The sweep gate: READY at h_m, UNAVAILABLE off the parent.
    BOOST_CHECK(node::EvaluateBridgeV2SweepGate(&lineage.context, index, params, h_m, 1'070).readiness ==
                node::BridgeBurnReadiness::READY);
    BOOST_CHECK(node::EvaluateBridgeV2SweepGate(&lineage.context, index, params, h_m + 1, 1'070).readiness ==
                node::BridgeBurnReadiness::UNAVAILABLE);
    {
        Consensus::Params no_v2{params};
        no_v2.bridge_v2.reset();
        BOOST_CHECK(node::EvaluateBridgeV2SweepGate(&lineage.context, index, no_v2, h_m, 1'070).readiness ==
                    node::BridgeBurnReadiness::UNAVAILABLE);
    }
}

BOOST_AUTO_TEST_SUITE_END()

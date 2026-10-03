// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/regtest_beacon.h>

#include <bridge/eth_light_client.h>
#include <bridge/lc_json.h>
#include <bridge/proof.h>
#include <bridge/ssz.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/bridge_params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <modern/bridge_binding.h>
#include <node/bridge_state.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <span.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bridge;
using regtest_beacon::SparseTree;
using regtest_beacon::SyntheticBeacon;
using regtest_beacon::UpdateOptions;

BOOST_FIXTURE_TEST_SUITE(bridge_regtest_beacon_tests, BasicTestingSetup)

namespace {

//! Fabricated execution blocks start here, so a deployment block 20,001
//! below the checkpoint is still positive.
constexpr uint64_t BASE_BLOCK{30'000};
constexpr size_t HEADER_COUNT{24};
//! The checkpoint sits ten slots before the end of BASE_PERIOD: blocks
//! BASE+0..9 are in BASE_PERIOD and block BASE+10 opens the next period.
//! slot(N) = SLOT_OFFSET + N, so the checkpoint slot must be >= BASE_BLOCK.
constexpr uint64_t BASE_PERIOD{3};
constexpr uint64_t BASE_SLOT{(BASE_PERIOD + 1) * SLOTS_PER_PERIOD - 10};
static_assert(BASE_SLOT >= BASE_BLOCK);
constexpr uint64_t SLOT_OFFSET{BASE_SLOT - BASE_BLOCK};

uint256 Sha256(std::string_view text)
{
    uint256 out;
    CSHA256().Write(UCharCast(text.data()), text.size()).Finalize(out.begin());
    return out;
}

uint256 Tagged(std::string_view tag, const uint64_t value)
{
    std::array<unsigned char, 8> le{};
    WriteLE64(le.data(), value);
    uint256 out;
    CSHA256().Write(UCharCast(tag.data()), tag.size()).Write(le.data(), le.size()).Finalize(out.begin());
    return out;
}

//! Parent-linked execution payload headers first..first+count-1.
std::vector<ssz::ExecutionPayloadHeader> FabricatedHeaders(const uint64_t first, const size_t count)
{
    std::vector<ssz::ExecutionPayloadHeader> out;
    uint256 parent{Tagged("parent", first)};
    for (size_t i{0}; i < count; ++i) {
        const uint64_t number{first + i};
        ssz::ExecutionPayloadHeader header;
        header.parent_hash = parent;
        header.fee_recipient.fill(0x5a);
        header.state_root = Tagged("state", number);
        header.receipts_root = Tagged("receipts", number);
        header.logs_bloom[i % header.logs_bloom.size()] = 0x01;
        header.prev_randao = Tagged("randao", number);
        header.block_number = number;
        header.gas_limit = 30'000'000;
        header.gas_used = 21'000 * (i % 3);
        header.timestamp = 1'700'000'000 + 12 * i;
        header.extra_data = {'b', '3'};
        header.base_fee_per_gas.begin()[0] = 7; // little-endian 7 wei
        header.block_hash = Tagged("block", number);
        header.transactions_root = Tagged("transactions", number);
        header.withdrawals_root = Tagged("withdrawals", number);
        parent = header.block_hash;
        out.push_back(header);
    }
    return out;
}

regtest_beacon::SyntheticDomain RegtestDomain()
{
    return {regtest_beacon::DefaultGenesisValidatorsRoot(), regtest_beacon::DEFAULT_FORK_VERSION, 0};
}

//! One shared chain, so each committee's 512 keys are derived once.
const SyntheticBeacon& Beacon()
{
    static const SyntheticBeacon beacon{RegtestDomain(), SLOT_OFFSET,
                                        FabricatedHeaders(BASE_BLOCK, HEADER_COUNT)};
    return beacon;
}

//! A light-client store holding the bootstrap and the same-period update
//! finalizing BASE+3 (which proves the next period's committee).
LightClientStore BootstrappedStore()
{
    const SyntheticBeacon& beacon{Beacon()};
    const BridgeBootstrapV1 bootstrap{beacon.Bootstrap()};
    LightClientStore store;
    BOOST_REQUIRE(InitStore(store, beacon.Config(), beacon.CheckpointRoot(), bootstrap.header,
                            bootstrap.current_committee, bootstrap.current_committee_branch) == LcResult::OK);
    BOOST_REQUIRE(ProcessUpdate(store, beacon.Config(), beacon.Update(BASE_BLOCK + 3)) == LcResult::OK);
    return store;
}

CTransactionRef CarrierTransaction(const BridgeRecordV1& record)
{
    const auto mpa{MakeBridgeMpaRecord(record)};
    BOOST_REQUIRE(mpa);
    const auto binding{modern::MakeBridgeBindingOutput(*mpa)};
    BOOST_REQUIRE(binding);
    CMutableTransaction tx;
    tx.vout.push_back(*binding);
    tx.mpa.push_back(*mpa);
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef UpdateTransaction(const LightClientUpdate& update)
{
    return CarrierTransaction(BridgeRecordV1{BridgeRecordKindV1::UPDATE, BridgeUpdateV1{update}});
}

CBlock Block(const CTransactionRef& tx)
{
    CBlock block;
    block.nTime = 1'700'000'000;
    block.vtx.push_back(tx);
    return block;
}

uint256 B3BlockHash(const uint8_t value)
{
    uint256 hash;
    hash.begin()[0] = value;
    return hash;
}

std::string Repeat(const std::string& byte_hex, const size_t count)
{
    std::string out{"0x"};
    for (size_t i{0}; i < count; ++i) out += byte_hex;
    return out;
}

//! A -b3bridgepins file pointing the flowmesh regtest bridge at the
//! synthetic checkpoint, with the vault deployed at `deployment_block`.
std::string PinsJson(const SyntheticBeacon& beacon, const uint64_t deployment_block)
{
    const BridgeBootstrapV1 bootstrap{beacon.Bootstrap()};
    UniValue fork{UniValue::VOBJ};
    fork.pushKV("activation_epoch", uint64_t{0});
    fork.pushKV("fork_version", "0x" + HexStr(regtest_beacon::DEFAULT_FORK_VERSION));
    UniValue forks{UniValue::VARR};
    forks.push_back(fork);
    UniValue light{UniValue::VOBJ};
    light.pushKV("trusted_checkpoint_root", lcjson::EthereumHex(beacon.CheckpointRoot()));
    light.pushKV("trusted_checkpoint_slot", bootstrap.header.beacon.slot);
    light.pushKV("checkpoint_execution_block", bootstrap.header.execution.block_number);
    light.pushKV("genesis_validators_root", lcjson::EthereumHex(regtest_beacon::DefaultGenesisValidatorsRoot()));
    light.pushKV("fork_schedule", forks);
    light.pushKV("fork_schedule_valid_through_epoch", EpochAtSlot(bootstrap.header.beacon.slot) + 1'024);
    light.pushKV("electra_epoch", uint64_t{0});
    UniValue v1{UniValue::VOBJ};
    v1.pushKV("origin_chain_id", uint64_t{31'337});
    v1.pushKV("vault", "0x9fe46736679d2d9a65f0992f2272de9f3c7fa6e0");
    v1.pushKV("token", "0x5fbdb2315678afecb367f032d93f642f64180aa3");
    v1.pushKV("origin_deployment_block", deployment_block);
    v1.pushKV("vault_runtime_code_hash", Repeat("a1", 32));
    v1.pushKV("token_runtime_code_hash", Repeat("b1", 32));
    v1.pushKV("max_deposit_raw", uint64_t{1'000'000'000});
    v1.pushKV("light_client", light);
    v1.pushKV("verifier", "0xe7f1725e7734ce288f8367e1bb143e90bb3f0512");
    v1.pushKV("verifier_runtime_code_hash", Repeat("e1", 32));
    v1.pushKV("bootstrap_validator_set_hash", Repeat("f1", 32));
    v1.pushKV("min_bridge_validators", uint64_t{4});
    v1.pushKV("max_bridge_validators", uint64_t{64});
    v1.pushKV("min_bridge_total_weight", uint64_t{4});
    v1.pushKV("max_epoch_lag", uint64_t{86'400});
    UniValue root{UniValue::VOBJ};
    root.pushKV("format", "b3-regtest-bridge-pins/1");
    root.pushKV("v1_decentralized", v1);
    return root.write();
}

} // namespace

BOOST_AUTO_TEST_CASE(sparse_tree_matches_ssz_merkleization)
{
    BOOST_CHECK(SparseTree{4}.Root() == ssz::ZeroSubtree(4));

    // The synthetic block body: one non-zero field, the execution payload.
    const uint256 payload{Tagged("payload", 1)};
    SparseTree body{4};
    body.Set(EXECUTION_PAYLOAD_GINDEX, payload);
    std::vector<uint256> body_fields(16);
    body_fields[EXECUTION_PAYLOAD_GINDEX - 16] = payload;
    BOOST_CHECK(body.Root() == ssz::Merkleize(body_fields, 4));
    const auto body_branch{body.Branch(EXECUTION_PAYLOAD_GINDEX)};
    BOOST_CHECK_EQUAL(body_branch.size(), BRIDGE_EXECUTION_BRANCH_NODES);
    BOOST_CHECK(ssz::VerifyBranch(payload, body_branch, EXECUTION_PAYLOAD_GINDEX, body.Root()));

    // The synthetic state: a depth-6 container whose field 20 (the
    // finalized checkpoint) is itself the container {epoch, root}.
    const uint256 gvr{Tagged("gvr", 1)};
    const uint256 slot{ssz::LeafUint64(12'345)};
    const uint256 epoch{ssz::LeafUint64(385)};
    const uint256 checkpoint{Tagged("checkpoint", 1)};
    const uint256 current{Tagged("current", 1)};
    const uint256 next{Tagged("next", 1)};
    SparseTree state{6};
    state.Set(65, gvr);
    state.Set(66, slot);
    state.Set(FINALIZED_ROOT_GINDEX_ELECTRA - 1, epoch);
    state.Set(FINALIZED_ROOT_GINDEX_ELECTRA, checkpoint);
    state.Set(CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA, current);
    state.Set(NEXT_SYNC_COMMITTEE_GINDEX_ELECTRA, next);
    std::vector<uint256> fields(64);
    fields[1] = gvr;
    fields[2] = slot;
    fields[20] = ssz::HashPair(epoch, checkpoint);
    fields[22] = current;
    fields[23] = next;
    BOOST_CHECK(state.Root() == ssz::Merkleize(fields, 6));
    BOOST_CHECK(ssz::VerifyBranch(checkpoint, state.Branch(FINALIZED_ROOT_GINDEX_ELECTRA),
                                  FINALIZED_ROOT_GINDEX_ELECTRA, state.Root()));
    BOOST_CHECK(ssz::VerifyBranch(current, state.Branch(CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA),
                                  CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA, state.Root()));
    BOOST_CHECK(ssz::VerifyBranch(next, state.Branch(NEXT_SYNC_COMMITTEE_GINDEX_ELECTRA),
                                  NEXT_SYNC_COMMITTEE_GINDEX_ELECTRA, state.Root()));
    BOOST_CHECK(!ssz::VerifyBranch(next, state.Branch(CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA),
                                   CURRENT_SYNC_COMMITTEE_GINDEX_ELECTRA, state.Root()));

    // Leaves never nest, and the root is not a leaf.
    BOOST_CHECK_THROW(state.Set(84, payload), std::logic_error);
    BOOST_CHECK_THROW(state.Set(2 * FINALIZED_ROOT_GINDEX_ELECTRA, payload), std::logic_error);
    BOOST_CHECK_THROW(state.Set(1, payload), std::logic_error);
}

BOOST_AUTO_TEST_CASE(committee_keys_follow_the_published_derivation)
{
    BOOST_CHECK(regtest_beacon::DefaultGenesisValidatorsRoot() ==
                Sha256("B3-REGTEST-SYNC-COMMITTEE-V1/genesis_validators_root"));

    const regtest_beacon::SyntheticCommittee& committee{Beacon().Committee(BASE_PERIOD)};
    BOOST_REQUIRE_EQUAL(committee.committee.pubkeys.size(), ssz::SYNC_COMMITTEE_SIZE);
    BOOST_REQUIRE_EQUAL(committee.secrets.size(), ssz::SYNC_COMMITTEE_SIZE);
    for (const uint32_t index : {uint32_t{0}, uint32_t{341}, uint32_t{511}}) {
        // secret i = KeyGen(SHA256("B3-REGTEST-SYNC-COMMITTEE-V1" || LE64(p) || LE32(i)))
        std::vector<unsigned char> preimage(
            UCharCast(regtest_beacon::COMMITTEE_KEY_TAG.data()),
            UCharCast(regtest_beacon::COMMITTEE_KEY_TAG.data()) + regtest_beacon::COMMITTEE_KEY_TAG.size());
        const std::array<unsigned char, 12> suffix{BASE_PERIOD, 0, 0, 0, 0, 0, 0, 0,
                                                   static_cast<unsigned char>(index & 0xff),
                                                   static_cast<unsigned char>(index >> 8), 0, 0};
        preimage.insert(preimage.end(), suffix.begin(), suffix.end());
        std::array<unsigned char, 32> ikm{};
        CSHA256().Write(preimage.data(), preimage.size()).Finalize(ikm.data());
        const auto secret{bls::SecretKey::FromIKM(ikm)};
        BOOST_REQUIRE(secret);
        BOOST_CHECK(secret->GetPublicKey().Compressed() == committee.committee.pubkeys[index]);
    }
    BOOST_CHECK(committee.root == committee.committee.HashTreeRoot());
    BOOST_CHECK(bls::PublicKey::Decode(committee.committee.aggregate_pubkey));
    BOOST_CHECK(Beacon().Committee(BASE_PERIOD + 1).root != committee.root);
    BOOST_CHECK(&Beacon().Committee(BASE_PERIOD) == &committee); // derived once
}

BOOST_AUTO_TEST_CASE(light_client_accepts_bootstrap_update_and_rotation)
{
    const SyntheticBeacon& beacon{Beacon()};
    const LightClientConfig cfg{beacon.Config()};
    const auto headers{FabricatedHeaders(BASE_BLOCK, HEADER_COUNT)};

    const BridgeBootstrapV1 bootstrap{beacon.Bootstrap()};
    BOOST_CHECK_EQUAL(bootstrap.header.beacon.slot, BASE_SLOT);
    BOOST_CHECK(bootstrap.header.beacon.parent_root.IsNull());
    BOOST_CHECK(bootstrap.header.execution == headers.front());
    BOOST_CHECK(bootstrap.header.beacon.HashTreeRoot() == beacon.CheckpointRoot());
    LightClientStore store;
    BOOST_REQUIRE(InitStore(store, cfg, beacon.CheckpointRoot(), bootstrap.header,
                            bootstrap.current_committee, bootstrap.current_committee_branch) == LcResult::OK);
    BOOST_CHECK_EQUAL(store.period, BASE_PERIOD);
    BOOST_CHECK(!store.next);

    // Same-period update: finalizes BASE+3, proves the next period's committee.
    const LightClientUpdate same{beacon.Update(BASE_BLOCK + 3)};
    BOOST_CHECK(same.has_next);
    BOOST_CHECK_EQUAL(same.sync_aggregate.Participation(), ssz::SYNC_COMMITTEE_SIZE);
    BOOST_CHECK_EQUAL(same.finalized.beacon.slot, BASE_SLOT + 3);
    BOOST_CHECK_EQUAL(same.attested.beacon.slot, BASE_SLOT + 4);
    BOOST_CHECK_EQUAL(same.signature_slot, BASE_SLOT + 5);
    BOOST_CHECK(same.attested.beacon.parent_root == same.finalized.beacon.HashTreeRoot());
    BOOST_CHECK(same.finalized.execution == headers[3]);
    BOOST_REQUIRE(ProcessUpdate(store, cfg, same) == LcResult::OK);
    BOOST_CHECK(store.finalized_header == same.finalized);
    BOOST_REQUIRE(store.next);
    BOOST_CHECK(store.next->HashTreeRoot() == beacon.Committee(BASE_PERIOD + 1).root);

    // Rotation across the period boundary, signed by store.next with exactly
    // the supermajority.
    UpdateOptions threshold;
    threshold.participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    const LightClientUpdate rotation{beacon.Update(BASE_BLOCK + 12, threshold)};
    BOOST_REQUIRE_EQUAL(PeriodAtSlot(rotation.finalized.beacon.slot), BASE_PERIOD + 1);
    BOOST_REQUIRE_EQUAL(PeriodAtSlot(rotation.signature_slot), BASE_PERIOD + 1);
    BOOST_CHECK_EQUAL(rotation.sync_aggregate.Participation(), 342U);
    BOOST_CHECK(rotation.has_next);
    const ssz::SyncCommittee proven_next{*store.next};
    BOOST_REQUIRE(ProcessUpdate(store, cfg, rotation) == LcResult::OK);
    BOOST_CHECK_EQUAL(store.period, BASE_PERIOD + 1);
    BOOST_CHECK(store.current == proven_next);
    BOOST_CHECK(store.finalized_header == rotation.finalized);
    BOOST_REQUIRE(store.next);
    BOOST_CHECK(store.next->HashTreeRoot() == beacon.Committee(BASE_PERIOD + 2).root);

    // A finality update (no next committee) follows under the new committee.
    UpdateOptions finality_only;
    finality_only.next_committee = false;
    const LightClientUpdate finality{beacon.Update(BASE_BLOCK + 14, finality_only)};
    BOOST_CHECK(!finality.has_next);
    BOOST_REQUIRE(ProcessUpdate(store, cfg, finality) == LcResult::OK);
    BOOST_CHECK(store.finalized_header == finality.finalized);

    // Deterministic: the same headers give byte-identical objects.
    BOOST_CHECK(beacon.Update(BASE_BLOCK + 3) == same);
    BOOST_CHECK(SyntheticBeacon(RegtestDomain(), SLOT_OFFSET, headers).Bootstrap() == bootstrap);

    // Beacon-API JSON round trips (ParseUpdate is the relayer/ethcheck reader).
    for (const LightClientUpdate* update : {&same, &rotation, &finality}) {
        const UniValue json{lcjson::UpdateJson(*update)};
        BOOST_CHECK_EQUAL(json.find_value("next_sync_committee").isNull(), !update->has_next);
        BOOST_CHECK(lcjson::ParseUpdate(json) == *update);
        UniValue reparsed;
        BOOST_REQUIRE(reparsed.read(json.write()));
        BOOST_CHECK(lcjson::ParseUpdate(reparsed) == *update);
    }
    const UniValue boot_json{lcjson::BootstrapJson(bootstrap.header, bootstrap.current_committee,
                                                   bootstrap.current_committee_branch)};
    BOOST_CHECK(lcjson::ParseHeader(boot_json.find_value("header")) == bootstrap.header);
    BOOST_CHECK(lcjson::ParseCommittee(boot_json.find_value("current_sync_committee")) ==
                bootstrap.current_committee);
    BOOST_CHECK(lcjson::Branch(boot_json.find_value("current_sync_committee_branch"), "branch") ==
                bootstrap.current_committee_branch);

    // Canonical type-10 records round trip.
    const std::vector<BridgeRecordV1> records{
        {BridgeRecordKindV1::BOOTSTRAP, bootstrap},
        {BridgeRecordKindV1::UPDATE, BridgeUpdateV1{same}},
        {BridgeRecordKindV1::UPDATE, BridgeUpdateV1{finality}},
    };
    for (const BridgeRecordV1& record : records) {
        const auto encoded{EncodeBridgeRecordV1(record)};
        BOOST_REQUIRE(encoded);
        BOOST_CHECK(DecodeBridgeRecordV1(*encoded) == record);
    }
}

BOOST_AUTO_TEST_CASE(light_client_rejections_have_exact_codes)
{
    const SyntheticBeacon& beacon{Beacon()};
    const LightClientConfig cfg{beacon.Config()};
    const LightClientStore store{BootstrappedStore()};
    BOOST_REQUIRE_EQUAL(store.period, BASE_PERIOD);

    UpdateOptions threshold;
    threshold.participants = 342;
    BOOST_CHECK(VerifyUpdate(store, cfg, beacon.Update(BASE_BLOCK + 5, threshold)) == LcResult::OK);

    UpdateOptions below;
    below.participants = 341;
    BOOST_CHECK(VerifyUpdate(store, cfg, beacon.Update(BASE_BLOCK + 5, below)) == LcResult::PARTICIPATION);

    UpdateOptions wrong_root;
    wrong_root.sign_genesis_validators_root = Sha256("wrong");
    BOOST_CHECK(VerifyUpdate(store, cfg, beacon.Update(BASE_BLOCK + 5, wrong_root)) == LcResult::SIGNATURE);

    UpdateOptions wrong_fork;
    wrong_fork.sign_fork_version = std::array<unsigned char, 4>{0xb3, 0x00, 0x00, 0x02};
    BOOST_CHECK(VerifyUpdate(store, cfg, beacon.Update(BASE_BLOCK + 5, wrong_fork)) == LcResult::SIGNATURE);

    // Every proof in the shifted chain is genuine; only the two-period jump
    // past the store's committees refuses it.
    UpdateOptions jump;
    jump.slot_shift = 2 * SLOTS_PER_PERIOD;
    const LightClientUpdate jumped{beacon.Update(BASE_BLOCK + 5, jump)};
    BOOST_CHECK_EQUAL(PeriodAtSlot(jumped.signature_slot), store.period + 2);
    BOOST_CHECK(VerifyUpdate(store, cfg, jumped) == LcResult::PERIOD);

    // A different header at the store's finalized slot, proven under its own
    // attested state, is refused by the equal-slot rule.
    UpdateOptions conflict;
    conflict.finalized_proposer_index = 1;
    const LightClientUpdate conflicting{beacon.Update(BASE_BLOCK + 3, conflict)};
    BOOST_CHECK_EQUAL(conflicting.finalized.beacon.slot, store.finalized_header.beacon.slot);
    BOOST_CHECK_EQUAL(conflicting.finalized.beacon.proposer_index, 1U);
    BOOST_CHECK(conflicting.finalized.beacon.HashTreeRoot() != store.finalized_header.beacon.HashTreeRoot());
    BOOST_CHECK(ssz::VerifyBranch(conflicting.finalized.beacon.HashTreeRoot(), conflicting.finality_branch,
                                  cfg.FinalityGindex(EpochAtSlot(conflicting.attested.beacon.slot)),
                                  conflicting.attested.beacon.state_root));
    LightClientStore mutable_store{store};
    BOOST_CHECK(ProcessUpdate(mutable_store, cfg, conflicting) == LcResult::MONOTONICITY);
    BOOST_CHECK(mutable_store == store);
}

BOOST_AUTO_TEST_CASE(domain_guard_refuses_real_networks)
{
    const auto headers{FabricatedHeaders(BASE_BLOCK, 4)};
    const auto refused{[&](const regtest_beacon::SyntheticDomain& domain, const std::string& reason) {
        BOOST_CHECK_EXCEPTION(SyntheticBeacon(domain, SLOT_OFFSET, headers), std::runtime_error,
                              HasReason(reason));
    }};

    auto domain{RegtestDomain()};
    domain.genesis_validators_root = Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    refused(domain, "refusing the Ethereum mainnet genesis validators root");
    domain.genesis_validators_root.SetNull();
    refused(domain, "genesis validators root must be non-zero");

    // Mainnet phase0 and Electra, Sepolia Electra, Holesky genesis, Hoodi genesis.
    for (const std::array<unsigned char, 4> version :
         {std::array<unsigned char, 4>{0x00, 0x00, 0x00, 0x00}, std::array<unsigned char, 4>{0x05, 0x00, 0x00, 0x00},
          std::array<unsigned char, 4>{0x90, 0x00, 0x00, 0x74}, std::array<unsigned char, 4>{0x01, 0x01, 0x70, 0x00},
          std::array<unsigned char, 4>{0x10, 0x00, 0x09, 0x10}}) {
        domain = RegtestDomain();
        domain.fork_version = version;
        refused(domain, "outside the synthetic 0xb3 namespace");
    }
    for (const uint64_t electra : {uint64_t{1}, std::numeric_limits<uint64_t>::max()}) {
        domain = RegtestDomain();
        domain.electra_epoch = electra;
        refused(domain, "electra_epoch must be 0");
    }

    // Per-output signing variants pass the same guard.
    const SyntheticBeacon beacon{RegtestDomain(), SLOT_OFFSET, headers};
    UpdateOptions mainnet_root;
    mainnet_root.sign_genesis_validators_root = Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    BOOST_CHECK_EXCEPTION(beacon.Update(BASE_BLOCK, mainnet_root), std::runtime_error,
                          HasReason("refusing the Ethereum mainnet genesis validators root"));
    UpdateOptions mainnet_fork;
    mainnet_fork.sign_fork_version = std::array<unsigned char, 4>{0x05, 0x00, 0x00, 0x00};
    BOOST_CHECK_EXCEPTION(beacon.Update(BASE_BLOCK, mainnet_fork), std::runtime_error,
                          HasReason("outside the synthetic 0xb3 namespace"));
    for (const unsigned participants : {0U, 513U}) {
        UpdateOptions options;
        options.participants = participants;
        BOOST_CHECK_EXCEPTION(beacon.Update(BASE_BLOCK, options), std::runtime_error,
                              HasReason("participants must be between 1 and 512"));
    }
    // N needs N+1 as its attested block.
    BOOST_CHECK_EXCEPTION(beacon.Update(BASE_BLOCK + 3), std::runtime_error,
                          HasReason("needs itself and its attested successor"));
    BOOST_CHECK_EXCEPTION(beacon.Update(BASE_BLOCK - 1), std::runtime_error,
                          HasReason("needs itself and its attested successor"));

    // Execution headers must be consecutive, parent-linked and hashable.
    auto unlinked{headers};
    unlinked[2].parent_hash = uint256::ONE;
    BOOST_CHECK_EXCEPTION(SyntheticBeacon(RegtestDomain(), SLOT_OFFSET, unlinked), std::runtime_error,
                          HasReason("does not link to its parent hash"));
    auto gap{headers};
    gap.erase(gap.begin() + 1);
    BOOST_CHECK_EXCEPTION(SyntheticBeacon(RegtestDomain(), SLOT_OFFSET, gap), std::runtime_error,
                          HasReason("not consecutive"));
    auto long_extra{headers};
    long_extra[1].extra_data.assign(33, 0x01);
    BOOST_CHECK_EXCEPTION(SyntheticBeacon(RegtestDomain(), SLOT_OFFSET, long_extra), std::runtime_error,
                          HasReason("more than 32 bytes of extra data"));
    BOOST_CHECK_EXCEPTION(SyntheticBeacon(RegtestDomain(), SLOT_OFFSET, {}), std::runtime_error,
                          HasReason("no execution headers"));
    BOOST_CHECK_EXCEPTION(SyntheticBeacon(RegtestDomain(), 0, FabricatedHeaders(0, 2)), std::runtime_error,
                          HasReason("the base block slot must be non-zero"));
    BOOST_CHECK_EXCEPTION(SyntheticBeacon(RegtestDomain(), std::numeric_limits<uint64_t>::max(), headers),
                          std::runtime_error, HasReason("slot_offset overflows"));
}

BOOST_AUTO_TEST_CASE(consensus_index_verifies_synthetic_light_client_records)
{
    const SyntheticBeacon& beacon{Beacon()};
    const BridgeBootstrapV1 bootstrap{beacon.Bootstrap()};
    const uint64_t checkpoint_block{bootstrap.header.execution.block_number};
    BOOST_REQUIRE_EQUAL(checkpoint_block, BASE_BLOCK);

    // The pin writer's window check refuses deployment + 20,001 at startup.
    BOOST_CHECK_EXCEPTION(ParseB3RegTestBridgePins(PinsJson(beacon, checkpoint_block - 20'001)),
                          std::runtime_error, HasReason("within 20000 blocks"));

    const std::string contents{PinsJson(beacon, checkpoint_block - 20'000)};
    const fs::path path{m_path_root / "synthetic_pins.json"};
    {
        std::ofstream file{path.std_path(), std::ios::binary | std::ios::trunc};
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    ArgsManager args;
    args.ForceSetArg("-b3modernregtest", "1");
    args.ForceSetArg("-b3flowmeshtest", "1");
    args.ForceSetArg("-b3corridorlength", "130");
    args.ForceSetArg("-b3bridgepins", fs::PathToString(path));
    const auto chain{CreateChainParams(args, ChainType::REGTEST)};
    const Consensus::Params& params{chain->GetConsensus()};
    BOOST_REQUIRE(params.busd_bridge);
    BOOST_REQUIRE(params.busd_bridge->activation_height);
    BOOST_REQUIRE(params.busd_bridge->withdrawal_mode);
    BOOST_CHECK(*params.busd_bridge->withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1);
    const int height{*params.busd_bridge->activation_height};
    BOOST_REQUIRE_EQUAL(height, 163);
    BOOST_CHECK_EQUAL(*params.busd_bridge->origin_deployment_block, checkpoint_block - 20'000);

    const CTransactionRef bootstrap_tx{
        CarrierTransaction(BridgeRecordV1{BridgeRecordKindV1::BOOTSTRAP, bootstrap})};
    node::BridgeBlockDelta delta;
    std::string error;

    // Not before A3.
    node::BridgeStateIndex early;
    BOOST_CHECK(!early.VerifyBlock(Block(bootstrap_tx), height - 1, B3BlockHash(1), params, delta, error));
    BOOST_CHECK_EQUAL(error, "bridge consensus rules are not active");

    // A vault deployed one block beyond the cumulative backfill bound.
    Consensus::Params too_far{params};
    too_far.busd_bridge->origin_deployment_block = checkpoint_block - 20'001;
    node::BridgeStateIndex refusing;
    error.clear();
    BOOST_CHECK(!refusing.VerifyBlock(Block(bootstrap_tx), height, B3BlockHash(2), too_far, delta, error));
    BOOST_CHECK_EQUAL(error, "bridge bootstrap cannot reach the configured vault deployment block");

    // Exactly 20,000 blocks: the bootstrap connects.
    node::BridgeStateIndex index;
    error.clear();
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(Block(bootstrap_tx), height, B3BlockHash(3), params, delta, error),
                          error);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(index.LightClient()->finalized_header == bootstrap.header);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 1U);

    const LightClientUpdate update{beacon.Update(BASE_BLOCK + 3)};
    const CTransactionRef update_tx{UpdateTransaction(update)};
    delta = {};
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(Block(update_tx), height + 1, B3BlockHash(4), params, delta, error),
                          error);
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_REQUIRE(index.LightClient());
    BOOST_CHECK(index.LightClient()->finalized_header == update.finalized);
    BOOST_REQUIRE(index.LightClient()->next);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 2U);
    BOOST_CHECK(index.Anchor(update.finalized.execution.block_hash).has_value());

    UpdateOptions below;
    below.participants = 341;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(Block(UpdateTransaction(beacon.Update(BASE_BLOCK + 5, below))), height + 2,
                                   B3BlockHash(5), params, delta, error));
    BOOST_CHECK_EQUAL(error, "bridge light-client update is invalid");

    UpdateOptions conflict;
    conflict.finalized_proposer_index = 1;
    error.clear();
    BOOST_CHECK(!index.VerifyBlock(Block(UpdateTransaction(beacon.Update(BASE_BLOCK + 3, conflict))), height + 2,
                                   B3BlockHash(6), params, delta, error));
    BOOST_CHECK_EQUAL(error, "bridge light-client update conflicts at a finalized slot");

    error.clear();
    BOOST_CHECK(!index.VerifyBlock(Block(update_tx), height + 2, B3BlockHash(7), params, delta, error));
    BOOST_CHECK_EQUAL(error, "bridge light-client update is a no-op");

    BOOST_CHECK(index.LightClient()->finalized_header == update.finalized);
    BOOST_CHECK_EQUAL(index.AnchorCount(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()

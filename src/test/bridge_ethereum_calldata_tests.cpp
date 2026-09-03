// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/ethereum_calldata.h>

#include <node/finality_binding_index.h>
#include <rpc/client.h>
#include <rpc/blockchain.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <test/util/finality_fixture.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

uint256 Filled(const unsigned char byte)
{
    uint256 out;
    std::fill(out.begin(), out.end(), byte);
    return out;
}

bls::SecretKey Secret(const uint32_t index)
{
    std::array<unsigned char, 32> ikm{};
    WriteBE32(ikm.data() + 28, index + 1);
    const auto secret{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(secret);
    return *secret;
}

struct SetFixture {
    std::vector<bls::SecretKey> secrets;
    std::optional<node::ValidatorSetSnapshot> signing;
    std::optional<node::ValidatorSetSnapshot> successor;

    explicit SetFixture(const uint32_t count)
    {
        node::FinalityBindingIndex bindings;
        std::vector<node::FinalityBindingIndex::Transition> transitions;
        std::map<node::ValidatorKey, CAmount> weights;
        secrets.reserve(count);
        for (uint32_t i{0}; i < count; ++i) {
            secrets.push_back(Secret(i));
            modern::ValidatorKeyBytes validator{};
            WriteBE32(validator.data() + 28, i + 1);
            transitions.push_back({
                validator,
                modern::BindingRecord{
                    secrets.back().GetPublicKey().Compressed(), 0, 1}});
            weights.emplace(validator, modern::FINALITY_WEIGHT_UNIT);
        }
        bindings.ConnectBlock(1, transitions);
        const auto snapshot{
            node::ValidatorSetSnapshot::Build(0, weights, bindings)};
        BOOST_REQUIRE(snapshot);
        signing = *snapshot;
        successor = signing->WithEpoch(1);
    }

    modern::FinalityCertificate Certificate(
        const uint256& domain, const modern::FinalizedBlock& finalized,
        const std::vector<uint32_t>& signer_indices) const
    {
        modern::FinalityCertificate certificate;
        certificate.signer_bitmap.assign(
            modern::SignerBitmapBytes(signing->Size()), 0);
        const uint256 digest{modern::FinalityDigest(domain, finalized)};
        std::vector<bls::Signature> signatures;
        for (const uint32_t index : signer_indices) {
            BOOST_REQUIRE(index < secrets.size());
            certificate.signer_bitmap[index >> 3] |=
                static_cast<unsigned char>(1U << (index & 7));
            signatures.push_back(secrets[index].Sign(
                std::span<const unsigned char>{digest.begin(), 32}));
        }
        const auto aggregate{bls::AggregateSignatures(signatures)};
        BOOST_REQUIRE(aggregate);
        certificate.aggregate_sig = aggregate->Compressed();
        return certificate;
    }
};

struct OutboundRpcFixture : public b3test::FinalityChainFixture {
    CRPCTable rpc_table;

    OutboundRpcFixture() { RegisterBlockchainRPCCommands(rpc_table); }

    void ConfigureBridge(const int activation)
    {
        Consensus::BridgeAssetParams bridge;
        bridge.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
        bridge.origin_deployment_block = 1;
        bridge.vault_runtime_code_hash = Filled(0x70);
        bridge.implementation_or_adapter = Filled(0x71);
        bridge.adapter_version = 1;
        bridge.recipient_encoding_version =
            Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
        bridge.activation_height = activation;
        bridge.mint_caps = Consensus::BridgeMintCaps{
            .max_per_block = 5'000'000,
            .max_per_epoch = 20'000'000,
            .epoch_length_blocks = 30};
        Consensus::EthereumLightClientPins light;
        light.trusted_checkpoint_root = Filled(0x72);
        light.trusted_checkpoint_slot = 1;
        light.genesis_validators_root =
            Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
        light.fork_schedule = {{0, {0, 0, 0, 0}}};
        light.fork_schedule_valid_through_epoch = 1'000'000;
        light.min_sync_committee_participants =
            Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
        light.max_sync_lag_slots = 8'192;
        bridge.light_client = light;
        bridge.withdrawal_mode =
            Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1;
        Consensus::BridgeDecentralizedWithdrawalPins withdrawal;
        withdrawal.ethereum_verifier_address.fill(0x74);
        withdrawal.ethereum_verifier_code_hash = Filled(0x75);
        withdrawal.bootstrap_validator_set_hash = Filled(0x76);
        withdrawal.withdrawal_rules_version =
            Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
        withdrawal.withdrawal_rules_commitment = Filled(0x77);
        withdrawal.min_bridge_validators = 4;
        withdrawal.max_bridge_validators = 64;
        withdrawal.min_bridge_total_weight = 34;
        withdrawal.max_epoch_lag = 86'400;
        bridge.decentralized_withdrawal = withdrawal;
        BOOST_REQUIRE(Consensus::BridgeMintParamsReady(bridge));
        MutableConsensus().busd_bridge = std::move(bridge);
        MutableConsensus().bridge_withdrawal_activation_height = activation;
    }

    UniValue CallRPC(const std::string& command)
    {
        std::vector<std::string> args{util::SplitString(command, ' ')};
        JSONRPCRequest request;
        request.context = &m_node;
        request.strMethod = args.front();
        args.erase(args.begin());
        request.params = RPCConvertValues(request.strMethod, args);
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        try {
            return rpc_table.execute(request);
        } catch (const UniValue& error) {
            throw std::runtime_error(
                error.find_value("message").get_str());
        }
    }
};

modern::FinalizedBlock Finalized(const SetFixture& fixture)
{
    return modern::FinalizedBlock{
        811001, Filled(0x01), Filled(0x02), fixture.successor->SetHash(), 0};
}

uint64_t AbiWord(const std::span<const unsigned char> bytes,
                 const size_t offset)
{
    BOOST_REQUIRE(offset + 32 <= bytes.size());
    for (size_t i{offset}; i < offset + 24; ++i) {
        BOOST_REQUIRE_EQUAL(bytes[i], 0);
    }
    return ReadBE64(bytes.data() + offset + 24);
}

} // namespace

BOOST_AUTO_TEST_SUITE(bridge_ethereum_calldata_tests)

BOOST_AUTO_TEST_CASE(finality_four_member_bitmap_and_selectors)
{
    SetFixture fixture{4};
    const uint256 domain{Filled(0xd0)};
    const modern::FinalizedBlock finalized{Finalized(fixture)};
    const modern::FinalityCertificate certificate{
        fixture.Certificate(domain, finalized, {0, 1, 3})};
    std::string error;
    const auto artifacts{bridge::BuildFinalityRelayArtifacts(
        domain, finalized, certificate, *fixture.signing, *fixture.successor,
        64, error)};
    BOOST_REQUIRE_MESSAGE(artifacts, error);
    BOOST_CHECK_EQUAL(HexStr(artifacts->signer_bitmap), "0b");
    BOOST_REQUIRE_EQUAL(artifacts->absent.size(), 1U);
    BOOST_CHECK_EQUAL(artifacts->absent[0].index, 2U);
    BOOST_CHECK_EQUAL(artifacts->signer_count, 3U);
    BOOST_CHECK_EQUAL(artifacts->signed_weight, 3U);
    BOOST_REQUIRE_GE(artifacts->submit_calldata.size(), 4U);
    BOOST_CHECK_EQUAL(
        HexStr(std::span<const unsigned char>{artifacts->submit_calldata}.first(4)),
        "44b9d159"); // cast sig / Solidity submitCertificate.selector
    BOOST_CHECK_EQUAL(AbiWord(artifacts->submit_calldata, 4 + 5 * 32),
                      7U * 32U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->submit_calldata, 4 + 6 * 32),
                      7U * 32U + 10U * 32U);

    uint256 root{modern::ValidatorSetLeaf(
        artifacts->absent[0].index,
        artifacts->absent[0].compressed_pubkey,
        artifacts->absent[0].weight)};
    for (unsigned level{0}; level < modern::FINALITY_SET_TREE_DEPTH; ++level) {
        root = ((artifacts->absent[0].index >> level) & 1U) != 0
                   ? modern::WithdrawalNodeHash(
                         artifacts->absent[0].siblings[level], root)
                   : modern::WithdrawalNodeHash(
                         root, artifacts->absent[0].siblings[level]);
    }
    BOOST_CHECK(root == fixture.signing->Header().members_root);
}

BOOST_AUTO_TEST_CASE(finality_transition_epoch_and_malformed_bitmap)
{
    SetFixture fixture{4};
    const uint256 domain{Filled(0xd0)};
    const node::ValidatorSetSnapshot signing{
        fixture.signing->WithEpoch(1)};
    const node::ValidatorSetSnapshot successor{signing.WithEpoch(2)};
    modern::FinalizedBlock finalized{
        812441, Filled(0x31), Filled(0x32), successor.SetHash(), 1};
    const modern::FinalityCertificate certificate{
        fixture.Certificate(domain, finalized, {0, 1, 2})};
    std::string error;
    const auto artifacts{bridge::BuildFinalityRelayArtifacts(
        domain, finalized, certificate, signing, successor, 64, error)};
    BOOST_REQUIRE_MESSAGE(artifacts, error);
    BOOST_CHECK_EQUAL(artifacts->finalized_block.epoch, 1U);
    BOOST_CHECK_EQUAL(artifacts->signing_set.epoch, 1U);
    BOOST_CHECK_EQUAL(artifacts->successor_set.epoch, 2U);

    modern::FinalityCertificate malformed{certificate};
    malformed.signer_bitmap[0] |= 0x80; // bits above n must remain zero
    BOOST_CHECK(!bridge::BuildFinalityRelayArtifacts(
        domain, finalized, malformed, signing, successor, 64, error));
    BOOST_CHECK(error.find("malformed-bitmap") != std::string::npos);

    modern::FinalizedBlock mismatched{finalized};
    mismatched.validator_set_hash = Filled(0x7f);
    BOOST_CHECK(!bridge::BuildFinalityRelayArtifacts(
        domain, mismatched, certificate, signing, successor, 64, error));
    BOOST_CHECK(error.find("successor set") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(finality_real_sixty_four_member_gas_fixture_shape)
{
    SetFixture fixture{64};
    const uint256 domain{Filled(0xd0)};
    const modern::FinalizedBlock finalized{Finalized(fixture)};
    std::vector<uint32_t> signers;
    for (uint32_t i{0}; i < 43; ++i) signers.push_back(i);
    const modern::FinalityCertificate certificate{
        fixture.Certificate(domain, finalized, signers)};
    std::string error;
    const auto artifacts{bridge::BuildFinalityRelayArtifacts(
        domain, finalized, certificate, *fixture.signing, *fixture.successor,
        64, error)};
    BOOST_REQUIRE_MESSAGE(artifacts, error);
    BOOST_CHECK_EQUAL(artifacts->signer_count, 43U);
    BOOST_CHECK_EQUAL(artifacts->absent.size(), 21U);
    BOOST_CHECK_EQUAL(artifacts->absent.front().index, 43U);
    BOOST_CHECK_EQUAL(artifacts->absent.back().index, 63U);
    // Cross-checks the exact fixture dimensions exercised by the Solidity
    // EIP-2537 target-fork gas test.
    BOOST_CHECK_EQUAL(artifacts->proof_abi.size(), 18144U);
    BOOST_CHECK_EQUAL(artifacts->submit_calldata.size(), 18724U);
}

BOOST_AUTO_TEST_CASE(withdrawal_leaf_path_and_release_calldata)
{
    std::vector<modern::BridgeWithdrawalV1> withdrawals;
    for (uint64_t i{0}; i < 3; ++i) {
        modern::BridgeWithdrawalV1 withdrawal;
        withdrawal.withdrawal_id = i;
        withdrawal.origin_chain_id = 1;
        for (size_t j{0}; j < 32; ++j) {
            withdrawal.asset_id.begin()[j] = static_cast<unsigned char>(j);
        }
        withdrawal.origin_token.fill(0x11);
        withdrawal.recipient.fill(static_cast<unsigned char>(0x22 + i));
        withdrawal.amount = 1'000'000 + static_cast<CAmount>(i);
        withdrawal.b3_height = 811001 + i;
        withdrawals.push_back(withdrawal);
    }
    const auto first_leaf{modern::BridgeWithdrawalLeafV1(withdrawals[0])};
    BOOST_REQUIRE(first_leaf);
    BOOST_CHECK_EQUAL(
        HexStr(*first_leaf),
        "80053a3a0d7b39f9ee1b9bffe67deba6b3118e7460143a89538c5afa4cd0667c");

    modern::WithdrawalTreeState tree;
    for (const auto& withdrawal : withdrawals) {
        BOOST_REQUIRE(modern::AppendBridgeWithdrawal(tree, withdrawal));
    }
    std::string error;
    const auto artifacts{bridge::BuildWithdrawalRelayArtifacts(
        withdrawals, 1, tree.root, error)};
    BOOST_REQUIRE_MESSAGE(artifacts, error);
    BOOST_CHECK_EQUAL(artifacts->release_calldata.size(), 1156U);
    BOOST_CHECK_EQUAL(
        HexStr(std::span<const unsigned char>{artifacts->release_calldata}.first(4)),
        "ca514def"); // cast sig / Solidity B3StakerBridge.release.selector
    BOOST_CHECK_EQUAL(
        AbiWord(artifacts->release_calldata, 4), 1U);
    BOOST_CHECK_EQUAL(
        AbiWord(artifacts->release_calldata, 4 + 2 * 32), 1'000'001U);
    BOOST_CHECK_EQUAL(
        AbiWord(artifacts->release_calldata, 4 + 3 * 32), 811002U);
    BOOST_CHECK(artifacts->root == tree.root);

    uint256 node{artifacts->leaf};
    for (unsigned level{0}; level < modern::WITHDRAWAL_TREE_DEPTH; ++level) {
        node = ((artifacts->withdrawal.withdrawal_id >> level) & 1U) != 0
                   ? modern::WithdrawalNodeHash(artifacts->path[level], node)
                   : modern::WithdrawalNodeHash(node, artifacts->path[level]);
    }
    BOOST_CHECK(node == tree.root);

    uint256 wrong_root{tree.root};
    wrong_root.begin()[0] ^= 1;
    BOOST_CHECK(!bridge::BuildWithdrawalRelayArtifacts(
        withdrawals, 1, wrong_root, error));
    BOOST_CHECK(!bridge::BuildWithdrawalRelayArtifacts(
        withdrawals, std::numeric_limits<uint64_t>::max(), tree.root,
        error));
}

BOOST_FIXTURE_TEST_CASE(getbridgeinfo_exports_complete_contract_pin,
                        OutboundRpcFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1,
                         /*reorg_horizon=*/200,
                         /*with_unbound_c=*/false,
                         /*with_bound_c=*/true,
                         /*with_bound_d=*/true);
    ConfigureBridge(/*activation=*/m_M + 100);
    const UniValue result{CallRPC("getbridgeinfo")};

    BOOST_CHECK(result.find_value("ready").get_bool());
    // The fixture tip is M - 1 while both bridge gates are M + 100. The RPC
    // must export the configured height without claiming burns are active yet.
    BOOST_CHECK(!result.find_value("withdrawal_active").get_bool());
    BOOST_CHECK_EQUAL(
        result.find_value("withdrawal_activation_height").getInt<int>(),
        m_M + 100);
    BOOST_CHECK(!result.find_value("registry_id").isNull());
    BOOST_CHECK_EQUAL(result.find_value("origin_deployment_block").getInt<uint64_t>(),
                      1U);
    BOOST_CHECK(!result.find_value("vault_runtime_code_hash").isNull());
    BOOST_CHECK(!result.find_value("implementation_or_adapter").isNull());
    BOOST_CHECK_EQUAL(result.find_value("adapter_version").getInt<uint32_t>(),
                      1U);
    BOOST_CHECK_EQUAL(
        result.find_value("recipient_encoding_version").getInt<uint32_t>(),
        Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1);
    BOOST_CHECK_EQUAL(result.find_value("withdrawal_mode").get_str(),
                      "decentralized-verifier-v1");
    BOOST_CHECK(!result.find_value("decentralized_verifier").isNull());
    BOOST_CHECK(!result.find_value("decentralized_verifier_code_hash").isNull());
    BOOST_CHECK(!result.find_value("bootstrap_validator_set_hash").isNull());
    BOOST_CHECK_EQUAL(result.find_value("withdrawal_rules_version").getInt<uint32_t>(),
                      Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1);
    BOOST_CHECK(!result.find_value("withdrawal_rules_commitment").isNull());
    BOOST_CHECK_EQUAL(result.find_value("min_bridge_validators").getInt<uint32_t>(),
                      4U);
    BOOST_CHECK_EQUAL(result.find_value("max_bridge_validators").getInt<uint32_t>(),
                      64U);
    BOOST_CHECK_EQUAL(result.find_value("max_per_block").getInt<int64_t>(),
                      5'000'000);
    BOOST_CHECK_EQUAL(result.find_value("max_per_epoch").getInt<int64_t>(),
                      20'000'000);
    BOOST_CHECK_EQUAL(result.find_value("mint_epoch_length_blocks").getInt<uint32_t>(),
                      30U);
}

BOOST_FIXTURE_TEST_CASE(historical_rpc_same_epoch_transition_and_fail_closed,
                        OutboundRpcFixture)
{
    PrepareFinalityChain(/*min_finality_set=*/1,
                         /*reorg_horizon=*/200,
                         /*with_unbound_c=*/false,
                         /*with_bound_c=*/true,
                         /*with_bound_d=*/true);
    const int M{m_M};
    // Keep bridge roots inactive for this finality-only history fixture; all
    // production pins are nevertheless complete and the four-member set is
    // within the deployment bounds.
    ConfigureBridge(M + 100);

    Produce(m_vk_a); // Set_0 begins at M.
    const node::ValidatorSetSnapshot set0{*FinalityState().current};
    const uint256 set1_hash{FinalityState().next->SetHash()};
    ProduceTo(M + 7, m_vk_a);
    Produce(m_vk_a, {MakeCertificate(
        {M + 5, 0, set1_hash, true, true, true}, set0)});
    const int first_inclusion{Tip()->nHeight};
    const UniValue first{CallRPC(
        "getbridgefinalityproof " + std::to_string(first_inclusion))};
    BOOST_CHECK_EQUAL(first.find_value("epoch").getInt<uint64_t>(), 0U);
    BOOST_CHECK_EQUAL(first.find_value("signer_count").getInt<uint64_t>(),
                      3U);
    BOOST_CHECK_EQUAL(first.find_value("absent_indices").size(), 1U);
    BOOST_CHECK_EQUAL(first.find_value("calldata").get_str().substr(0, 10),
                      "0x44b9d159");

    // The handover rotates at M+30. A successor-signed epoch-1 certificate
    // exercises the verifier's transition calldata using historical replay.
    ProduceTo(M + 30, m_vk_a);
    const node::ValidatorSetSnapshot set1{*FinalityState().current};
    BOOST_REQUIRE_EQUAL(set1.Epoch(), 1U);
    const uint256 set2_hash{FinalityState().next->SetHash()};
    ProduceTo(M + 32, m_vk_a);
    Produce(m_vk_a, {MakeCertificate(
        {M + 30, 1, set2_hash, true, true, true}, set1)});
    const int transition_inclusion{Tip()->nHeight};
    const UniValue transition{CallRPC(
        "getbridgefinalityproof " +
        std::to_string(transition_inclusion))};
    BOOST_CHECK_EQUAL(
        transition.find_value("epoch").getInt<uint64_t>(), 1U);
    BOOST_CHECK_EQUAL(
        transition.find_value("successor_set_hash").get_str(),
        "0x" + HexStr(set2_hash));

    // An active block without a certificate is not a proof source.
    Produce(m_vk_a);
    BOOST_CHECK_THROW(
        CallRPC("getbridgefinalityproof " +
                std::to_string(Tip()->nHeight)),
        std::runtime_error);

    // A stored but consensus-rejected block is not on the active chain and
    // must be rejected before any of its certificate bytes are considered.
    const CBlock rejected{BuildPosBlock(
        m_vk_a,
        {MakeCertificate(
            {M + 30, 1, set2_hash, true, true, true}, set1)})};
    if (rejected.GetBlockTime() > GetTime()) {
        SetMockTime(rejected.GetBlockTime());
    }
    SubmitExpectConnectFailure(rejected);
    BOOST_CHECK_THROW(
        CallRPC("getbridgefinalityproof " + rejected.GetHash().GetHex()),
        std::runtime_error);

    // Clearing HAVE_DATA models an otherwise-active certificate block whose
    // body has been pruned. The RPC must not infer or fabricate its proof.
    CBlockIndex* pruned{nullptr};
    {
        LOCK(cs_main);
        pruned = m_node.chainman->ActiveChain()[first_inclusion];
        BOOST_REQUIRE(pruned != nullptr);
        BOOST_REQUIRE(pruned->nStatus & BLOCK_HAVE_DATA);
        pruned->nStatus &= ~BLOCK_HAVE_DATA;
    }
    BOOST_CHECK_THROW(
        CallRPC("getbridgefinalityproof " +
                std::to_string(first_inclusion)),
        std::runtime_error);
    {
        LOCK(cs_main);
        pruned->nStatus |= BLOCK_HAVE_DATA;
    }
}

BOOST_AUTO_TEST_SUITE_END()

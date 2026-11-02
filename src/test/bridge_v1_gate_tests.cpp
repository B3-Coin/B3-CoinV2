// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// The V1 bridge burn gate on a real modern-PoS chain, under mainnet's bridge
// shape: bUSD in verifier mode (DECENTRALIZED_VERIFIER_V1, mainnet from
// 811,001), withdrawals open from the bridge height. A decentralized
// BRIDGE_BURN destroys bUSD for a withdrawal leaf that only a certificate of
// the V1 validator sets can carry to Ethereum, so mempool admission, block
// validation, block assembly and wallet bridge prevalidation refuse it unless
// the sets projected for its block meet the withdrawal pins on an intact
// lineage (node::BridgeWithdrawalValidatorSetsReady).
// finality_qualification_tests tests that predicate itself, a broken lineage
// included, and finality_epoch_tests its rebuild and reorg;
// bridge_v2_gate_tests covers the V2 gate, on a managed V1 bridge whose V1
// readiness is always READY. This file shows that the four paths apply the
// V1 readiness, each at the height of the block the burn is for.
//
// FinalityChainFixture's chain (legacy prefix, corridor, four staked and
// bound validators A, B, C, D with weights 15, 1, 10, 8) gains a
// verifier-mode bUSD bridge from M + 1 whose withdrawal height is also
// M + 1. Its pins want 4 to 64 validators of total weight at least 34, which
// Set_0 = Set_1 = {A, B, C, D} meet exactly. No block carries a certificate,
// so epoch 0's extension runs out without a handover and the V1 lineage
// breaks at M + E + MAX_EXTENSION (M + 60), the way mainnet's broke in
// epoch 10: the gate is READY for the blocks M + 1 to M + 59 and NOT_READY
// from M + 60 on, for good.
//
// The burn is synthetic: it spends a native funding output and burns bUSD it
// does not hold, so it can never be valid. Every path runs the gate before
// asset conservation, so a READY gate shows as a bad-asset-transition refusal
// (in block assembly, as a template that fails TestBlockValidity because the
// assembler selected the burn), and a closed gate as
// bridge-withdrawal-not-ready (and a burn the assembler skips). Wallet
// prevalidation checks no assets: it accepts the burn while the gate is
// READY. Block assembly gets the burn straight in its mempool, as if it had
// been admitted earlier.

#include <bridge/proof.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <interfaces/chain.h>
#include <modern/asset_output.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <node/bridge_state.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {

using b3test::FinalityChainFixture;

//! The synthetic burn's amount, in raw bUSD units.
constexpr CAmount BURN_AMOUNT{5};
//! The funding output the burn spends (outputs 0-3, 6, 7, 10 and 11 carry
//! the corridor's stakes and bindings).
constexpr unsigned BURN_FUNDING{8};
//! The burn's native fee.
constexpr CAmount BURN_FEE{10'000};

//! The gate's refusal, as mempool admission and block validation report it,
//! and the error text wallet prevalidation returns.
const std::string NOT_READY{"bridge-withdrawal-not-ready"};
const std::string NOT_READY_ERROR{"decentralized bridge burn requires bridge-ready current and next validator sets"};
//! The refusal of a burn that passed the gate: it holds no bUSD.
const std::string NO_BUSD{"bad-asset-transition"};

//! bUSD in verifier mode from `activation`, deposits and withdrawals alike,
//! as on mainnet from 811,001. The pins are those of finality_h1_tests'
//! ConfigureVerifierBridge (OutboundRpcFixture::ConfigureBridge in
//! bridge_ethereum_calldata_tests).
Consensus::BridgeAssetParams VerifierBusd(const int activation)
{
    const auto filled{[](const unsigned char byte) {
        uint256 out;
        std::fill(out.begin(), out.end(), byte);
        return out;
    }};
    Consensus::BridgeAssetParams bridge;
    bridge.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
    bridge.origin_deployment_block = 1;
    bridge.vault_runtime_code_hash = filled(0x70);
    bridge.implementation_or_adapter = filled(0x71);
    bridge.adapter_version = 1;
    bridge.recipient_encoding_version = Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
    bridge.activation_height = activation;
    bridge.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = 5'000'000, .max_per_epoch = 20'000'000, .epoch_length_blocks = 30};
    Consensus::EthereumLightClientPins& light{bridge.light_client.emplace()};
    light.trusted_checkpoint_root = filled(0x72);
    light.trusted_checkpoint_slot = 1;
    light.genesis_validators_root = Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    light.fork_schedule = {{0, {0, 0, 0, 0}}};
    light.fork_schedule_valid_through_epoch = 1'000'000;
    light.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 8'192;
    bridge.withdrawal_mode = Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1;
    Consensus::BridgeDecentralizedWithdrawalPins& withdrawal{bridge.decentralized_withdrawal.emplace()};
    withdrawal.ethereum_verifier_address.fill(0x74);
    withdrawal.ethereum_verifier_code_hash = filled(0x75);
    withdrawal.bootstrap_validator_set_hash = filled(0x76);
    withdrawal.withdrawal_rules_version = Consensus::DECENTRALIZED_WITHDRAWAL_RULES_VERSION_V1;
    withdrawal.withdrawal_rules_commitment = Consensus::DECENTRALIZED_WITHDRAWAL_RULES_COMMITMENT_V1;
    withdrawal.min_bridge_validators = 4;
    withdrawal.max_bridge_validators = 64;
    withdrawal.min_bridge_total_weight = 34;
    withdrawal.max_epoch_lag = 86'400;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(bridge));
    return bridge;
}

struct BridgeV1GateFixture : public FinalityChainFixture {
    //! The first height whose projected V1 lineage is broken.
    int m_break{0};

    //! The chain to M - 1 with the bridge configured (see file comment).
    void PrepareBridgeChain()
    {
        PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/false,
                             /*with_bound_c=*/true, /*with_bound_d=*/true);
        Consensus::Params& c{MutableConsensus()};
        const int bridge_height{m_M + 1};
        c.busd_bridge = VerifierBusd(bridge_height);
        c.bridge_withdrawal_activation_height = bridge_height;
        BOOST_REQUIRE(!Consensus::BridgeRulesActive(bridge_height - 1, c));
        BOOST_REQUIRE(Consensus::BridgeWithdrawalRulesActive(bridge_height, c));
        BOOST_REQUIRE(!c.bridge_v2);
        m_break = m_M + SCALED_E + SCALED_MAX_EXTENSION;
    }

    const Consensus::Params& Params() { return m_node.chainman->GetConsensus(); }
    const Consensus::BridgeDecentralizedWithdrawalPins& Pins() { return *Params().busd_bridge->decentralized_withdrawal; }

    //! The block a burn judged at the tip is for, as "M + k".
    std::string Next() { return strprintf("M + %d", Tip()->nHeight + 1 - m_M); }

    //! Each check names the path and the block it judged for.
    void Has(const std::string& path, const std::string& text, const std::string& part)
    {
        BOOST_CHECK_MESSAGE(text.find(part) != std::string::npos,
                            path << " for " << Next() << ": '" << text << "' lacks '" << part << "'");
    }
    void Lacks(const std::string& path, const std::string& text, const std::string& part)
    {
        BOOST_CHECK_MESSAGE(text.find(part) == std::string::npos,
                            path << " for " << Next() << ": '" << text << "' has '" << part << "'");
    }

    //! A decentralized burn of BURN_AMOUNT bUSD it does not hold, to
    //! 0x9999...99, funded by a native funding output, with change.
    CMutableTransaction SyntheticBurn()
    {
        const auto asset{modern::ConfiguredBridgeAssetId(Params())};
        const auto registry{modern::ConfiguredBridgeRegistryId(Params())};
        BOOST_REQUIRE(asset && registry);
        bridge::BridgeBurnV1 burn;
        burn.registry_id = *registry;
        burn.burn_output_index = 0;
        burn.raw_amount = BURN_AMOUNT;
        burn.ethereum_recipient.fill(0x99);
        const auto record{bridge::MakeBridgeMpaRecord(
            bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::BRIDGE_BURN, burn})};
        BOOST_REQUIRE(record);
        const auto burned{modern::MakeAssetBurnOutput(*asset, BURN_AMOUNT)};
        const auto binding{modern::MakeBridgeBindingOutput(*record)};
        BOOST_REQUIRE(burned && binding);
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{m_fund_txid, BURN_FUNDING};
        tx.vout = {*burned, *binding, CTxOut{m_fund_value / 16 - BURN_FEE, CScript() << OP_TRUE}};
        tx.mpa = {*record};
        return tx;
    }

    //! The time of A's next block, the time block validation and block
    //! assembly judge it at; the mock clock moves there when it is behind.
    void AlignClock()
    {
        const int64_t next{BuildPosBlock(m_vk_a).GetBlockTime()};
        if (next > GetTime()) SetMockTime(next);
    }

    //! Mempool admission dry run at the tip: "" or "reason, debug".
    std::string Admit(const CMutableTransaction& tx)
    {
        LOCK(cs_main);
        const MempoolAcceptResult result{
            m_node.chainman->ProcessTransaction(MakeTransactionRef(tx), /*test_accept=*/true)};
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID ? std::string{}
                                                                               : result.m_state.ToString();
    }

    //! Block validity of A's next block carrying `tx`: "" or the state.
    std::string BlockWith(const CMutableTransaction& tx)
    {
        const CBlock block{BuildPosBlock(m_vk_a, {}, {tx})};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return Probe(block);
    }

    //! node::interfaces wallet prevalidation (bridgewithdraw).
    interfaces::BridgePrevalidationResult Prevalidate(const CMutableTransaction& tx, std::string& error,
                                                     node::BridgeTxAuthorization& authorization)
    {
        const CBlockIndex* tip{Tip()};
        return m_node.chain->prevalidateBridgeTransaction(CTransaction{tx}, tip->GetBlockHash(),
                                                          tip->nHeight + 1, authorization, error);
    }

    /**
     * node::BlockAssembler for validator A with `tx` placed straight in the
     * mempool. CreateNewBlock runs TestBlockValidity, so it throws if the
     * assembler selected a transaction the block rule refuses. Returns "" and
     * sets `carried` when a template was built, else the exception text. The
     * mempool is emptied again.
     */
    std::string Assemble(const CMutableTransaction& mtx, bool& carried)
    {
        carried = false;
        AlignClock();
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        CTxMemPool& pool{*m_node.mempool};
        TryAddToMempool(pool, TestMemPoolEntryHelper{}.Fee(BURN_FEE).FromTx(tx));
        BOOST_REQUIRE(WITH_LOCK(pool.cs, return pool.exists(tx->GetWitnessHash())));
        node::BlockAssembler::Options options;
        options.coinbase_output_script = CScript() << OP_TRUE;
        options.include_dummy_extranonce = true;
        options.modern_pos_validator_key = m_vk_a;
        BOOST_REQUIRE(options.test_block_validity);
        std::string error;
        try {
            const auto tmpl{
                node::BlockAssembler(m_node.chainman->ActiveChainstate(), &pool, options).CreateNewBlock()};
            BOOST_REQUIRE(tmpl);
            carried = std::any_of(tmpl->block.vtx.begin(), tmpl->block.vtx.end(), [&](const CTransactionRef& in) {
                return in && in->GetHash() == tx->GetHash();
            });
        } catch (const std::runtime_error& e) {
            error = e.what();
        }
        WITH_LOCK(pool.cs, pool.removeRecursive(*tx, MemPoolRemovalReason::REPLACED));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
        return error;
    }

    /** The burn passes the gate in all four paths and fails only at asset
     * conservation; wallet prevalidation, which checks no assets, accepts it
     * as the chain's first withdrawal leaf. */
    void ExpectBurnPassesGate()
    {
        AlignClock();
        const CMutableTransaction burn{SyntheticBurn()};
        const std::string admitted{Admit(burn)};
        Has("mempool admission", admitted, NO_BUSD);
        Lacks("mempool admission", admitted, NOT_READY);
        const std::string block{BlockWith(burn)};
        Has("block validation", block, NO_BUSD);
        Lacks("block validation", block, NOT_READY);
        std::string error;
        node::BridgeTxAuthorization authorization;
        const interfaces::BridgePrevalidationResult result{Prevalidate(burn, error, authorization)};
        BOOST_CHECK_MESSAGE(result == interfaces::BridgePrevalidationResult::VALID,
                            "wallet prevalidation for " << Next() << ": '" << error << "'");
        BOOST_CHECK_MESSAGE(authorization.decentralized_withdrawal.has_value(),
                            "wallet prevalidation for " << Next() << ": no withdrawal leaf");
        if (authorization.decentralized_withdrawal) {
            BOOST_CHECK_EQUAL(authorization.decentralized_withdrawal->withdrawal.withdrawal_id, 0U);
            BOOST_CHECK_EQUAL(authorization.decentralized_withdrawal->withdrawal.amount, BURN_AMOUNT);
        }
        bool carried{false};
        Has("block assembly", Assemble(burn, carried), "TestBlockValidity failed: " + NO_BUSD);
    }

    /** The burn is refused at the gate by mempool admission, block validation
     * and wallet prevalidation, and skipped by block assembly. */
    void ExpectBurnClosed()
    {
        AlignClock();
        const CMutableTransaction burn{SyntheticBurn()};
        Has("mempool admission", Admit(burn), NOT_READY + ", " + NOT_READY_ERROR);
        Has("block validation", BlockWith(burn), NOT_READY + ", " + NOT_READY_ERROR);
        std::string error;
        node::BridgeTxAuthorization authorization;
        const interfaces::BridgePrevalidationResult result{Prevalidate(burn, error, authorization)};
        BOOST_CHECK_MESSAGE(result == interfaces::BridgePrevalidationResult::REJECTED,
                            "wallet prevalidation for " << Next() << " did not refuse: '" << error << "'");
        Has("wallet prevalidation", error, NOT_READY_ERROR);
        BOOST_CHECK(!authorization.HasWithdrawal());
        bool carried{false};
        const std::string assembled{Assemble(burn, carried)};
        BOOST_CHECK_MESSAGE(assembled.empty(), "block assembly for " << Next() << ": '" << assembled << "'");
        BOOST_CHECK_MESSAGE(!carried, "block assembly for " << Next() << " carried the burn");
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_v1_gate_tests, BridgeV1GateFixture)

// The gate follows the V1 lineage projected for the burn's block: open from
// the first withdrawal block while the sets meet the pins, open still at the
// last block before the lineage breaks, shut at the break and for good.
BOOST_AUTO_TEST_CASE(v1_burn_gate_follows_the_projected_lineage)
{
    PrepareBridgeChain();
    Produce(m_vk_a); // M: the V1 bootstrap, Set_0 = Set_1 = {A, B, C, D}

    // M + 1, the first withdrawal block: the sets meet the pins exactly.
    {
        const node::FinalityTracker::State projected{ProjectedNext()};
        BOOST_REQUIRE(projected.bootstrapped && projected.current && projected.next);
        BOOST_CHECK_EQUAL(projected.epoch, 0U);
        BOOST_CHECK_EQUAL(projected.current->Size(), 4U);
        BOOST_CHECK_EQUAL(projected.current->TotalWeight(), 34U);
        BOOST_CHECK_EQUAL(projected.next->Epoch(), 1U);
        BOOST_CHECK(!projected.lineage_broken);
        BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(projected, Pins()));
    }
    ExpectBurnPassesGate();

    // M + 59, the last block of the extended epoch 0: still open.
    ProduceTo(m_break - 2, m_vk_a);
    BOOST_CHECK(!ProjectedNext().lineage_broken);
    BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(ProjectedNext(), Pins()));
    ExpectBurnPassesGate();

    // M + 60: no certificate was ever carried, so the lineage is broken and
    // the gate shut, although the frozen sets alone still meet the pins.
    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_break - 1);
    {
        const node::FinalityTracker::State projected{ProjectedNext()};
        BOOST_CHECK(projected.lineage_broken);
        BOOST_CHECK(!node::BridgeWithdrawalValidatorSetsReady(projected, Pins()));
        node::FinalityTracker::State intact{projected};
        intact.lineage_broken = false;
        BOOST_CHECK(node::BridgeWithdrawalValidatorSetsReady(intact, Pins()));
    }
    ExpectBurnClosed();

    // M + 90: V1 never recovers.
    ProduceTo(m_break + SCALED_E - 1, m_vk_a);
    BOOST_CHECK(ProjectedNext().lineage_broken);
    ExpectBurnClosed();
}

BOOST_AUTO_TEST_SUITE_END()

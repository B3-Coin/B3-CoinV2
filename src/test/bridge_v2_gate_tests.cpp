// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Bridge V2 node wiring on a real modern-PoS chain (stage S3, RT4a slice):
// the TEMPORARY V2 burn gate and the V2 receipt limits as mempool admission,
// block validation, block assembly and wallet bridge prevalidation apply
// them, not only as the pure predicates bridge_v2_tests and
// bridge_state_tests cover.
//
// FinalityChainFixture's chain (legacy prefix, corridor, four staked and
// bound validators A, B, C, D with weights 15, 1, 10, 8) gains a managed V1
// bUSD bridge from M + 1 whose V1 withdrawal height is also M + 1 (so the V1
// readiness alone would say READY for every burn), the fresh lineage from
// R_f = M + 11 and a regtest-shaped bridge V2 envelope from V = R_f + 1.
// The kind 6 burns here are synthetic: they spend a native funding output and
// burn bUSD V2 that does not exist, so they can never be valid. They reach
// the gate because every gate check runs before asset conservation, and the
// block-assembly case puts one straight into the mempool (as if admitted
// while the gate was READY) to show the assembler skips it.

#include <addresstype.h>
#include <bridge/proof.h>
#include <consensus/params.h>
#include <interfaces/chain.h>
#include <key.h>
#include <modern/asset_output.h>
#include <modern/bridge_binding.h>
#include <modern/bridge_v2.h>
#include <modern/mpa.h>
#include <node/bridge_state.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {

using b3test::FinalityChainFixture;

Consensus::BridgeEthAddress FillAddress(const uint8_t fill)
{
    Consensus::BridgeEthAddress out;
    out.fill(fill);
    return out;
}

//! A managed V1 bUSD bridge active from `activation` (bridge_state_tests'
//! BridgeParams() pins).
Consensus::BridgeAssetParams ManagedBusd(const int activation)
{
    Consensus::BridgeAssetParams busd;
    busd.asset = Consensus::ETHEREUM_MAINNET_BUSD_IDENTITY;
    busd.vault_runtime_code_hash = uint256{uint8_t{6}};
    busd.implementation_or_adapter = uint256{uint8_t{3}};
    busd.adapter_version = 1;
    busd.origin_deployment_block = 1;
    busd.recipient_encoding_version = Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1;
    busd.activation_height = activation;
    busd.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = 100,
        .max_per_epoch = 150,
        .epoch_length_blocks = 10,
    };
    Consensus::EthereumLightClientPins light;
    light.trusted_checkpoint_root = uint256{uint8_t{4}};
    light.trusted_checkpoint_slot = 32;
    light.genesis_validators_root = Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT;
    light.fork_schedule = {{0, {0, 0, 0, 0}}};
    light.fork_schedule_valid_through_epoch = 100;
    light.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    light.max_sync_lag_slots = 10;
    busd.light_client = light;
    busd.withdrawal_mode = Consensus::BridgeWithdrawalMode::MANAGED_V1;
    Consensus::BridgeManagedWithdrawalPins managed;
    managed.authority_address.fill(0x11);
    managed.vault_runtime_code_hash = *busd.vault_runtime_code_hash;
    managed.withdrawal_rules_version = Consensus::MANAGED_WITHDRAWAL_RULES_VERSION_V1;
    managed.withdrawal_rules_commitment = uint256{uint8_t{7}};
    busd.managed_withdrawal = managed;
    BOOST_REQUIRE(Consensus::BridgeMintParamsReady(busd));
    return busd;
}

//! A regtest-shaped (anvil 31337, OI-3 hashes unset) envelope from `v`.
Consensus::BridgeV2Params V2Envelope(const int v)
{
    Consensus::BridgeV2Params out;
    out.activation_height = v;
    out.burn_height = v;
    out.light_client.trusted_checkpoint_root = uint256{uint8_t{8}};
    out.light_client.trusted_checkpoint_slot = 8'192;
    out.light_client.genesis_validators_root = uint256{uint8_t{9}};
    out.light_client.fork_schedule = {{0, {0x00, 0x00, 0x00, 0xb3}}};
    out.light_client.fork_schedule_valid_through_epoch = 1'024;
    out.light_client.min_sync_committee_participants = Consensus::ETHEREUM_SYNC_COMMITTEE_SUPERMAJORITY;
    out.light_client.max_sync_lag_slots = 65'536;
    out.verifier.address = FillAddress(0x66);
    out.verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1;
    out.verifier.max_bridge_validators = Consensus::MAX_PROVEN_BRIDGE_VALIDATORS_V1;
    out.verifier.min_bridge_total_weight = 4;
    out.vault.origin_chain_id = 31'337;
    out.vault.vault_address = FillAddress(0x44);
    out.vault.token_address = FillAddress(0x55);
    out.vault.token_runtime_code_hash = uint256{uint8_t{10}};
    out.vault.origin_deployment_block = 3;
    out.vault.max_deposit_raw = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW;
    out.vault.mint_caps = Consensus::BridgeMintCaps{
        .max_per_block = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .max_per_epoch = Consensus::BRIDGE_V2_DEFAULT_CAP_RAW,
        .epoch_length_blocks = 1'440,
    };
    BOOST_REQUIRE(out.Valid());
    return out;
}

struct BridgeV2GateFixture : public FinalityChainFixture {
    int m_bridge_height{0};
    int m_v{0};

    //! The chain to V - 2 with every regime configured (see file comment).
    void Prepare()
    {
        PrepareFinalityChain(/*min_finality_set=*/1, /*reorg_horizon=*/200, /*with_unbound_c=*/false,
                             /*with_bound_c=*/true, /*with_bound_d=*/true);
        Consensus::Params& c{MutableConsensus()};
        m_bridge_height = m_M + 1;
        const int fresh_height{m_M + 11}; // GENESIS_HEIGHT M + 10 lies on the V1 grid
        m_v = fresh_height + 1;
        c.busd_bridge = ManagedBusd(m_bridge_height);
        c.bridge_withdrawal_activation_height = m_bridge_height;
        c.fresh_lineage = Consensus::FreshLineageParams{
            .activation_height = fresh_height, .max_epoch_extension = 30, .staging_signer = true};
        c.bridge_v2 = V2Envelope(m_v);
        BOOST_REQUIRE(c.BridgeV2ConfigurationValid());
        BOOST_REQUIRE(Consensus::BridgeRulesActive(m_bridge_height, c));
        BOOST_REQUIRE(Consensus::BridgeWithdrawalRulesActive(m_bridge_height, c));
        ProduceTo(m_v - 2, m_vk_a);
    }

    Consensus::BridgeV2Params& V2() { return *MutableConsensus().bridge_v2; }

    //! A native funding input (fund outputs 0-3, 6, 7, 10, 11 carry stakes
    //! and bindings) with a change output after `outputs`, `record` as MPA.
    CMutableTransaction BridgeTx(const unsigned fund_output, std::vector<CTxOut> outputs, const CMpaRecord& record)
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{m_fund_txid, fund_output};
        tx.vout = std::move(outputs);
        tx.vout.emplace_back(m_fund_value / 16 - 10'000, CScript() << OP_TRUE);
        tx.mpa = {record};
        return tx;
    }

    //! A kind 6 burn of 5 (non-existent) bUSD V2 naming the V2 vault entry.
    CMutableTransaction SyntheticBurn()
    {
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
        const auto entry{modern::ConfiguredBridgeV2EntryId(params)};
        BOOST_REQUIRE(asset && entry);
        bridge::BridgeBurnV1 burn;
        burn.registry_id = *entry;
        burn.burn_output_index = 0;
        burn.raw_amount = 5;
        burn.ethereum_recipient.fill(0x99);
        const auto record{bridge::MakeBridgeMpaRecord(
            bridge::BridgeRecordV1{bridge::BridgeRecordKindV1::BRIDGE_BURN, burn})};
        BOOST_REQUIRE(record);
        const auto burned{modern::MakeAssetBurnOutput(*asset, 5)};
        const auto binding{modern::MakeBridgeBindingOutput(*record)};
        BOOST_REQUIRE(burned && binding);
        return BridgeTx(8, {*burned, *binding}, *record);
    }

    //! A kind 3 mint naming the V2 vault entry whose last receipt-proof node
    //! is `terminal_size` bytes: over V1's 1,024, within V2's 8,192. The
    //! proof is junk; only the record limits matter before bridge state.
    CMutableTransaction LargeReceiptMint(const size_t terminal_size)
    {
        const Consensus::Params& params{m_node.chainman->GetConsensus()};
        const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
        const auto entry{modern::ConfiguredBridgeV2EntryId(params)};
        BOOST_REQUIRE(asset && entry);
        bridge::BridgeMintV1 mint;
        mint.registry_id = *entry;
        mint.output_index = 0;
        mint.finalized_anchor_hash = uint256{uint8_t{0x21}};
        mint.target_block_number = 10;
        mint.tx_index = 0;
        mint.receipt_log_index = 0;
        mint.ancestry_headers = {std::vector<unsigned char>(600, 0x22)};
        mint.mpt_nodes = {std::vector<unsigned char>(500, 0x23), std::vector<unsigned char>(terminal_size, 0x24)};
        const bridge::BridgeRecordV1 record{bridge::BridgeRecordKindV1::MINT, mint};
        BOOST_REQUIRE(!bridge::MakeBridgeMpaRecord(record)); // V1 limits refuse it
        const auto mpa{bridge::MakeBridgeMpaRecord(record, bridge::BRIDGE_MPT_LIMITS_V2)};
        BOOST_REQUIRE(mpa);
        CKey key;
        key.MakeNewKey(/*fCompressed=*/true);
        const auto owner{modern::MakeAssetOwnerOutput(
            *asset, 60, GetScriptForDestination(PKHash(key.GetPubKey())))};
        const auto binding{modern::MakeBridgeBindingOutput(*mpa)};
        BOOST_REQUIRE(owner && binding);
        return BridgeTx(9, {*owner, *binding}, *mpa);
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

    //! Block validity of a block on the tip carrying `tx`: "" or the state.
    std::string BlockWith(const CMutableTransaction& tx)
    {
        CBlock block{BuildPosBlock(m_vk_a, {}, {tx})};
        if (block.GetBlockTime() > GetTime()) SetMockTime(block.GetBlockTime());
        return Probe(block);
    }

    //! node::interfaces wallet prevalidation (bridgewithdraw, claimbridgedeposit).
    interfaces::BridgePrevalidationResult Prevalidate(const CMutableTransaction& tx, std::string& error,
                                                     node::BridgeTxAuthorization& authorization)
    {
        const CBlockIndex* tip{Tip()};
        return m_node.chain->prevalidateBridgeTransaction(CTransaction{tx}, tip->GetBlockHash(),
                                                          tip->nHeight + 1, authorization, error);
    }

    //! node::BlockAssembler for validator A with `tx` placed straight in the
    //! mempool. CreateNewBlock runs TestBlockValidity, so it throws if the
    //! assembler selects a burn the block rule refuses. Returns whether the
    //! template carries `tx`; the mempool is emptied again.
    bool TemplateCarries(const CMutableTransaction& mtx)
    {
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        CTxMemPool& pool{*m_node.mempool};
        TryAddToMempool(pool, TestMemPoolEntryHelper{}.Fee(10'000).FromTx(tx));
        BOOST_REQUIRE(WITH_LOCK(pool.cs, return pool.exists(tx->GetWitnessHash())));
        SetMockTime(std::max<int64_t>(GetTime(), Tip()->GetBlockTime() + 3'600));
        node::BlockAssembler::Options options;
        options.coinbase_output_script = CScript() << OP_TRUE;
        options.include_dummy_extranonce = true;
        options.modern_pos_validator_key = m_vk_a;
        BOOST_REQUIRE(options.test_block_validity);
        const auto assemble{[&] {
            return node::BlockAssembler(m_node.chainman->ActiveChainstate(), &pool, options).CreateNewBlock();
        }};
        decltype(assemble()) tmpl;
        BOOST_REQUIRE_NO_THROW(tmpl = assemble());
        BOOST_REQUIRE(tmpl);
        const bool carried{std::any_of(tmpl->block.vtx.begin(), tmpl->block.vtx.end(),
                                       [&](const CTransactionRef& in) {
                                           return in && in->GetHash() == tx->GetHash();
                                       })};
        WITH_LOCK(pool.cs, pool.removeRecursive(*tx, MemPoolRemovalReason::REPLACED));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(pool.cs, return pool.size()), 0U);
        return carried;
    }
};

bool Has(const std::string& text, const std::string& part)
{
    const bool found{text.find(part) != std::string::npos};
    BOOST_CHECK_MESSAGE(found, "'" << text << "' lacks '" << part << "'");
    return found;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_v2_gate_tests, BridgeV2GateFixture)

BOOST_AUTO_TEST_CASE(gate_and_receipt_limits_in_mempool_blocks_assembly_and_wallet)
{
    Prepare();
    const Consensus::Params& params{m_node.chainman->GetConsensus()};
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_v - 2);

    // -- V2 receipt limits (modern/mpa.h CheckTransactionMpa) ---------------
    // A V2 mint whose terminal receipt node needs the V2 limits is malformed
    // for the block at V - 1, both in the mempool and in a block...
    const CMutableTransaction mint{LargeReceiptMint(2'000)};
    {
        std::string error;
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{mint}, params, m_v - 1, error));
        BOOST_CHECK_EQUAL(error, "mpa-malformed-bridge-record");
        error.clear();
        BOOST_CHECK_MESSAGE(modern::CheckTransactionMpa(CTransaction{mint}, params, m_v, error), error);
    }
    Has(Admit(mint), "mpa-malformed-bridge-record");
    Has(BlockWith(mint), "mpa-malformed-bridge-record");

    Produce(m_vk_a);
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_v - 1);
    BOOST_REQUIRE(params.BridgeV2RulesActive(m_v) && params.BridgeV2BurnRulesActive(m_v));
    // ... and from V it decodes and reaches bridge state, which refuses it
    // for its own reason (no V2 light client), not as malformed.
    {
        const std::string admitted{Admit(mint)};
        Has(admitted, "bad-bridge-authorization");
        Has(admitted, "bridge mint precedes light-client bootstrap");
        BOOST_CHECK(admitted.find("mpa-malformed") == std::string::npos);
        const std::string block{BlockWith(mint)};
        Has(block, "bridge mint precedes light-client bootstrap");
        BOOST_CHECK(block.find("mpa-malformed") == std::string::npos);
    }

    const CMutableTransaction burn{SyntheticBurn()};

    // -- W2 above the next height: bridge-v2-burn-rules-inactive ------------
    V2().burn_height = m_v + 5;
    BOOST_REQUIRE(params.BridgeV2RulesActive(m_v) && !params.BridgeV2BurnRulesActive(m_v));
    Has(Admit(burn), "bridge-v2-burn-rules-inactive");
    Has(BlockWith(burn), "bridge-v2-burn-rules-inactive");
    {
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(burn, error, authorization) ==
                    interfaces::BridgePrevalidationResult::RULES_INACTIVE);
        Has(error, "Bridge V2 withdrawals are disabled");
    }
    BOOST_CHECK(!TemplateCarries(burn));
    V2().burn_height = m_v;

    // -- Committees outside the Verifier V2 bounds: bridge-v2-burn-not-ready.
    // The four-member fresh committees miss a five-member minimum. The V1
    // (managed) readiness would still be READY here, so block assembly and
    // wallet prevalidation must apply the V2 gate themselves.
    V2().verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1 + 1;
    BOOST_REQUIRE(V2().Valid());
    {
        const std::string admitted{Admit(burn)};
        Has(admitted, "bridge-v2-burn-not-ready");
        Has(admitted, "temporary gate");
        Has(admitted, "current fresh-lineage committee cannot authorize a Verifier V2 root");
        const std::string block{BlockWith(burn)};
        Has(block, "bridge-v2-burn-not-ready");
        Has(block, "current fresh-lineage committee cannot authorize a Verifier V2 root");
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK(Prevalidate(burn, error, authorization) == interfaces::BridgePrevalidationResult::REJECTED);
        Has(error, "temporary gate");
        BOOST_CHECK(!authorization.withdrawal_v2);
    }
    // A burn admitted while the gate was READY and still in the mempool is
    // skipped by the assembler, which still produces a valid block.
    BOOST_CHECK(!TemplateCarries(burn));
    // Weight below the verifier's minimum is NOT_READY too.
    V2().verifier.min_bridge_validators = Consensus::MIN_BRIDGE_VALIDATORS_V1;
    V2().verifier.min_bridge_total_weight = 15 + 1 + 10 + 8 + 1;
    Has(Admit(burn), "bridge-v2-burn-not-ready");
    BOOST_CHECK(!TemplateCarries(burn));
    V2().verifier.min_bridge_total_weight = 4;

    // -- READY: the projected fresh lineage (4 members, weight 34) passes the
    // gate everywhere; the synthetic burn then fails asset conservation.
    {
        std::string reason;
        LOCK(cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        node::BridgeStateTracker& bridge{chainstate.ModernBridgeState()};
        BOOST_REQUIRE(bridge.Sync(chainstate.m_chain, chainstate.m_blockman, params, *chainstate.m_chain.Tip()));
        BOOST_CHECK(node::ProjectedBridgeV2BurnReadiness(chainstate.ModernFinality(), chainstate.m_chain,
                                                         chainstate.m_blockman, params, *chainstate.m_chain.Tip(),
                                                         bridge.Index(), reason) ==
                    node::BridgeBurnReadiness::READY);
        BOOST_CHECK_MESSAGE(reason.empty(), reason);
    }
    {
        const std::string admitted{Admit(burn)};
        Has(admitted, "bad-asset-transition");
        BOOST_CHECK(admitted.find("bridge-v2-burn") == std::string::npos);
        const std::string block{BlockWith(burn)};
        BOOST_CHECK(!block.empty());
        BOOST_CHECK(block.find("bridge-v2-burn") == std::string::npos);
        // Wallet prevalidation stops at bridge state (asset selection is the
        // wallet's): the consensus-derived leaf V2 for the next height.
        std::string error;
        node::BridgeTxAuthorization authorization;
        BOOST_CHECK_MESSAGE(Prevalidate(burn, error, authorization) == interfaces::BridgePrevalidationResult::VALID,
                            error);
        BOOST_REQUIRE(authorization.withdrawal_v2);
        BOOST_CHECK_EQUAL(authorization.withdrawal_v2->withdrawal.withdrawal_id, 0U);
        BOOST_CHECK_EQUAL(authorization.withdrawal_v2->withdrawal.b3_height, static_cast<uint64_t>(m_v));
        BOOST_CHECK_EQUAL(authorization.withdrawal_v2->withdrawal.amount, 5);
    }
}

BOOST_AUTO_TEST_SUITE_END()

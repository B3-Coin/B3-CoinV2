// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 3 of the Modern PoS V1 finality plan: metadata cells (policy
// types 6/7/8/9 carriers) — recognition grammar, zero-value rule, exclusion
// from the spendable UTXO set, exact ConnectBlock/DisconnectBlock symmetry,
// no OP_RETURN semantics, no activation, legacy era untouched.

#include <chainparams.h>
#include <coins.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <kernel/chainparams.h>
#include <modern/bridge_binding.h>
#include <modern/metadata_cell.h>
#include <modern/policy.h>
#include <modern/stake.h>
#include <node/utxo_commitment.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/finality_fixture.h>
#include <test/util/modern_pos_setup.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace b3test;

namespace {

const uint256 COMMIT{uint256{"0707070707070707070707070707070707070707070707070707070707070707"}};

CScript Cell(const uint16_t type, const uint16_t version, const size_t params_len)
{
    const std::vector<unsigned char> params(params_len, 0x5A);
    const auto script{modern::MakeMetadataCellScript(type, version, COMMIT, params)};
    BOOST_REQUIRE(script.has_value());
    return *script;
}

CMpaRecord TestBridgeRecord()
{
    CMpaRecord record;
    record.payload_type = modern::CREATION_ACTION_BRIDGE;
    record.payload_version = modern::POLICY_VERSION_V1;
    record.payload = {0x01, 0x02, 0x03};
    return record;
}

CTxOut BridgeBinding()
{
    const auto output{modern::MakeBridgeBindingOutput(TestBridgeRecord())};
    BOOST_REQUIRE(output);
    return *output;
}

bool ScriptSpendable(const CScript& spk)
{
    ScriptError err;
    return VerifyScript(CScript{}, spk, nullptr, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_TAPROOT,
                        BaseSignatureChecker{}, &err);
}

} // namespace

BOOST_AUTO_TEST_SUITE(metadata_cell_tests)

BOOST_AUTO_TEST_CASE(carrier_grammar_round_trip_and_claims)
{
    for (const uint16_t type : {6, 7, 8}) {
        for (const size_t plen : {0u, 16u, 52u, 80u}) {
            const CScript spk{Cell(type, 1, plen)};
            BOOST_CHECK(modern::ClaimsMetadataCell(spk));
            BOOST_CHECK(modern::IsMetadataCell(spk));
            const auto cell{modern::ParseMetadataCell(spk)};
            BOOST_REQUIRE(cell.has_value());
            BOOST_CHECK_EQUAL(cell->policy_type, type);
            BOOST_CHECK_EQUAL(cell->policy_version, 1);
            BOOST_CHECK(cell->commitment == COMMIT);
            BOOST_CHECK_EQUAL(cell->params.size(), plen);
            // Not OP_RETURN semantics, yet unspendable by evaluation.
            BOOST_CHECK(!spk.IsUnspendable());
            BOOST_CHECK(!ScriptSpendable(spk));
        }
    }
    const CMpaRecord bridge_record{TestBridgeRecord()};
    const auto bridge_commitment{
        modern::BridgeRecordCommitmentV1(bridge_record)};
    const CTxOut bridge_binding{BridgeBinding()};
    const auto bridge_cell{
        modern::ParseMetadataCell(bridge_binding.scriptPubKey)};
    BOOST_REQUIRE(bridge_commitment);
    BOOST_REQUIRE(bridge_cell);
    BOOST_CHECK_EQUAL(bridge_binding.nValue, 0);
    BOOST_CHECK_EQUAL(
        bridge_cell->policy_type,
        static_cast<uint16_t>(modern::PolicyType::BRIDGE_RECORD));
    BOOST_CHECK_EQUAL(bridge_cell->policy_version, modern::POLICY_VERSION_V1);
    BOOST_CHECK(bridge_cell->commitment == *bridge_commitment);
    BOOST_CHECK(bridge_cell->params.empty());
    BOOST_CHECK(!bridge_binding.scriptPubKey.IsUnspendable());
    BOOST_CHECK(!ScriptSpendable(bridge_binding.scriptPubKey));

    // params above the permanent bound cannot even be built
    BOOST_CHECK(!modern::MakeMetadataCellScript(7, 1, COMMIT, std::vector<unsigned char>(81, 0)).has_value());
    // Layout: PUSH(payload) OP_DROP OP_FALSE; payload header 40 B
    const CScript spk{Cell(7, 1, 52)};
    BOOST_CHECK_EQUAL(spk.size(), 2u + 92u + 2u); // PUSHDATA1 + len + 92 payload + OP_DROP + OP_0
    const CScript small{Cell(8, 1, 0)};
    BOOST_CHECK_EQUAL(small.size(), 1u + 40u + 2u); // direct push opcode
}

BOOST_AUTO_TEST_CASE(claims_that_are_malformed_are_claims_but_not_cells)
{
    const std::vector<unsigned char> params(16, 0x11);
    std::vector<unsigned char> payload;
    payload.insert(payload.end(), modern::METADATA_CELL_MAGIC.begin(), modern::METADATA_CELL_MAGIC.end());
    payload.push_back(0x00); payload.push_back(0x07); // type 7
    payload.push_back(0x00); payload.push_back(0x01); // v1
    payload.insert(payload.end(), COMMIT.begin(), COMMIT.end());
    payload.insert(payload.end(), params.begin(), params.end());
    const CScript good{CScript() << payload << OP_DROP << OP_FALSE};
    BOOST_CHECK(modern::IsMetadataCell(good));

    // non-minimal push (PUSHDATA1 for a 56-byte payload that fits a direct push)
    CScript nonmin;
    nonmin.push_back(OP_PUSHDATA1);
    nonmin.push_back(static_cast<unsigned char>(payload.size()));
    nonmin.insert(nonmin.end(), payload.begin(), payload.end());
    nonmin << OP_DROP << OP_FALSE;
    BOOST_CHECK(modern::ClaimsMetadataCell(nonmin));
    BOOST_CHECK(!modern::IsMetadataCell(nonmin));
    // PUSHDATA2 likewise
    CScript nonmin2;
    nonmin2.push_back(OP_PUSHDATA2);
    nonmin2.push_back(static_cast<unsigned char>(payload.size()));
    nonmin2.push_back(0x00);
    nonmin2.insert(nonmin2.end(), payload.begin(), payload.end());
    nonmin2 << OP_DROP << OP_FALSE;
    BOOST_CHECK(modern::ClaimsMetadataCell(nonmin2) && !modern::IsMetadataCell(nonmin2));
    // wrong / missing suffix, trailing ops
    BOOST_CHECK(modern::ClaimsMetadataCell(CScript() << payload) && !modern::IsMetadataCell(CScript() << payload));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << payload << OP_DROP));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << payload << OP_DROP << OP_TRUE));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << payload << OP_FALSE));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << payload << OP_DROP << OP_FALSE << OP_NOP));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << payload << OP_DROP << OP_FALSE << OP_FALSE));
    // too short (header only minus one) / too long (81 params)
    std::vector<unsigned char> shortp(payload.begin(), payload.begin() + 39);
    BOOST_CHECK(modern::ClaimsMetadataCell(CScript() << shortp << OP_DROP << OP_FALSE));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << shortp << OP_DROP << OP_FALSE));
    std::vector<unsigned char> longp(payload.begin(), payload.begin() + 40);
    longp.insert(longp.end(), 81, 0x22);
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << longp << OP_DROP << OP_FALSE));
    // Non-metadata policy types with the magic (5 = FN, 10 = unknown):
    // claim, but not a cell. Type 9 is the reserved bridge binding cell.
    std::vector<unsigned char> fnp{payload};
    fnp[5] = 0x05;
    BOOST_CHECK(modern::ClaimsMetadataCell(CScript() << fnp << OP_DROP << OP_FALSE));
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << fnp << OP_DROP << OP_FALSE));
    std::vector<unsigned char> unk{payload};
    unk[5] = 0x0a;
    BOOST_CHECK(!modern::IsMetadataCell(CScript() << unk << OP_DROP << OP_FALSE));
}

BOOST_AUTO_TEST_CASE(ordinary_outputs_are_never_metadata)
{
    std::vector<unsigned char> vk(32, 0x33);
    std::array<unsigned char, 32> vkey; vkey.fill(0x33);
    const std::vector<CScript> ordinary{
        CScript() << OP_TRUE,
        CScript() << OP_FALSE,
        CScript() << OP_DROP << OP_FALSE,
        CScript() << std::vector<unsigned char>(40, 0x00) << OP_DROP << OP_FALSE,  // no magic
        CScript() << std::vector<unsigned char>{'B', '3', 'M'} << OP_DROP << OP_FALSE, // partial magic
        CScript() << std::vector<unsigned char>{'B', '3', 'S', '1'} << OP_DROP << OP_FALSE, // STAKE-like magic, wrong shape
        modern::MakeStakeScript(vkey, CScript() << OP_TRUE), // the STAKE carrier
        CScript() << OP_RETURN << std::vector<unsigned char>{'B', '3', 'M', 'C'}, // OP_RETURN data, not a cell
        CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0x44) << OP_EQUALVERIFY << OP_CHECKSIG,
        CScript{},
    };
    for (const auto& spk : ordinary) {
        BOOST_CHECK(!modern::ClaimsMetadataCell(spk));
        BOOST_CHECK(!modern::IsMetadataCell(spk));
    }
    // Zero-value ordinary outputs are still added as coins (exclusion is by
    // recognition, never by value).
    CCoinsView base;
    CCoinsViewCache view{&base};
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"1111111111111111111111111111111111111111111111111111111111111111"}), 0};
    mtx.vout.emplace_back(0, CScript() << OP_TRUE);
    mtx.vout.emplace_back(0, Cell(7, 1, 52));
    mtx.vout.push_back(BridgeBinding());
    mtx.vout.emplace_back(500, CScript() << OP_TRUE);
    const CTransaction tx{mtx};
    AddCoins(view, tx, 100, /*check=*/false, /*nTxOffset=*/0, /*exclude_modern_cells=*/true);
    BOOST_CHECK(view.HaveCoin(COutPoint{tx.GetHash(), 0}));  // zero-value ordinary: present
    BOOST_CHECK(!view.HaveCoin(COutPoint{tx.GetHash(), 1})); // metadata cell: never a coin
    BOOST_CHECK(!view.HaveCoin(COutPoint{tx.GetHash(), 2})); // bridge binding: never a coin
    BOOST_CHECK(view.HaveCoin(COutPoint{tx.GetHash(), 3}));
    // Legacy-era semantics (flag false): the same script IS an ordinary coin.
    CCoinsView base2;
    CCoinsViewCache view2{&base2};
    AddCoins(view2, tx, 100, false, 0, /*exclude_modern_cells=*/false);
    BOOST_CHECK(view2.HaveCoin(COutPoint{tx.GetHash(), 1}));
    BOOST_CHECK(view2.HaveCoin(COutPoint{tx.GetHash(), 2}));
}

BOOST_AUTO_TEST_CASE(check_metadata_cell_outputs_rules)
{
    Consensus::Params production{};
    production.legacy_b3coin = true;
    Consensus::Params test_active{production};
    test_active.hard_fork_height = 100;
    test_active.legacy_final_hash = uint256::ONE;
    test_active.modern_pos = Consensus::ModernPosParams{};
    std::string err;

    auto tx_with = [](const CAmount value, const CScript& spk) {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vout.emplace_back(value, spk);
        mtx.vout.emplace_back(1'000, CScript() << OP_TRUE);
        return CTransaction{mtx};
    };
    // No claims: fine everywhere.
    BOOST_CHECK(modern::CheckMetadataCellOutputs(tx_with(1, CScript() << OP_TRUE), production, err));
    // Production: every well-formed claim is INVALID (inactive) — fail closed.
    for (const uint16_t t : {6, 7, 8}) {
        BOOST_CHECK(!modern::CheckMetadataCellOutputs(tx_with(0, Cell(t, 1, 16)), production, err));
        BOOST_CHECK(err.find("inactive") != std::string::npos);
    }
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(
        tx_with(0, BridgeBinding().scriptPubKey), production, err));
    BOOST_CHECK(err.find("inactive") != std::string::npos);
    // Test activation: zero-valued well-formed v1 cells pass the carrier layer.
    for (const uint16_t t : {6, 7, 8}) {
        BOOST_CHECK(modern::CheckMetadataCellOutputs(tx_with(0, Cell(t, 1, 16)), test_active, err));
    }
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(
        tx_with(0, BridgeBinding().scriptPubKey), test_active, err));
    BOOST_CHECK(err.find("inactive") != std::string::npos);

    CChainParams::RegTestOptions options;
    CChainParams::B3ModernRegTestOptions b3;
    b3.flowmesh_test = true;
    options.b3_modern = b3;
    const auto bridge_chain{CChainParams::RegTest(options)};
    const Consensus::Params& bridge_params{bridge_chain->GetConsensus()};
    BOOST_REQUIRE(bridge_params.busd_bridge);
    BOOST_REQUIRE(bridge_params.busd_bridge->activation_height);
    const int bridge_activation{*bridge_params.busd_bridge->activation_height};
    const CTransaction bridge_tx{
        tx_with(0, BridgeBinding().scriptPubKey)};
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(
        bridge_tx, bridge_params, bridge_activation - 1, err));
    BOOST_CHECK(err.find("inactive") != std::string::npos);
    BOOST_CHECK(modern::CheckMetadataCellOutputs(
        bridge_tx, bridge_params, bridge_activation, err));
    // amount != 0 is invalid even when active
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(tx_with(1, Cell(7, 1, 16)), test_active, err));
    BOOST_CHECK(err.find("zero-valued") != std::string::npos);
    // version 2 is not activated
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(tx_with(0, Cell(7, 2, 16)), test_active, err));
    // unknown / non-metadata type with the magic: malformed claim
    std::vector<unsigned char> payload(40, 0);
    std::copy(modern::METADATA_CELL_MAGIC.begin(), modern::METADATA_CELL_MAGIC.end(), payload.begin());
    payload[5] = 0x0a; payload[7] = 0x01;
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(tx_with(0, CScript() << payload << OP_DROP << OP_FALSE), test_active, err));
    BOOST_CHECK(err.find("malformed") != std::string::npos);
    payload[5] = 0x04; // STAKE number with the metadata magic: not a metadata type
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(tx_with(0, CScript() << payload << OP_DROP << OP_FALSE), test_active, err));
    // malformed push
    CScript nonmin;
    nonmin.push_back(OP_PUSHDATA1);
    nonmin.push_back(40);
    payload[5] = 0x07;
    nonmin.insert(nonmin.end(), payload.begin(), payload.end());
    nonmin << OP_DROP << OP_FALSE;
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(tx_with(0, nonmin), test_active, err));
    // The test flag never activates the model-layer policies (IsActivatedPolicy)
    BOOST_CHECK(!modern::IsActivatedPolicy(6, 1) &&
                !modern::IsActivatedPolicy(7, 1) &&
                !modern::IsActivatedPolicy(8, 1) &&
                !modern::IsActivatedPolicy(9, 1));
    // Real chainparams never use the test override. Mainnet's sealed H/X/PoS
    // pins now activate the production modern-object context; other ordinary
    // shipped networks remain fail closed.
    for (const auto& chain : {ChainType::MAIN, ChainType::TESTNET,
                              ChainType::TESTNET4, ChainType::SIGNET,
                              ChainType::REGTEST}) {
        const auto chain_params{CreateChainParams(ArgsManager{}, chain)};
        const Consensus::Params& params{
            chain_params->GetConsensus()};
        BOOST_CHECK(!params.test_only_asset_policies_active);
        BOOST_CHECK_EQUAL(Consensus::ModernObjectRulesActive(params),
                          chain == ChainType::MAIN);
    }
}

//! Block level: a modern-era (corridor) block carrying a metadata cell
//! connects with the test activation; the cell never becomes a coin; the
//! ordinary outputs do; disconnecting restores the exact previous UTXO set;
//! reconnecting reproduces the exact same state; without activation the
//! same block is refused.
BOOST_FIXTURE_TEST_CASE(connect_disconnect_symmetry_and_exclusion, BindingFixture)
{
    // Legacy prefix, funding outputs at H, corridor configured with X = tip
    // (BindingFixture::Prepare); the cell mechanics are exercised with the one
    // cell type that is valid in an ordinary transaction: a FINALITY_KEY
    // binding with its evidence record (FINALITY_CERT is coinbase-only,
    // MODERN_PAYLOAD_ROOT coinbase-only/MPA-bound). The test flags are
    // switched off again for step 1.
    Prepare();
    const auto saved_pos{MutableConsensus().modern_pos};
    MutableConsensus().modern_pos.reset();
    const Txid fund_txid{m_fund_txid};
    const bls::SecretKey k1{Bls(1)};
    const auto bind{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const CMutableTransaction spend{MakeTx(0, {bind.cell}, {bind.record})};
    const Txid spend_txid{CTransaction{spend}.GetHash()};

    // 1. Not activated: the claiming output makes the block invalid; tip holds.
    {
        // (different timestamp, so the later activated block is a distinct block)
        const CBlock refused{BuildCorridorWithRoot({spend}, /*time_delta=*/61)};
        BOOST_CHECK(!Submit(refused));
        BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    }
    // 2. Activation on (the X-pin configuration restored): snapshot the
    //    UTXO set, connect, inspect.
    MutableConsensus().modern_pos = saved_pos;
    const auto snapshot_before{[&] {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        return node::EnumerateUtxos(m_node.chainman->ActiveChainstate().CoinsDB());
    }()};
    const uint256 commit_before{node::UtxoSetCommitment(snapshot_before)};
    const CBlock cell_block{BuildCorridorWithRoot({spend})};
    BOOST_REQUIRE(Submit(cell_block));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H + 1);
    {
        LOCK(cs_main);
        CCoinsViewCache& view{m_node.chainman->ActiveChainstate().CoinsTip()};
        BOOST_CHECK(!view.HaveCoin(COutPoint{fund_txid, 0}));  // spent
        BOOST_CHECK(view.HaveCoin(COutPoint{spend_txid, 0}));  // ordinary output
        BOOST_CHECK(!view.HaveCoin(COutPoint{spend_txid, 1})); // metadata cell: never a coin
    }
    const auto snapshot_connected{[&] {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        return node::EnumerateUtxos(m_node.chainman->ActiveChainstate().CoinsDB());
    }()};
    const uint256 commit_connected{node::UtxoSetCommitment(snapshot_connected)};
    BOOST_CHECK(commit_connected != commit_before);
    for (const auto& e : snapshot_connected) {
        BOOST_CHECK(!(e.outpoint.hash == spend_txid && e.outpoint.n == 1));
    }
    // 3. Disconnect: exact previous state.
    {
        BlockValidationState state;
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(cell_block.GetHash()))};
        BOOST_REQUIRE(index != nullptr);
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, index));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);
    const auto snapshot_after{[&] {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        return node::EnumerateUtxos(m_node.chainman->ActiveChainstate().CoinsDB());
    }()};
    BOOST_CHECK(node::UtxoSetCommitment(snapshot_after) == commit_before);
    BOOST_CHECK(node::CompareUtxoSets(snapshot_before, snapshot_after).mismatches.empty());
    // 4. Reconsider: identical connected state again.
    {
        CBlockIndex* index{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(cell_block.GetHash()))};
        {
            // Mirror the reconsiderblock RPC: clearing failure flags is paired
            // with a best-header recalculation.
            LOCK(cs_main);
            m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(index);
            m_node.chainman->RecalculateBestHeader();
        }
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H + 1);
    const auto snapshot_again{[&] {
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ForceFlushStateToDisk();
        return node::EnumerateUtxos(m_node.chainman->ActiveChainstate().CoinsDB());
    }()};
    BOOST_CHECK(node::UtxoSetCommitment(snapshot_again) == commit_connected);
}

//! Legacy era: a metadata-shaped script in a legacy block is an ordinary
//! (unspendable-by-evaluation) coin, exactly as before — the exclusion and
//! the claim rule apply only from H+1.
BOOST_FIXTURE_TEST_CASE(legacy_era_unchanged, ModernPosSetup)
{
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    Txid coinbase1{};
    for (int height{1}; height <= SYN_H - 1; ++height) {
        const CBlock block{BuildLegacy(Tip(), {})};
        BOOST_REQUIRE(Submit(block));
        if (height == 1) coinbase1 = block.vtx[0]->GetHash();
    }
    CMutableTransaction tx;
    tx.version = 1;
    tx.nTime = static_cast<uint32_t>(Tip()->GetBlockTime() + 17);
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{coinbase1, 0};
    const CAmount v{legacy::GetProofOfWorkReward(0, 1, consensus)};
    tx.vout.emplace_back(v - 1, CScript() << OP_TRUE);
    tx.vout.emplace_back(0, Cell(7, 1, 52)); // metadata-shaped, in the LEGACY era
    const CBlock block_h{BuildLegacy(Tip(), {tx})};
    const Txid txid{block_h.vtx[1]->GetHash()};
    BOOST_REQUIRE(Submit(block_h)); // no claim rule below H+1
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);
    LOCK(cs_main);
    BOOST_CHECK(m_node.chainman->ActiveChainstate().CoinsTip().HaveCoin(COutPoint{txid, 1})); // added, as any legacy output
}

BOOST_AUTO_TEST_SUITE_END()

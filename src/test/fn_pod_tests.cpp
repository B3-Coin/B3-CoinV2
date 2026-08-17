// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Deterministic PodRecord derivation (doc/design/b3-fn-pod.md §8.2):
//! the single classifier, the persisted PodDB, and the sync path across
//! restart, reindex, trusted-replay-mode sync and legacy-era rollback.
//! Includes one chain-level derivation over a GENUINELY SIGNED P2PK
//! funding input, so the production path is proven for records that can
//! actually be claimed. No claim transactions, no minting, no claimed[]
//! state — derivation only.

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <dbwrapper.h>
#include <key.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/fn.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <node/fn_pod.h>
#include <node/utxo_equivalence_check.h>
#include <node/kernel_notifications.h>
#include <primitives/block.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <streams.h>
#include <txdb.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

using node::ClassifyPod;
using node::IsSupportedFundingScript;
using node::PodClaimability;
using node::PodDB;
using node::PodRecord;

namespace {

constexpr CAmount COIN_B3{1'000'000};
constexpr CAmount TEST_COLLATERAL{100 * COIN_B3};
constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr uint32_t EASY_BITS{0x207fffff};

CBlock MakePodGenesis()
{
    CMutableTransaction coinbase;
    coinbase.version = 1;
    coinbase.nTime = GENESIS_TIME;
    coinbase.m_legacy_encoding = true;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << CScriptNum{0} << CScriptNum{7};
    coinbase.vout.emplace_back(0, CScript{});
    CBlock genesis;
    genesis.nVersion = 1;
    genesis.hashPrevBlock.SetNull();
    genesis.nTime = GENESIS_TIME;
    genesis.nBits = EASY_BITS;
    genesis.nNonce = 0;
    genesis.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

//! Disk-backed legacy-B3 regtest with the test PoD collateral override.
struct PodTestSetup : public ChainTestingSetup {
    PodTestSetup()
        : ChainTestingSetup{ChainType::REGTEST,
                            {.extra_args = {"-acceptnonstdtxn=1"},
                             .coins_db_in_memory = false,
                             .block_tree_db_in_memory = false}}
    {
        SetMockTime(GENESIS_TIME + 1000);
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = 1000;
        consensus.legacy_fn_collateral_test_override = TEST_COLLATERAL;
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = std::numeric_limits<int>::max();
        consensus.CSVHeight = std::numeric_limits<int>::max();
        consensus.SegwitHeight = std::numeric_limits<int>::max();
        CBlock& genesis{const_cast<CBlock&>(m_node.chainman->GetParams().GenesisBlock())};
        genesis = MakePodGenesis();
        consensus.hashGenesisBlock = genesis.GetLegacyB3Hash();
        LoadVerifyActivateChainstate();
    }
};

CTxOut Prev(const CAmount value, const CScript& script) { return CTxOut{value, script}; }

CScript P2pkScript(const CPubKey& pubkey)
{
    return CScript() << std::vector<unsigned char>(pubkey.begin(), pubkey.end()) << OP_CHECKSIG;
}

CScript P2pkhScript(const CPubKey& pubkey)
{
    return CScript() << OP_DUP << OP_HASH160 << ToByteVector(pubkey.GetID())
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

//! The smallest test-only legacy signing helper: a fresh SIGHASH_ALL
//! signature over the legacy-aware base sighash (interpreter.cpp commits
//! nTime for legacy-encoded transactions).
void SignLegacyInput(const CKey& key, const CScript& funding_script,
                     CMutableTransaction& tx, const unsigned int n_in, const bool p2pkh)
{
    const uint256 sighash{SignatureHash(funding_script, tx, n_in, SIGHASH_ALL,
                                        /*amount=*/0, SigVersion::BASE)};
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(key.Sign(sighash, sig));
    sig.push_back(static_cast<unsigned char>(SIGHASH_ALL));
    CScript script_sig;
    script_sig << sig;
    if (p2pkh) {
        const CPubKey pubkey{key.GetPubKey()};
        script_sig << std::vector<unsigned char>(pubkey.begin(), pubkey.end());
    }
    tx.vin[n_in].scriptSig = script_sig;
}

//! Raw-byte key/value writer for planting exact database corruption.
struct RawBytes {
    std::vector<unsigned char> bytes;
    template <typename S> void Serialize(S& s) const { s.write(MakeByteSpan(bytes)); }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(fn_pod_tests, BasicTestingSetup)

//! Pure-classifier fixtures: script support, dedupe, ordering, marker
//! metadata, claimability, and the qualification boundary — no chain.
BOOST_AUTO_TEST_CASE(classifier_fixtures)
{
    Consensus::Params params;
    params.legacy_b3coin = true;
    params.legacy_fn_collateral_test_override = TEST_COLLATERAL;
    const int height{50};

    CKey key_a;
    key_a.MakeNewKey(true);
    CKey key_b;
    key_b.MakeNewKey(false);
    const CScript p2pk_c{P2pkScript(key_a.GetPubKey())};   // compressed
    const CScript p2pk_u{P2pkScript(key_b.GetPubKey())};   // uncompressed
    const CScript p2pkh{P2pkhScript(key_a.GetPubKey())};
    const CScript op_true{CScript() << OP_TRUE};

    BOOST_CHECK(IsSupportedFundingScript({p2pk_c.begin(), p2pk_c.end()}));
    BOOST_CHECK(IsSupportedFundingScript({p2pk_u.begin(), p2pk_u.end()}));
    BOOST_CHECK(IsSupportedFundingScript({p2pkh.begin(), p2pkh.end()}));
    BOOST_CHECK(!IsSupportedFundingScript({op_true.begin(), op_true.end()}));

    const auto pod_tx{[&](const CAmount out_value) {
        CMutableTransaction tx;
        tx.version = 1;
        tx.nTime = GENESIS_TIME;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256::ONE), 0};
        tx.vout.emplace_back(out_value, CScript() << OP_TRUE);
        return tx;
    }};

    // One P2PKH funding script: qualifying and claimable.
    {
        CMutableTransaction tx{pod_tx(50 * COIN_B3)};
        const auto record{ClassifyPod(CTransaction{tx},
                                      {Prev(150 * COIN_B3 + 1000, p2pkh)}, height, params)};
        BOOST_REQUIRE(record);
        BOOST_CHECK_EQUAL(record->disintegrated, 100 * COIN_B3 + 1000);
        BOOST_CHECK_EQUAL(record->tier, TEST_COLLATERAL);
        BOOST_CHECK(record->claimable);
        BOOST_CHECK(record->reason == PodClaimability::SUPPORTED);
        BOOST_REQUIRE_EQUAL(record->funding_scripts.size(), 1U);
    }
    // One P2PK funding script (both key sizes).
    {
        CMutableTransaction tx{pod_tx(10 * COIN_B3)};
        const auto rec{ClassifyPod(CTransaction{tx}, {Prev(110 * COIN_B3, p2pk_c)}, height, params)};
        BOOST_REQUIRE(rec);
        BOOST_CHECK(rec->claimable);
        const auto rec_u{ClassifyPod(CTransaction{tx}, {Prev(110 * COIN_B3, p2pk_u)}, height, params)};
        BOOST_REQUIRE(rec_u);
        BOOST_CHECK(rec_u->claimable);
    }
    // Multiple inputs, same script: deduplicated to one distinct script.
    {
        CMutableTransaction tx{pod_tx(20 * COIN_B3)};
        tx.vin.resize(3);
        for (uint32_t i{0}; i < 3; ++i) {
            tx.vin[i].prevout = COutPoint{Txid::FromUint256(uint256::ONE), i};
        }
        const auto record{ClassifyPod(
            CTransaction{tx},
            {Prev(40 * COIN_B3, p2pkh), Prev(40 * COIN_B3, p2pkh), Prev(40 * COIN_B3, p2pkh)},
            height, params)};
        BOOST_REQUIRE(record);
        BOOST_CHECK(record->claimable);
        BOOST_CHECK_EQUAL(record->funding_scripts.size(), 1U);
    }
    // Multiple distinct supported scripts: all retained, canonically sorted.
    {
        CMutableTransaction tx{pod_tx(5 * COIN_B3)};
        tx.vin.resize(3);
        for (uint32_t i{0}; i < 3; ++i) {
            tx.vin[i].prevout = COutPoint{Txid::FromUint256(uint256::ONE), i};
        }
        const auto record{ClassifyPod(
            CTransaction{tx},
            {Prev(35 * COIN_B3, p2pk_u), Prev(35 * COIN_B3, p2pkh), Prev(35 * COIN_B3, p2pk_c)},
            height, params)};
        BOOST_REQUIRE(record);
        BOOST_CHECK(record->claimable);
        BOOST_REQUIRE_EQUAL(record->funding_scripts.size(), 3U);
        BOOST_CHECK(std::is_sorted(record->funding_scripts.begin(), record->funding_scripts.end()));
    }
    // Mixed supported + unsupported: the WHOLE PoD is unclaimable.
    {
        CMutableTransaction tx{pod_tx(5 * COIN_B3)};
        tx.vin.resize(2);
        tx.vin[1].prevout = COutPoint{Txid::FromUint256(uint256::ONE), 1};
        const auto record{ClassifyPod(CTransaction{tx},
                                      {Prev(60 * COIN_B3, p2pkh), Prev(50 * COIN_B3, op_true)},
                                      height, params)};
        BOOST_REQUIRE(record);
        BOOST_CHECK(!record->claimable);
        BOOST_CHECK(record->reason == PodClaimability::UNSUPPORTED_FUNDING_SCRIPT);
        BOOST_CHECK_EQUAL(record->funding_scripts.size(), 2U);
    }
    // Markers: missing, one matching, several, and non-1-B3 values — audit
    // metadata only; claimability identical in every case.
    {
        CMutableTransaction none{pod_tx(2 * COIN_B3)};
        CMutableTransaction one{pod_tx(2 * COIN_B3)};
        one.vout.emplace_back(COIN_B3, CScript() << OP_TRUE);
        CMutableTransaction many{pod_tx(2 * COIN_B3)};
        many.vout.emplace_back(COIN_B3, CScript() << OP_TRUE);
        many.vout.emplace_back(COIN_B3, CScript() << OP_2);
        CMutableTransaction odd{pod_tx(2 * COIN_B3)};
        odd.vout.emplace_back(COIN_B3 + 1, CScript() << OP_TRUE);
        const std::vector<CTxOut> prev{Prev(110 * COIN_B3, p2pkh)};
        const auto r_none{ClassifyPod(CTransaction{none}, prev, height, params)};
        const auto r_one{ClassifyPod(CTransaction{one}, prev, height, params)};
        const auto r_many{ClassifyPod(CTransaction{many}, prev, height, params)};
        const auto r_odd{ClassifyPod(CTransaction{odd}, prev, height, params)};
        BOOST_REQUIRE(r_none && r_one && r_many && r_odd);
        BOOST_CHECK_EQUAL(r_none->marker_vouts.size(), 0U);
        BOOST_CHECK_EQUAL(r_one->marker_vouts.size(), 1U);
        BOOST_CHECK_EQUAL(r_many->marker_vouts.size(), 2U);
        BOOST_CHECK_EQUAL(r_odd->marker_vouts.size(), 0U);
        for (const auto* r : {&*r_none, &*r_one, &*r_many, &*r_odd}) {
            BOOST_CHECK(r->claimable);
        }
    }
    // Qualification boundary and exclusions.
    {
        CMutableTransaction tx{pod_tx(10 * COIN_B3)};
        BOOST_CHECK(!ClassifyPod(CTransaction{tx},
                                 {Prev(110 * COIN_B3 - 1, p2pkh)}, height, params)); // gap short by 1
        BOOST_CHECK(ClassifyPod(CTransaction{tx},
                                {Prev(110 * COIN_B3, p2pkh)}, height, params)); // exactly the tier
        CMutableTransaction coinbase{pod_tx(10 * COIN_B3)};
        coinbase.vin[0].prevout.SetNull();
        BOOST_CHECK(!ClassifyPod(CTransaction{coinbase}, {Prev(0, CScript{})}, height, params));
    }
    // Serialization: stable bytes, equality round trip, version rejection.
    {
        CMutableTransaction tx{pod_tx(50 * COIN_B3)};
        const auto record{ClassifyPod(CTransaction{tx},
                                      {Prev(151 * COIN_B3, p2pkh)}, height, params)};
        BOOST_REQUIRE(record);
        DataStream a;
        a << *record;
        DataStream b;
        b << *record;
        BOOST_CHECK(std::ranges::equal(a, b));
        PodRecord decoded;
        a >> decoded;
        BOOST_CHECK(decoded == *record);
        DataStream bad;
        bad << int32_t{99};
        BOOST_CHECK_THROW(bad >> decoded, std::ios_base::failure);
    }
}

//! Chain-level derivation: a genuinely SIGNED P2PK-funded PoD (claimable),
//! an OP_TRUE-funded PoD (recorded, unclaimable), a BELOW-TIER spender at
//! exactly H (never recorded — the at-H qualifying case lives in the
//! corrective-sync test), no records past H, and identical persisted
//! records across restart,
//! chainstate reindex, trusted-replay-mode second-node sync, and
//! legacy-era rollback recovery.
BOOST_FIXTURE_TEST_CASE(chain_derivation_and_determinism, PodTestSetup)
{
    const auto cm{[&]() -> ChainstateManager& { return *m_node.chainman; }};
    const Consensus::Params& consensus{cm().GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};
    const auto tip{[&] { return WITH_LOCK(cs_main, return cm().ActiveChain().Tip()); }};

    const auto submit{[&](const CBlock& block) {
        const int prev_height{tip()->nHeight};
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        DataStream bytes;
        bytes << legacy::TX_LEGACY(block);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(cm().ProcessNewBlock(decoded, true, true, &new_block),
                              "block at height " << prev_height + 1 << " rejected");
        BOOST_REQUIRE_MESSAGE(tip()->nHeight == prev_height + 1,
                              "block at height " << prev_height + 1 << " accepted but not connected");
    }};
    const auto build_legacy{[&](std::vector<CMutableTransaction> txs) {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 360);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{9};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits = legacy::GetNextTargetRequired(prev, false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};

    CKey funding_key;
    funding_key.MakeNewKey(true);
    const CScript p2pk{P2pkScript(funding_key.GetPubKey())};

    // Heights 1..31: coinbases; 31 also fans coinbase 1 into a P2PK coin
    // (150 B3) and two OP_TRUE coins.
    Txid coinbase1{};
    std::vector<Txid> small_coinbases; // heights 2..11, for the post-H gap tx
    for (int height{1}; height <= 30; ++height) {
        CBlock block{build_legacy({})};
        submit(block);
        if (height == 1) coinbase1 = block.vtx[0]->GetHash();
        if (height >= 2 && height <= 11) small_coinbases.push_back(block.vtx[0]->GetHash());
    }
    CMutableTransaction fan;
    fan.version = 1;
    fan.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    fan.vin.resize(1);
    fan.vin[0].prevout = COutPoint{coinbase1, 0};
    fan.vin[0].scriptSig = CScript{};
    fan.vout.emplace_back(150 * COIN_B3, p2pk);
    fan.vout.emplace_back(150 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(110 * COIN_B3, CScript() << OP_TRUE);
    const Txid fan_txid{[&] {
        CBlock block{build_legacy({fan})};
        submit(block);
        return block.vtx[1]->GetHash();
    }()}; // height 31

    // Height 32: the SIGNED P2PK-funded PoD — a real signature over the
    // legacy sighash, verified by live legacy script validation at connect.
    CMutableTransaction pod_signed;
    pod_signed.version = 1;
    pod_signed.m_legacy_encoding = true; // the sighash preimage commits nTime
    pod_signed.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    pod_signed.vin.resize(1);
    pod_signed.vin[0].prevout = COutPoint{fan_txid, 0};
    pod_signed.vout.emplace_back(COIN_B3, CScript() << OP_TRUE); // marker (audit only)
    pod_signed.vout.emplace_back(150 * COIN_B3 - COIN_B3 - TEST_COLLATERAL - 1000,
                                 CScript() << OP_TRUE);
    SignLegacyInput(funding_key, p2pk, pod_signed, 0, /*p2pkh=*/false);
    const Txid pod_signed_id{[&] {
        CBlock block{build_legacy({pod_signed})};
        submit(block); // height 32
        return block.vtx[1]->GetHash();
    }()};

    // Height 33: the OP_TRUE-funded PoD — recorded, unclaimable.
    CMutableTransaction pod_optrue;
    pod_optrue.version = 1;
    pod_optrue.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    pod_optrue.vin.resize(1);
    pod_optrue.vin[0].prevout = COutPoint{fan_txid, 1};
    pod_optrue.vin[0].scriptSig = CScript{};
    pod_optrue.vout.emplace_back(150 * COIN_B3 - TEST_COLLATERAL - 1000, CScript() << OP_TRUE);
    const Txid pod_optrue_id{[&] {
        CBlock block{build_legacy({pod_optrue})};
        submit(block); // height 33
        return block.vtx[1]->GetHash();
    }()};

    // Heights 34..39 plain; height 40 = H carries a below-tier spender
    // (never recorded).
    while (tip()->nHeight < 39) submit(build_legacy({}));
    CMutableTransaction not_pod;
    not_pod.version = 1;
    not_pod.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    not_pod.vin.resize(1);
    not_pod.vin[0].prevout = COutPoint{fan_txid, 2};
    not_pod.vin[0].scriptSig = CScript{};
    not_pod.vout.emplace_back(110 * COIN_B3 - TEST_COLLATERAL + 1, CScript() << OP_TRUE);
    submit(build_legacy({not_pod})); // height 40 == H
    BOOST_REQUIRE_EQUAL(tip()->nHeight, 40);

    // ---- Derive pre-pin (grows with the chain), then pin H = 40 and prove
    // no record can exist past H.
    const fs::path pod_path{m_args.GetDataDirBase() / "fnpod"};
    std::string error;
    {
        PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
        const auto records{db.ReadAll()};
        // Three qualifying PoDs: the fan itself (its 260,000-B3 coinbase-1
        // input dwarfs its outputs; OP_TRUE-funded, unclaimable), the
        // SIGNED P2PK PoD, and the OP_TRUE PoD.
        BOOST_REQUIRE_EQUAL(records.size(), 3U);
        BOOST_CHECK(records[0].pod_id == fan_txid);
        BOOST_CHECK_EQUAL(records[0].height, 31);
        BOOST_CHECK(!records[0].claimable);
        BOOST_CHECK(records[1].pod_id == pod_signed_id);
        BOOST_CHECK_EQUAL(records[1].height, 32);
        BOOST_CHECK(records[1].claimable); // the signed P2PK case
        BOOST_REQUIRE_EQUAL(records[1].funding_scripts.size(), 1U);
        BOOST_CHECK(CScript(records[1].funding_scripts[0].begin(),
                            records[1].funding_scripts[0].end()) == p2pk);
        BOOST_CHECK_EQUAL(records[1].marker_vouts.size(), 1U);
        BOOST_CHECK(records[2].pod_id == pod_optrue_id);
        BOOST_CHECK(!records[2].claimable);
        BOOST_CHECK(records[2].reason == PodClaimability::UNSUPPORTED_FUNDING_SCRIPT);

        const auto report{node::BuildPodCapacityReport(records)};
        BOOST_CHECK_EQUAL(report.total_qualifying, 3U);
        BOOST_CHECK_EQUAL(report.claimable, 1U);
        BOOST_CHECK_EQUAL(report.max_distinct_funding_scripts, 1U);
        BOOST_CHECK_EQUAL(report.max_action_payload,
                          modern::WorstCaseFnClaimActionPayload(1));
        BOOST_CHECK_EQUAL(report.within_native_bound, 1U);
        BOOST_CHECK_EQUAL(report.exceeding_native_bound, 0U);
        BOOST_CHECK(report.fits_native_action);
    }

    mutable_consensus.hard_fork_height = 41;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = 4;
    mutable_consensus.transition_pow_bits = EASY_BITS;

    // A corridor block at H+1 spends a large legacy coin with a
    // collateral-sized gap: NEVER a PoD record (events after H are
    // impossible by construction).
    {
        const CBlockIndex* prev{tip()};
        CMutableTransaction gap_after_h;
        gap_after_h.version = 2;
        gap_after_h.vin.resize(small_coinbases.size());
        for (size_t i{0}; i < small_coinbases.size(); ++i) {
            gap_after_h.vin[i].prevout = COutPoint{small_coinbases[i], 0};
        }
        // 10 x 10 B3 in, zero out: gap == the test collateral exactly.
        gap_after_h.vout.emplace_back(0, CScript() << OP_TRUE);
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{prev->nHeight + 1} << CScriptNum{7};
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = EASY_BITS;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.vtx.push_back(MakeTransactionRef(std::move(gap_after_h)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        bool new_block{false};
        BOOST_REQUIRE(cm().ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block));
        BOOST_REQUIRE_EQUAL(tip()->nHeight, 41);
    }

    std::vector<PodRecord> baseline;
    DataStream baseline_bytes;
    {
        PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20}};
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
        baseline = db.ReadAll();
        BOOST_REQUIRE_EQUAL(baseline.size(), 3U); // nothing past H, ever
        baseline_bytes << baseline;
    }

    // ---- Restart: reload the persisted chain and PodDB; identical bytes.
    {
        LOCK(cs_main);
        cm().ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_make_chainman();
    LoadVerifyActivateChainstate();
    {
        PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20}};
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
        DataStream bytes;
        bytes << db.ReadAll();
        BOOST_CHECK(std::ranges::equal(bytes, baseline_bytes));
    }

    // ---- Chainstate reindex, then a from-scratch PodDB: identical bytes.
    m_node.chainman.reset();
    m_args.ForceSetArg("-reindex-chainstate", "1");
    m_make_chainman();
    LoadVerifyActivateChainstate();
    m_args.ForceSetArg("-reindex-chainstate", "0");
    {
        PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
        DataStream bytes;
        bytes << db.ReadAll();
        BOOST_CHECK(std::ranges::equal(bytes, baseline_bytes));
    }

    // ---- Trusted-replay-mode second node: sync the raw blocks with the
    // boundary pinned from genesis, then derive from ITS storage.
    {
        const fs::path b_dir{m_args.GetDataDirNet() / "nodeB"};
        fs::create_directories(b_dir / "blocks");
        ChainstateManager::Options b_chainman_opts{
            .chainparams = cm().GetParams(),
            .datadir = b_dir,
            .check_block_index = 1,
            .notifications = *m_node.notifications,
            .worker_threads_num = 0,
        };
        const node::BlockManager::Options b_blockman_opts{
            .chainparams = b_chainman_opts.chainparams,
            .blocks_dir = b_dir / "blocks",
            .notifications = b_chainman_opts.notifications,
            .block_tree_db_params = DBParams{
                .path = b_dir / "blocks" / "index",
                .cache_bytes = m_kernel_cache_sizes.block_tree_db,
                .memory_only = true,
            },
        };
        ChainstateManager chainman_b{*Assert(m_node.shutdown_signal), b_chainman_opts, b_blockman_opts};
        {
            node::ChainstateLoadOptions b_load;
            b_load.mempool = nullptr;
            b_load.coins_db_in_memory = true;
            const auto [status, load_error]{
                node::LoadChainstate(chainman_b, m_kernel_cache_sizes, b_load)};
            BOOST_REQUIRE_MESSAGE(status == node::ChainstateLoadStatus::SUCCESS, load_error.original);
            BlockValidationState state;
            BOOST_REQUIRE(chainman_b.ActiveChainstate().ActivateBestChain(state));
        }
        for (int height{1}; height <= 41; ++height) {
            const CBlockIndex* pindex{WITH_LOCK(cs_main, return cm().ActiveChain()[height])};
            CBlock block;
            BOOST_REQUIRE(cm().m_blockman.ReadBlock(block, *pindex));
            bool new_block{false};
            BOOST_REQUIRE_MESSAGE(
                chainman_b.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                "node B refused block at height " << height);
        }
        PodDB db{DBParams{.path = b_dir / "fnpod", .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(chainman_b, db, error), error);
        DataStream bytes;
        bytes << db.ReadAll();
        BOOST_CHECK(std::ranges::equal(bytes, baseline_bytes));
    }
}

//! Legacy-era rollback: a stale marker whose block leaves the active chain
//! is detected and the database re-derives deterministically.
BOOST_FIXTURE_TEST_CASE(rollback_recovery, PodTestSetup)
{
    const auto cm{[&]() -> ChainstateManager& { return *m_node.chainman; }};
    const Consensus::Params& consensus{cm().GetConsensus()};
    const auto tip{[&] { return WITH_LOCK(cs_main, return cm().ActiveChain().Tip()); }};

    const auto submit_plain{[&] {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 360);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{9};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits = legacy::GetNextTargetRequired(prev, false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        DataStream bytes;
        bytes << legacy::TX_LEGACY(block);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        bool new_block{false};
        BOOST_REQUIRE(cm().ProcessNewBlock(decoded, true, true, &new_block));
    }};

    for (int height{1}; height <= 10; ++height) submit_plain();

    const fs::path pod_path{m_args.GetDataDirBase() / "fnpod-rollback"};
    std::string error;
    PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
    BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
    BOOST_REQUIRE(db.ReadMarker());
    BOOST_CHECK_EQUAL(db.ReadMarker()->height, 10);

    // Invalidate height 9: the marker's block leaves the chain.
    {
        CBlockIndex* pindex{WITH_LOCK(
            cs_main, return cm().ActiveChain()[9])};
        BlockValidationState state;
        BOOST_REQUIRE(cm().ActiveChainstate().InvalidateBlock(state, pindex));
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, 8);
    BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
    BOOST_CHECK_EQUAL(db.ReadMarker()->height, 8);
    BOOST_CHECK_EQUAL(db.ReadMarker()->hash.GetHex(), tip()->GetBlockHash().GetHex());
    BOOST_CHECK(db.ReadAll().empty()); // no PoDs on this plain chain, before or after
}

//! Sync-level corrective + hardening behavior on a connected regtest
//! chain: undo-file corruption fails closed at the consistent prefix;
//! restoring recovers deterministically; the H/X anchor is enforced
//! before any mutation; a qualifying PoD EXACTLY AT the pinned H
//! survives the rewind while above-H records disappear; a qualifying
//! PoD processed before a later malformed transaction never partially
//! escapes; -podreport publication is transactional; and PodDB
//! corruption (marker or record keys) fails closed.
BOOST_FIXTURE_TEST_CASE(pinned_h_rewind_and_fail_closed_sync, PodTestSetup)
{
    const auto cm{[&]() -> ChainstateManager& { return *m_node.chainman; }};
    const Consensus::Params& consensus{cm().GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};
    const auto tip{[&] { return WITH_LOCK(cs_main, return cm().ActiveChain().Tip()); }};
    const auto chain_hash{[&](const int height) {
        return WITH_LOCK(cs_main, return cm().ActiveChain()[height]->GetBlockHash());
    }};

    const auto submit{[&](std::vector<CMutableTransaction> txs) {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 360);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{9};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits = legacy::GetNextTargetRequired(prev, false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& mtx : txs) {
            mtx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        block.nNonce = 0;
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        DataStream bytes;
        bytes << legacy::TX_LEGACY(block);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        bool new_block{false};
        BOOST_REQUIRE(cm().ProcessNewBlock(decoded, true, true, &new_block));
        BOOST_REQUIRE_EQUAL(tip()->nHeight, prev->nHeight + 1);
        return tip()->GetBlockHash();
    }};
    const auto spend{[&](const Txid& from, const uint32_t vout, const CAmount out_value) {
        CMutableTransaction tx;
        tx.version = 1;
        tx.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{from, vout};
        tx.vin[0].scriptSig = CScript{};
        tx.vout.emplace_back(out_value, CScript() << OP_TRUE);
        return tx;
    }};

    // Heights 1..30 plain; 31 fans coinbase 1; qualifying PoDs at 32, 33
    // (the eventual H), 34 and 35; block 35 additionally carries a plain
    // spender AFTER its PoD; plain to 36.
    for (int height{1}; height <= 30; ++height) submit({});
    const Txid coinbase1{[&] {
        CBlock block;
        BOOST_REQUIRE(cm().m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return cm().ActiveChain()[1])));
        return block.vtx[0]->GetHash();
    }()};
    const CAmount reward1{legacy::GetProofOfWorkReward(0, 1, consensus)};
    BOOST_REQUIRE_GT(reward1, 520 * COIN_B3);
    CMutableTransaction fan{spend(coinbase1, 0, 150 * COIN_B3)};
    fan.vout.emplace_back(130 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(110 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(120 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(5 * COIN_B3, CScript() << OP_TRUE);
    // Change keeps the fan's own gap at a plain 1000-unit fee so the fan
    // itself is NOT a PoD.
    fan.vout.emplace_back(reward1 - 515 * COIN_B3 - 1000, CScript() << OP_TRUE);
    submit({fan}); // height 31
    const Txid fan_txid{[&] {
        CBlock block;
        BOOST_REQUIRE(cm().m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return cm().ActiveChain()[31])));
        return block.vtx[1]->GetHash();
    }()};

    submit({spend(fan_txid, 0, 150 * COIN_B3 - TEST_COLLATERAL - 1000)}); // PoD @32
    submit({spend(fan_txid, 1, 130 * COIN_B3 - TEST_COLLATERAL - 1000)}); // PoD @33 == H
    submit({spend(fan_txid, 2, 110 * COIN_B3 - TEST_COLLATERAL - 1000)}); // PoD @34
    submit({spend(fan_txid, 3, 120 * COIN_B3 - TEST_COLLATERAL - 1000),
            spend(fan_txid, 4, 5 * COIN_B3 - 1000)}); // PoD then plain @35
    {
        // Height 36: a genuine TWO-INPUT plain spend (fan change + the
        // mature coinbase of height 2) for the summation fail-closed
        // tests below. Its 1000-unit fee keeps it far below the tier.
        const Txid coinbase2{[&] {
            CBlock block;
            BOOST_REQUIRE(cm().m_blockman.ReadBlock(
                block, *WITH_LOCK(cs_main, return cm().ActiveChain()[2])));
            return block.vtx[0]->GetHash();
        }()};
        const CAmount reward2{legacy::GetProofOfWorkReward(0, 2, consensus)};
        const CAmount change{reward1 - 515 * COIN_B3 - 1000};
        CMutableTransaction two_in{spend(fan_txid, 5, change + reward2 - 1000)};
        two_in.vin.resize(2);
        two_in.vin[1].prevout = COutPoint{coinbase2, 0};
        two_in.vin[1].scriptSig = CScript{};
        submit({two_in});
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, 36);
    WITH_LOCK(cs_main, cm().ActiveChainstate().ForceFlushStateToDisk());

    // ---- Undo-FILE corruption: sync fails closed at the first block
    // needing undo data (31), leaving the consistent prefix through 30.
    fs::path rev_path;
    for (const auto& entry : fs::directory_iterator{m_args.GetDataDirNet() / "blocks"}) {
        const std::string name{fs::PathToString(entry.path().filename())};
        if (name.starts_with("rev") && name.ends_with(".dat")) rev_path = entry.path();
    }
    BOOST_REQUIRE(!rev_path.empty());
    std::vector<char> saved_undo;
    {
        std::ifstream in{rev_path.std_path(), std::ios::binary};
        saved_undo.assign(std::istreambuf_iterator<char>{in}, {});
        BOOST_REQUIRE(!saved_undo.empty());
    }
    {
        std::ofstream out{rev_path.std_path(), std::ios::binary | std::ios::trunc};
        out.write(saved_undo.data(), 8);
    }

    const fs::path pod_path{m_args.GetDataDirBase() / "fnpod-corrective"};
    std::string error;
    {
        PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20, .wipe_data = true}};
        BOOST_CHECK(!node::SyncPodRecords(cm(), db, error));
        BOOST_CHECK(!error.empty());
        BOOST_REQUIRE(db.ReadMarker());
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 30);
        BOOST_CHECK(db.ReadAll().empty());

        // ---- Restore: deterministic recovery to the full local prefix.
        {
            std::ofstream out{rev_path.std_path(), std::ios::binary | std::ios::trunc};
            out.write(saved_undo.data(), static_cast<std::streamsize>(saved_undo.size()));
        }
        error.clear();
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 36);
        const auto recovered{db.ReadAll()};
        BOOST_REQUIRE_EQUAL(recovered.size(), 4U);
        BOOST_CHECK_EQUAL(recovered[0].height, 32);
        BOOST_CHECK_EQUAL(recovered[1].height, 33);
        BOOST_CHECK_EQUAL(recovered[2].height, 34);
        BOOST_CHECK_EQUAL(recovered[3].height, 35);

        PodDB fresh{DBParams{.path = m_args.GetDataDirBase() / "fnpod-fresh",
                             .cache_bytes = size_t{1} << 20,
                             .wipe_data = true}};
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), fresh, error), error);
        BOOST_CHECK(fresh.ReadAll() == recovered);

        // ---- A qualifying PoD processed BEFORE a later malformed
        // transaction never partially escapes: block 35's GENUINE undo,
        // with only the SECOND transaction's entry damaged.
        CBlock block35;
        CBlockUndo undo35;
        {
            LOCK(cs_main);
            const CBlockIndex* pindex{cm().ActiveChain()[35]};
            BOOST_REQUIRE(cm().m_blockman.ReadBlock(block35, *pindex));
            BOOST_REQUIRE(cm().m_blockman.ReadBlockUndo(undo35, *pindex));
        }
        std::vector<PodRecord> derived35;
        BOOST_REQUIRE_MESSAGE(
            node::DerivePodRecords(block35, undo35, 35, consensus, derived35, error), error);
        BOOST_REQUIRE_EQUAL(derived35.size(), 1U); // the PoD, not the plain spend
        {
            CBlockUndo damaged{undo35};
            BOOST_REQUIRE_EQUAL(damaged.vtxundo.size(), 2U);
            damaged.vtxundo[1].vprevout.clear(); // the LATER transaction
            std::vector<PodRecord> sentinel{derived35[0]};
            std::string later_error;
            BOOST_CHECK(!node::DerivePodRecords(block35, damaged, 35, consensus, sentinel,
                                                later_error));
            BOOST_CHECK(later_error.find("undo data mismatched") != std::string::npos);
            BOOST_REQUIRE_EQUAL(sentinel.size(), 1U); // COMPLETELY unchanged
            BOOST_CHECK(sentinel[0] == derived35[0]);
        }
        // Obviously-invalid Coins also fail closed: a spent/null coin and
        // an out-of-range amount in genuine undo shapes.
        {
            CBlockUndo spent_coin{undo35};
            spent_coin.vtxundo[0].vprevout[0] = Coin{};
            std::vector<PodRecord> untouched;
            BOOST_CHECK(!node::DerivePodRecords(block35, spent_coin, 35, consensus, untouched,
                                                error));
            BOOST_CHECK(error.find("spent/null coin") != std::string::npos);
            BOOST_CHECK(untouched.empty());
            CBlockUndo bad_amount{undo35};
            bad_amount.vtxundo[0].vprevout[0].out.nValue = MAX_MONEY + 1;
            BOOST_CHECK(!node::DerivePodRecords(block35, bad_amount, 35, consensus, untouched,
                                                error));
            BOOST_CHECK(error.find("out of range") != std::string::npos);
        }

        // ---- Transactional -podreport: records accumulate privately and
        // publish only after the complete anchored replay succeeds.
        {
            const auto read_block{[&](const int height) -> std::optional<CBlock> {
                CBlock block;
                const CBlockIndex* pindex{WITH_LOCK(cs_main, return cm().ActiveChain()[height])};
                if (!pindex || !cm().m_blockman.ReadBlock(block, *pindex)) return std::nullopt;
                return block;
            }};
            const auto read_block_failing_late{[&](const int height) -> std::optional<CBlock> {
                if (height == 36) return std::nullopt; // AFTER the PoDs at 32..35
                return read_block(height);
            }};
            {
                CCoinsViewDB scratch{DBParams{.path = "", .cache_bytes = size_t{1} << 22,
                                              .memory_only = true},
                                     {}};
                const node::ReplayEquivalenceResult late{node::VerifyReplayEquivalence(
                    consensus, cm().ActiveChainstate().CoinsDB(), read_block_failing_late,
                    scratch,
                    {.final_height = 36, .final_hash = chain_hash(36),
                     .derive_pod_report = true})};
                BOOST_CHECK(!late.errors.empty());
                BOOST_CHECK(late.pod_records.empty()); // no partial records
                BOOST_CHECK(!late.pod_report);         // no report
            }
            {
                CCoinsViewDB scratch{DBParams{.path = "", .cache_bytes = size_t{1} << 22,
                                              .memory_only = true},
                                     {}};
                const node::ReplayEquivalenceResult ok{node::VerifyReplayEquivalence(
                    consensus, cm().ActiveChainstate().CoinsDB(), read_block, scratch,
                    {.final_height = 36, .final_hash = chain_hash(36),
                     .derive_pod_report = true})};
                BOOST_REQUIRE_MESSAGE(ok.errors.empty(),
                                      (ok.errors.empty() ? std::string{}
                                                         : ok.errors.front()));
                BOOST_CHECK(ok.ok);
                BOOST_REQUIRE(ok.pod_report);
                BOOST_CHECK_EQUAL(ok.pod_records.size(), 4U);
                BOOST_CHECK_EQUAL(ok.pod_report->total_qualifying, 4U);
            }
            // Publication requires EQUIVALENCE too: a replay that
            // completes against a live view that does not match yields
            // no records and no report — the activation-gate report is
            // never presented as authoritative when U_port != U_replay.
            {
                CCoinsViewDB empty_live{DBParams{.path = "", .cache_bytes = size_t{1} << 22,
                                                 .memory_only = true},
                                        {}};
                {
                    CCoinsViewCache seed{&empty_live};
                    seed.SetBestBlock(chain_hash(36)); // right anchor, wrong coins
                    seed.Flush();
                }
                CCoinsViewDB scratch{DBParams{.path = "", .cache_bytes = size_t{1} << 22,
                                              .memory_only = true},
                                     {}};
                const node::ReplayEquivalenceResult mismatch{node::VerifyReplayEquivalence(
                    consensus, empty_live, read_block, scratch,
                    {.final_height = 36, .final_hash = chain_hash(36),
                     .derive_pod_report = true})};
                BOOST_CHECK(!mismatch.ok);                 // equivalence failed
                BOOST_CHECK(mismatch.pod_records.empty()); // no partial publication
                BOOST_CHECK(!mismatch.pod_report);
            }
        }

        // ---- Summation and coin validity on GENUINE two-input undo (block
        // 36): every failure leaves a seeded sentinel unchanged.
        {
            CBlock block36;
            CBlockUndo undo36;
            {
                LOCK(cs_main);
                const CBlockIndex* pindex{cm().ActiveChain()[36]};
                BOOST_REQUIRE(cm().m_blockman.ReadBlock(block36, *pindex));
                BOOST_REQUIRE(cm().m_blockman.ReadBlockUndo(undo36, *pindex));
            }
            BOOST_REQUIRE_EQUAL(undo36.vtxundo.size(), 1U);
            BOOST_REQUIRE_EQUAL(undo36.vtxundo[0].vprevout.size(), 2U);
            std::vector<PodRecord> clean;
            std::string e;
            BOOST_REQUIRE_MESSAGE(node::DerivePodRecords(block36, undo36, 36, consensus, clean, e),
                                  e);
            BOOST_CHECK(clean.empty()); // plain spend, not a PoD
            const PodRecord sentinel_record{[&] {
                std::vector<PodRecord> one_again;
                CBlock block32;
                CBlockUndo undo32;
                LOCK(cs_main);
                const CBlockIndex* pindex{cm().ActiveChain()[32]};
                BOOST_REQUIRE(cm().m_blockman.ReadBlock(block32, *pindex));
                BOOST_REQUIRE(cm().m_blockman.ReadBlockUndo(undo32, *pindex));
                BOOST_REQUIRE(node::DerivePodRecords(block32, undo32, 32, consensus, one_again, e));
                return one_again[0];
            }()};
            const auto expect_fail36{[&](CBlockUndo mutated, const std::string& needle) {
                std::vector<PodRecord> sentinel{sentinel_record};
                std::string fail_error;
                BOOST_CHECK(!node::DerivePodRecords(block36, mutated, 36, consensus, sentinel,
                                                    fail_error));
                BOOST_CHECK_MESSAGE(fail_error.find(needle) != std::string::npos, fail_error);
                BOOST_REQUIRE_EQUAL(sentinel.size(), 1U); // seeded sentinel untouched
                BOOST_CHECK(sentinel[0] == sentinel_record);
            }};
            {
                // nValue == -1 IS the null-coin encoding (indistinguishable
                // from spent), so the pure negative-amount case uses -2.
                CBlockUndo negative{undo36};
                negative.vtxundo[0].vprevout[0].out.nValue = -2;
                expect_fail36(std::move(negative), "out of range");
                CBlockUndo minus_one{undo36};
                minus_one.vtxundo[0].vprevout[0].out.nValue = -1;
                expect_fail36(std::move(minus_one), "spent/null");
            }
            {
                CBlockUndo null_coin{undo36};
                null_coin.vtxundo[0].vprevout[0] = Coin{};
                expect_fail36(std::move(null_coin), "spent/null");
            }
            {
                CBlockUndo above{undo36};
                above.vtxundo[0].vprevout[0].out.nValue = MAX_MONEY + 1;
                expect_fail36(std::move(above), "out of range");
            }
            {
                // Two INDIVIDUALLY valid coins whose combined value exceeds
                // MAX_MONEY: the PRE-ADD guard fires before the addition.
                CBlockUndo combined{undo36};
                combined.vtxundo[0].vprevout[0].out.nValue = MAX_MONEY - 5;
                combined.vtxundo[0].vprevout[1].out.nValue = 100;
                expect_fail36(std::move(combined), "sum out");
            }
        }

        // ---- Pin H = 33: the AT-H qualifying PoD survives the atomic
        // rewind; every above-H record disappears.
        mutable_consensus.hard_fork_height = 34;
        mutable_consensus.legacy_final_hash = chain_hash(33);
        mutable_consensus.transition_pow_length = 4;
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error), error);
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 33);
        BOOST_CHECK_EQUAL(db.ReadMarker()->hash.GetHex(), chain_hash(33).GetHex());
        const auto after_pin{db.ReadAll()};
        BOOST_REQUIRE_EQUAL(after_pin.size(), 2U);
        BOOST_CHECK_EQUAL(after_pin[0].height, 32);
        BOOST_CHECK_EQUAL(after_pin[1].height, 33); // AT H: survives
    }
    // ---- Restart after the rewind; idempotent re-sync.
    {
        PodDB db{DBParams{.path = pod_path, .cache_bytes = size_t{1} << 20,
                          .wipe_data = false}};
        BOOST_REQUIRE(db.ReadMarker());
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 33);
        BOOST_REQUIRE_EQUAL(db.ReadAll().size(), 2U);
        std::string error2;
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error2), error2);
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 33);
        BOOST_CHECK_EQUAL(db.ReadAll().size(), 2U);

        // ---- H/X anchor enforcement: every broken anchor fails closed
        // with the LOGICAL marker and records unchanged (asserted via
        // snapshots of both).
        const auto snapshot_marker{*db.ReadMarker()};
        const auto snapshot_records{db.ReadAll()};
        const auto expect_unchanged{[&](const std::string& why) {
            BOOST_REQUIRE_MESSAGE(db.ReadMarker(), why);
            BOOST_CHECK_EQUAL(db.ReadMarker()->height, snapshot_marker.height);
            BOOST_CHECK_EQUAL(db.ReadMarker()->hash.GetHex(), snapshot_marker.hash.GetHex());
            BOOST_CHECK(db.ReadAll() == snapshot_records);
        }};
        // Only H configured.
        mutable_consensus.legacy_final_hash = std::nullopt;
        BOOST_CHECK(!node::SyncPodRecords(cm(), db, error2));
        BOOST_CHECK(error2.find("configured together") != std::string::npos);
        expect_unchanged("H-only");
        // Only X configured.
        mutable_consensus.hard_fork_height = std::nullopt;
        mutable_consensus.legacy_final_hash = chain_hash(33);
        BOOST_CHECK(!node::SyncPodRecords(cm(), db, error2));
        BOOST_CHECK(error2.find("configured together") != std::string::npos);
        expect_unchanged("X-only");
        // Negative H.
        mutable_consensus.hard_fork_height = 0; // H = -1
        BOOST_CHECK(!node::SyncPodRecords(cm(), db, error2));
        BOOST_CHECK(error2.find("negative") != std::string::npos);
        expect_unchanged("negative-H");
        // H on the chain but X contradicted.
        mutable_consensus.hard_fork_height = 34;
        mutable_consensus.legacy_final_hash = uint256::ONE;
        BOOST_CHECK(!node::SyncPodRecords(cm(), db, error2));
        BOOST_CHECK(error2.find("does not carry X") != std::string::npos);
        expect_unchanged("wrong-X");
        // H beyond the tip: only the LOCAL PREFIX may be derived (this is
        // not an anchored/completed claim set) — records regrow to 36.
        mutable_consensus.hard_fork_height = 100; // H = 99 > tip 36
        mutable_consensus.legacy_final_hash = uint256::ONE; // unverifiable yet
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error2), error2);
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 36);
        BOOST_CHECK_EQUAL(db.ReadAll().size(), 4U);
        // Re-pin the real boundary: the rewind reproduces the H-prefix.
        mutable_consensus.hard_fork_height = 34;
        mutable_consensus.legacy_final_hash = chain_hash(33);
        BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, error2), error2);
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 33);
        BOOST_CHECK_EQUAL(db.ReadAll().size(), 2U);
    }

    // ---- PodDB corruption fails closed: a damaged marker is CORRUPT
    // (never "missing"), and damaged record keys abort rewinds and reads.
    {
        const fs::path corrupt_path{m_args.GetDataDirBase() / "fnpod-corrupt-marker"};
        {
            PodDB db{DBParams{.path = corrupt_path, .cache_bytes = size_t{1} << 20,
                              .wipe_data = true}};
            db.WriteHeight(3, chain_hash(3), {});
        }
        {
            CDBWrapper raw{DBParams{.path = corrupt_path, .cache_bytes = size_t{1} << 20}};
            raw.Write(uint8_t{'m'}, std::vector<unsigned char>{0x01}); // undecodable marker
        }
        PodDB db{DBParams{.path = corrupt_path, .cache_bytes = size_t{1} << 20}};
        PodDB::Marker marker;
        BOOST_CHECK(db.ReadMarkerChecked(marker) == PodDB::MarkerRead::CORRUPT);
        // Corruption is NEVER flattened into "missing": the convenience
        // reader throws instead of returning a false nullopt.
        BOOST_CHECK_THROW(db.ReadMarker(), std::runtime_error);
        std::string error3;
        BOOST_CHECK(!node::SyncPodRecords(cm(), db, error3));
        BOOST_CHECK(error3.find("undecodable") != std::string::npos);
    }
    {
        const fs::path corrupt_path{m_args.GetDataDirBase() / "fnpod-corrupt-key"};
        {
            PodDB db{DBParams{.path = corrupt_path, .cache_bytes = size_t{1} << 20,
                              .wipe_data = true}};
            std::vector<PodRecord> one;
            std::string derive_error;
            CBlock block32;
            CBlockUndo undo32;
            {
                LOCK(cs_main);
                const CBlockIndex* pindex{cm().ActiveChain()[32]};
                BOOST_REQUIRE(cm().m_blockman.ReadBlock(block32, *pindex));
                BOOST_REQUIRE(cm().m_blockman.ReadBlockUndo(undo32, *pindex));
            }
            BOOST_REQUIRE(node::DerivePodRecords(block32, undo32, 32, consensus, one,
                                                 derive_error));
            db.WriteHeight(32, chain_hash(32), one);
        }
        {
            CDBWrapper raw{DBParams{.path = corrupt_path, .cache_bytes = size_t{1} << 20}};
            // A truncated 'p'-prefixed key inside the record range.
            raw.Write(std::pair<uint8_t, uint8_t>{'p', 0xff},
                      std::vector<unsigned char>{0x00});
        }
        PodDB db{DBParams{.path = corrupt_path, .cache_bytes = size_t{1} << 20}};
        BOOST_CHECK_THROW(db.ReadAll(), std::runtime_error); // never a partial prefix
        std::string rewind_error;
        BOOST_CHECK(!db.RewindTo(0, chain_hash(0), rewind_error)); // abort pre-marker
        BOOST_CHECK(!rewind_error.empty());
        // The abort wrote nothing: the original marker is intact.
        BOOST_REQUIRE(db.ReadMarker());
        BOOST_CHECK_EQUAL(db.ReadMarker()->height, 32);
    }

    // ---- Strictly canonical PodDB decoding: raw-byte corruption of
    // every shape fails closed; a failed rewind preserves everything and
    // removing the injected key restores full function.
    {
        const auto raw_pod_key{[](const int height, const Txid& txid) {
            std::vector<unsigned char> b{'p'};
            for (int i{3}; i >= 0; --i) {
                b.push_back((static_cast<uint32_t>(height) >> (8 * i)) & 0xff);
            }
            const uint256 raw{txid.ToUint256()};
            b.insert(b.end(), raw.begin(), raw.end());
            return b;
        }};

        // Build one genuine record to plant.
        std::vector<PodRecord> one;
        std::string derive_error;
        {
            CBlock block32;
            CBlockUndo undo32;
            LOCK(cs_main);
            const CBlockIndex* pindex{cm().ActiveChain()[32]};
            BOOST_REQUIRE(cm().m_blockman.ReadBlock(block32, *pindex));
            BOOST_REQUIRE(cm().m_blockman.ReadBlockUndo(undo32, *pindex));
            BOOST_REQUIRE(node::DerivePodRecords(block32, undo32, 32, consensus, one,
                                                 derive_error));
            BOOST_REQUIRE_EQUAL(one.size(), 1U);
        }
        const auto fresh_db{[&](const std::string& name) {
            return DBParams{.path = m_args.GetDataDirBase() / fs::PathFromString(name),
                            .cache_bytes = size_t{1} << 20, .wipe_data = true};
        }};

        // (a) Populated database with the marker ERASED: records without
        // a marker are corruption — sync refuses to rebuild over them.
        {
            const auto params_db{fresh_db("fnpod-marker-erased")};
            {
                PodDB db{params_db};
                db.WriteHeight(32, chain_hash(32), one);
            }
            {
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Erase(uint8_t{'m'});
            }
            PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
            BOOST_CHECK(!db.ReadMarker()); // truly missing
            std::string e;
            BOOST_CHECK(!node::SyncPodRecords(cm(), db, e));
            BOOST_CHECK(e.find("records without a marker") != std::string::npos);
        }
        // (b) Truncated key sorting BEFORE the canonical seek position
        // (the raw one-byte 'p'): caught by the raw-prefix scan.
        // (c) Canonical key + trailing byte. (d) Marker + trailing byte.
        // (e) Record value + trailing byte. (f) Key/value identity
        // mismatch. Each fails ReadAll and blocks sync.
        {
            const auto plant{[&](const std::string& name,
                                 const std::vector<unsigned char>& key_bytes,
                                 const std::vector<unsigned char>& value_bytes) {
                const auto params_db{fresh_db(name)};
                {
                    PodDB db{params_db};
                    db.WriteHeight(32, chain_hash(32), one);
                }
                {
                    CDBWrapper raw{DBParams{.path = params_db.path,
                                            .cache_bytes = size_t{1} << 20}};
                    raw.Write(RawBytes{key_bytes}, RawBytes{value_bytes});
                }
                PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
                BOOST_CHECK_THROW(db.ReadAll(), std::runtime_error);
                // The planted corruption must fail the CHECKED SYNC PATH
                // too — a valid marker never bypasses record validation.
                std::string sync_error;
                BOOST_CHECK(!node::SyncPodRecords(cm(), db, sync_error));
                BOOST_CHECK(!sync_error.empty());
                return params_db.path;
            }};
            const auto record_bytes{[&](const PodRecord& record) {
                DataStream ss;
                ss << record;
                const auto span{MakeUCharSpan(ss)};
                return std::vector<unsigned char>{span.begin(), span.end()};
            }};
            plant("fnpod-key-short", {'p'}, {0x00});                       // (b)
            {
                auto key{raw_pod_key(32, one[0].pod_id)};
                key.push_back(0xab);
                plant("fnpod-key-trailing", key, record_bytes(one[0]));     // (c)
            }
            {
                auto value{record_bytes(one[0])};
                value.push_back(0xab);
                plant("fnpod-value-trailing", raw_pod_key(32, one[0].pod_id), value); // (e)
            }
            plant("fnpod-identity", raw_pod_key(7, one[0].pod_id),
                  record_bytes(one[0]));                                    // (f)
            // (d) Marker with a trailing byte: CORRUPT, and sync fails.
            {
                const auto params_db{fresh_db("fnpod-marker-trailing")};
                {
                    PodDB db{params_db};
                    db.WriteHeight(32, chain_hash(32), one);
                }
                {
                    DataStream ss;
                    ss << PodDB::Marker{.height = 32, .hash = chain_hash(32)};
                    const auto span{MakeUCharSpan(ss)};
                    std::vector<unsigned char> value{span.begin(), span.end()};
                    value.push_back(0xab);
                    CDBWrapper raw{DBParams{.path = params_db.path,
                                            .cache_bytes = size_t{1} << 20}};
                    raw.Write(RawBytes{std::vector<unsigned char>{'m'}}, RawBytes{value});
                }
                PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
                PodDB::Marker m;
                BOOST_CHECK(db.ReadMarkerChecked(m) == PodDB::MarkerRead::CORRUPT);
                BOOST_CHECK_THROW(db.ReadMarker(), std::runtime_error);
                std::string e;
                BOOST_CHECK(!node::SyncPodRecords(cm(), db, e));
                BOOST_CHECK(e.find("undecodable") != std::string::npos);
            }
        }
        // (d2) A key beginning with 'm' but carrying trailing bytes
        // ("m\0") is CORRUPT — never MISSING — for ReadMarkerChecked,
        // ReadMarker, SyncPodRecords and RewindTo alike.
        {
            const auto params_db{fresh_db("fnpod-marker-key-trailing")};
            {
                PodDB db{params_db};
                db.WriteHeight(32, chain_hash(32), one);
            }
            {
                DataStream ss;
                ss << PodDB::Marker{.height = 32, .hash = chain_hash(32)};
                const auto span{MakeUCharSpan(ss)};
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Erase(uint8_t{'m'});
                raw.Write(RawBytes{std::vector<unsigned char>{'m', 0x00}},
                          RawBytes{std::vector<unsigned char>{span.begin(), span.end()}});
            }
            PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
            PodDB::Marker m;
            BOOST_CHECK(db.ReadMarkerChecked(m) == PodDB::MarkerRead::CORRUPT);
            BOOST_CHECK_THROW(db.ReadMarker(), std::runtime_error);
            std::string e;
            BOOST_CHECK(!node::SyncPodRecords(cm(), db, e));
            BOOST_CHECK(e.find("undecodable") != std::string::npos);
            e.clear();
            BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
            BOOST_CHECK(e.find("undecodable") != std::string::npos);
        }
        // (d2b) COEXISTENCE: a malformed marker-prefixed key ("m\0")
        // alongside the VALID canonical marker is corruption for every
        // path; removing it restores normal operation with the
        // canonical marker and records unchanged.
        {
            const auto params_db{fresh_db("fnpod-marker-coexist")};
            {
                PodDB db{params_db};
                db.WriteHeight(32, chain_hash(32), one);
            }
            const std::vector<unsigned char> extra_key{'m', 0x00};
            {
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Write(RawBytes{extra_key}, RawBytes{{0x01}});
            }
            {
                PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
                PodDB::Marker m;
                BOOST_CHECK(db.ReadMarkerChecked(m) == PodDB::MarkerRead::CORRUPT);
                BOOST_CHECK_THROW(db.ReadMarker(), std::runtime_error);
                std::string e;
                BOOST_CHECK(!node::SyncPodRecords(cm(), db, e));
                BOOST_CHECK(e.find("undecodable") != std::string::npos);
                e.clear();
                BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
                BOOST_CHECK(e.find("undecodable") != std::string::npos);
            }
            {
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Erase(RawBytes{extra_key});
            }
            PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
            BOOST_REQUIRE(db.ReadMarker()); // canonical marker unchanged
            BOOST_CHECK_EQUAL(db.ReadMarker()->height, 32);
            BOOST_CHECK(db.ReadAll() == one); // records unchanged
            std::string e;
            BOOST_REQUIRE_MESSAGE(db.RewindTo(0, chain_hash(0), e), e); // normal again
            BOOST_CHECK(db.ReadAll().empty());
        }
        // (d3) Direct RewindTo on a marker-less database fails without
        // changing anything (its public contract).
        {
            const auto params_db{fresh_db("fnpod-rewind-no-marker")};
            {
                PodDB db{params_db};
                db.WriteHeight(32, chain_hash(32), one);
            }
            {
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Erase(uint8_t{'m'});
            }
            PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
            std::string e;
            BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
            BOOST_CHECK(e.find("no marker") != std::string::npos);
            BOOST_CHECK(db.ReadAll() == one); // records untouched
        }
        // (d4) Obfuscated PodDB round-trip and checked sync: the exact
        // typed value reads apply the normal deobfuscation.
        {
            const fs::path obf_path{m_args.GetDataDirBase() / "fnpod-obfuscated"};
            {
                PodDB db{DBParams{.path = obf_path, .cache_bytes = size_t{1} << 20,
                                  .wipe_data = true, .obfuscate = true}};
                std::string e;
                BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), db, e), e);
                BOOST_CHECK_EQUAL(db.ReadMarker()->height, 33);
                BOOST_CHECK_EQUAL(db.ReadAll().size(), 2U);
            }
            PodDB reopened{DBParams{.path = obf_path, .cache_bytes = size_t{1} << 20,
                                    .obfuscate = true}};
            BOOST_CHECK_EQUAL(reopened.ReadAll().size(), 2U);
            std::string e;
            BOOST_REQUIRE_MESSAGE(node::SyncPodRecords(cm(), reopened, e), e);
            BOOST_CHECK_EQUAL(reopened.ReadMarker()->height, 33);
        }

        // (h) RewindTo's complete public contract: version, height and
        // hash preconditions each fail closed with marker and records
        // preserved; the method never advances the marker.
        {
            const auto marker_bytes{[](const int32_t version, const int32_t height,
                                       const uint256& hash) {
                DataStream ss;
                ss << PodDB::Marker{.version = version, .height = height, .hash = hash};
                const auto span{MakeUCharSpan(ss)};
                return std::vector<unsigned char>{span.begin(), span.end()};
            }};
            const auto plant_marker{[&](const std::string& name,
                                        const std::vector<unsigned char>& value,
                                        const std::vector<PodRecord>& records,
                                        const int write_height) {
                const auto params_db{fresh_db(name)};
                {
                    PodDB db{params_db};
                    db.WriteHeight(write_height, chain_hash(write_height), records);
                }
                {
                    CDBWrapper raw{DBParams{.path = params_db.path,
                                            .cache_bytes = size_t{1} << 20}};
                    raw.Write(uint8_t{'m'}, RawBytes{value});
                }
                return params_db.path;
            }};
            std::string e;
            // Unknown marker version.
            {
                const auto path{plant_marker("fnpod-rw-version",
                                             marker_bytes(9, 5, chain_hash(5)), {}, 5)};
                PodDB db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
                BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
                BOOST_CHECK(e.find("unknown format version") != std::string::npos);
            }
            // Negative existing marker height.
            {
                const auto path{plant_marker("fnpod-rw-negmark",
                                             marker_bytes(1, -3, chain_hash(5)), {}, 5)};
                PodDB db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
                BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
                BOOST_CHECK(e.find("negative height") != std::string::npos);
            }
            // Negative target, upward "rewind", same-height/different-hash
            // — on one healthy database; then the valid same-height/
            // same-hash call succeeds.
            {
                const auto params_db{fresh_db("fnpod-rw-target")};
                PodDB db{params_db};
                db.WriteHeight(5, chain_hash(5), {});
                BOOST_CHECK(!db.RewindTo(-1, chain_hash(0), e));
                BOOST_CHECK(e.find("target height is negative") != std::string::npos);
                BOOST_CHECK(!db.RewindTo(9, chain_hash(9), e));
                BOOST_CHECK(e.find("never advances") != std::string::npos);
                BOOST_CHECK(!db.RewindTo(5, uint256::ONE, e));
                BOOST_CHECK(e.find("contradicts the marker") != std::string::npos);
                BOOST_CHECK_EQUAL(db.ReadMarker()->height, 5); // all preserved
                BOOST_REQUIRE_MESSAGE(db.RewindTo(5, chain_hash(5), e), e);
                BOOST_CHECK_EQUAL(db.ReadMarker()->height, 5);
            }
            // A VALID record above the existing marker height fails the
            // pre-batch scan; marker and records stay untouched.
            {
                const auto path{plant_marker("fnpod-rw-above",
                                             marker_bytes(1, 10, chain_hash(10)), one, 32)};
                PodDB db{DBParams{.path = path, .cache_bytes = size_t{1} << 20}};
                BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
                BOOST_CHECK(e.find("above the marker height") != std::string::npos);
                BOOST_CHECK_EQUAL(db.ReadMarker()->height, 10); // preserved
                BOOST_CHECK(db.ReadAll() == one);               // preserved
            }
        }

        // (g) A failed rewind preserves BOTH the original marker and the
        // original records; removing the injected corrupt key restores
        // full function deterministically.
        {
            const auto params_db{fresh_db("fnpod-rewind-preserve")};
            {
                PodDB db{params_db};
                db.WriteHeight(32, chain_hash(32), one);
            }
            const std::vector<unsigned char> bad_key{'p', 0x01};
            {
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Write(RawBytes{bad_key}, RawBytes{{0x00}});
            }
            {
                PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
                std::string e;
                BOOST_CHECK(!db.RewindTo(0, chain_hash(0), e));
                BOOST_CHECK(!e.empty());
                BOOST_REQUIRE(db.ReadMarker());
                BOOST_CHECK_EQUAL(db.ReadMarker()->height, 32); // marker preserved
            }
            {
                CDBWrapper raw{DBParams{.path = params_db.path,
                                        .cache_bytes = size_t{1} << 20}};
                raw.Erase(RawBytes{bad_key});
            }
            PodDB db{DBParams{.path = params_db.path, .cache_bytes = size_t{1} << 20}};
            BOOST_CHECK(db.ReadAll() == one); // records preserved
            std::string e;
            BOOST_REQUIRE_MESSAGE(db.RewindTo(0, chain_hash(0), e), e);
            BOOST_CHECK(db.ReadAll().empty());
            BOOST_CHECK_EQUAL(db.ReadMarker()->height, 0);
        }
    }

}

BOOST_AUTO_TEST_SUITE_END()

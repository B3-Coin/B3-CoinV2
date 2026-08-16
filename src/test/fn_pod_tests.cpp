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
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <node/fn_pod.h>
#include <node/kernel_notifications.h>
#include <primitives/block.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
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
//! an OP_TRUE-funded PoD (recorded, unclaimable), a mixed PoD at exactly H,
//! no records past H, and identical persisted records across restart,
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
        BOOST_CHECK(report.fits_b3fp_carrier);
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

BOOST_AUTO_TEST_SUITE_END()

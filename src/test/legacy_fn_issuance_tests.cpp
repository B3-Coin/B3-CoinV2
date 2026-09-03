// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Historical FN Genesis manifest construction. These tests deliberately
//! contain no holder claim, funding-key authorization, or proof carrier.

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/fn_genesis.h>
#include <node/blockstorage.h>
#include <node/fn_pod.h>
#include <node/legacy_fn_issuance.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr CAmount COIN_B3{1'000'000};
constexpr CAmount TEST_COLLATERAL{100 * COIN_B3};
constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr uint32_t EASY_BITS{0x207fffff};

CScript P2pkhScript(const uint8_t fill)
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

CMutableTransaction MakeLegacyTx(const uint32_t ntime, const uint8_t tag)
{
    CMutableTransaction tx;
    tx.version = 1;
    tx.nTime = ntime;
    tx.m_legacy_encoding = true;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{tag}), 0};
    return tx;
}

CBlock MakeOfflineBlock(std::vector<CMutableTransaction> txs)
{
    CBlock block;
    block.nVersion = 4;
    for (CMutableTransaction& tx : txs) {
        tx.m_legacy_encoding = true;
        block.vtx.push_back(MakeTransactionRef(std::move(tx)));
    }
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

struct ManifestFixture {
    Consensus::Params params;
    std::map<int, CBlock> blocks;
    Txid first{};
    Txid second{};
    Txid ignored{};
    uint256 final_hash{};

    ManifestFixture()
    {
        params.legacy_b3coin = true;
        params.hard_fork_height = 6; // H = 5; FN Genesis is H+1.
        params.hashGenesisBlock =
            uint256{"1111111111111111111111111111111111111111111111111111111111111111"};

        CMutableTransaction a{MakeLegacyTx(GENESIS_TIME + 4, 0x11)};
        a.vout.emplace_back(COIN_B3, CScript() << OP_TRUE); // decoy
        a.vout.emplace_back(COIN_B3, P2pkhScript(0x21));

        CMutableTransaction no_designation{MakeLegacyTx(GENESIS_TIME + 4, 0x22)};
        no_designation.vout.emplace_back(COIN_B3, CScript() << OP_TRUE);

        CMutableTransaction b{MakeLegacyTx(GENESIS_TIME + 4, 0x33)};
        b.vout.emplace_back(COIN_B3, P2pkhScript(0x43));

        blocks.emplace(4, MakeOfflineBlock({a, no_designation, b}));
        first = blocks.at(4).vtx[0]->GetHash();
        ignored = blocks.at(4).vtx[1]->GetHash();
        second = blocks.at(4).vtx[2]->GetHash();

        CMutableTransaction final_tx{MakeLegacyTx(GENESIS_TIME + 5, 0x55)};
        final_tx.vout.emplace_back(0, CScript() << OP_TRUE);
        blocks.emplace(5, MakeOfflineBlock({final_tx}));
        final_hash = blocks.at(5).GetMarkerHash(params);
        params.legacy_final_hash = final_hash;
    }

    node::LegacyFnBlockAt BlockAt() const
    {
        return [this](const int height, CBlock& block) {
            const auto it{blocks.find(height)};
            if (it == blocks.end()) return false;
            block = it->second;
            return true;
        };
    }
};

//! Disk-backed legacy-B3 regtest for the complete archival sweep.
CBlock MakeIssuanceGenesis()
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

struct IssuanceChainSetup : public ChainTestingSetup {
    IssuanceChainSetup()
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
        genesis = MakeIssuanceGenesis();
        consensus.hashGenesisBlock = genesis.GetLegacyB3Hash();
        LoadVerifyActivateChainstate();
    }

    ~IssuanceChainSetup()
    {
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.hard_fork_height = std::nullopt;
        consensus.legacy_final_hash = std::nullopt;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(legacy_fn_issuance_tests)

BOOST_AUTO_TEST_CASE(recipient_rule)
{
    CMutableTransaction tx{MakeLegacyTx(GENESIS_TIME, 0x91)};

    BOOST_CHECK(!modern::FindLegacyFnRecipientVout(CTransaction{tx}));
    tx.vout.emplace_back(COIN_B3, CScript() << OP_TRUE);
    BOOST_CHECK(!modern::FindLegacyFnRecipientVout(CTransaction{tx}));
    tx.vout.emplace_back(COIN_B3 + 1, P2pkhScript(0x33));
    BOOST_CHECK(!modern::FindLegacyFnRecipientVout(CTransaction{tx}));
    tx.vout.emplace_back(COIN_B3, P2pkhScript(0x44));
    BOOST_REQUIRE(modern::FindLegacyFnRecipientVout(CTransaction{tx}));
    BOOST_CHECK_EQUAL(*modern::FindLegacyFnRecipientVout(CTransaction{tx}), 2U);
    tx.vout.emplace_back(COIN_B3, P2pkhScript(0x55));
    BOOST_CHECK_EQUAL(*modern::FindLegacyFnRecipientVout(CTransaction{tx}), 2U);
}

BOOST_AUTO_TEST_CASE(fn_genesis_manifest_from_records)
{
    const ManifestFixture fixture;
    std::vector<node::PodRecord> records{
        {.pod_id = fixture.second, .height = 4},
        {.pod_id = fixture.ignored, .height = 4},
        {.pod_id = fixture.first, .height = 4},
    };

    node::LegacyFnGenesisManifest manifest;
    std::string error;
    BOOST_REQUIRE_MESSAGE(node::BuildLegacyFnGenesisManifestFromRecords(
                              fixture.params, 5, fixture.final_hash, records,
                              fixture.BlockAt(), manifest, error),
                          error);
    BOOST_REQUIRE_EQUAL(manifest.rights.size(), 2U);
    BOOST_CHECK_EQUAL(manifest.genesis_height, 6U);
    BOOST_CHECK_EQUAL(manifest.manifest_version,
                      modern::FN_GENESIS_MANIFEST_VERSION_V1);
    BOOST_CHECK(manifest.rights[0].pod_id.Compare(manifest.rights[1].pod_id) < 0);

    const auto check_recipient{[&](const Consensus::FnGenesisRight& right) {
        const unsigned char expected{static_cast<unsigned char>(
            right.pod_id == fixture.first.ToUint256() ? 0x21 : 0x43)};
        BOOST_CHECK(right.pod_id == fixture.first.ToUint256() ||
                    right.pod_id == fixture.second.ToUint256());
        BOOST_CHECK(std::ranges::all_of(
            right.recipient_key_hash,
            [&](const unsigned char byte) { return byte == expected; }));
    }};
    check_recipient(manifest.rights[0]);
    check_recipient(manifest.rights[1]);

    const auto root{modern::ComputeFnGenesisManifestRootV1(
        manifest.chain_domain, manifest.genesis_height, manifest.rights, &error)};
    BOOST_REQUIRE_MESSAGE(root, error);
    BOOST_CHECK(*root == manifest.root);

    std::reverse(records.begin(), records.end());
    node::LegacyFnGenesisManifest again;
    BOOST_REQUIRE_MESSAGE(node::BuildLegacyFnGenesisManifestFromRecords(
                              fixture.params, 5, fixture.final_hash, records,
                              fixture.BlockAt(), again, error),
                          error);
    BOOST_CHECK(again == manifest);

    records = {{.pod_id = fixture.first, .height = 4},
               {.pod_id = fixture.first, .height = 4}};
    BOOST_CHECK(!node::BuildLegacyFnGenesisManifestFromRecords(
        fixture.params, 5, fixture.final_hash, records, fixture.BlockAt(),
        again, error));
    BOOST_CHECK(error.find("duplicate") != std::string::npos);

    BOOST_CHECK(!node::BuildLegacyFnGenesisManifestFromRecords(
        fixture.params, 5, uint256{0x99}, records, fixture.BlockAt(), again, error));
    BOOST_CHECK(error.find("configured final legacy hash") != std::string::npos);

    Consensus::Params wrong_height{fixture.params};
    wrong_height.hard_fork_height = 7;
    BOOST_CHECK(!node::BuildLegacyFnGenesisManifestFromRecords(
        wrong_height, 5, fixture.final_hash, records, fixture.BlockAt(), again, error));
    BOOST_CHECK(error.find("configured H+1") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(archival_sweep_end_to_end, IssuanceChainSetup)
{
    ChainstateManager& chainman{*m_node.chainman};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};
    const auto tip{[&] { return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip()); }};

    const auto submit{[&](std::vector<CMutableTransaction> txs) {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 360);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig =
            CScript() << (prev->nHeight + 1) << CScriptNum{9};
        coinbase.vout.emplace_back(
            legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
            CScript() << OP_TRUE);

        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits =
            legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        for (CMutableTransaction& tx : txs) {
            tx.m_legacy_encoding = true;
            block.vtx.push_back(MakeTransactionRef(std::move(tx)));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;

        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        DataStream bytes;
        bytes << legacy::TX_LEGACY(block);
        auto decoded{std::make_shared<CBlock>()};
        bytes >> legacy::TX_LEGACY(*decoded);
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(decoded, true, true, &new_block));
        BOOST_REQUIRE_EQUAL(tip()->nHeight, prev->nHeight + 1);
    }};

    for (int height{1}; height <= 32; ++height) submit({});
    const Txid coinbase1{[&] {
        CBlock block;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return chainman.ActiveChain()[1])));
        return block.vtx[0]->GetHash();
    }()};
    const CAmount reward1{legacy::GetProofOfWorkReward(0, 1, consensus)};

    CMutableTransaction fan;
    fan.version = 1;
    fan.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    fan.vin.resize(1);
    fan.vin[0].prevout = COutPoint{coinbase1, 0};
    fan.vout.emplace_back(150 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(140 * COIN_B3, CScript() << OP_TRUE);
    fan.vout.emplace_back(reward1 - 290 * COIN_B3 - 1000, CScript() << OP_TRUE);
    submit({fan}); // 33

    const Txid fan_txid{[&] {
        CBlock block;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return chainman.ActiveChain()[33])));
        return block.vtx[1]->GetHash();
    }()};

    CMutableTransaction pod;
    pod.version = 1;
    pod.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    pod.vin.resize(1);
    pod.vin[0].prevout = COutPoint{fan_txid, 0};
    pod.vout.emplace_back(COIN_B3, P2pkhScript(0x77));
    pod.vout.emplace_back(150 * COIN_B3 - TEST_COLLATERAL - COIN_B3 - 1000,
                          CScript() << OP_TRUE);
    submit({pod}); // 34

    const Txid expected_pod_id{[&] {
        CBlock block;
        BOOST_REQUIRE(chainman.m_blockman.ReadBlock(
            block, *WITH_LOCK(cs_main, return chainman.ActiveChain()[34])));
        return block.vtx[1]->GetHash();
    }()};

    CMutableTransaction ignored;
    ignored.version = 1;
    ignored.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + 360);
    ignored.vin.resize(1);
    ignored.vin[0].prevout = COutPoint{fan_txid, 1};
    ignored.vout.emplace_back(140 * COIN_B3 - TEST_COLLATERAL - 1000,
                              CScript() << OP_TRUE);
    submit({ignored}); // 35
    submit({});        // 36

    node::LegacyFnGenesisManifest manifest;
    std::string error;
    BOOST_CHECK(!node::BuildLegacyFnGenesisManifest(chainman, manifest, error));
    BOOST_CHECK(error.find("not pinned") != std::string::npos);

    mutable_consensus.hard_fork_height = 37;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();

    BOOST_REQUIRE_MESSAGE(
        node::BuildLegacyFnGenesisManifest(chainman, manifest, error), error);
    BOOST_REQUIRE_EQUAL(manifest.rights.size(), 1U);
    BOOST_CHECK_EQUAL(manifest.genesis_height, 37U);
    BOOST_CHECK(manifest.rights[0].pod_id == expected_pod_id.ToUint256());
    BOOST_CHECK(std::ranges::all_of(
        manifest.rights[0].recipient_key_hash,
        [](const unsigned char byte) { return byte == 0x77; }));

    const auto root{modern::ComputeFnGenesisManifestRootV1(
        manifest.chain_domain, manifest.genesis_height, manifest.rights, &error)};
    BOOST_REQUIRE_MESSAGE(root, error);
    BOOST_CHECK(*root == manifest.root);

    node::LegacyFnGenesisManifest again;
    BOOST_REQUIRE_MESSAGE(
        node::BuildLegacyFnGenesisManifest(chainman, again, error), error);
    BOOST_CHECK(again == manifest);
}

BOOST_AUTO_TEST_SUITE_END()

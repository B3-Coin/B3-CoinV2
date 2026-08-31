// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <arith_uint256.h>
#include <chain.h>
#include <coins.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/fn_params.h>
#include <consensus/merkle.h>
#include <crypto/common.h>
#include <kernel/mempool_entry.h>
#include <kernel/mempool_removal_reason.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/asset_output.h>
#include <modern/chain_domain.h>
#include <modern/fn.h>
#include <modern/fn_genesis_validation.h>
#include <modern/fn_pod.h>
#include <modern/payload_root.h>
#include <modern/policy.h>
#include <modern/pos.h>
#include <modern/pos_v1.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/check.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr int64_t MOCK_NOW{1'400'100'000};
constexpr uint32_t EASY_BITS{0x207fffff};

CBlock MakeFnMempoolGenesis()
{
    CMutableTransaction coinbase;
    coinbase.version = 1;
    coinbase.nTime = GENESIS_TIME;
    coinbase.m_legacy_encoding = true;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << CScriptNum{0} << CScriptNum{42};
    coinbase.vout.emplace_back(0, CScript{});

    CBlock genesis;
    genesis.nVersion = 1;
    genesis.nTime = GENESIS_TIME;
    genesis.nBits = EASY_BITS;
    genesis.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

class AcceptAllModernPos final : public modern::PosValidator
{
public:
    bool CheckStake(const CBlock&, const CBlockIndex&, const CCoinsViewCache&,
                    BlockValidationState&) const override
    {
        return true;
    }
};

struct FnPodMempoolSetup : public ChainTestingSetup {
    FnPodMempoolSetup()
        : ChainTestingSetup{ChainType::REGTEST,
                            {.extra_args = {"-acceptnonstdtxn=1"}}}
    {
        SetMockTime(MOCK_NOW);
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = 1'000;
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = std::numeric_limits<int>::max();
        consensus.CSVHeight = std::numeric_limits<int>::max();
        consensus.SegwitHeight = std::numeric_limits<int>::max();
        CBlock& genesis{const_cast<CBlock&>(m_node.chainman->GetParams().GenesisBlock())};
        genesis = MakeFnMempoolGenesis();
        consensus.hashGenesisBlock = genesis.GetLegacyB3Hash();
        LoadVerifyActivateChainstate();
    }
};

std::shared_ptr<CBlock> LegacyRoundTrip(const CBlock& block)
{
    DataStream bytes;
    bytes << legacy::TX_LEGACY(block);
    auto decoded{std::make_shared<CBlock>()};
    bytes >> legacy::TX_LEGACY(*decoded);
    return decoded;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(fn_pod_mempool_tests, FnPodMempoolSetup)

BOOST_AUTO_TEST_CASE(slot_competitors_and_descendants_follow_confirmed_branch)
{
    ChainstateManager& chainman{*m_node.chainman};
    CTxMemPool& pool{*Assert(m_node.mempool)};
    const Consensus::Params& consensus{chainman.GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};
    constexpr int H{32};
    constexpr int CORRIDOR{2};
    constexpr int MODERN_START{H + 1 + CORRIDOR};
    constexpr int A1{MODERN_START + 1};
    constexpr CAmount FEE{1'000};
    const CScript padded_script{
        CScript() << std::vector<unsigned char>(24, 0xc4) << OP_DROP << OP_TRUE};
    const auto tip{[&] {
        return WITH_LOCK(cs_main, return chainman.ActiveChain().Tip());
    }};

    const auto build_legacy{[&](const CBlockIndex* prev) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        // Keep the synthetic legacy target at its easy limit so this focused
        // mempool regression does not spend minutes grinding retargets.
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 360);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << height << CScriptNum{9};
        coinbase.vout.emplace_back(
            legacy::GetProofOfWorkReward(0, height, consensus), CScript() << OP_TRUE);

        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits = legacy::GetNextTargetRequired(prev, false, consensus);
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const arith_uint256 target{arith_uint256().SetCompact(block.nBits)};
        while (UintToArith256(block.GetLegacyB3Hash()) > target) ++block.nNonce;
        return block;
    }};

    for (int height{1}; height <= H; ++height) {
        bool new_block{false};
        BOOST_REQUIRE(chainman.ProcessNewBlock(
            LegacyRoundTrip(build_legacy(tip())), true, true, &new_block));
        BOOST_REQUIRE(new_block);
    }

    mutable_consensus.hard_fork_height = H + 1;
    mutable_consensus.legacy_final_hash = tip()->GetBlockHash();
    mutable_consensus.transition_pow_length = CORRIDOR;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.transition_pow_reward = 0;
    mutable_consensus.fn_genesis_required = true;
    mutable_consensus.modern_pos.emplace();
    mutable_consensus.modern_pos->sentinel_bits = EASY_BITS;
    mutable_consensus.fn_pod_activation_height = A1;
    mutable_consensus.asset_activation_height = A1;

    mutable_consensus.fn_genesis_manifest.reserve(
        Consensus::HISTORICAL_FN_PROVEN_FLOOR);
    for (uint32_t i{0}; i < Consensus::HISTORICAL_FN_PROVEN_FLOOR; ++i) {
        Consensus::FnGenesisRight right;
        WriteBE32(right.pod_id.begin(), i + 1);
        right.recipient_key_hash.fill(static_cast<unsigned char>(i & 0xff));
        mutable_consensus.fn_genesis_manifest.push_back(right);
    }
    const auto domain{modern::ModernChainDomain(
        mutable_consensus.hashGenesisBlock, *mutable_consensus.legacy_final_hash)};
    BOOST_REQUIRE(domain);
    mutable_consensus.fn_genesis_rights_root =
        modern::ComputeFnGenesisManifestRootV1(
            *domain, H + 1, mutable_consensus.fn_genesis_manifest);
    BOOST_REQUIRE(mutable_consensus.fn_genesis_rights_root);
    std::string fn_error;
    const auto genesis_outputs{
        modern::ExpectedFnGenesisOutputs(mutable_consensus, fn_error)};
    BOOST_REQUIRE_MESSAGE(genesis_outputs, fn_error);

    AcceptAllModernPos pos;
    mutable_consensus.test_only_modern_pos_validator = &pos;

    const auto build_modern{[&](const CBlockIndex* prev,
                                const std::vector<CTransactionRef>& txs,
                                const CAmount fees) {
        const int height{prev->nHeight + 1};
        CMutableTransaction coinbase;
        coinbase.version = 2;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << CScriptNum{height} << CScriptNum{7};
        if (fees != 0) coinbase.vout.emplace_back(fees, CScript() << OP_TRUE);
        if (height == H + 1) {
            coinbase.vout.insert(coinbase.vout.end(), genesis_outputs->begin(),
                                 genesis_outputs->end());
        }
        if (coinbase.vout.empty()) coinbase.vout.emplace_back(0, CScript() << OP_TRUE);

        CBlock block;
        block.nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = static_cast<uint32_t>(prev->GetBlockTime() + 60);
        block.nBits = height <= H + CORRIDOR
                          ? EASY_BITS
                          : mutable_consensus.modern_pos->sentinel_bits;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        if (modern::BlockHasAnyMpa(block)) {
            CMutableTransaction committed_coinbase{*block.vtx.front()};
            committed_coinbase.vout.emplace_back(
                0, modern::MakePayloadRootCellScript(modern::ComputePayloadRoot(block)));
            block.vtx.front() = MakeTransactionRef(std::move(committed_coinbase));
        }
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        if (height <= H + CORRIDOR) {
            while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        } else {
            // The test adapter judges stake eligibility, while the shared
            // header-shape rule still requires the frozen 64-byte field.
            block.vchBlockSig.assign(modern::MODERN_POS_SIG_SIZE, 0x42);
        }
        return block;
    }};
    const auto submit{[&](const CBlock& block) {
        {
            LOCK(cs_main);
            const BlockValidationState preflight{TestBlockValidity(
                chainman.ActiveChainstate(), block, /*check_pow=*/true,
                /*check_merkle_root=*/true)};
            BOOST_REQUIRE_MESSAGE(preflight.IsValid(), preflight.ToString());
        }
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(
            chainman.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true,
                                     &new_block),
            "modern block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }};

    submit(build_modern(tip(), {}, 0)); // H+1: mandatory historical FN Genesis
    submit(build_modern(tip(), {}, 0)); // H+2: final corridor block
    submit(build_modern(tip(), {}, 0)); // M: test-adapter modern PoS
    BOOST_REQUIRE_EQUAL(tip()->nHeight, MODERN_START);

    // The frozen legacy maturity follows a legacy coinstake coin into the
    // modern era. A height-H coinstake is still immature; a height-1
    // coinstake is accepted and tagged for future reorg maturity rescans.
    uint256 maturity_source;
    WriteBE32(maturity_source.begin(), 0xabc001U);
    const COutPoint immature_out{Txid::FromUint256(maturity_source), 0};
    const COutPoint mature_out{Txid::FromUint256(maturity_source), 1};
    {
        LOCK(cs_main);
        CCoinsViewCache& coins{chainman.ActiveChainstate().CoinsTip()};
        coins.AddCoin(immature_out,
                      Coin{CTxOut{COIN, CScript() << OP_TRUE}, H,
                           /*coinbase=*/false, /*coinstake=*/true},
                      /*possible_overwrite=*/false);
        coins.AddCoin(mature_out,
                      Coin{CTxOut{COIN, CScript() << OP_TRUE}, 1,
                           /*coinbase=*/false, /*coinstake=*/true},
                      /*possible_overwrite=*/false);
    }
    const auto plain_spend{[&](const COutPoint& prevout) {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = prevout;
        tx.vout.emplace_back(COIN - FEE, padded_script);
        return MakeTransactionRef(std::move(tx));
    }};
    const CTransactionRef immature_spend{plain_spend(immature_out)};
    const auto immature_result{WITH_LOCK(
        cs_main, return chainman.ProcessTransaction(immature_spend))};
    BOOST_REQUIRE(immature_result.m_result_type !=
                  MempoolAcceptResult::ResultType::VALID);
    BOOST_CHECK_EQUAL(immature_result.m_state.GetRejectReason(),
                      "bad-txns-premature-spend-of-legacy-coin");

    const CTransactionRef mature_spend{plain_spend(mature_out)};
    const auto mature_result{WITH_LOCK(
        cs_main, return chainman.ProcessTransaction(mature_spend))};
    BOOST_REQUIRE_MESSAGE(
        mature_result.m_result_type == MempoolAcceptResult::ResultType::VALID,
        mature_result.m_state.ToString());
    {
        LOCK(pool.cs);
        const CTxMemPoolEntry* entry{pool.GetEntry(mature_spend->GetHash())};
        BOOST_REQUIRE(entry);
        BOOST_CHECK(entry->GetSpendsCoinbase());
        pool.removeRecursive(*mature_spend, MemPoolRemovalReason::REORG);
    }

    // Seed three disjoint, modern, non-coinbase test UTXOs. Their values are
    // deliberately large enough for the real 15,000-B3 slot-0/1 destruction.
    const CAmount required{modern::RequiredFnPodDisintegration(0)};
    const CAmount funding_value{required + 10 * COIN};
    uint256 funding_source;
    WriteBE32(funding_source.begin(), 0xabc002U);
    const Txid funding_txid{Txid::FromUint256(funding_source)};
    {
        LOCK(cs_main);
        CCoinsViewCache& coins{chainman.ActiveChainstate().CoinsTip()};
        for (uint32_t n{0}; n < 3; ++n) {
            coins.AddCoin(COutPoint{funding_txid, n},
                          Coin{CTxOut{funding_value, CScript() << OP_TRUE},
                               MODERN_START, /*coinbase=*/false},
                          /*possible_overwrite=*/false);
        }
    }
    const auto fn_asset{modern::ConfiguredFnAssetId(mutable_consensus)};
    BOOST_REQUIRE(fn_asset);
    const auto make_pod{[&](const uint32_t input_index,
                            const uint32_t created_before) {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{funding_txid, input_index};
        tx.vout.emplace_back(funding_value -
                                 modern::RequiredFnPodDisintegration(created_before) - FEE,
                             padded_script);
        const auto fn_output{modern::MakeAssetOwnerOutput(
            *fn_asset, 1, modern::PolicyType::FN, CScript() << OP_TRUE)};
        BOOST_REQUIRE(fn_output);
        tx.vout.push_back(*fn_output);
        tx.mpa.push_back(modern::MakeModernFnPodRecord(created_before, 1));
        return MakeTransactionRef(std::move(tx));
    }};
    const auto make_child{[&](const CTransactionRef& parent) {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{parent->GetHash(), 0};
        tx.vout.emplace_back(parent->vout[0].nValue - FEE, padded_script);
        return MakeTransactionRef(std::move(tx));
    }};
    const auto admit{[&](const CTransactionRef& tx) {
        const auto result{WITH_LOCK(cs_main, return chainman.ProcessTransaction(tx))};
        BOOST_REQUIRE_MESSAGE(
            result.m_result_type == MempoolAcceptResult::ResultType::VALID,
            result.m_state.ToString());
    }};

    // Both disjoint transactions can legitimately compete for current slot
    // zero. Confirming one advances the counter and recursively evicts the
    // loser plus its ordinary child.
    const CTransactionRef winner{make_pod(0, 0)};
    const CTransactionRef loser{make_pod(1, 0)};
    const CTransactionRef loser_child{make_child(loser)};
    admit(winner);
    admit(loser);
    admit(loser_child);
    BOOST_REQUIRE_EQUAL(pool.size(), 3U);

    const CBlock winning_block{build_modern(tip(), {winner}, FEE)};
    submit(winning_block);
    BOOST_REQUIRE_EQUAL(tip()->nHeight, A1);
    BOOST_CHECK_EQUAL(tip()->m_fn_pod_issued_total, 1U);
    BOOST_CHECK(tip()->m_fn_pod_issued_total_known);
    BOOST_CHECK(!pool.exists(loser->GetHash()));
    BOOST_CHECK(!pool.exists(loser_child->GetHash()));
    BOOST_CHECK_EQUAL(pool.size(), 0U);

    // On the count-1 branch, slot one and its child are admissible. Reverting
    // the winning block returns the branch count to zero: the disconnected
    // slot-zero winner is resurrected, while slot one and its child are
    // recursively purged as future-slot transactions.
    const CTransactionRef future{make_pod(2, 1)};
    const CTransactionRef future_child{make_child(future)};
    admit(future);
    admit(future_child);
    BOOST_REQUIRE_EQUAL(pool.size(), 2U);

    CBlockIndex* winning_index{WITH_LOCK(
        cs_main, return chainman.m_blockman.LookupBlockIndex(winning_block.GetHash()))};
    BOOST_REQUIRE(winning_index);
    BlockValidationState invalidate_state;
    BOOST_REQUIRE(chainman.ActiveChainstate().InvalidateBlock(
        invalidate_state, winning_index));
    BOOST_REQUIRE_EQUAL(tip()->nHeight, MODERN_START);
    BOOST_CHECK(pool.exists(winner->GetHash()));
    BOOST_CHECK(!pool.exists(future->GetHash()));
    BOOST_CHECK(!pool.exists(future_child->GetHash()));
    BOOST_CHECK_EQUAL(pool.size(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

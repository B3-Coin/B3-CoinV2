// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! The complete B3 evolution scenario on an isolated regtest chain:
//! legacy PoW history -> honest legacy PoS history (real coinstakes, real
//! kernel, real rewards) -> frozen (H, X) boundary -> 1,000-block
//! temporary-PoW corridor with legacy-UTXO crossing, STAKE policy outputs,
//! model-level colored-asset checks and test-convention FN burns ->
//! deterministic validator registry (restart / reindex / second node) ->
//! the deliberate no-modern-pos-rules gate at the first modern-PoS height.
//! Everything before Modern PoS; nothing of Modern PoS.

#include <chainparams.h>
#include <coins.h>
#include <consensus/amount.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <key.h>
#include <legacy/codec.h>
#include <legacy/consensus.h>
#include <modern/asset.h>
#include <modern/policy.h>
#include <modern/pos.h>
#include <modern/proof.h>
#include <modern/stake.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <node/kernel_notifications.h>
#include <node/stake_registry.h>
#include <node/utxo_commitment.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <script/solver.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

//! Evolution timeline (test parameters, not mainnet values).
constexpr int LEGACY_POW_END{300};   // heights 1..300: legacy PoW
constexpr int LEGACY_POS_BLOCKS{1000}; // heights 301..1300: legacy PoS
constexpr int EVO_H{LEGACY_POW_END + LEGACY_POS_BLOCKS}; // final legacy height
constexpr int EVO_CORRIDOR{1000};    // heights 1301..2300: transition PoW
constexpr uint32_t EASY_BITS{0x207fffff};
constexpr uint32_t GENESIS_TIME{1'400'000'000};
constexpr int64_t SPACING{360}; // the real legacy target spacing: holds difficulty at the limit

CBlock MakeEvolutionGenesis()
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
    genesis.hashPrevBlock.SetNull();
    genesis.nTime = GENESIS_TIME;
    genesis.nBits = EASY_BITS;
    genesis.nNonce = 0;
    genesis.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

//! Disk-backed synthetic legacy-B3 regtest with the evolution phase
//! parameters: LEGACY_POW_END as the last PoW height, PoS afterwards.
struct EvolutionSetup : public ChainTestingSetup {
    EvolutionSetup()
        : ChainTestingSetup{ChainType::REGTEST,
                            {.extra_args = {"-acceptnonstdtxn=1"},
                             .coins_db_in_memory = false,
                             .block_tree_db_in_memory = false}}
    {
        SetMockTime(GENESIS_TIME + 1000);
        auto& consensus{const_cast<Consensus::Params&>(m_node.chainman->GetConsensus())};
        consensus.legacy_b3coin = true;
        consensus.legacy_last_pow_block = LEGACY_POW_END;
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = std::numeric_limits<int>::max();
        consensus.CSVHeight = std::numeric_limits<int>::max();
        consensus.SegwitHeight = std::numeric_limits<int>::max();
        CBlock& genesis{const_cast<CBlock&>(m_node.chainman->GetParams().GenesisBlock())};
        genesis = MakeEvolutionGenesis();
        consensus.hashGenesisBlock = genesis.GetLegacyB3Hash();
        LoadVerifyActivateChainstate();
    }
};

std::shared_ptr<CBlock> CodecRoundTrip(const CBlock& block)
{
    DataStream bytes;
    bytes << legacy::TX_LEGACY(block);
    auto decoded{std::make_shared<CBlock>()};
    bytes >> legacy::TX_LEGACY(*decoded);
    return decoded;
}

//! A test-convention FN object derived from an on-chain burn. TEST-ONLY:
//! the marker and cost are a test convention, not consensus; FN economics
//! (OD-4/OD-5) remain open.
constexpr CAmount FN_TEST_COST{50'000};
const std::vector<unsigned char> FN_MARKER{'B', '3', 'F', 'N'};

struct FnObject {
    COutPoint burn_outpoint;
    std::array<unsigned char, 32> claim_key{};
};

CScript MakeFnBurnScript(const std::array<unsigned char, 32>& claim_key)
{
    std::vector<unsigned char> payload{FN_MARKER};
    payload.insert(payload.end(), claim_key.begin(), claim_key.end());
    return CScript() << OP_RETURN << payload;
}

//! Deterministically derive the FN set from the chain: every unspendable
//! OP_RETURN burn output carrying the marker and EXACTLY the test cost
//! yields one FN object, keyed (and deduplicated) by its burn outpoint.
std::map<COutPoint, FnObject> DeriveFnObjects(ChainstateManager& chainman, const int from_height)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    std::map<COutPoint, FnObject> objects;
    for (int height{from_height}; height <= chainman.ActiveChain().Height(); ++height) {
        const CBlockIndex* pindex{chainman.ActiveChain()[height]};
        CBlock block;
        if (!chainman.m_blockman.ReadBlock(block, *pindex)) continue;
        for (const auto& tx : block.vtx) {
            for (uint32_t n{0}; n < tx->vout.size(); ++n) {
                const CTxOut& out{tx->vout[n]};
                CScript::const_iterator it{out.scriptPubKey.begin()};
                opcodetype opcode;
                std::vector<unsigned char> data;
                if (!out.scriptPubKey.GetOp(it, opcode) || opcode != OP_RETURN) continue;
                if (!out.scriptPubKey.GetOp(it, opcode, data)) continue;
                if (data.size() != FN_MARKER.size() + 32 ||
                    !std::equal(FN_MARKER.begin(), FN_MARKER.end(), data.begin())) {
                    continue;
                }
                if (out.nValue != FN_TEST_COST) continue; // wrong-amount burns create nothing
                FnObject fn;
                fn.burn_outpoint = COutPoint{tx->GetHash(), n};
                std::copy(data.begin() + FN_MARKER.size(), data.end(), fn.claim_key.begin());
                objects.emplace(fn.burn_outpoint, fn); // outpoint-keyed: same burn never doubles
            }
        }
    }
    return objects;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(b3_evolution_tests, EvolutionSetup)

BOOST_AUTO_TEST_CASE(full_evolution_scenario)
{
    const auto cm{[&]() -> ChainstateManager& { return *m_node.chainman; }};
    const Consensus::Params& consensus{cm().GetConsensus()};
    auto& mutable_consensus{const_cast<Consensus::Params&>(consensus)};
    const auto tip{[&] { return WITH_LOCK(cs_main, return cm().ActiveChain().Tip()); }};

    CKey stake_key;
    stake_key.MakeNewKey(/*fCompressed=*/false);
    const CPubKey stake_pubkey{stake_key.GetPubKey()};
    const CScript stake_payout_script{CScript()
                                      << std::vector<unsigned char>(stake_pubkey.begin(), stake_pubkey.end())
                                      << OP_CHECKSIG};
    BOOST_REQUIRE_MESSAGE(stake_payout_script.size() == stake_pubkey.size() + 2,
                          "payout script size " << stake_payout_script.size()
                                                << " pubkey size " << stake_pubkey.size());

    // ---------------------------------------------------------------- utils
    const auto submit{[&](const CBlock& block) {
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(cm().ProcessNewBlock(CodecRoundTrip(block), true, true, &new_block),
                              "block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }};

    const auto build_pow{[&](std::vector<CMutableTransaction> txs) {
        const CBlockIndex* prev{tip()};
        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = static_cast<uint32_t>(prev->GetBlockTime() + SPACING);
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(legacy::GetProofOfWorkReward(0, prev->nHeight + 1, consensus),
                                   CScript() << OP_TRUE);
        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = coinbase.nTime;
        block.nBits = legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/false, consensus);
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

    // Honest legacy PoS block: grind the coinstake time through the REAL
    // ported kernel (CheckStakeKernel), claim exactly the real reward, pay
    // to P2PK and sign the block with that key — the historical
    // construction path, no PoW shortcut.
    const auto build_pos{[&](const COutPoint& stake_prevout) {
        const CBlockIndex* prev{tip()};
        const uint32_t bits{legacy::GetNextTargetRequired(prev, /*proof_of_stake=*/true, consensus)};

        LOCK(cs_main);
        const CCoinsViewCache& view{cm().ActiveChainstate().CoinsTip()};
        const Coin& coin{view.AccessCoin(stake_prevout)};
        BOOST_REQUIRE(!coin.IsSpent());

        CMutableTransaction coinstake;
        coinstake.version = 1;
        coinstake.m_legacy_encoding = true;
        coinstake.vin.resize(1);
        coinstake.vin[0].prevout = stake_prevout;
        coinstake.vin[0].scriptSig = CScript{}; // OP_TRUE stake pool coin
        coinstake.vout.resize(2);
        coinstake.vout[0] = CTxOut{0, CScript{}}; // the coinstake marker

        uint32_t found_time{0};
        for (uint32_t t{static_cast<uint32_t>(prev->GetBlockTime() + SPACING - 8)};
             t < prev->GetBlockTime() + SPACING + 50'000; ++t) {
            coinstake.nTime = t;
            if (legacy::CheckStakeKernel(prev, CTransaction{coinstake}, view, bits)) {
                found_time = t;
                break;
            }
        }
        BOOST_REQUIRE_MESSAGE(found_time != 0, "no kernel solution found at height " << prev->nHeight + 1);
        coinstake.nTime = found_time;

        const auto coin_age{legacy::GetCoinAge(CTransaction{coinstake}, prev, view)};
        BOOST_REQUIRE(coin_age.has_value());
        const CAmount reward{legacy::GetProofOfStakeReward(prev, *coin_age, /*fees=*/0)};
        coinstake.vout[1] = CTxOut{coin.out.nValue + reward, stake_payout_script};

        CMutableTransaction coinbase;
        coinbase.version = 1;
        coinbase.nTime = found_time;
        coinbase.m_legacy_encoding = true;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (prev->nHeight + 1) << CScriptNum{99};
        coinbase.vout.emplace_back(0, CScript{}); // PoS coinbase is empty

        CBlock block;
        block.nVersion = 4;
        block.hashPrevBlock = prev->GetBlockHash();
        block.nTime = found_time; // must equal the coinstake time
        block.nBits = bits;
        block.nNonce = 0;
        block.vtx.push_back(MakeTransactionRef(std::move(coinbase)));
        block.vtx.push_back(MakeTransactionRef(std::move(coinstake)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        BOOST_REQUIRE(stake_key.Sign(block.GetLegacyB3Hash(), block.vchBlockSig));
        return block;
    }};

    const auto build_corridor{[&](std::vector<CMutableTransaction> txs) {
        const CBlockIndex* prev{tip()};
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
        for (CMutableTransaction& mtx : txs) block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        block.hashMerkleRoot = BlockMerkleRoot(block);
        block.nNonce = 0;
        while (!CheckTransitionPowEligibility(block)) ++block.nNonce;
        return block;
    }};
    const auto submit_corridor{[&](std::vector<CMutableTransaction> txs) {
        CBlock block{build_corridor(std::move(txs))};
        SetMockTime(static_cast<int64_t>(block.nTime) + 600);
        bool new_block{false};
        BOOST_REQUIRE_MESSAGE(cm().ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                              "corridor block at height " << tip()->nHeight + 1 << " rejected");
        BOOST_REQUIRE(new_block);
    }};

    // ------------------------------------------------ Phase 1: legacy PoW
    // 300 PoW blocks at the real 360 s spacing. From height 31 every block
    // also SPENDS the just-matured coinbase (height - 30) into four
    // anyone-can-spend pool coins: real transactions, spent and unspent
    // outputs, and the stake pool for phase 2.
    // The height-1 coinbase carries the historical 260,000-COIN block: once
    // matured it fans into the 1,040-coin stake pool (250 COIN each), giving
    // every later coinstake ample kernel weight.
    constexpr int POOL_SIZE{1040};
    constexpr CAmount POOL_COIN_VALUE{250LL * 1'000'000};
    std::vector<Txid> coinbases;
    coinbases.push_back(Txid{}); // index 0 unused
    std::vector<COutPoint> stake_pool;
    for (int height{1}; height <= LEGACY_POW_END; ++height) {
        std::vector<CMutableTransaction> txs;
        if (height == 31) {
            CMutableTransaction fan;
            fan.version = 1;
            fan.nTime = static_cast<uint32_t>(tip()->GetBlockTime() + SPACING);
            fan.vin.resize(1);
            fan.vin[0].prevout = COutPoint{coinbases[1], 0};
            fan.vin[0].scriptSig = CScript{};
            for (int i{0}; i < POOL_SIZE; ++i) fan.vout.emplace_back(POOL_COIN_VALUE, CScript() << OP_TRUE);
            txs.push_back(fan);
        }
        CBlock block{build_pow(std::move(txs))};
        submit(block);
        coinbases.push_back(block.vtx[0]->GetHash());
        if (block.vtx.size() > 1) {
            const Txid fan_txid{block.vtx[1]->GetHash()};
            for (uint32_t n{0}; n < POOL_SIZE; ++n) stake_pool.emplace_back(fan_txid, n);
        }
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, LEGACY_POW_END);
    BOOST_REQUIRE_GE(stake_pool.size(), static_cast<size_t>(LEGACY_POS_BLOCKS));
    {
        LOCK(cs_main);
        const CCoinsViewCache& coins{cm().ActiveChainstate().CoinsTip()};
        BOOST_CHECK(!coins.HaveCoin(COutPoint{coinbases[1], 0}));            // split-spent
        BOOST_CHECK(coins.HaveCoin(COutPoint{coinbases[LEGACY_POW_END - 5], 0})); // reserve, unspent
        BOOST_CHECK(coins.HaveCoin(stake_pool.front()));                     // pool coin exists
    }

    // ------------------------------------------------ Phase 2: legacy PoS
    // 1000 honest PoS blocks, each staking one aged pool coin through the
    // real kernel and claiming exactly the real reward. The staked outpoint
    // is consumed and a fresh P2PK output appears: legacy stake churn.
    CAmount total_pos_rewards{0};
    for (int i{0}; i < LEGACY_POS_BLOCKS; ++i) {
        const COutPoint staked{stake_pool[i]};
        const CAmount staked_value{WITH_LOCK(
            cs_main, return cm().ActiveChainstate().CoinsTip().AccessCoin(staked).out.nValue)};
        CBlock block{build_pos(staked)};
        submit(block);
        total_pos_rewards += block.vtx[1]->vout[1].nValue - staked_value;
        if (i % 250 == 0) {
            LOCK(cs_main);
            BOOST_CHECK(cm().ActiveChain().Tip()->m_legacy_proof_of_stake);
            BOOST_CHECK(!cm().ActiveChainstate().CoinsTip().HaveCoin(staked)); // churned away
            BOOST_CHECK(cm().ActiveChainstate().CoinsTip().HaveCoin(
                COutPoint{block.vtx[1]->GetHash(), 1})); // new stake output
        }
    }
    BOOST_REQUIRE_EQUAL(tip()->nHeight, EVO_H);
    BOOST_CHECK_GE(total_pos_rewards, 0);

    // ---------------------------------- Freeze the boundary; open corridor
    const uint256 X{tip()->GetBlockHash()};
    mutable_consensus.hard_fork_height = EVO_H + 1;
    mutable_consensus.legacy_final_hash = X;
    mutable_consensus.transition_pow_length = EVO_CORRIDOR;
    mutable_consensus.transition_pow_bits = EASY_BITS;
    mutable_consensus.min_stake_amount = 1000;
    BOOST_REQUIRE(consensus.test_only_modern_pos_validator == nullptr);
    BOOST_CHECK(WITH_LOCK(cs_main, return cm().ActiveChainstate().LegacyBoundaryActive()));

    uint256 commitment_at_h;
    {
        LOCK(cs_main);
        cm().ActiveChainstate().ForceFlushStateToDisk();
        commitment_at_h = node::UtxoSetCommitment(
            node::EnumerateUtxos(cm().ActiveChainstate().CoinsDB()));
    }

    // -------------------------- Phase 3+4: corridor and legacy crossing
    // H+1 spends an untouched PoW-era coinbase through LEGACY_LOCK: the
    // historical txid and outpoint are unchanged, ownership is proven by
    // the frozen legacy rules, and ten modern funding outputs appear.
    const int crossing_height{LEGACY_POW_END - 39};
    const Txid crossing_source{coinbases[crossing_height]};
    BOOST_CHECK(WITH_LOCK(
        cs_main, return cm().ActiveChainstate().CoinsTip().HaveCoin(COutPoint{crossing_source, 0})));
    CMutableTransaction crossing;
    crossing.version = 2;
    crossing.vin.resize(1);
    crossing.vin[0].prevout = COutPoint{crossing_source, 0};
    crossing.vin[0].scriptSig = CScript{};
    for (int i{0}; i < 10; ++i) crossing.vout.emplace_back(900'000, CScript() << OP_TRUE);
    crossing.vout.emplace_back(legacy::GetProofOfWorkReward(0, crossing_height, consensus) - 9'000'000 - 1000,
                               CScript() << OP_TRUE);
    const Txid crossing_txid{CTransaction{crossing}.GetHash()};
    submit_corridor({crossing});
    {
        LOCK(cs_main);
        BOOST_CHECK(!cm().ActiveChainstate().CoinsTip().HaveCoin(COutPoint{crossing_source, 0}));
        BOOST_CHECK(cm().ActiveChainstate().CoinsTip().HaveCoin(COutPoint{crossing_txid, 0}));
    }

    // ------------------------------------------ Phase 5: STAKE policies
    std::array<unsigned char, 32> key_a{};
    key_a.fill(0xaa);
    std::array<unsigned char, 32> key_b{};
    key_b.fill(0xbb);
    std::array<unsigned char, 32> key_c{};
    key_c.fill(0xcc);
    std::array<unsigned char, 32> key_d{};
    key_d.fill(0xdd);
    const CScript owner{CScript() << OP_TRUE};
    const auto stake_from{[&](const uint32_t fund_n, std::vector<std::pair<std::array<unsigned char, 32>, CAmount>> stakes) {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{crossing_txid, fund_n};
        tx.vin[0].scriptSig = CScript{};
        CAmount total{0};
        for (const auto& [key, amount] : stakes) {
            tx.vout.emplace_back(amount, modern::MakeStakeScript(key, owner));
            total += amount;
        }
        tx.vout.emplace_back(900'000 - total - 1000, CScript() << OP_TRUE);
        return tx;
    }};

    // Single 100k for A; the SAME total as 30k/30k/40k for B.
    const CMutableTransaction stake_a{stake_from(0, {{key_a, 100'000}})};
    const CMutableTransaction stake_b{stake_from(1, {{key_b, 30'000}, {key_b, 30'000}, {key_b, 40'000}})};
    const Txid stake_a_txid{CTransaction{stake_a}.GetHash()};
    const int stake_height{tip()->nHeight + 1};
    submit_corridor({stake_a, stake_b});

    // A duplicate identity for A (aggregation, not conflict) and a stake
    // for D that will be cancelled by its owner.
    const CMutableTransaction stake_a2{stake_from(2, {{key_a, 50'000}})};
    const CMutableTransaction stake_d{stake_from(3, {{key_d, 25'000}})};
    const Txid stake_d_txid{CTransaction{stake_d}.GetHash()};
    submit_corridor({stake_a2, stake_d});

    // Negative cases: below-minimum and zero-key claims fail the block.
    {
        CMutableTransaction below_min{stake_from(4, {{key_c, 999}})};
        CBlock block{build_corridor({below_min})};
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(cm().ActiveChainstate(), block, true, true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-output");
    }
    {
        CMutableTransaction zero_key{stake_from(4, {{std::array<unsigned char, 32>{}, 50'000}})};
        CBlock block{build_corridor({zero_key})};
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(cm().ActiveChainstate(), block, true, true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-stake-output");
    }

    // Owner cancellation: D spends its stake output; the stake is removed.
    {
        CMutableTransaction cancel;
        cancel.version = 2;
        cancel.vin.resize(1);
        cancel.vin[0].prevout = COutPoint{stake_d_txid, 0};
        cancel.vin[0].scriptSig = CScript{};
        cancel.vout.emplace_back(25'000 - 1000, CScript() << OP_TRUE);
        submit_corridor({cancel});
    }

    // Activation boundary: at depth 19 the stakes are PENDING, at 20 ACTIVE
    // with split == single aggregated weight.
    while (tip()->nHeight < stake_height + 19) submit_corridor({});
    {
        LOCK(cs_main);
        cm().ActiveChainstate().ForceFlushStateToDisk();
        const node::StakeRegistry registry{node::DeriveStakeRegistry(
            cm().ActiveChainstate().CoinsDB(), tip()->nHeight, consensus)};
        BOOST_CHECK_EQUAL(registry.validators.at(key_a).total_weight, 0); // depth 19: pending
    }
    submit_corridor({});
    {
        LOCK(cs_main);
        cm().ActiveChainstate().ForceFlushStateToDisk();
        const node::StakeRegistry registry{node::DeriveStakeRegistry(
            cm().ActiveChainstate().CoinsDB(), tip()->nHeight, consensus)};
        BOOST_CHECK_EQUAL(registry.validators.at(key_a).total_weight, 100'000); // depth 20: active
        BOOST_CHECK_EQUAL(registry.validators.at(key_b).total_weight, 100'000); // split == single
        BOOST_CHECK(!registry.validators.contains(key_d));                      // cancelled
    }

    // ------------------------- Phase 7: FN burns (test convention only)
    std::array<unsigned char, 32> fn_key_1{};
    fn_key_1.fill(0x11);
    std::array<unsigned char, 32> fn_key_2{};
    fn_key_2.fill(0x22);
    const auto burn_from{[&](const uint32_t fund_n, const CAmount burn_value,
                             const std::array<unsigned char, 32>& claim_key, const bool spendable_fake) {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint{crossing_txid, fund_n};
        tx.vin[0].scriptSig = CScript{};
        if (spendable_fake) {
            // Fake burn: carries the marker but is spendable (no OP_RETURN).
            std::vector<unsigned char> payload{FN_MARKER};
            payload.insert(payload.end(), claim_key.begin(), claim_key.end());
            tx.vout.emplace_back(burn_value, CScript() << payload << OP_DROP << OP_TRUE);
        } else {
            tx.vout.emplace_back(burn_value, MakeFnBurnScript(claim_key));
        }
        tx.vout.emplace_back(900'000 - burn_value - 1000, CScript() << OP_TRUE);
        return tx;
    }};
    const CMutableTransaction fn_valid{burn_from(5, FN_TEST_COST, fn_key_1, false)};
    const CMutableTransaction fn_wrong_amount{burn_from(6, FN_TEST_COST - 10'000, fn_key_2, false)};
    const CMutableTransaction fn_fake{burn_from(7, FN_TEST_COST, fn_key_2, true)};
    const CMutableTransaction fn_valid_2{burn_from(8, FN_TEST_COST, fn_key_2, false)};
    const Txid fn_valid_txid{CTransaction{fn_valid}.GetHash()};
    submit_corridor({fn_valid, fn_wrong_amount, fn_fake, fn_valid_2});
    {
        LOCK(cs_main);
        const auto fn_set{DeriveFnObjects(cm(), EVO_H + 1)};
        BOOST_REQUIRE_EQUAL(fn_set.size(), 2U); // valid + valid_2; wrong amount and fake create nothing
        BOOST_CHECK(fn_set.contains(COutPoint{fn_valid_txid, 0}));
        BOOST_CHECK(fn_set.at(COutPoint{fn_valid_txid, 0}).claim_key == fn_key_1);
        // Deriving again never doubles an FN from the same burn.
        const auto fn_again{DeriveFnObjects(cm(), EVO_H + 1)};
        BOOST_CHECK_EQUAL(fn_again.size(), fn_set.size());
    }

    // ----------------- Phase 6: colored assets (model level, test flag)
    // The asset engine is header-only model machinery gated behind the
    // test-only activation, per the locked sequencing (nothing wires into
    // consensus before a clean H+1). AssetIds derive from REAL chain
    // outpoints of this evolution chain.
    mutable_consensus.test_only_asset_policies_active = true;
    {
        const int model_height{tip()->nHeight};
        const COutPoint defining{crossing_txid, 9};
        const modern::AssetId test_token{modern::IssuanceAssetId(defining)};
        const modern::AssetId test_usdt{modern::IssuanceAssetId(COutPoint{crossing_txid, 10})};
        BOOST_CHECK(test_token != test_usdt); // distinct defining outpoints, distinct ids

        const auto out{[&](const modern::AssetId& asset, const CAmount amount,
                           const modern::PolicyType type = modern::PolicyType::OWNER) {
            modern::ModernOutput o;
            o.asset = asset;
            o.amount = amount;
            o.policy_type = static_cast<uint16_t>(type);
            o.policy_version = modern::POLICY_VERSION_V1;
            if (type == modern::PolicyType::OWNER) o.policy_commitment = uint256::ONE;
            return o;
        }};
        const auto spend{[&](const COutPoint& first) {
            modern::ModernTransition t;
            t.inputs.resize(1);
            t.inputs[0].prevout = first;
            return t;
        }};

        // Issuance: 1,000,000 TEST_TOKEN in two outputs.
        modern::ModernTransition issue{spend(defining)};
        issue.outputs.push_back(out(test_token, 600'000));
        issue.outputs.push_back(out(test_token, 400'000));
        const std::vector<modern::ModernOutput> native_prev{out(modern::NativeAsset(), 900'000)};
        BOOST_CHECK(modern::CheckAssetConservation(native_prev, issue, model_height, consensus) ==
                    modern::AssetCheck::OK);

        // Transfer conserves exactly; split and merge are transfers too.
        const std::vector<modern::ModernOutput> token_prev{out(test_token, 600'000)};
        modern::ModernTransition transfer{spend(COutPoint{crossing_txid, 11})};
        transfer.outputs.push_back(out(test_token, 600'000));
        BOOST_CHECK(modern::CheckAssetConservation(token_prev, transfer, model_height, consensus) ==
                    modern::AssetCheck::OK);
        modern::ModernTransition split_t{spend(COutPoint{crossing_txid, 12})};
        split_t.outputs.push_back(out(test_token, 200'000));
        split_t.outputs.push_back(out(test_token, 250'000));
        split_t.outputs.push_back(out(test_token, 150'000));
        BOOST_CHECK(modern::CheckAssetConservation(token_prev, split_t, model_height, consensus) ==
                    modern::AssetCheck::OK);
        modern::ModernTransition merge_t{spend(COutPoint{crossing_txid, 13})};
        merge_t.inputs.resize(3);
        merge_t.inputs[1].prevout = COutPoint{crossing_txid, 14};
        merge_t.inputs[1].proof_index = 1;
        merge_t.inputs[2].prevout = COutPoint{crossing_txid, 15};
        merge_t.inputs[2].proof_index = 2;
        const std::vector<modern::ModernOutput> merge_prev{
            out(test_token, 200'000), out(test_token, 250'000), out(test_token, 150'000)};
        merge_t.outputs.push_back(out(test_token, 600'000));
        BOOST_CHECK(modern::CheckAssetConservation(merge_prev, merge_t, model_height, consensus) ==
                    modern::AssetCheck::OK);

        // Burn is exact and visible; silent loss is a mismatch; reissuance
        // is an unauthorized mint.
        modern::ModernTransition burn_t{spend(COutPoint{crossing_txid, 16})};
        burn_t.outputs.push_back(out(test_token, 500'000));
        burn_t.outputs.push_back(out(test_token, 100'000, modern::PolicyType::BURN));
        BOOST_CHECK(modern::CheckAssetConservation(token_prev, burn_t, model_height, consensus) ==
                    modern::AssetCheck::OK);
        modern::ModernTransition lossy{spend(COutPoint{crossing_txid, 17})};
        lossy.outputs.push_back(out(test_token, 500'000));
        BOOST_CHECK(modern::CheckAssetConservation(token_prev, lossy, model_height, consensus) ==
                    modern::AssetCheck::CONSERVATION_MISMATCH);
        modern::ModernTransition reissue{spend(COutPoint{crossing_txid, 18})};
        reissue.outputs.push_back(out(test_token, 1));
        BOOST_CHECK(modern::CheckAssetConservation(native_prev, reissue, model_height, consensus) ==
                    modern::AssetCheck::UNAUTHORIZED_MINT);

        // Off flag: everything fails closed again.
        mutable_consensus.test_only_asset_policies_active = false;
        BOOST_CHECK(modern::CheckAssetConservation(native_prev, issue, model_height, consensus) ==
                    modern::AssetCheck::NOT_ACTIVE);
        mutable_consensus.test_only_asset_policies_active = true;
    }

    // ------------------------------------ march to the corridor's end
    // A late stake for C lands five blocks before the end: PENDING at the
    // handoff.
    while (tip()->nHeight < EVO_H + EVO_CORRIDOR - 5) submit_corridor({});
    const CMutableTransaction stake_c{stake_from(4, {{key_c, 60'000}})};
    submit_corridor({stake_c});
    while (tip()->nHeight < EVO_H + EVO_CORRIDOR) submit_corridor({});
    BOOST_REQUIRE_EQUAL(tip()->nHeight, EVO_H + EVO_CORRIDOR);

    // ------------------- Phase 8: deterministic registry at the handoff
    const auto expect_registry{[&](const node::StakeRegistry& registry) {
        BOOST_REQUIRE_EQUAL(registry.validators.size(), 3U);
        BOOST_CHECK_EQUAL(registry.validators.at(key_a).total_weight, 150'000);
        BOOST_CHECK_EQUAL(registry.validators.at(key_a).outputs.size(), 2U);
        BOOST_CHECK_EQUAL(registry.validators.at(key_b).total_weight, 100'000);
        BOOST_CHECK_EQUAL(registry.validators.at(key_b).outputs.size(), 3U);
        BOOST_CHECK_EQUAL(registry.validators.at(key_c).total_weight, 0); // late: pending
        BOOST_CHECK(!registry.validators.contains(key_d));                // cancelled
        BOOST_CHECK_EQUAL(registry.total_weight, 250'000);
    }};
    uint256 commitment_at_end;
    node::StakeRegistry registry_a;
    {
        LOCK(cs_main);
        cm().ActiveChainstate().ForceFlushStateToDisk();
        commitment_at_end = node::UtxoSetCommitment(
            node::EnumerateUtxos(cm().ActiveChainstate().CoinsDB()));
        BOOST_CHECK(commitment_at_end != commitment_at_h);
        registry_a = node::DeriveStakeRegistry(cm().ActiveChainstate().CoinsDB(),
                                               EVO_H + EVO_CORRIDOR, consensus);
        expect_registry(registry_a);
    }
    const uint256 tip_at_end{tip()->GetBlockHash()};

    // Restart: same tip, same commitment, same registry.
    m_node.chainman.reset();
    m_make_chainman();
    LoadVerifyActivateChainstate();
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(cm().ActiveChain().Height(), EVO_H + EVO_CORRIDOR);
        BOOST_CHECK_EQUAL(cm().ActiveChain().Tip()->GetBlockHash().GetHex(), tip_at_end.GetHex());
        cm().ActiveChainstate().ForceFlushStateToDisk();
        BOOST_CHECK_EQUAL(node::UtxoSetCommitment(node::EnumerateUtxos(
                              cm().ActiveChainstate().CoinsDB())).GetHex(),
                          commitment_at_end.GetHex());
        const node::StakeRegistry after_restart{node::DeriveStakeRegistry(
            cm().ActiveChainstate().CoinsDB(), EVO_H + EVO_CORRIDOR, consensus)};
        expect_registry(after_restart);
        BOOST_CHECK(after_restart.WeightView() == registry_a.WeightView());
    }

    // Reindex: rebuild the chainstate from the block files across all three
    // phases; identical result.
    m_node.chainman.reset();
    m_args.ForceSetArg("-reindex-chainstate", "1");
    m_make_chainman();
    LoadVerifyActivateChainstate();
    m_args.ForceSetArg("-reindex-chainstate", "0");
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(cm().ActiveChain().Height(), EVO_H + EVO_CORRIDOR);
        cm().ActiveChainstate().ForceFlushStateToDisk();
        BOOST_CHECK_EQUAL(node::UtxoSetCommitment(node::EnumerateUtxos(
                              cm().ActiveChainstate().CoinsDB())).GetHex(),
                          commitment_at_end.GetHex());
        const node::StakeRegistry after_reindex{node::DeriveStakeRegistry(
            cm().ActiveChainstate().CoinsDB(), EVO_H + EVO_CORRIDOR, consensus)};
        expect_registry(after_reindex);
        // The FN derivation is deterministic across the reindex too.
        BOOST_CHECK_EQUAL(DeriveFnObjects(cm(), EVO_H + 1).size(), 2U);
    }

    // Node B: an empty second node syncs the whole evolution chain.
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
            const auto [status, error]{node::LoadChainstate(chainman_b, m_kernel_cache_sizes, b_load)};
            BOOST_REQUIRE_MESSAGE(status == node::ChainstateLoadStatus::SUCCESS, error.original);
            BlockValidationState state;
            BOOST_REQUIRE(chainman_b.ActiveChainstate().ActivateBestChain(state));
        }
        for (int height{1}; height <= EVO_H + EVO_CORRIDOR; ++height) {
            const CBlockIndex* pindex{WITH_LOCK(cs_main, return cm().ActiveChain()[height])};
            CBlock block;
            BOOST_REQUIRE(cm().m_blockman.ReadBlock(block, *pindex));
            bool new_block{false};
            BOOST_REQUIRE_MESSAGE(
                chainman_b.ProcessNewBlock(std::make_shared<const CBlock>(block), true, true, &new_block),
                "node B refused block at height " << height);
        }
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(chainman_b.ActiveChain().Height(), EVO_H + EVO_CORRIDOR);
        BOOST_CHECK_EQUAL(chainman_b.ActiveChain().Tip()->GetBlockHash().GetHex(), tip_at_end.GetHex());
        chainman_b.ActiveChainstate().ForceFlushStateToDisk();
        const node::UtxoComparison cmp{node::CompareUtxoViews(
            cm().ActiveChainstate().CoinsDB(), chainman_b.ActiveChainstate().CoinsDB())};
        BOOST_CHECK(cmp.Equal());
        const node::StakeRegistry registry_b{node::DeriveStakeRegistry(
            chainman_b.ActiveChainstate().CoinsDB(), EVO_H + EVO_CORRIDOR, consensus)};
        expect_registry(registry_b);
        BOOST_CHECK(registry_b.WeightView() == registry_a.WeightView());
    }

    // ----------------------- Phase 9: the modern PoS gate at H+corridor+1
    {
        CBlock modern_attempt{build_corridor({})};
        modern_attempt.nBits = GetNextWorkRequired(tip(), &modern_attempt, consensus);
        const arith_uint256 sha_target{arith_uint256().SetCompact(modern_attempt.nBits)};
        modern_attempt.nNonce = 0;
        while (UintToArith256(modern_attempt.GetHash()) > sha_target) ++modern_attempt.nNonce;
        {
            LOCK(cs_main);
            const BlockValidationState state{TestBlockValidity(
                cm().ActiveChainstate(), modern_attempt, /*check_pow=*/true, /*check_merkle_root=*/true)};
            BOOST_REQUIRE(state.IsInvalid());
            BOOST_CHECK_EQUAL(state.GetRejectReason(), "no-modern-pos-rules");
        }
        bool new_block{false};
        cm().ProcessNewBlock(std::make_shared<const CBlock>(modern_attempt), true, true, &new_block);
        LOCK(cs_main);
        const CBlockIndex* pindex{cm().m_blockman.LookupBlockIndex(modern_attempt.GetHash())};
        BOOST_REQUIRE(pindex != nullptr);
        BOOST_CHECK(pindex->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(!cm().ActiveChain().Contains(pindex));
        BOOST_CHECK_EQUAL(cm().ActiveChain().Tip()->GetBlockHash().GetHex(), tip_at_end.GetHex());
    }

    // The failed attempt corrupts nothing: one more restart stays clean.
    {
        LOCK(cs_main);
        cm().ActiveChainstate().ForceFlushStateToDisk();
    }
    m_node.chainman.reset();
    m_make_chainman();
    LoadVerifyActivateChainstate();
    {
        LOCK(cs_main);
        BOOST_REQUIRE_EQUAL(cm().ActiveChain().Height(), EVO_H + EVO_CORRIDOR);
        BOOST_CHECK_EQUAL(cm().ActiveChain().Tip()->GetBlockHash().GetHex(), tip_at_end.GetHex());
        BOOST_CHECK(cm().ActiveChainstate().LegacyBoundaryActive());
    }
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 8 of the Modern PoS V1 finality plan: MPA x4 weight, deterministic
// per-type payload verification costs, per-transaction / per-block cost
// budgets checked before cryptography, and policy vsize pricing of cost.

#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <modern/mpa.h>
#include <modern/payload_cost.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>

#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace b3test;

namespace {

CMpaRecord Rec(const uint16_t type, const size_t len, const unsigned char fill)
{
    CMpaRecord r;
    r.payload_type = type;
    r.payload_version = 1;
    r.payload.assign(len, fill);
    return r;
}

CMutableTransaction Plain()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"5555555555555555555555555555555555555555555555555555555555555555"}), 0};
    mtx.vout.emplace_back(100, CScript() << OP_TRUE);
    return mtx;
}

size_t SerSize(const CTransaction& tx, const TransactionSerParams& p)
{
    DataStream ss;
    ss << p(tx);
    return ss.size();
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(payload_cost_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(weight_rule_x4)
{
    const CTransaction plain{Plain()};
    BOOST_CHECK_EQUAL(GetTransactionWeight(plain), 4 * static_cast<int32_t>(SerSize(plain, TX_NO_WITNESS)));
    CMutableTransaction m{Plain()};
    m.mpa.push_back(Rec(5, 244, 0x11));
    const CTransaction t{m};
    const size_t section{GetMpaSectionSerializedSize(t.mpa)};
    BOOST_CHECK_EQUAL(section, 1u + 2 + 2 + 1 + 244);
    // TX_MODERN bytes = TX_WITH_WITNESS bytes + marker/flags(2) + section
    BOOST_CHECK_EQUAL(SerSize(t, TX_MODERN), SerSize(t, TX_WITH_WITNESS) + 2 + section);
    // weight = 3*base + witness-form + 4*section  (MPA at the full scale factor)
    BOOST_CHECK_EQUAL(GetTransactionWeight(t),
                      3 * static_cast<int32_t>(SerSize(t, TX_NO_WITNESS)) + static_cast<int32_t>(SerSize(t, TX_WITH_WITNESS)) +
                          static_cast<int32_t>(MPA_WEIGHT_FACTOR * section));
    BOOST_CHECK_EQUAL(GetTransactionWeight(t) - GetTransactionWeight(plain), static_cast<int32_t>(4 * section));
    // witness + MPA: witness keeps its x1 accounting, MPA x4
    CMutableTransaction wm{m};
    wm.vin[0].scriptWitness.stack.push_back(std::vector<unsigned char>(10, 0xAA));
    const CTransaction twm{wm};
    const int32_t witness_bytes{static_cast<int32_t>(SerSize(twm, TX_WITH_WITNESS) - SerSize(twm, TX_NO_WITNESS))};
    BOOST_CHECK_EQUAL(GetTransactionWeight(twm), 4 * static_cast<int32_t>(SerSize(twm, TX_NO_WITNESS)) + witness_bytes +
                                                     static_cast<int32_t>(4 * section));
    // block weight sums the MPA term over all transactions
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(plain));
    block.vtx.push_back(MakeTransactionRef(t));
    block.vtx.push_back(MakeTransactionRef(t));
    BOOST_CHECK_EQUAL(GetBlockWeight(block),
                      3 * static_cast<int64_t>(::GetSerializeSize(TX_NO_WITNESS(block))) +
                          static_cast<int64_t>(::GetSerializeSize(TX_WITH_WITNESS(block))) + 2 * 4 * static_cast<int64_t>(section));
}

BOOST_AUTO_TEST_CASE(deterministic_costs_and_budgets)
{
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(5, 1), 700);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(4, 1), 2000);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(1, 1), 0);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(5, 2), 0);
    BOOST_CHECK_EQUAL(modern::PayloadRecordVerifyCost(9, 1), 0);
    BOOST_CHECK_EQUAL(MAX_TX_PAYLOAD_COST, 12000);
    BOOST_CHECK_EQUAL(MAX_BLOCK_PAYLOAD_COST, 120000);
    BOOST_CHECK_EQUAL(PAYLOAD_COST_TO_VBYTES, 1);
    std::string err;
    // per-transaction: 17 evidence records = 11,900 ok; 18 = 12,600 rejected; 6 certs = 12,000 ok; 7 rejected
    {
        CMutableTransaction m{Plain()};
        for (int i = 0; i < 17; ++i) m.mpa.push_back(Rec(5, 244, static_cast<unsigned char>(i)));
        BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(CTransaction{m}), 11900);
        BOOST_CHECK(modern::CheckTransactionPayloadCost(CTransaction{m}, err));
        m.mpa.push_back(Rec(5, 244, 0xFF));
        BOOST_CHECK(!modern::CheckTransactionPayloadCost(CTransaction{m}, err));
        BOOST_CHECK_EQUAL(err, "bad-payload-cost");
    }
    {
        CMutableTransaction m{Plain()};
        for (int i = 0; i < 6; ++i) m.mpa.push_back(Rec(4, 100, static_cast<unsigned char>(i)));
        BOOST_CHECK(modern::CheckTransactionPayloadCost(CTransaction{m}, err));
        m.mpa.push_back(Rec(4, 100, 0xFF));
        BOOST_CHECK(!modern::CheckTransactionPayloadCost(CTransaction{m}, err));
    }
    // CheckTransactionMpa enforces the per-tx budget (under the test activation)
    {
        Consensus::Params active{};
        active.legacy_b3coin = true;
        active.hard_fork_height = 100;
        active.legacy_final_hash = uint256::ONE;
        active.modern_pos = Consensus::ModernPosParams{};
        CMutableTransaction m{Plain()};
        for (int i = 0; i < 18; ++i) m.mpa.push_back(Rec(5, 244, static_cast<unsigned char>(i)));
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{m}, active, err));
        BOOST_CHECK_EQUAL(err, "bad-payload-cost");
    }
    // per-block: 171 evidence records = 119,700 ok; 172 = 120,400 rejected
    {
        CBlock block;
        block.vtx.push_back(MakeTransactionRef(Plain()));
        int added{0};
        while (added < 171) {
            CMutableTransaction m{Plain()};
            for (int i = 0; i < 17 && added < 171; ++i, ++added) m.mpa.push_back(Rec(5, 244, static_cast<unsigned char>(added)));
            block.vtx.push_back(MakeTransactionRef(m));
        }
        BOOST_CHECK_EQUAL(modern::BlockPayloadVerifyCost(block), 171 * 700);
        BOOST_CHECK(modern::CheckBlockPayloadCost(block, err));
        CMutableTransaction extra{Plain()};
        extra.mpa.push_back(Rec(5, 244, 0xEE));
        block.vtx.push_back(MakeTransactionRef(extra));
        BOOST_CHECK(!modern::CheckBlockPayloadCost(block, err));
        BOOST_CHECK_EQUAL(err, "bad-block-payload-cost");
    }
}

BOOST_AUTO_TEST_CASE(policy_vsize_prices_cost)
{
    const CTransaction plain{Plain()};
    BOOST_CHECK_EQUAL(GetVirtualTransactionSize(plain), (GetTransactionWeight(plain) + 3) / 4);
    CMutableTransaction m{Plain()};
    m.mpa.push_back(Rec(5, 244, 0x11));
    const CTransaction t{m};
    // weight/4 of a ~350-byte tx is far below the 700-vbyte cost price: vsize == cost
    BOOST_CHECK_EQUAL(GetVirtualTransactionSize(t), 700);
    BOOST_CHECK_EQUAL(GetVirtualTransactionSize(t, 0, 0), 700);
    // sigops and cost pricing compose through the same max()
    BOOST_CHECK_EQUAL(GetVirtualTransactionSize(t, /*sigops=*/100, /*bytes_per_sigop=*/50), std::max<int64_t>(700, (100 * 50 + 3) / 4));
    BOOST_CHECK_EQUAL(GetSigOpsAdjustedWeight(1000, 0, 0, 700), 2800);
    BOOST_CHECK_EQUAL(GetSigOpsAdjustedWeight(5000, 0, 0, 700), 5000);
    // a bigger-than-cost transaction is priced by its weight
    CMutableTransaction big{m};
    big.vout.emplace_back(1, CScript() << std::vector<unsigned char>(4000, 0x00));
    const CTransaction tb{big};
    BOOST_CHECK(GetVirtualTransactionSize(tb) > 700);
    BOOST_CHECK_EQUAL(GetVirtualTransactionSize(tb), (GetTransactionWeight(tb) + 3) / 4);
}

//! Block level: a block whose payload cost exceeds the budget is refused at
//! ContextualCheckBlock (Submit returns false) even though every record is
//! structurally well-formed and cryptographically INVALID — i.e. the budget is
//! enforced before any BLS/BIP340 verification. The same evidence under budget
//! reaches ConnectBlock and is refused there (Submit true, tip unmoved).
BOOST_FIXTURE_TEST_CASE(block_budget_checked_before_crypto, BindingFixture)
{
    Prepare();
    const auto k{Bls(1)};
    // 11 transactions x 17 pairs = 187 pairs = 130,900 > 120,000 (each tx at
    // 11,900 <= 12,000). All evidence is signed by the wrong identity: valid
    // structure, invalid crypto. Each tx spends a distinct fund output.
    std::vector<CMutableTransaction> txs;
    unsigned seed{0x30};
    for (int t = 0; t < 11; ++t) {
        std::vector<CScript> cells;
        std::vector<CMpaRecord> records;
        for (int i = 0; i < 17; ++i) {
            const CKey id{MakeValidatorKey(static_cast<unsigned char>(seed++))};
            const auto vk{XOnly(id)};
            const auto pair{MakeBinding(id, vk, &k, 0, /*signer=*/&m_validator_b)}; // wrong signer
            cells.push_back(pair.cell);
            records.push_back(pair.record);
        }
        txs.push_back(MakeTx(t, cells, records));
    }
    const int before{Tip()->nHeight};
    BOOST_CHECK(!SubmitCorridor(txs, 60)); // refused in ContextualCheckBlock: over budget
    BOOST_CHECK_EQUAL(Tip()->nHeight, before);
    // Under budget (10 x 17 = 170 pairs = 119,000): passes the structural/budget
    // layer (Submit stores it) and fails at connect on the bad signatures.
    txs.pop_back();
    (void)SubmitCorridor(txs, 61);
    BOOST_CHECK_EQUAL(Tip()->nHeight, before);
    // A single valid pair still connects (the fixture is healthy).
    const auto good{MakeBinding(m_validator_a, m_vk_a, &k, 0)};
    BOOST_CHECK(SubmitCorridor({MakeTx(0, {good.cell}, {good.record})}, 62));
    BOOST_CHECK_EQUAL(Tip()->nHeight, before + 1);
}

BOOST_AUTO_TEST_SUITE_END()

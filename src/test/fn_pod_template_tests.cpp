// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Block assembly's modern FN PoD filter (node::BlockAssembler::
// TestChunkTransactions) on a real modern-PoS chain. Mempool admission
// takes every type-6 declaration that names the tip's next slot, so two of
// them for the same slot can wait side by side (fn_pod_mempool_tests). A
// block may carry only one per slot, and none once the lifetime capacity is
// spent. CreateNewBlock checks its template with TestBlockValidity and
// throws when it selected a transaction the block rule refuses, so a filter
// that let such a pair through would stop every producer holding it, at no
// cost to whoever sent the pair while no block carries it.
//
// FinalityChainFixture's chain (validators A and B) gains FN PoD and simple
// assets from M + 1, with as many historical FN rights as the case needs:
// one right leaves a capacity of 4,999 modern creations, 4,999 leave one.
// No chain output holds a PoD's 15,000 B3, so each declaration spends a coin
// added straight to the tip's coins view.

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/era.h>
#include <consensus/fn_params.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <kernel/mempool_removal_reason.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/fn_pod.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <node/miner.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using b3test::FinalityChainFixture;

//! Each declaration's ordinary fee, on top of the destroyed amount. This
//! chain's modern subsidy is zero, so a template's coinbase claims the fees
//! alone.
constexpr CAmount POD_FEE{10'000};

struct FnPodTemplateFixture : public FinalityChainFixture {
    uint32_t m_next_coin{0};

    const Consensus::Params& Params() { return m_node.chainman->GetConsensus(); }

    //! The chain to M, with FN PoD and assets active from M + 1 and
    //! `rights` historical FN rights.
    void PrepareFnPodChain(const uint32_t rights)
    {
        PrepareFinalityChain();
        Consensus::Params& c{MutableConsensus()};
        c.fn_genesis_manifest.clear();
        for (uint32_t i{0}; i < rights; ++i) {
            Consensus::FnGenesisRight right;
            WriteBE32(right.pod_id.begin(), i + 1);
            right.recipient_key_hash.fill(static_cast<unsigned char>(i & 0xff));
            c.fn_genesis_manifest.push_back(right);
        }
        c.fn_genesis_rights_root = uint256::ONE;
        c.fn_pod_activation_height = m_M + 1;
        c.asset_activation_height = m_M + 1;
        BOOST_REQUIRE(!Consensus::FnPodRulesActive(m_M, c));
        BOOST_REQUIRE(Consensus::FnPodRulesActive(m_M + 1, c));
        BOOST_REQUIRE_EQUAL(modern::ModernFnCapacity(c).value(),
                            Consensus::MAX_FN_EVER_ISSUED - rights);
        Produce(m_vk_a);
        BOOST_REQUIRE_EQUAL(Tip()->nHeight, m_M);
    }

    //! A native coin the tip's coins view holds as if a block had made it.
    COutPoint FundCoin(const CAmount value)
    {
        uint256 hash;
        WriteBE32(hash.begin(), 0x0f0d0000U + m_next_coin++);
        const COutPoint coin{Txid::FromUint256(hash), 0};
        const int height{Tip()->nHeight};
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().CoinsTip().AddCoin(
            coin, Coin{CTxOut{value, CScript() << OP_TRUE}, height, /*fCoinBaseIn=*/false},
            /*possible_overwrite=*/false);
        return coin;
    }

    //! A declaration of `slot` creating one FN unit at output 1: it spends
    //! a fresh coin, destroys the slot's price and pays POD_FEE.
    CMutableTransaction Pod(const uint32_t slot)
    {
        const auto fn_asset{modern::ConfiguredFnAssetId(Params())};
        BOOST_REQUIRE(fn_asset);
        const auto fn_output{modern::MakeAssetOwnerOutput(
            *fn_asset, 1, modern::PolicyType::FN, CScript() << OP_TRUE)};
        BOOST_REQUIRE(fn_output);
        constexpr CAmount change{10 * COIN};
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.resize(1);
        tx.vin[0].prevout =
            FundCoin(modern::RequiredFnPodDisintegration(slot) + POD_FEE + change);
        tx.vout.emplace_back(change, CScript() << std::vector<unsigned char>(24, 0xc4)
                                               << OP_DROP << OP_TRUE);
        tx.vout.push_back(*fn_output);
        tx.mpa.push_back(modern::MakeModernFnPodRecord(slot, 1));
        return tx;
    }

    //! Mempool admission at the tip: "" or "reason, debug".
    std::string Admit(const CMutableTransaction& tx)
    {
        LOCK(cs_main);
        const MempoolAcceptResult result{
            m_node.chainman->ProcessTransaction(MakeTransactionRef(tx))};
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID
                   ? std::string{}
                   : result.m_state.ToString();
    }

    //! Straight into the mempool, as if admitted under other rules (an older
    //! binary, or test and debug code).
    void Inject(const CMutableTransaction& tx)
    {
        TryAddToMempool(*m_node.mempool,
                        TestMemPoolEntryHelper{}.Fee(POD_FEE).FromTx(MakeTransactionRef(tx)));
    }

    void Remove(const CMutableTransaction& tx)
    {
        CTxMemPool& pool{*m_node.mempool};
        WITH_LOCK(pool.cs, pool.removeRecursive(CTransaction{tx}, MemPoolRemovalReason::REPLACED));
    }

    /**
     * node::BlockAssembler for validator A over the mempool. Returns "" and
     * fills `carried` (the template's transactions after the coinbase) and
     * `coinbase_value`, or CreateNewBlock's exception text.
     */
    std::string Assemble(std::vector<CTransactionRef>& carried, CAmount& coinbase_value)
    {
        carried.clear();
        coinbase_value = 0;
        const int64_t next{BuildPosBlock(m_vk_a).GetBlockTime()};
        if (next > GetTime()) SetMockTime(next);
        node::BlockAssembler::Options options;
        options.coinbase_output_script = CScript() << OP_TRUE;
        options.include_dummy_extranonce = true;
        options.modern_pos_validator_key = m_vk_a;
        BOOST_REQUIRE(options.test_block_validity);
        try {
            const auto tmpl{node::BlockAssembler(m_node.chainman->ActiveChainstate(),
                                                 m_node.mempool.get(), options)
                                .CreateNewBlock()};
            BOOST_REQUIRE(tmpl);
            carried.assign(tmpl->block.vtx.begin() + 1, tmpl->block.vtx.end());
            coinbase_value = tmpl->block.vtx[0]->GetValueOut();
        } catch (const std::runtime_error& e) {
            return e.what();
        }
        return {};
    }
};

bool Carries(const std::vector<CTransactionRef>& carried, const CMutableTransaction& tx)
{
    const Txid txid{CTransaction{tx}.GetHash()};
    return std::any_of(carried.begin(), carried.end(),
                       [&](const CTransactionRef& in) { return in->GetHash() == txid; });
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(fn_pod_template_tests, FnPodTemplateFixture)

BOOST_AUTO_TEST_CASE(a_template_carries_one_creation_per_slot)
{
    PrepareFnPodChain(/*rights=*/1);
    // Two declarations of slot 0, each admissible on its own.
    const CMutableTransaction first{Pod(0)};
    const CMutableTransaction second{Pod(0)};
    BOOST_REQUIRE_EQUAL(Admit(first), "");
    BOOST_REQUIRE_EQUAL(Admit(second), "");

    std::vector<CTransactionRef> carried;
    CAmount coinbase_value{0};
    BOOST_REQUIRE_EQUAL(Assemble(carried, coinbase_value), "");
    BOOST_CHECK_EQUAL(carried.size(), 1U);
    BOOST_CHECK(Carries(carried, first) != Carries(carried, second));
    // The coinbase claims the carried declaration's fee, none of its
    // destroyed amount.
    BOOST_CHECK_EQUAL(coinbase_value, POD_FEE);
}

BOOST_AUTO_TEST_CASE(a_template_skips_declarations_the_block_rule_refuses)
{
    PrepareFnPodChain(/*rights=*/Consensus::MAX_FN_EVER_ISSUED - 1);
    std::vector<CTransactionRef> carried;
    CAmount coinbase_value{0};

    // Two declarations in one transaction, each naming its own FN unit. Both
    // name the next slot, so only the declaration count refuses it.
    CMutableTransaction twice{Pod(0)};
    const CTxOut fn_unit{twice.vout[1]};
    twice.vout.push_back(fn_unit);
    twice.mpa.push_back(modern::MakeModernFnPodRecord(0, 2));
    std::sort(twice.mpa.begin(), twice.mpa.end(), modern::MpaRecordLess);
    BOOST_CHECK_EQUAL(Admit(twice), "bad-asset-transition, modern FN PoD transaction must "
                                    "have exactly one declaration");
    Inject(twice);
    BOOST_CHECK_EQUAL(Assemble(carried, coinbase_value), "");
    BOOST_CHECK(carried.empty());
    Remove(twice);

    // The one modern slot, confirmed: the capacity is spent.
    Produce(m_vk_a, {}, {Pod(0)});
    BOOST_REQUIRE_EQUAL(Tip()->m_fn_pod_issued_total, 1U);
    const CMutableTransaction beyond{Pod(1)};
    BOOST_CHECK_EQUAL(Admit(beyond), "bad-asset-transition, FN lifetime issuance cap is exhausted");
    Inject(beyond);
    BOOST_CHECK_EQUAL(Assemble(carried, coinbase_value), "");
    BOOST_CHECK(carried.empty());
    Remove(beyond);
}

BOOST_AUTO_TEST_SUITE_END()

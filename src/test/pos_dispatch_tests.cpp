// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Tests for the legacy/modern proof-of-stake dispatch boundary. No modern
//! PoS semantics exist; only the boundary, its fail-closed default and the
//! test-adapter plumbing are exercised.

#include <modern/pos.h>

#include <chain.h>
#include <coins.h>
#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/validation.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(pos_dispatch_tests)

namespace {

constexpr int32_t LEGACY_VERSION{4};
constexpr int32_t MODERN_VERSION{static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION)};

CBlock ModernBlock()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000dd"}), 0};
    mtx.vout.emplace_back(1, CScript() << OP_TRUE);

    CBlock block;
    block.nVersion = MODERN_VERSION;
    block.vtx.push_back(MakeTransactionRef(std::move(mtx)));
    return block;
}

//! Test adapter: records the dispatch and accepts.
class RecordingValidator final : public modern::PosValidator
{
public:
    mutable int m_calls{0};
    bool CheckStake(const CBlock&, const CBlockIndex&, const CCoinsViewCache&,
                    BlockValidationState&) const override
    {
        ++m_calls;
        return true;
    }
};

struct ValidatorGuard {
    explicit ValidatorGuard(const modern::PosValidator* v) { modern::SetModernPosValidatorForTesting(v); }
    ~ValidatorGuard() { modern::SetModernPosValidatorForTesting(nullptr); }
};

} // namespace

BOOST_AUTO_TEST_CASE(stake_rules_follow_marker_and_era)
{
    using modern::SelectStakeRules;
    using modern::StakeRules;
    using Consensus::B3Era;

    // Legacy blocks are judged by legacy rules only; modern blocks by
    // modern rules only; a marker/era mismatch belongs to neither.
    BOOST_CHECK(SelectStakeRules(LEGACY_VERSION, B3Era::LEGACY) == StakeRules::LEGACY);
    BOOST_CHECK(SelectStakeRules(MODERN_VERSION, B3Era::MODERN) == StakeRules::MODERN);
    BOOST_CHECK(SelectStakeRules(MODERN_VERSION, B3Era::LEGACY) == StakeRules::MISMATCH);
    BOOST_CHECK(SelectStakeRules(LEGACY_VERSION, B3Era::MODERN) == StakeRules::MISMATCH);
}

BOOST_AUTO_TEST_CASE(modern_pos_fails_closed_without_a_rule_set)
{
    const CBlock block{ModernBlock()};
    CBlockIndex parent{CBlockHeader{}};
    CCoinsView base;
    CCoinsViewCache view{&base};
    BlockValidationState state;

    // No rule set installed: every modern block is rejected, with the
    // documented reason.
    BOOST_CHECK(!modern::CheckModernStake(block, parent, view, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "no-modern-pos-rules");
}

BOOST_AUTO_TEST_CASE(modern_pos_consumes_only_modern_codecs)
{
    CBlockIndex parent{CBlockHeader{}};
    CCoinsView base;
    CCoinsViewCache view{&base};
    RecordingValidator recorder;
    const ValidatorGuard guard{&recorder};

    // A legacy-codec block never reaches the installed rule set.
    {
        CBlock legacy_block{ModernBlock()};
        legacy_block.nVersion = LEGACY_VERSION;
        BlockValidationState state;
        BOOST_CHECK(!modern::CheckModernStake(legacy_block, parent, view, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pos-codec");
        BOOST_CHECK_EQUAL(recorder.m_calls, 0);
    }

    // A legacy-encoded transaction inside a marker-modern block never
    // reaches the rule set either.
    {
        CBlock block{ModernBlock()};
        CMutableTransaction legacy_tx{*block.vtx[0]};
        legacy_tx.nTime = 12345;
        legacy_tx.m_legacy_encoding = true;
        block.vtx[0] = MakeTransactionRef(std::move(legacy_tx));
        BlockValidationState state;
        BOOST_CHECK(!modern::CheckModernStake(block, parent, view, state));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pos-codec");
        BOOST_CHECK_EQUAL(recorder.m_calls, 0);
    }

    // A fully modern block is dispatched to the installed adapter exactly
    // once.
    {
        const CBlock block{ModernBlock()};
        BlockValidationState state;
        BOOST_CHECK(modern::CheckModernStake(block, parent, view, state));
        BOOST_CHECK_EQUAL(recorder.m_calls, 1);
    }
}

BOOST_AUTO_TEST_SUITE_END()

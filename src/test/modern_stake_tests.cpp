// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/params.h>
#include <modern/policy.h>
#include <modern/stake.h>
#include <node/stake_registry.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <string>

using modern::ClaimsStakeMagic;
using modern::MakeStakeScript;
using modern::ParseStakeOutput;
using modern::STAKE_PAYLOAD_SIZE;

namespace {
std::array<unsigned char, 32> TestKey(const unsigned char fill = 0x11)
{
    std::array<unsigned char, 32> key{};
    key.fill(fill);
    return key;
}
} // namespace

BOOST_AUTO_TEST_SUITE(modern_stake_tests)

BOOST_AUTO_TEST_CASE(stake_policy_type_is_appended_and_active)
{
    // Values are consensus-stable; STAKE is appended at 4 and active in v1.
    BOOST_CHECK_EQUAL(static_cast<uint16_t>(modern::PolicyType::STAKE), 4U);
    BOOST_CHECK(modern::IsActivatedPolicy(4, modern::POLICY_VERSION_V1, /*assets_active=*/false));
    BOOST_CHECK(!modern::IsActivatedPolicy(4, modern::POLICY_VERSION_V1 + 1, false));
}

BOOST_AUTO_TEST_CASE(make_parse_round_trip)
{
    const CScript owner{CScript() << std::vector<unsigned char>(24, 0xc4) << OP_DROP << OP_TRUE};
    const CScript script{MakeStakeScript(TestKey(), owner)};
    BOOST_REQUIRE(ClaimsStakeMagic(script));

    std::string error;
    const auto view{ParseStakeOutput(CTxOut{50 * 1'000'000, script}, error)};
    BOOST_REQUIRE_MESSAGE(view.has_value(), error);
    BOOST_CHECK_EQUAL(view->amount, 50 * 1'000'000);
    BOOST_CHECK(view->validator_key == TestKey());
    BOOST_CHECK(view->owner_script == owner);
}

BOOST_AUTO_TEST_CASE(non_claiming_scripts_are_ordinary)
{
    // Wrong payload size, wrong magic, ordinary scripts: no claim, no rule.
    BOOST_CHECK(!ClaimsStakeMagic(CScript() << OP_TRUE));
    BOOST_CHECK(!ClaimsStakeMagic(CScript() << std::vector<unsigned char>(24, 0xb3) << OP_DROP << OP_TRUE));
    std::vector<unsigned char> wrong_magic(STAKE_PAYLOAD_SIZE, 0x00);
    wrong_magic[0] = 'X';
    BOOST_CHECK(!ClaimsStakeMagic(CScript() << wrong_magic << OP_DROP << OP_TRUE));
}

BOOST_AUTO_TEST_CASE(claiming_but_malformed_outputs_are_invalid)
{
    const CScript owner{CScript() << OP_TRUE};
    std::string error;

    // Zero validator key.
    {
        const CScript script{MakeStakeScript(std::array<unsigned char, 32>{}, owner)};
        BOOST_REQUIRE(ClaimsStakeMagic(script));
        BOOST_CHECK(!ParseStakeOutput(CTxOut{1000, script}, error));
        BOOST_CHECK_EQUAL(error, "stake validator key is zero");
    }
    // Zero principal.
    {
        const CScript script{MakeStakeScript(TestKey(), owner)};
        BOOST_CHECK(!ParseStakeOutput(CTxOut{0, script}, error));
        BOOST_CHECK_EQUAL(error, "stake principal must be positive");
    }
    // Missing OP_DROP after the payload.
    {
        std::vector<unsigned char> payload(STAKE_PAYLOAD_SIZE, 0x22);
        std::copy(modern::STAKE_MAGIC.begin(), modern::STAKE_MAGIC.end(), payload.begin());
        payload[STAKE_PAYLOAD_SIZE - 1] = 0;
        payload[STAKE_PAYLOAD_SIZE - 2] = 0;
        const CScript script{CScript() << payload << OP_TRUE};
        BOOST_REQUIRE(ClaimsStakeMagic(script));
        BOOST_CHECK(!ParseStakeOutput(CTxOut{1000, script}, error));
        BOOST_CHECK_EQUAL(error, "stake payload not followed by OP_DROP");
    }
    // Missing owner script.
    {
        std::vector<unsigned char> payload(STAKE_PAYLOAD_SIZE, 0x22);
        std::copy(modern::STAKE_MAGIC.begin(), modern::STAKE_MAGIC.end(), payload.begin());
        payload[STAKE_PAYLOAD_SIZE - 1] = 0;
        payload[STAKE_PAYLOAD_SIZE - 2] = 0;
        const CScript script{CScript() << payload << OP_DROP};
        BOOST_REQUIRE(ClaimsStakeMagic(script));
        BOOST_CHECK(!ParseStakeOutput(CTxOut{1000, script}, error));
        BOOST_CHECK_EQUAL(error, "stake output has no owner script");
    }
    // A PUSHDATA1-encoded payload claims but is not minimally encoded:
    // exactly one byte representation is valid.
    {
        std::vector<unsigned char> payload(STAKE_PAYLOAD_SIZE, 0x22);
        std::copy(modern::STAKE_MAGIC.begin(), modern::STAKE_MAGIC.end(), payload.begin());
        payload[STAKE_PAYLOAD_SIZE - 1] = 0;
        payload[STAKE_PAYLOAD_SIZE - 2] = 0;
        CScript script;
        script.push_back(OP_PUSHDATA1);
        script.push_back(static_cast<unsigned char>(STAKE_PAYLOAD_SIZE));
        script.insert(script.end(), payload.begin(), payload.end());
        script << OP_DROP << OP_TRUE;
        BOOST_REQUIRE(ClaimsStakeMagic(script));
        BOOST_CHECK(!ParseStakeOutput(CTxOut{1000, script}, error));
        BOOST_CHECK_EQUAL(error, "stake payload not minimally encoded");
    }
    // Non-zero reserved bytes.
    {
        std::vector<unsigned char> payload(STAKE_PAYLOAD_SIZE, 0x22);
        std::copy(modern::STAKE_MAGIC.begin(), modern::STAKE_MAGIC.end(), payload.begin());
        payload[STAKE_PAYLOAD_SIZE - 1] = 0x01;
        const CScript script{CScript() << payload << OP_DROP << OP_TRUE};
        BOOST_REQUIRE(ClaimsStakeMagic(script));
        BOOST_CHECK(!ParseStakeOutput(CTxOut{1000, script}, error));
        BOOST_CHECK_EQUAL(error, "stake reserved bytes must be zero");
    }

    // CheckStakeOutputs flags the malformed claim inside a transaction while
    // accepting valid and non-claiming outputs around it; duplicate
    // validator identities in one transaction are valid (aggregation, not
    // conflict); an unconfigured or unmet minimum fails closed.
    Consensus::Params params;
    params.min_stake_amount = 500;
    CMutableTransaction tx;
    tx.vout.emplace_back(1000, CScript() << OP_TRUE);
    tx.vout.emplace_back(1000, MakeStakeScript(TestKey(), owner));
    tx.vout.emplace_back(1000, MakeStakeScript(TestKey(), owner)); // duplicate key: valid
    BOOST_CHECK(modern::CheckStakeOutputs(CTransaction{tx}, params, error));
    {
        Consensus::Params unconfigured;
        BOOST_CHECK(!modern::CheckStakeOutputs(CTransaction{tx}, unconfigured, error));
        BOOST_CHECK_EQUAL(error, "stake minimum amount is not configured");
    }
    tx.vout.emplace_back(499, MakeStakeScript(TestKey(), owner)); // below minimum
    BOOST_CHECK(!modern::CheckStakeOutputs(CTransaction{tx}, params, error));
    BOOST_CHECK_EQUAL(error, "stake principal below the configured minimum");
    tx.vout.pop_back();
    tx.vout.emplace_back(0, MakeStakeScript(TestKey(), owner)); // zero principal
    BOOST_CHECK(!modern::CheckStakeOutputs(CTransaction{tx}, params, error));
}

BOOST_AUTO_TEST_CASE(stake_activation_depth_exact_boundary)
{
    // h - b >= 20, exactly: created at b, immature through b+19, mature at
    // b+20. Written against the constant so a drift in either fails here.
    BOOST_CHECK_EQUAL(modern::STAKE_ACTIVATION_DEPTH, 20);
    constexpr int b{1'000};
    BOOST_CHECK(!modern::IsStakeMature(b, b));
    BOOST_CHECK(!modern::IsStakeMature(b, b + 1));
    BOOST_CHECK(!modern::IsStakeMature(b, b + modern::STAKE_ACTIVATION_DEPTH - 1));
    BOOST_CHECK(modern::IsStakeMature(b, b + modern::STAKE_ACTIVATION_DEPTH));
    BOOST_CHECK(modern::IsStakeMature(b, b + modern::STAKE_ACTIVATION_DEPTH + 1));
}

BOOST_AUTO_TEST_CASE(validator_weight_aggregates_per_key)
{
    // LOCKED design rule: splitting principal across outputs manufactures
    // nothing — 100,000 split over many outputs weighs exactly one output
    // of 100,000, and immature/pre-H/non-stake entries contribute nothing.
    Consensus::Params params;
    params.legacy_b3coin = true;
    const int H{100};
    params.hard_fork_height = H + 1;

    const CScript owner{CScript() << OP_TRUE};
    const auto key_a{TestKey(0xaa)};
    const auto key_b{TestKey(0xbb)};
    constexpr CAmount total{100'000};

    std::vector<node::UtxoEntry> entries;
    const auto add{[&](const uint32_t n, const CAmount amount, const auto& key, const int height) {
        node::UtxoEntry e;
        e.outpoint = COutPoint{Txid::FromUint256(uint256::ONE), n};
        e.coin = Coin{CTxOut{amount, modern::MakeStakeScript(key, owner)}, height,
                      /*fCoinBaseIn=*/false, /*fCoinStakeIn=*/false};
        entries.push_back(e);
    }};

    // Validator A: one output of `total`. Validator B: the same total split
    // into 10 outputs. Both created at H+1, evaluated when mature.
    add(0, total, key_a, H + 1);
    for (uint32_t i{1}; i <= 10; ++i) add(i, total / 10, key_b, H + 1);
    // Immature stake for B (created too recently), a pre-H lookalike, and an
    // ordinary output: all weightless.
    add(20, 555'555, key_b, H + 30);
    {
        node::UtxoEntry e;
        e.outpoint = COutPoint{Txid::FromUint256(uint256::ONE), 21};
        e.coin = Coin{CTxOut{777'777, modern::MakeStakeScript(key_a, owner)}, H - 1, false, false};
        entries.push_back(e);
    }
    {
        node::UtxoEntry e;
        e.outpoint = COutPoint{Txid::FromUint256(uint256::ONE), 22};
        e.coin = Coin{CTxOut{999'999, CScript() << OP_TRUE}, H + 1, false, false};
        entries.push_back(e);
    }

    const int eval_height{H + 1 + modern::STAKE_ACTIVATION_DEPTH};
    const node::StakeRegistry registry{node::DeriveStakeRegistry(entries, eval_height, params)};

    BOOST_REQUIRE_EQUAL(registry.weights.size(), 2U);
    BOOST_CHECK_EQUAL(registry.weights.at(key_a), total);
    BOOST_CHECK_EQUAL(registry.weights.at(key_b), total); // split == single
    BOOST_CHECK_EQUAL(registry.total_weight, 2 * total);
    BOOST_CHECK_EQUAL(registry.mature_outputs, 11U);
    BOOST_CHECK_EQUAL(registry.immature_outputs, 1U);

    // One block earlier the H+1 outputs are not yet mature.
    const node::StakeRegistry early{node::DeriveStakeRegistry(entries, eval_height - 1, params)};
    BOOST_CHECK(early.weights.empty());
    BOOST_CHECK_EQUAL(early.immature_outputs, 12U);
}

BOOST_AUTO_TEST_SUITE_END()

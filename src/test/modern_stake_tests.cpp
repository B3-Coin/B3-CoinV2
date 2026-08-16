// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <modern/policy.h>
#include <modern/stake.h>
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
    // accepting valid and non-claiming outputs around it.
    CMutableTransaction tx;
    tx.vout.emplace_back(1000, CScript() << OP_TRUE);
    tx.vout.emplace_back(1000, MakeStakeScript(TestKey(), owner));
    BOOST_CHECK(modern::CheckStakeOutputs(CTransaction{tx}, error));
    tx.vout.emplace_back(0, MakeStakeScript(TestKey(), owner)); // zero principal
    BOOST_CHECK(!modern::CheckStakeOutputs(CTransaction{tx}, error));
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

BOOST_AUTO_TEST_SUITE_END()

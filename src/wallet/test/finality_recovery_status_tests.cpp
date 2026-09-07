// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/rpc/finality_recovery_status.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

namespace wallet {
namespace {
std::array<unsigned char, 32> Validator(unsigned char value)
{
    std::array<unsigned char, 32> key{};
    key.fill(value);
    return key;
}

uint256 Hash(unsigned char value)
{
    uint256 hash;
    std::fill(hash.begin(), hash.end(), value);
    return hash;
}

interfaces::StakingStatus ObservedSigner()
{
    interfaces::StakingStatus status;
    status.available = true;
    status.running = true;
    status.validator_key = Validator(1);
    auto& recovery{status.finality_recovery.emplace()};
    recovery.state = "blocked_on_orphan_vote";
    recovery.observed_tip_height = 814600;
    recovery.observed_tip_hash = Hash(1);
    recovery.journal_open = true;
    recovery.journal_present = true;
    recovery.blocked_on_orphan_vote = true;
    recovery.last_signed_height = 814551;
    recovery.last_signed_hash = Hash(2);
    recovery.last_signed_digest = Hash(3);
    recovery.lock_height = 814561;
    recovery.lock_hash = Hash(4);
    recovery.lock_digest = Hash(5);
    recovery.lock_epoch = 2;
    recovery.lock_signing_set_hash = Hash(6);
    recovery.lock_successor_set_hash = Hash(7);
    recovery.current_chain_hash = Hash(8);
    recovery.finalized_height = 814541;
    recovery.finalized_hash = Hash(9);
    recovery.finalized_epoch = 2;
    recovery.finalized_certified_at = 814554;
    recovery.finalized_signing_set_hash = Hash(6);
    recovery.certificate_included = true;
    recovery.certificate_same_epoch = true;
    recovery.certificate_same_set = true;
    recovery.certificate_reconstructible = true;
    return status;
}
} // namespace

BOOST_AUTO_TEST_SUITE(finality_recovery_status_tests)

BOOST_AUTO_TEST_CASE(unavailable_or_other_wallet_never_exports_a_lock)
{
    auto status{ObservedSigner()};
    const auto other{FinalityRecoveryStatusToJSON(status, Validator(2))};
    BOOST_CHECK(!other.find_value("available").get_bool());
    BOOST_CHECK_EQUAL(other.find_value("state").get_str(), "different_wallet_signer");
    BOOST_CHECK(other.find_value("lock").isNull());
    BOOST_CHECK(other.find_value("blocked_on_orphan_vote").isNull());
    status.running = false;
    const auto stopped{FinalityRecoveryStatusToJSON(status, Validator(1))};
    BOOST_CHECK_EQUAL(stopped.find_value("state").get_str(), "signer_not_running");
    BOOST_CHECK(stopped.find_value("last_vote").isNull());
    status.running = true;
    status.finality_recovery.reset();
    // Error text is deliberately not a source of diagnostic state.
    status.last_error = "active chain does not descend from signed checkpoint 999 fake";
    const auto pending{FinalityRecoveryStatusToJSON(status, Validator(1))};
    BOOST_CHECK(!pending.find_value("available").get_bool());
    BOOST_CHECK_EQUAL(pending.find_value("state").get_str(), "awaiting_signer_observation");
    BOOST_CHECK(pending.find_value("lock").isNull());
}

BOOST_AUTO_TEST_CASE(exact_vote_lock_and_certificate_prerequisites_are_separate)
{
    const auto status{ObservedSigner()};
    const auto obj{FinalityRecoveryStatusToJSON(status, Validator(1))};
    BOOST_CHECK(obj.find_value("available").get_bool());
    BOOST_CHECK(obj.find_value("blocked_on_orphan_vote").get_bool());
    BOOST_CHECK_EQUAL(obj.find_value("observed_tip").find_value("height").getInt<int>(), 814600);
    const auto& vote{obj.find_value("last_vote")};
    BOOST_CHECK_EQUAL(vote.find_value("height").getInt<int>(), 814551);
    BOOST_CHECK_EQUAL(vote.find_value("hash").get_str(), Hash(2).GetHex());
    BOOST_CHECK_EQUAL(vote.find_value("digest").get_str(), Hash(3).GetHex());
    const auto& lock{obj.find_value("lock")};
    BOOST_CHECK_EQUAL(lock.find_value("height").getInt<int>(), 814561);
    BOOST_CHECK_EQUAL(lock.find_value("hash").get_str(), Hash(4).GetHex());
    BOOST_CHECK_EQUAL(lock.find_value("digest").get_str(), Hash(5).GetHex());
    BOOST_CHECK_EQUAL(lock.find_value("current_chain_hash").get_str(), Hash(8).GetHex());
    const auto& proof{obj.find_value("required_proof")};
    BOOST_CHECK_EQUAL(proof.find_value("checkpoint_height_strictly_greater_than").getInt<int>(), 814561);
    BOOST_CHECK_EQUAL(proof.find_value("epoch").getInt<int>(), 2);
    BOOST_CHECK_EQUAL(proof.find_value("signing_set_hash").get_str(), Hash(6).GetHex());
    const auto& checks{obj.find_value("latest_certificate_checks")};
    BOOST_CHECK(checks.find_value("included_on_active_chain").get_bool());
    BOOST_CHECK(!checks.find_value("strictly_newer").get_bool());
    BOOST_CHECK(checks.find_value("same_epoch").get_bool());
    BOOST_CHECK(checks.find_value("same_signing_set").get_bool());
    BOOST_CHECK(obj.find_value("ready").isNull());
}

BOOST_AUTO_TEST_CASE(missing_height_and_permanent_errors_are_not_safe_unlock_claims)
{
    auto status{ObservedSigner()};
    status.finality_recovery->current_chain_hash.reset();
    status.finality_recovery->blocked_on_orphan_vote = false;
    status.finality_recovery->state = "waiting_for_locked_height";
    auto obj{FinalityRecoveryStatusToJSON(status, Validator(1))};
    BOOST_CHECK(obj.find_value("lock").find_value("current_chain_hash").isNull());
    BOOST_CHECK_EQUAL(obj.find_value("state").get_str(), "waiting_for_locked_height");
    status.finality_recovery->permanent_error = true;
    status.finality_recovery->state = "signer_error";
    status.finality_recovery->certificate_strictly_newer = true;
    obj = FinalityRecoveryStatusToJSON(status, Validator(1));
    BOOST_CHECK(obj.find_value("permanent_error").get_bool());
    BOOST_CHECK_EQUAL(obj.find_value("state").get_str(), "signer_error");
    BOOST_CHECK(obj.find_value("ready").isNull());
    BOOST_CHECK(obj.find_value("safe_to_sign").isNull());
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet

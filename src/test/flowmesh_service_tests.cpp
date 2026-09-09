// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_service.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <memory>
#include <thread>

namespace {

bls::SecretKey SeatKey(const unsigned char tag)
{
    std::array<unsigned char, 32> ikm{};
    ikm.fill(tag);
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

struct FlowMeshServiceSetup : TestingSetup {
    node::FlowMeshService service;

    FlowMeshServiceSetup()
        : TestingSetup(ChainType::MAIN),
          service{*m_node.chainman, m_args.GetDataDirNet() / "fn-operator-test"}
    {
        // Mainnet's schedule is configured, but the isolated chain is at
        // genesis: no markets, transaction broadcast, peers or signing work.
        BOOST_REQUIRE(m_node.peerman);
        std::string error;
        BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
        BOOST_REQUIRE(service.Running());
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_service_tests, FlowMeshServiceSetup)

BOOST_AUTO_TEST_CASE(public_snapshot_and_fingerprint_are_read_only_and_canonical)
{
    const auto empty{service.SeatKeyStatus()};
    BOOST_CHECK(empty.enabled);
    BOOST_CHECK(empty.running);
    BOOST_CHECK(empty.armed_pubkeys.empty());
    BOOST_CHECK(!empty.fingerprint.IsNull());
    BOOST_CHECK(empty.fingerprint == node::FlowMeshSeatKeysFingerprint({}));
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == empty.fingerprint);
    BOOST_CHECK(service.Markets().empty());

    const auto a{SeatKey(1).GetPublicKey().Compressed()};
    const auto b{SeatKey(2).GetPublicKey().Compressed()};
    BOOST_CHECK(node::FlowMeshSeatKeysFingerprint({a, b}) ==
                node::FlowMeshSeatKeysFingerprint({b, a, a, b}));
    BOOST_CHECK(node::FlowMeshSeatKeysFingerprint({a, b}) !=
                node::FlowMeshSeatKeysFingerprint({a}));
    BOOST_CHECK(node::FlowMeshSeatKeysFingerprint({a}) != empty.fingerprint);
}

BOOST_AUTO_TEST_CASE(stale_start_and_stop_preserve_the_other_wallet_keys)
{
    const auto empty{service.SeatKeyStatus()};
    const auto a{SeatKey(3)};
    const auto b{SeatKey(4)};
    std::string error;
    node::FlowMeshSeatKeyStatus started;
    BOOST_REQUIRE(service.ArmSeatKeys({b, a, b}, error, empty.fingerprint, &started));
    BOOST_CHECK_EQUAL(started.armed_pubkeys.size(), 2U);
    BOOST_CHECK(started.fingerprint == node::FlowMeshSeatKeysFingerprint(
        {a.GetPublicKey().Compressed(), b.GetPublicKey().Compressed()}));

    node::FlowMeshSeatKeyStatus untouched{empty};
    BOOST_CHECK(!service.ArmSeatKeys({SeatKey(5)}, error, empty.fingerprint, &untouched));
    BOOST_CHECK(error.find("changed") != std::string::npos);
    BOOST_CHECK(untouched.fingerprint == empty.fingerprint);
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == started.fingerprint);
    error.clear();
    BOOST_CHECK(!service.DisarmSeatKeys(error, empty.fingerprint, &untouched));
    BOOST_CHECK(error.find("changed") != std::string::npos);
    BOOST_CHECK(untouched.fingerprint == empty.fingerprint);
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == started.fingerprint);

    node::FlowMeshSeatKeyStatus stopped;
    BOOST_REQUIRE(service.DisarmSeatKeys(error, started.fingerprint, &stopped));
    BOOST_CHECK(stopped.running); // Worker can run as an observer without keys.
    BOOST_CHECK(stopped.armed_pubkeys.empty());
    BOOST_CHECK(stopped.fingerprint == empty.fingerprint);
}

BOOST_AUTO_TEST_CASE(omitted_guard_retains_legacy_replacement_and_clear)
{
    std::string error;
    BOOST_REQUIRE(service.ArmSeatKeys({SeatKey(6)}, error));
    const auto replacement{SeatKey(7)};
    BOOST_REQUIRE(service.ArmSeatKeys({replacement}, error));
    BOOST_CHECK(service.SeatKeyStatus().fingerprint ==
                node::FlowMeshSeatKeysFingerprint({replacement.GetPublicKey().Compressed()}));
    service.DisarmSeatKeys();
    BOOST_CHECK(service.SeatKeyStatus().armed_pubkeys.empty());
}

BOOST_AUTO_TEST_CASE(two_concurrent_approvals_cannot_both_replace_the_same_snapshot)
{
    const auto expected{service.SeatKeyStatus().fingerprint};
    const auto a{SeatKey(8)};
    const auto b{SeatKey(9)};
    bool accepted_a{false};
    bool accepted_b{false};
    std::string error_a;
    std::string error_b;
    std::thread first{[&] { accepted_a = service.ArmSeatKeys({a}, error_a, expected); }};
    std::thread second{[&] { accepted_b = service.ArmSeatKeys({b}, error_b, expected); }};
    first.join();
    second.join();
    BOOST_CHECK(accepted_a != accepted_b);
    const auto winner{accepted_a ? a.GetPublicKey().Compressed() : b.GetPublicKey().Compressed()};
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == node::FlowMeshSeatKeysFingerprint({winner}));
}

BOOST_AUTO_TEST_CASE(empty_or_stopped_service_cannot_arm_and_failure_keeps_result)
{
    std::string error;
    const auto empty{service.SeatKeyStatus()};
    node::FlowMeshSeatKeyStatus result{empty};
    BOOST_CHECK(!service.ArmSeatKeys({}, error, empty.fingerprint, &result));
    BOOST_CHECK(service.SeatKeyStatus().armed_pubkeys.empty());
    BOOST_CHECK(result.running);
    service.Stop();
    BOOST_CHECK(!service.ArmSeatKeys({SeatKey(10)}, error, empty.fingerprint, &result));
    BOOST_CHECK(!service.SeatKeyStatus().running);
    BOOST_CHECK(service.SeatKeyStatus().armed_pubkeys.empty());
    BOOST_CHECK(result.running); // Failure never fills an alleged success snapshot.
}

BOOST_AUTO_TEST_SUITE_END()

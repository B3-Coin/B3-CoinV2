// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/flowmesh_action_wait.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
using Slots = node::FlowMeshActionWaitSlots;
} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_action_wait_tests)

BOOST_AUTO_TEST_CASE(slots_respect_global_and_per_peer_caps)
{
    Slots slots{3};
    auto a1{slots.TryAcquire("192.0.2.1")};
    auto a2{slots.TryAcquire("192.0.2.1")};
    BOOST_REQUIRE(a1 && a2);
    BOOST_CHECK(!slots.TryAcquire("192.0.2.1")); // Per-peer limit.
    auto b1{slots.TryAcquire("192.0.2.2")};
    BOOST_REQUIRE(b1);
    BOOST_CHECK(!slots.TryAcquire("192.0.2.3")); // Global capacity.
    BOOST_CHECK_EQUAL(slots.Active(), 3U);
    a1.reset();
    auto c1{slots.TryAcquire("192.0.2.3")};
    BOOST_CHECK(c1);
    BOOST_CHECK(!slots.TryAcquire("192.0.2.1"));
    BOOST_CHECK_EQUAL(slots.Active(), 3U);
    Slots none{0};
    BOOST_CHECK(!none.TryAcquire("192.0.2.1"));
}

BOOST_AUTO_TEST_CASE(slot_release_is_raii_and_erases_peer_entry)
{
    Slots slots{4};
    {
        auto first{slots.TryAcquire("192.0.2.1")};
        BOOST_REQUIRE(first);
        auto moved{std::move(*first)};
        first.reset(); // A moved-from slot releases nothing.
        BOOST_CHECK_EQUAL(slots.Active(), 1U);
        BOOST_CHECK_EQUAL(slots.Peers(), 1U);
        std::vector<Slots::Slot> held;
        held.push_back(std::move(moved));
        auto second{slots.TryAcquire("192.0.2.1")};
        BOOST_REQUIRE(second);
        held.push_back(std::move(*second));
        BOOST_CHECK(!slots.TryAcquire("192.0.2.1"));
        held.pop_back();
        BOOST_CHECK(slots.TryAcquire("192.0.2.1")); // Temporary: released at once.
        BOOST_CHECK_EQUAL(slots.Active(), 1U);
    }
    BOOST_CHECK_EQUAL(slots.Active(), 0U);
    BOOST_CHECK_EQUAL(slots.Peers(), 0U);
}

BOOST_AUTO_TEST_CASE(close_refuses_new_and_interrupts_existing_epoch)
{
    Slots slots{4};
    auto held{slots.TryAcquire("192.0.2.1")};
    BOOST_REQUIRE(held);
    BOOST_CHECK(!slots.Interrupted(*held));
    slots.Close();
    BOOST_CHECK(slots.Interrupted(*held));
    BOOST_CHECK(!slots.TryAcquire("192.0.2.2"));
    slots.Close(); // Idempotent while closed.
    BOOST_CHECK(slots.Interrupted(*held));
    held.reset();
    BOOST_CHECK_EQUAL(slots.Active(), 0U);
}

BOOST_AUTO_TEST_CASE(open_restores_admission_without_reviving_old_epoch)
{
    Slots slots{4};
    auto old{slots.TryAcquire("192.0.2.1")};
    BOOST_REQUIRE(old);
    slots.Close();
    slots.Open();
    BOOST_CHECK(slots.Interrupted(*old));
    auto fresh{slots.TryAcquire("192.0.2.1")};
    BOOST_REQUIRE(fresh);
    BOOST_CHECK(!slots.Interrupted(*fresh));
    // The interrupted slot still counts until it is released.
    BOOST_CHECK(!slots.TryAcquire("192.0.2.1"));
    old.reset();
    BOOST_CHECK(slots.TryAcquire("192.0.2.1"));
}

BOOST_AUTO_TEST_CASE(per_peer_map_is_bounded_by_capacity)
{
    Slots slots{5};
    std::vector<Slots::Slot> held;
    for (int i{0}; i < 100; ++i) {
        if (auto slot{slots.TryAcquire("198.51.100." + std::to_string(i))}) held.push_back(std::move(*slot));
        BOOST_CHECK_LE(slots.Peers(), slots.Capacity());
    }
    BOOST_CHECK_EQUAL(held.size(), 5U);
    BOOST_CHECK_EQUAL(slots.Peers(), 5U);
    held.clear();
    BOOST_CHECK_EQUAL(slots.Peers(), 0U);
}

BOOST_AUTO_TEST_CASE(peer_keys_bucket_ipv6_by_64_and_mapped_ipv4)
{
    const auto key = [](const std::string& address) { return Slots::PeerKey(address); };
    BOOST_CHECK_EQUAL(key("192.0.2.1"), "192.0.2.1");
    BOOST_CHECK(key("192.0.2.1") != key("192.0.2.2"));
    BOOST_CHECK_EQUAL(key("::ffff:192.0.2.1"), "192.0.2.1");
    BOOST_CHECK_EQUAL(key("2001:db8::1"), key("2001:db8::ffff:1"));
    BOOST_CHECK_EQUAL(key("2001:db8::1"), key("2001:db8:0:0:1234:5678:9abc:def0"));
    BOOST_CHECK(key("2001:db8::1") != key("2001:db8:0:1::1"));
    BOOST_CHECK_EQUAL(key("2001:db8::1"), "ipv6/64:20010db800000000");
    BOOST_CHECK_EQUAL(key("::1"), key("::2"));
    // Rotating addresses inside one /64 share its two slots.
    Slots slots{8};
    auto first{slots.TryAcquire("2001:db8::a")};
    auto second{slots.TryAcquire("2001:db8::b")};
    BOOST_REQUIRE(first && second);
    BOOST_CHECK(!slots.TryAcquire("2001:db8::c"));
    BOOST_CHECK(slots.TryAcquire("2001:db8:0:1::c"));
}

BOOST_AUTO_TEST_SUITE_END()

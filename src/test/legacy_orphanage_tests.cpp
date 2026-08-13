// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/legacy_orphanage.h>

#include <primitives/block.h>
#include <uint256.h>
#include <util/time.h>

#include <boost/test/unit_test.hpp>

#include <memory>

using node::LegacyBlockOrphanage;

namespace {

uint256 H(uint64_t n)
{
    uint256 out;
    out.data()[0] = static_cast<unsigned char>(n & 0xff);
    out.data()[1] = static_cast<unsigned char>(n >> 8);
    return out;
}

std::shared_ptr<const CBlock> DummyBlock()
{
    return std::make_shared<const CBlock>();
}

constexpr NodeClock::time_point T0{std::chrono::seconds{1'700'000'000}};

} // namespace

BOOST_AUTO_TEST_SUITE(legacy_orphanage_tests)

BOOST_AUTO_TEST_CASE(add_take_and_duplicates)
{
    LegacyBlockOrphanage orphanage;
    const uint256 parent{H(1000)};

    BOOST_CHECK(orphanage.Add(H(1), parent, DummyBlock(), /*from=*/7, /*bytes=*/500, T0));
    BOOST_CHECK(!orphanage.Add(H(1), parent, DummyBlock(), 7, 500, T0)); // duplicate hash
    BOOST_CHECK(orphanage.Add(H(2), parent, DummyBlock(), 8, 700, T0));
    BOOST_CHECK(orphanage.Add(H(3), H(2000), DummyBlock(), 7, 300, T0)); // other parent
    BOOST_CHECK_EQUAL(orphanage.Count(), 3U);
    BOOST_CHECK_EQUAL(orphanage.Bytes(), 1500U);

    auto children{orphanage.TakeChildrenOf(parent)};
    BOOST_CHECK_EQUAL(children.size(), 2U);
    BOOST_CHECK_EQUAL(orphanage.Count(), 1U);
    BOOST_CHECK_EQUAL(orphanage.Bytes(), 300U);
    BOOST_CHECK(!orphanage.Contains(H(1)));
    BOOST_CHECK(orphanage.Contains(H(3)));
    // Taking again yields nothing.
    BOOST_CHECK(orphanage.TakeChildrenOf(parent).empty());
}

BOOST_AUTO_TEST_CASE(per_peer_caps_are_enforced_without_evicting_others)
{
    LegacyBlockOrphanage orphanage;

    // Peer 1 fills its per-peer entry count.
    for (size_t i{0}; i < LegacyBlockOrphanage::MAX_PEER_ORPHAN_COUNT; ++i) {
        BOOST_CHECK(orphanage.Add(H(i + 1), H(9000 + i), DummyBlock(), 1, 100, T0));
    }
    BOOST_CHECK(!orphanage.Add(H(500), H(9500), DummyBlock(), 1, 100, T0)); // over count cap
    // Another peer is unaffected.
    BOOST_CHECK(orphanage.Add(H(501), H(9501), DummyBlock(), 2, 100, T0));
    // Peer 1's entries were not evicted to make room.
    BOOST_CHECK_EQUAL(orphanage.PeerCount(1), LegacyBlockOrphanage::MAX_PEER_ORPHAN_COUNT);

    // Per-peer byte cap: one block larger than the cap is refused outright.
    BOOST_CHECK(!orphanage.Add(H(600), H(9600), DummyBlock(), 3,
                               LegacyBlockOrphanage::MAX_PEER_ORPHAN_BYTES + 1, T0));
    // And a peer cannot exceed the byte cap cumulatively.
    BOOST_CHECK(orphanage.Add(H(601), H(9601), DummyBlock(), 3,
                              LegacyBlockOrphanage::MAX_PEER_ORPHAN_BYTES - 50, T0));
    BOOST_CHECK(!orphanage.Add(H(602), H(9602), DummyBlock(), 3, 100, T0));
    BOOST_CHECK_EQUAL(orphanage.PeerBytes(3), LegacyBlockOrphanage::MAX_PEER_ORPHAN_BYTES - 50);
}

BOOST_AUTO_TEST_CASE(global_caps_evict_oldest_first)
{
    LegacyBlockOrphanage orphanage;

    // Fill the global count cap across many peers (peer ids vary so the
    // per-peer cap does not interfere). Insertion times increase.
    for (size_t i{0}; i < LegacyBlockOrphanage::MAX_ORPHAN_COUNT; ++i) {
        BOOST_CHECK(orphanage.Add(H(i + 1), H(9000 + i), DummyBlock(),
                                  static_cast<NodeId>(i), 100,
                                  T0 + std::chrono::seconds{i}));
    }
    BOOST_CHECK_EQUAL(orphanage.Count(), LegacyBlockOrphanage::MAX_ORPHAN_COUNT);

    // The next insertion evicts exactly the oldest entry (H(1)).
    BOOST_CHECK(orphanage.Add(H(700), H(9700), DummyBlock(), 99, 100,
                              T0 + std::chrono::seconds{LegacyBlockOrphanage::MAX_ORPHAN_COUNT}));
    BOOST_CHECK_EQUAL(orphanage.Count(), LegacyBlockOrphanage::MAX_ORPHAN_COUNT);
    BOOST_CHECK(!orphanage.Contains(H(1)));
    BOOST_CHECK(orphanage.Contains(H(2)));
    BOOST_CHECK(orphanage.Contains(H(700)));

    // Byte cap: a large block evicts as many oldest entries as needed.
    LegacyBlockOrphanage bytes_bound;
    const size_t big{LegacyBlockOrphanage::MAX_PEER_ORPHAN_BYTES};
    BOOST_CHECK(bytes_bound.Add(H(1), H(9001), DummyBlock(), 1, big, T0));
    BOOST_CHECK(bytes_bound.Add(H(2), H(9002), DummyBlock(), 2, big, T0 + 1s));
    // 16 MiB global cap, two 6 MiB entries stored: a third 6 MiB entry
    // exceeds the cap and evicts the oldest.
    BOOST_CHECK(bytes_bound.Add(H(3), H(9003), DummyBlock(), 3, big, T0 + 2s));
    BOOST_CHECK(!bytes_bound.Contains(H(1)));
    BOOST_CHECK(bytes_bound.Contains(H(2)));
    BOOST_CHECK(bytes_bound.Contains(H(3)));
    BOOST_CHECK(bytes_bound.Bytes() <= LegacyBlockOrphanage::MAX_ORPHAN_BYTES);
}

BOOST_AUTO_TEST_CASE(entries_expire)
{
    LegacyBlockOrphanage orphanage;
    BOOST_CHECK(orphanage.Add(H(1), H(9001), DummyBlock(), 1, 100, T0));
    BOOST_CHECK(orphanage.Add(H(2), H(9002), DummyBlock(), 1, 100,
                              T0 + std::chrono::minutes{5}));

    // Just before the first entry's deadline nothing expires.
    orphanage.Expire(T0 + LegacyBlockOrphanage::ORPHAN_EXPIRY);
    BOOST_CHECK_EQUAL(orphanage.Count(), 2U);

    // Past it, only the first entry goes.
    orphanage.Expire(T0 + LegacyBlockOrphanage::ORPHAN_EXPIRY + 1s);
    BOOST_CHECK(!orphanage.Contains(H(1)));
    BOOST_CHECK(orphanage.Contains(H(2)));
    BOOST_CHECK_EQUAL(orphanage.Bytes(), 100U);

    // Expiry is also applied on Add.
    BOOST_CHECK(orphanage.Add(H(3), H(9003), DummyBlock(), 1, 100,
                              T0 + std::chrono::minutes{16}));
    BOOST_CHECK(!orphanage.Contains(H(2)));
    BOOST_CHECK(orphanage.Contains(H(3)));
    BOOST_CHECK_EQUAL(orphanage.Count(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

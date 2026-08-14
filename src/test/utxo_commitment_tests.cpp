// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>
#include <node/utxo_commitment.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using node::CompareUtxoSets;
using node::UtxoEntry;
using node::UtxoSetCommitment;

namespace {

COutPoint Op(const uint8_t tag, const uint32_t n)
{
    uint256 h;
    h.begin()[0] = tag;
    return COutPoint{Txid::FromUint256(h), n};
}

Coin MakeCoin(const CAmount value, const int height, const bool coinbase, const bool coinstake,
              const uint32_t nTime = 0, const uint32_t nTxOffset = 0)
{
    return Coin{CTxOut{value, CScript() << OP_TRUE}, height, coinbase, coinstake, nTime, nTxOffset};
}

UtxoEntry Entry(const COutPoint& op, const Coin& coin) { return UtxoEntry{op, coin}; }

} // namespace

BOOST_AUTO_TEST_SUITE(utxo_commitment_tests)

BOOST_AUTO_TEST_CASE(commitment_is_deterministic_and_order_independent)
{
    const std::vector<UtxoEntry> forward{
        Entry(Op(1, 0), MakeCoin(100, 5, false, true, 111, 80)),
        Entry(Op(2, 1), MakeCoin(200, 6, true, false)),
    };
    std::vector<UtxoEntry> reversed{forward.rbegin(), forward.rend()};

    // CompareUtxoSets sorts internally, so submission order does not matter:
    // identical contents in any order commit to the same value and match.
    const auto cmp{CompareUtxoSets(forward, reversed)};
    BOOST_CHECK(cmp.Equal());
    BOOST_CHECK_EQUAL(cmp.commitment_a.GetHex(), cmp.commitment_b.GetHex());
    BOOST_CHECK(cmp.mismatches.empty());

    // The empty set has a stable, distinct commitment.
    BOOST_CHECK(UtxoSetCommitment({}) != cmp.commitment_a);
    BOOST_CHECK_EQUAL(UtxoSetCommitment({}).GetHex(), UtxoSetCommitment({}).GetHex());
}

BOOST_AUTO_TEST_CASE(one_sided_outpoints_are_reported)
{
    const std::vector<UtxoEntry> a{
        Entry(Op(1, 0), MakeCoin(100, 5, false, true)),
        Entry(Op(2, 0), MakeCoin(200, 6, true, false)),
    };
    const std::vector<UtxoEntry> b{
        Entry(Op(2, 0), MakeCoin(200, 6, true, false)),
        Entry(Op(3, 0), MakeCoin(300, 7, false, false)),
    };

    const auto cmp{CompareUtxoSets(a, b)};
    BOOST_CHECK(!cmp.Equal());
    BOOST_CHECK(cmp.commitment_a != cmp.commitment_b);
    BOOST_REQUIRE_EQUAL(cmp.mismatches.size(), 2U);

    // Op(1) only in a; Op(3) only in b; canonical order by outpoint tag.
    BOOST_CHECK(cmp.mismatches[0].outpoint == Op(1, 0));
    BOOST_CHECK(cmp.mismatches[0].in_a.has_value());
    BOOST_CHECK(!cmp.mismatches[0].in_b.has_value());
    BOOST_CHECK(cmp.mismatches[1].outpoint == Op(3, 0));
    BOOST_CHECK(!cmp.mismatches[1].in_a.has_value());
    BOOST_CHECK(cmp.mismatches[1].in_b.has_value());
}

BOOST_AUTO_TEST_CASE(exact_coin_content_differences_are_detected)
{
    const COutPoint op{Op(9, 3)};
    const Coin base{MakeCoin(500, 42, false, true, 1'400'000'017, 96)};

    // Each field of the coin, varied one at a time, must break equality --
    // a same-outpoint, same-value check (or an aggregate supply total) would
    // miss most of these.
    const std::vector<Coin> perturbations{
        MakeCoin(501, 42, false, true, 1'400'000'017, 96), // value
        MakeCoin(500, 43, false, true, 1'400'000'017, 96), // height
        MakeCoin(500, 42, true, true, 1'400'000'017, 96),  // coinbase flag
        MakeCoin(500, 42, false, false, 1'400'000'017, 96), // coinstake flag
        MakeCoin(500, 42, false, true, 1'400'000'018, 96), // legacy nTime
        MakeCoin(500, 42, false, true, 1'400'000'017, 97),  // legacy nTxOffset
    };

    for (const Coin& perturbed : perturbations) {
        const auto cmp{CompareUtxoSets({Entry(op, base)}, {Entry(op, perturbed)})};
        BOOST_CHECK(!cmp.Equal());
        BOOST_CHECK(cmp.commitment_a != cmp.commitment_b);
        BOOST_REQUIRE_EQUAL(cmp.mismatches.size(), 1U);
        BOOST_CHECK(cmp.mismatches[0].outpoint == op);
        BOOST_CHECK(cmp.mismatches[0].in_a.has_value());
        BOOST_CHECK(cmp.mismatches[0].in_b.has_value());
    }

    // The identical coin matches and commits equally.
    const auto same{CompareUtxoSets({Entry(op, base)}, {Entry(op, base)})};
    BOOST_CHECK(same.Equal());
    BOOST_CHECK_EQUAL(same.commitment_a.GetHex(), same.commitment_b.GetHex());
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/utxo_rows.h>

#include <coins.h>
#include <node/utxo_commitment.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <sstream>
#include <string>
#include <vector>

using node::ReadUtxoRows;
using node::UtxoEntry;
using node::UtxoRowLine;
using node::UtxoRowsFile;
using node::UtxoSetCommitment;
using node::WriteUtxoRows;

namespace {

UtxoEntry MakeEntry(const uint256& txid, const uint32_t n, const CAmount value, const int height,
                    const bool coinbase, const bool coinstake, const uint32_t ntime,
                    const uint32_t offset, const CScript& script)
{
    UtxoEntry e;
    e.outpoint = COutPoint{Txid::FromUint256(txid), n};
    e.coin = Coin{CTxOut{value, script}, height, coinbase, coinstake, ntime, offset};
    return e;
}

bool EntriesEqual(const UtxoEntry& a, const UtxoEntry& b)
{
    if (!(a.outpoint == b.outpoint)) return false;
    DataStream sa;
    DataStream sb;
    a.coin.Serialize(sa);
    b.coin.Serialize(sb);
    return sa.size() == sb.size() && std::equal(sa.begin(), sa.end(), sb.begin());
}

const uint256 TXID_A{"00000000000000000000000000000000000000000000000000000000000000aa"};
const uint256 TXID_B{"00000000000000000000000000000000000000000000000000000000000000bb"};

} // namespace

BOOST_AUTO_TEST_SUITE(utxo_rows_tests)

//! The row grammar is pinned byte for byte: the legacy master client's
//! exporter emits these exact lines with independent code, and two row
//! files must be directly diffable.
BOOST_AUTO_TEST_CASE(row_line_is_canonical)
{
    const UtxoEntry entry{MakeEntry(TXID_A, 7, 123456, 42, /*coinbase=*/true, /*coinstake=*/false,
                                    1'400'000'123, 81, CScript() << OP_TRUE)};
    BOOST_CHECK_EQUAL(
        UtxoRowLine(entry),
        "00000000000000000000000000000000000000000000000000000000000000aa:7 "
        "123456 42 1 0 1400000123 81 51");

    // An empty script (spendable, value > 0) is the "-" sentinel.
    const UtxoEntry empty_script{MakeEntry(TXID_A, 0, 5, 1, false, true, 10, 20, CScript{})};
    BOOST_CHECK_EQUAL(
        UtxoRowLine(empty_script),
        "00000000000000000000000000000000000000000000000000000000000000aa:0 5 1 0 1 10 20 -");
}

BOOST_AUTO_TEST_CASE(round_trips_exactly)
{
    UtxoRowsFile file;
    file.tip_hash = TXID_B;
    file.tip_height = 34;
    // Deliberately unsorted: the writer canonicalizes.
    file.entries.push_back(MakeEntry(TXID_B, 1, 450, 2, false, true, 1'200, 99, CScript() << OP_2));
    file.entries.push_back(MakeEntry(TXID_A, 2, 100, 1, false, false, 1'100, 123, CScript() << OP_TRUE));
    file.entries.push_back(MakeEntry(TXID_A, 0, 0, 1, true, false, 1'100, 123, CScript() << OP_RETURN));
    file.entries.push_back(MakeEntry(TXID_B, 0, 7, 2, false, false, 1'200, 99, CScript{}));

    std::ostringstream out;
    std::string error;
    BOOST_REQUIRE_MESSAGE(WriteUtxoRows(out, file, error), error);

    std::istringstream in{out.str()};
    UtxoRowsFile read;
    BOOST_REQUIRE_MESSAGE(ReadUtxoRows(in, read, error), error);

    BOOST_CHECK_EQUAL(read.tip_hash.GetHex(), file.tip_hash.GetHex());
    BOOST_CHECK_EQUAL(read.tip_height, 34);
    BOOST_REQUIRE_EQUAL(read.entries.size(), 4U);
    // Canonical order: TXID_A:0, TXID_A:2, TXID_B:0, TXID_B:1.
    BOOST_CHECK(read.entries[0].outpoint == COutPoint(Txid::FromUint256(TXID_A), 0));
    BOOST_CHECK(read.entries[1].outpoint == COutPoint(Txid::FromUint256(TXID_A), 2));
    BOOST_CHECK(read.entries[2].outpoint == COutPoint(Txid::FromUint256(TXID_B), 0));
    BOOST_CHECK(read.entries[3].outpoint == COutPoint(Txid::FromUint256(TXID_B), 1));

    // Every coin survives byte-exactly, so the canonical commitment is
    // computable from a row file alone.
    std::vector<UtxoEntry> sorted{file.entries};
    std::sort(sorted.begin(), sorted.end(),
              [](const UtxoEntry& a, const UtxoEntry& b) { return a.outpoint < b.outpoint; });
    for (size_t i{0}; i < sorted.size(); ++i) {
        BOOST_CHECK(EntriesEqual(sorted[i], read.entries[i]));
    }
    BOOST_CHECK_EQUAL(UtxoSetCommitment(sorted).GetHex(), UtxoSetCommitment(read.entries).GetHex());
}

BOOST_AUTO_TEST_CASE(rejects_malformed_input)
{
    const auto reject{[](const std::string& text, const std::string& why) {
        std::istringstream in{text};
        UtxoRowsFile read;
        std::string error;
        BOOST_CHECK_MESSAGE(!ReadUtxoRows(in, read, error), why + " was accepted");
        BOOST_CHECK_MESSAGE(!error.empty(), why + " produced no error message");
    }};

    const std::string header{
        "b3-utxo-rows/v1\n"
        "tip_hash=00000000000000000000000000000000000000000000000000000000000000bb\n"
        "tip_height=34\n"};
    const std::string row_a0{
        "00000000000000000000000000000000000000000000000000000000000000aa:0 5 1 0 0 10 20 51\n"};
    const std::string row_b0{
        "00000000000000000000000000000000000000000000000000000000000000bb:0 5 1 0 0 10 20 51\n"};

    reject("nonsense\n", "a foreign format tag");
    reject("b3-utxo-rows/v1\ntip_height=34\n", "a missing tip_hash");
    reject("b3-utxo-rows/v1\ntip_hash=zz\ntip_height=34\ncount=0\n", "a malformed tip_hash");
    reject(header + "count=1\n", "a count disagreeing with the rows");
    reject(header + row_a0 + row_a0 + "count=2\n", "a duplicated outpoint");
    reject(header + row_b0 + row_a0 + "count=2\n", "out-of-order rows");
    reject(header + row_a0, "a missing count line");
    reject(header + row_a0 + "count=1\nextra\n", "content after the count line");
    reject(header +
               "00000000000000000000000000000000000000000000000000000000000000aa:0 5 1 0 0 10 20\n" +
               "count=1\n",
           "a row with a missing field");
    reject(header +
               "00000000000000000000000000000000000000000000000000000000000000aa:0 5 1 0 0 10 20 zz\n" +
               "count=1\n",
           "a row with non-hex script bytes");
    reject(header +
               "00000000000000000000000000000000000000000000000000000000000000aa:0 -5 1 0 0 10 20 51\n" +
               "count=1\n",
           "a negative value");
    reject(header +
               "00000000000000000000000000000000000000000000000000000000000000aa:0 5 1 2 0 10 20 51\n" +
               "count=1\n",
           "a non-boolean coinbase flag");

    // Membership is deliberately NOT validated at parse time: a row another
    // producer should never have emitted (a value-0 empty-script marker)
    // must surface as a comparison difference, not a parse failure.
    {
        std::istringstream in{header +
                              "00000000000000000000000000000000000000000000000000000000000000aa:0 "
                              "0 1 0 1 10 20 -\ncount=1\n"};
        UtxoRowsFile read;
        std::string error;
        BOOST_CHECK_MESSAGE(ReadUtxoRows(in, read, error), error);
        BOOST_REQUIRE_EQUAL(read.entries.size(), 1U);
        BOOST_CHECK_EQUAL(read.entries[0].coin.out.nValue, 0);
        BOOST_CHECK(read.entries[0].coin.out.scriptPubKey.empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()

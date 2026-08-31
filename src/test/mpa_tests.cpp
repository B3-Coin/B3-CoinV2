// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Modern Payload Area
// serialization (BIP144 flag 0x02 under the Modern context only), canonical
// encoding, the typed/versioned registry (unknown / known-inactive / active),
// activation fail-closed behaviour, and the transaction-identity guardrail
// (txid excludes the MPA; ptxid and the wtxid-shaped relay slot commit to it).

#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <hash.h>
#include <legacy/codec.h>
#include <modern/creation_action.h>
#include <modern/finality_types.h>
#include <modern/mpa.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

namespace {

CMutableTransaction BaseTx()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"1111111111111111111111111111111111111111111111111111111111111111"}), 3};
    mtx.vin[0].scriptSig = CScript() << OP_1;
    mtx.vout.emplace_back(1234, CScript() << OP_TRUE);
    mtx.nLockTime = 77;
    return mtx;
}

CMpaRecord Rec(const uint16_t type, const uint16_t version, const std::vector<unsigned char>& payload)
{
    CMpaRecord r;
    r.payload_type = type;
    r.payload_version = version;
    r.payload = payload;
    return r;
}

std::vector<unsigned char> Ser(const CMutableTransaction& tx, const TransactionSerParams& p)
{
    DataStream ss;
    ss << p(tx);
    const auto bytes{ss.str()};
    return std::vector<unsigned char>(bytes.begin(), bytes.end());
}

template <typename P>
bool Deser(const std::vector<unsigned char>& bytes, const P& params, CMutableTransaction& out)
{
    try {
        DataStream ss{bytes};
        ss >> params(out);
        if (!ss.empty()) return false; // trailing bytes
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<unsigned char> Evidence244() { return std::vector<unsigned char>(modern::FINALITY_KEY_EVIDENCE_SIZE, 0x5A); }

} // namespace

BOOST_FIXTURE_TEST_SUITE(mpa_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(frozen_limits_and_params)
{
    BOOST_CHECK_EQUAL(MAX_PAYLOAD_RECORDS_PER_TX, 64u);
    BOOST_CHECK_EQUAL(MAX_PAYLOAD_RECORD_SIZE, 32768u);
    BOOST_CHECK_EQUAL(MAX_PAYLOAD_SECTION_SIZE, 65536u);
    BOOST_CHECK(TX_MODERN.allow_witness && TX_MODERN.allow_mpa && !TX_MODERN.legacy_time);
    BOOST_CHECK(TX_WITH_WITNESS.allow_witness && !TX_WITH_WITNESS.allow_mpa);
    BOOST_CHECK(!TX_NO_WITNESS.allow_witness && !TX_NO_WITNESS.allow_mpa);
    BOOST_CHECK(TX_LEGACY_B3.legacy_time && !TX_LEGACY_B3.allow_mpa);
}

BOOST_AUTO_TEST_CASE(mpa_absent_serialization_is_byte_identical)
{
    const CMutableTransaction tx{BaseTx()};
    BOOST_CHECK(tx.mpa.empty());
    // Under every context the bytes of an MPA-free transaction are the classic ones,
    // and TX_MODERN == TX_WITH_WITNESS byte for byte (no alternate encoding).
    const auto modern{Ser(tx, TX_MODERN)};
    const auto witness{Ser(tx, TX_WITH_WITNESS)};
    const auto base{Ser(tx, TX_NO_WITNESS)};
    BOOST_CHECK(modern == witness);
    BOOST_CHECK(modern == base); // no witness, no mpa: not even the extended prefix
    BOOST_CHECK_EQUAL(base[4], 0x01); // vin count directly after version: classic form
    // Round trips
    CMutableTransaction back;
    BOOST_CHECK(Deser(modern, TX_MODERN, back));
    BOOST_CHECK(back.mpa.empty());
    BOOST_CHECK(CTransaction{back}.GetHash() == CTransaction{tx}.GetHash());
    // A witness-bearing transaction: flag 0x01 keeps its meaning; MPA flag absent.
    CMutableTransaction wtx{tx};
    wtx.vin[0].scriptWitness.stack.push_back({0xAA, 0xBB});
    const auto w_modern{Ser(wtx, TX_MODERN)};
    const auto w_witness{Ser(wtx, TX_WITH_WITNESS)};
    BOOST_CHECK(w_modern == w_witness);
    BOOST_CHECK_EQUAL(w_modern[4], 0x00); // dummy vin
    BOOST_CHECK_EQUAL(w_modern[5], 0x01); // flags = witness only
}

BOOST_AUTO_TEST_CASE(mpa_round_trip_layout_and_contexts)
{
    CMutableTransaction tx{BaseTx()};
    tx.mpa.push_back(Rec(5, 1, Evidence244()));
    const CTransaction ctx{tx};
    BOOST_CHECK(ctx.HasMpa());
    // Layout under TX_MODERN: version | 0x00 | 0x02 | vin | vout | section | locktime
    const auto bytes{Ser(tx, TX_MODERN)};
    BOOST_CHECK_EQUAL(bytes[4], 0x00);
    BOOST_CHECK_EQUAL(bytes[5], 0x02);
    // section sits right before the 4-byte locktime: count(1) | type(2) | version(2) | len(1+1? 244 -> 0xfd?)...
    // 244 < 253 so CompactSize is one byte.
    const size_t section_len{1 + 2 + 2 + 1 + 244};
    const size_t section_start{bytes.size() - 4 - section_len};
    BOOST_CHECK_EQUAL(bytes[section_start], 0x01);                 // count
    BOOST_CHECK_EQUAL(bytes[section_start + 1], 0x05);             // type LE
    BOOST_CHECK_EQUAL(bytes[section_start + 2], 0x00);
    BOOST_CHECK_EQUAL(bytes[section_start + 3], 0x01);             // version LE
    BOOST_CHECK_EQUAL(bytes[section_start + 4], 0x00);
    BOOST_CHECK_EQUAL(bytes[section_start + 5], 244);              // payload len
    // Round trip under TX_MODERN
    CMutableTransaction back;
    BOOST_REQUIRE(Deser(bytes, TX_MODERN, back));
    BOOST_CHECK(back.mpa == tx.mpa);
    BOOST_CHECK(CTransaction{back}.GetHash() == ctx.GetHash());
    // The same bytes are rejected by the witness-only and base contexts (unknown flag 0x02).
    CMutableTransaction rej;
    BOOST_CHECK(!Deser(bytes, TX_WITH_WITNESS, rej));
    BOOST_CHECK(!Deser(bytes, TX_NO_WITNESS, rej));
    // Serializing an MPA transaction under the witness/base contexts EXCLUDES the MPA
    // (they are the wtxid/txid forms), i.e. identical to the MPA-free transaction.
    BOOST_CHECK(Ser(tx, TX_WITH_WITNESS) == Ser(BaseTx(), TX_WITH_WITNESS));
    BOOST_CHECK(Ser(tx, TX_NO_WITNESS) == Ser(BaseTx(), TX_NO_WITNESS));
    // Witness + MPA together: flags = 0x03, witness first, then the section.
    CMutableTransaction both{tx};
    both.vin[0].scriptWitness.stack.push_back({0xCC});
    const auto b{Ser(both, TX_MODERN)};
    BOOST_CHECK_EQUAL(b[5], 0x03);
    CMutableTransaction back2;
    BOOST_REQUIRE(Deser(b, TX_MODERN, back2));
    BOOST_CHECK(back2.mpa == both.mpa && back2.vin[0].scriptWitness.stack == both.vin[0].scriptWitness.stack);
    // The legacy codec never carries an MPA (legacy_time contexts ignore allow_mpa).
    CMutableTransaction ltx{tx};
    ltx.m_legacy_encoding = true;
    ltx.nTime = 5;
    const auto lbytes{Ser(ltx, TX_LEGACY_B3)};
    CMutableTransaction lback;
    BOOST_REQUIRE(Deser(lbytes, TX_LEGACY_B3, lback));
    BOOST_CHECK(lback.mpa.empty());
}

BOOST_AUTO_TEST_CASE(unknown_flags_and_malformed_sections_rejected)
{
    CMutableTransaction tx{BaseTx()};
    tx.mpa.push_back(Rec(5, 1, Evidence244()));
    const auto good{Ser(tx, TX_MODERN)};
    CMutableTransaction out;
    BOOST_REQUIRE(Deser(good, TX_MODERN, out));
    // unknown flag bits (0x04, 0x80) with otherwise identical bytes
    for (const unsigned char extra : {0x04, 0x80, 0x06}) {
        auto bad{good};
        bad[5] |= extra;
        BOOST_CHECK(!Deser(bad, TX_MODERN, out));
    }
    // flag 0x02 claimed but count = 0 (no records): not a second encoding of the MPA-free tx
    {
        CMutableTransaction empty{BaseTx()};
        auto bytes{Ser(empty, TX_NO_WITNESS)}; // version | vin | vout | locktime
        std::vector<unsigned char> forged;
        forged.insert(forged.end(), bytes.begin(), bytes.begin() + 4); // version
        forged.push_back(0x00);                                         // dummy
        forged.push_back(0x02);                                         // flags: MPA
        forged.insert(forged.end(), bytes.begin() + 4, bytes.end() - 4); // vin | vout
        forged.push_back(0x00);                                         // count = 0
        forged.insert(forged.end(), bytes.end() - 4, bytes.end());      // locktime
        BOOST_CHECK(!Deser(forged, TX_MODERN, out));
    }
    // truncated section
    for (size_t cut : {1u, 50u, 100u}) {
        std::vector<unsigned char> trunc(good.begin(), good.end() - cut);
        BOOST_CHECK(!Deser(trunc, TX_MODERN, out));
    }
    // trailing bytes after the transaction
    {
        auto t{good};
        t.push_back(0x00);
        BOOST_CHECK(!Deser(t, TX_MODERN, out));
    }
    // non-minimal CompactSize for the record count (0xfd 0x01 0x00 instead of 0x01)
    {
        const size_t section_start{good.size() - 4 - (1 + 2 + 2 + 1 + 244)};
        std::vector<unsigned char> nm(good.begin(), good.begin() + section_start);
        nm.push_back(0xfd); nm.push_back(0x01); nm.push_back(0x00);
        nm.insert(nm.end(), good.begin() + section_start + 1, good.end());
        BOOST_CHECK(!Deser(nm, TX_MODERN, out));
    }
    // non-minimal CompactSize for the payload length
    {
        const size_t len_pos{good.size() - 4 - 244 - 1};
        std::vector<unsigned char> nm(good.begin(), good.begin() + len_pos);
        nm.push_back(0xfd); nm.push_back(244); nm.push_back(0x00);
        nm.insert(nm.end(), good.begin() + len_pos + 1, good.end());
        BOOST_CHECK(!Deser(nm, TX_MODERN, out));
    }
}

BOOST_AUTO_TEST_CASE(record_count_size_and_section_boundaries)
{
    CMutableTransaction out;
    // record count: 64 ok, 65 rejected (count bound enforced before reading records)
    {
        CMutableTransaction tx{BaseTx()};
        for (int i = 0; i < 64; ++i) tx.mpa.push_back(Rec(5, 1, std::vector<unsigned char>(1, static_cast<unsigned char>(i))));
        BOOST_CHECK(Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
        tx.mpa.push_back(Rec(5, 1, {0xFF}));
        BOOST_CHECK(!Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
    }
    // record size: 32,768 ok, 32,769 rejected
    {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(Rec(4, 1, std::vector<unsigned char>(MAX_PAYLOAD_RECORD_SIZE, 0x01)));
        BOOST_CHECK(Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
        tx.mpa[0].payload.push_back(0x02);
        BOOST_CHECK(!Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
    }
    // section size: two 32,768-byte records exceed 65,536 with framing -> rejected;
    // records sized so the section is exactly at the cap are accepted.
    {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(Rec(4, 1, std::vector<unsigned char>(MAX_PAYLOAD_RECORD_SIZE, 0x01)));
        tx.mpa.push_back(Rec(4, 1, std::vector<unsigned char>(MAX_PAYLOAD_RECORD_SIZE, 0x02)));
        BOOST_CHECK(!Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
        // exact cap: count(1) + 2 * (2+2+3 + len) == 65536 -> len = (65536 - 1 - 14) / 2 = 32760.5 -> use
        // one record of 32,768 and one of 65536 - 1 - (7 + 32768) - 7 = 32753
        tx.mpa[1].payload.assign(32753, 0x02);
        BOOST_CHECK(Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
        tx.mpa[1].payload.push_back(0x03); // one byte over the section cap
        BOOST_CHECK(!Deser(Ser(tx, TX_MODERN), TX_MODERN, out));
    }
}

BOOST_AUTO_TEST_CASE(registry_and_activation_fail_closed)
{
    Consensus::Params production{};
    production.legacy_b3coin = true;
    Consensus::Params active{production};
    active.hard_fork_height = 100;
    active.legacy_final_hash = uint256::ONE;
    active.modern_pos = Consensus::ModernPosParams{};
    using modern::PayloadTypeStatus;
    // Statuses
    for (const uint16_t t : {1, 2, 3}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, production) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, active) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 2, active) == PayloadTypeStatus::UNKNOWN);
    }
    // 4 (certificate, Commit 12) and 5 (key evidence): production inactive,
    // test context active.
    for (const uint16_t t : {4, 5}) {
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, production) == PayloadTypeStatus::INACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, active) == PayloadTypeStatus::ACTIVE);
        BOOST_CHECK(modern::GetPayloadTypeStatus(t, 2, active) == PayloadTypeStatus::UNKNOWN);
    }
    BOOST_CHECK(modern::GetPayloadTypeStatus(6, 1, active) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(6, 2, active) == PayloadTypeStatus::UNKNOWN);
    BOOST_CHECK(modern::GetPayloadTypeStatus(7, 1, active) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(7, 2, active) == PayloadTypeStatus::UNKNOWN);
    BOOST_CHECK(modern::GetPayloadTypeStatus(8, 1, active) == PayloadTypeStatus::INACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(9, 1, active) == PayloadTypeStatus::INACTIVE);
    for (const uint16_t t : {0, 10, 99, 65535}) BOOST_CHECK(modern::GetPayloadTypeStatus(t, 1, active) == PayloadTypeStatus::UNKNOWN);
    std::string err;
    // Production: any MPA is invalid (not active), even a perfectly formed type-5 record.
    {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(Rec(5, 1, Evidence244()));
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, production, err));
        BOOST_CHECK_EQUAL(err, "mpa-not-active");
        BOOST_CHECK(modern::CheckTransactionMpa(CTransaction{tx}, active, err));
    }
    // Known-but-inactive types rejected under the test context; unknown rejected.
    for (const uint16_t t : {1, 2, 3}) {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(Rec(t, 1, {0x00}));
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        BOOST_CHECK_EQUAL(err, "mpa-inactive-type");
    }
    {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(Rec(10, 1, {0x00}));
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        BOOST_CHECK_EQUAL(err, "mpa-unknown-type");
        tx.mpa[0] = Rec(5, 2, Evidence244());
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        BOOST_CHECK_EQUAL(err, "mpa-unknown-type");
    }
    // Type 5 grammar: exactly 244 bytes.
    {
        CMutableTransaction tx{BaseTx()};
        tx.mpa.push_back(Rec(5, 1, std::vector<unsigned char>(243, 0)));
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        BOOST_CHECK_EQUAL(err, "mpa-bad-record-size");
    }
    // Canonical order: strictly increasing (type, version, payload); duplicates rejected.
    {
        CMutableTransaction tx{BaseTx()};
        auto a{Evidence244()}; auto b{Evidence244()}; b[0] = 0x5B;
        tx.mpa.push_back(Rec(5, 1, b));
        tx.mpa.push_back(Rec(5, 1, a));
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        BOOST_CHECK_EQUAL(err, "mpa-record-order");
        std::swap(tx.mpa[0], tx.mpa[1]);
        BOOST_CHECK(modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        tx.mpa.push_back(Rec(5, 1, b)); // duplicate
        BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{tx}, active, err));
        BOOST_CHECK_EQUAL(err, "mpa-record-order");
    }
    // No chainparams enable the context.
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::REGTEST}) {
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(CreateChainParams(ArgsManager{}, chain)->GetConsensus()));
    }
    // Standalone creation actions know 1-3 and proof-free modern FN type 6;
    // finality records 4/5 and FlowMesh records 7/8/9 remain MPA-only.
    BOOST_CHECK(modern::IsKnownCreationAction(1, 1) && modern::IsKnownCreationAction(2, 1) &&
                modern::IsKnownCreationAction(3, 1) && modern::IsKnownCreationAction(6, 1));
    BOOST_CHECK(!modern::IsKnownCreationAction(4, 1) &&
                !modern::IsKnownCreationAction(5, 1) &&
                !modern::IsKnownCreationAction(7, 1) &&
                !modern::IsKnownCreationAction(8, 1) &&
                !modern::IsKnownCreationAction(9, 1));
}

BOOST_AUTO_TEST_CASE(txid_unchanged_by_mpa_full_relay_id_changes)
{
    CMutableTransaction a{BaseTx()};
    CMutableTransaction b{BaseTx()};
    CMutableTransaction c{BaseTx()};
    a.mpa.push_back(Rec(5, 1, Evidence244()));
    auto other{Evidence244()}; other[10] = 0x00;
    b.mpa.push_back(Rec(5, 1, other));
    const CTransaction ta{a}, tb{b}, tc{c};
    // Same base data, different (or no) MPA: the existing txid is identical...
    BOOST_CHECK(ta.GetHash() == tb.GetHash());
    BOOST_CHECK(ta.GetHash() == tc.GetHash());
    // ...while the existing wtxid-shaped relay slot carries the normative
    // ptxid bytes and therefore distinguishes exact MPA variants.
    BOOST_CHECK(ta.GetWitnessHash() != tb.GetWitnessHash());
    BOOST_CHECK(ta.GetWitnessHash() != tc.GetWitnessHash());
    BOOST_CHECK(ta.GetWitnessHash().ToUint256() == ta.GetPtxid().ToUint256());
    // The full Modern serializations differ accordingly.
    BOOST_CHECK(Ser(a, TX_MODERN) != Ser(b, TX_MODERN));
    BOOST_CHECK(Ser(a, TX_MODERN) != Ser(c, TX_MODERN));
    // The txid form is literally the base bytes.
    BOOST_CHECK((HashWriter{} << TX_NO_WITNESS(ta)).GetHash() == ta.GetHash().ToUint256());
    // CMutableTransaction <-> CTransaction keeps the MPA.
    const CMutableTransaction back{ta};
    BOOST_CHECK(back.mpa == a.mpa);
}

BOOST_AUTO_TEST_SUITE_END()

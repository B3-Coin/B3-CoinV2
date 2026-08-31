// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 6 of the Modern PoS V1 finality plan: the normative ptxid =
// SHA256d(canonical full transaction serialization incl. witness and MPA),
// its identity rules, and the guardrail that txid remains the only state
// identity (outpoints/UTXO, merkle, legacy paths untouched).

#include <coins.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <legacy/codec.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <util/strencodings.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <type_traits>
#include <vector>

namespace {

CMutableTransaction BaseTx()
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"2222222222222222222222222222222222222222222222222222222222222222"}), 1};
    mtx.vin[0].scriptSig = CScript() << OP_1;
    mtx.vout.emplace_back(5000, CScript() << OP_TRUE);
    mtx.nLockTime = 9;
    return mtx;
}

CMpaRecord Rec(const uint16_t type, const std::vector<unsigned char>& payload)
{
    CMpaRecord r;
    r.payload_type = type;
    r.payload_version = 1;
    r.payload = payload;
    return r;
}

std::vector<unsigned char> Bytes(const CTransaction& tx, const TransactionSerParams& p)
{
    DataStream ss;
    ss << p(tx);
    const auto s{ss.str()};
    return std::vector<unsigned char>(s.begin(), s.end());
}

uint256 Sha256d(const std::vector<unsigned char>& b)
{
    return (HashWriter{} << std::span<const unsigned char>(b)).GetHash();
}

std::vector<unsigned char> Ev(const unsigned char fill) { return std::vector<unsigned char>(modern::FINALITY_KEY_EVIDENCE_SIZE, fill); }

// Ptxid must not compare with Txid / Wtxid / uint256 (distinct identity).
static_assert(!std::is_convertible_v<Ptxid, Txid> && !std::is_convertible_v<Txid, Ptxid>);
static_assert(!std::is_convertible_v<Ptxid, Wtxid> && !std::is_convertible_v<Ptxid, uint256>);

} // namespace

BOOST_FIXTURE_TEST_SUITE(ptxid_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(no_optional_data_ptxid_equals_txid_and_txid_pinned)
{
    const CTransaction tx{BaseTx()};
    BOOST_CHECK(!tx.HasWitness() && !tx.HasMpa());
    BOOST_CHECK(tx.GetPtxid().ToUint256() == tx.GetHash().ToUint256());
    // Normative byte definition: SHA256d of the canonical full serialization,
    // which for this transaction is exactly the base bytes.
    BOOST_CHECK(tx.GetPtxid().ToUint256() == Sha256d(Bytes(tx, TX_MODERN)));
    BOOST_CHECK(Bytes(tx, TX_MODERN) == Bytes(tx, TX_NO_WITNESS));
    // txid is the pre-Commit-6 definition: SHA256d(base serialization), pinned.
    BOOST_CHECK(tx.GetHash().ToUint256() == Sha256d(Bytes(tx, TX_NO_WITNESS)));
    BOOST_CHECK_EQUAL(tx.GetHash().GetHex(), "8048bbd0ae3b368d381b05880aaff535ee2c36cd8c91d873f75a1f9a92f3c4c1");
}

BOOST_AUTO_TEST_CASE(mpa_changes_ptxid_not_txid)
{
    CMutableTransaction a{BaseTx()}, b{BaseTx()}, c{BaseTx()};
    a.mpa.push_back(Rec(5, Ev(0x11)));
    b.mpa.push_back(Rec(5, Ev(0x22)));
    const CTransaction ta{a}, tb{b}, tc{c};
    BOOST_CHECK(ta.GetHash() == tb.GetHash() && ta.GetHash() == tc.GetHash());
    BOOST_CHECK(!(ta.GetPtxid() == tb.GetPtxid()));
    BOOST_CHECK(!(ta.GetPtxid() == tc.GetPtxid()));
    BOOST_CHECK(ta.GetPtxid().ToUint256() == Sha256d(Bytes(ta, TX_MODERN)));
    // one-byte payload change
    CMutableTransaction a2{a};
    a2.mpa[0].payload[100] ^= 0x01;
    BOOST_CHECK(!(CTransaction{a2}.GetPtxid() == ta.GetPtxid()));
    BOOST_CHECK(CTransaction{a2}.GetHash() == ta.GetHash());
    // The existing wtxid-shaped relay slot is the ptxid mechanism on B3, so
    // exact MPA variants cannot alias in mempool/orphan/reject maps.
    BOOST_CHECK(ta.GetWitnessHash() != tb.GetWitnessHash());
    BOOST_CHECK(ta.GetWitnessHash().ToUint256() == ta.GetPtxid().ToUint256());
}

BOOST_AUTO_TEST_CASE(record_order_and_noncanonical)
{
    // Two records in canonical order vs swapped: different canonical bytes, the
    // swapped form is rejected by the structural rule (not a second encoding
    // of the same transaction), and ptxid follows the bytes.
    CMutableTransaction sorted{BaseTx()}, swapped{BaseTx()};
    const auto r1{Rec(5, Ev(0x11))}, r2{Rec(5, Ev(0x22))};
    sorted.mpa = {r1, r2};
    swapped.mpa = {r2, r1};
    const CTransaction ts{sorted}, tw{swapped};
    BOOST_CHECK(ts.GetHash() == tw.GetHash());
    BOOST_CHECK(!(ts.GetPtxid() == tw.GetPtxid()));
    Consensus::Params active{};
    active.legacy_b3coin = true;
    active.hard_fork_height = 100;
    active.legacy_final_hash = uint256::ONE;
    active.modern_pos = Consensus::ModernPosParams{};
    std::string err;
    BOOST_CHECK(modern::CheckTransactionMpa(ts, active, err));
    BOOST_CHECK(!modern::CheckTransactionMpa(tw, active, err));
    BOOST_CHECK_EQUAL(err, "mpa-record-order");
    // Non-minimal CompactSize bytes cannot even decode, so no alternative bytes
    // (hence no alternative ptxid) exist for the same transaction.
    auto bytes{Bytes(ts, TX_MODERN)};
    const size_t count_pos{bytes.size() - 4 - (1 + 2 * (2 + 2 + 1 + 244))};
    BOOST_REQUIRE_EQUAL(bytes[count_pos], 0x02);
    std::vector<unsigned char> nm(bytes.begin(), bytes.begin() + count_pos);
    nm.push_back(0xfd); nm.push_back(0x02); nm.push_back(0x00);
    nm.insert(nm.end(), bytes.begin() + count_pos + 1, bytes.end());
    DataStream ss{nm};
    CMutableTransaction out;
    BOOST_CHECK_THROW(ss >> TX_MODERN(out), std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(witness_and_witness_plus_mpa)
{
    CMutableTransaction w{BaseTx()};
    w.vin[0].scriptWitness.stack.push_back({0xAA, 0xBB, 0xCC});
    const CTransaction tw{w};
    BOOST_CHECK(tw.HasWitness() && !tw.HasMpa());
    // Witness contributes: ptxid != txid, and follows the canonical full bytes.
    BOOST_CHECK(!(tw.GetPtxid().ToUint256() == tw.GetHash().ToUint256()));
    BOOST_CHECK(tw.GetPtxid().ToUint256() == Sha256d(Bytes(tw, TX_MODERN)));
    // For a witness-only transaction the canonical full bytes happen to be the
    // existing witness serialization, so the hashes coincide — an implementation
    // fact, tested as such, not the definition.
    BOOST_CHECK(Bytes(tw, TX_MODERN) == Bytes(tw, TX_WITH_WITNESS));
    BOOST_CHECK(tw.GetPtxid().ToUint256() == tw.GetWitnessHash().ToUint256());
    // Witness + MPA: both committed.
    CMutableTransaction wm{w};
    wm.mpa.push_back(Rec(5, Ev(0x33)));
    const CTransaction twm{wm};
    BOOST_CHECK(twm.GetHash() == tw.GetHash());
    BOOST_CHECK(twm.GetWitnessHash() != tw.GetWitnessHash()); // full relay id commits to MPA
    BOOST_CHECK(twm.GetWitnessHash().ToUint256() == twm.GetPtxid().ToUint256());
    BOOST_CHECK(!(twm.GetPtxid() == tw.GetPtxid()));
    CMutableTransaction wm2{wm};
    wm2.vin[0].scriptWitness.stack[0][0] ^= 0x01;               // witness change also changes ptxid
    BOOST_CHECK(!(CTransaction{wm2}.GetPtxid() == twm.GetPtxid()));
    BOOST_CHECK(twm.GetPtxid().ToUint256() == Sha256d(Bytes(twm, TX_MODERN)));
    // None of this activates SegWit: the witness bytes are only serialized, never
    // validated here, and the base txid ignores them exactly as before.
    BOOST_CHECK(twm.GetHash().ToUint256() == Sha256d(Bytes(twm, TX_NO_WITNESS)));
}

BOOST_AUTO_TEST_CASE(round_trip_and_mutable_conversion_preserve_ptxid)
{
    CMutableTransaction m{BaseTx()};
    m.vin[0].scriptWitness.stack.push_back({0x01});
    m.mpa.push_back(Rec(5, Ev(0x44)));
    const CTransaction t{m};
    // serialize / deserialize under the Modern context
    DataStream ss;
    ss << TX_MODERN(t);
    CMutableTransaction back;
    ss >> TX_MODERN(back);
    BOOST_CHECK(CTransaction{back}.GetPtxid() == t.GetPtxid());
    BOOST_CHECK(CTransaction{back}.GetHash() == t.GetHash());
    // mutable -> immutable -> mutable -> immutable
    const CMutableTransaction again{t};
    BOOST_CHECK(CTransaction{again}.GetPtxid() == t.GetPtxid());
    // deserialize-constructed CTransaction
    DataStream ss2;
    ss2 << TX_MODERN(t);
    const CTransaction from_stream{deserialize, TX_MODERN, ss2};
    BOOST_CHECK(from_stream.GetPtxid() == t.GetPtxid());
}

BOOST_AUTO_TEST_CASE(state_identity_stays_txid)
{
    CMutableTransaction m{BaseTx()};
    m.mpa.push_back(Rec(5, Ev(0x55)));
    const CTransaction t{m};
    // Outpoints / UTXO keys use txid.
    CCoinsView base;
    CCoinsViewCache view{&base};
    AddCoins(view, t, 100, /*check=*/false, 0, /*exclude_modern_cells=*/true);
    BOOST_CHECK(view.HaveCoin(COutPoint{t.GetHash(), 0}));
    BOOST_CHECK(!view.HaveCoin(COutPoint{Txid::FromUint256(t.GetPtxid().ToUint256()), 0}));
    // Transaction merkle root uses txid.
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(t));
    CMutableTransaction m2{BaseTx()};
    m2.mpa.push_back(Rec(5, Ev(0x66)));
    const uint256 root_a{BlockMerkleRoot(block)};
    block.vtx[0] = MakeTransactionRef(m2);
    BOOST_CHECK(BlockMerkleRoot(block) == root_a); // different MPA, same txid, same merkle root
    // Finality digests are tagged hashes over FinalizedBlock fields (block hash,
    // roots, heights) — no ptxid enters them (structural: no API takes one).
    static_assert(!std::is_constructible_v<modern::FinalizedBlock, Ptxid>);
}

BOOST_AUTO_TEST_CASE(no_coinbase_payload_root_ptxid_circularity)
{
    // Model the Commit-7 dependency: section bytes -> section hash -> payload root
    // -> coinbase cell commitment -> coinbase txid. None of it reads a ptxid.
    CMutableTransaction tx{BaseTx()};
    tx.mpa.push_back(Rec(5, Ev(0x77)));
    DataStream section;
    SerializeMpaSection(section, tx.mpa);
    const auto sec_str{section.str()};
    const std::vector<unsigned char> sec_bytes(sec_str.begin(), sec_str.end());
    HashWriter w{TaggedHash("B3/MPA/SECTION/V1")};
    w << std::span<const unsigned char>(sec_bytes);
    const uint256 section_hash{w.GetSHA256()};
    // The same section hash is obtained from the transaction's own MPA, regardless
    // of the transaction's outputs or its ptxid.
    CMutableTransaction other_outputs{tx};
    other_outputs.vout[0].nValue = 1;
    DataStream section2;
    SerializeMpaSection(section2, other_outputs.mpa);
    BOOST_CHECK(section2.str() == sec_str);
    BOOST_CHECK(!(CTransaction{other_outputs}.GetPtxid() == CTransaction{tx}.GetPtxid()));
    // A coinbase carrying a MODERN_PAYLOAD_ROOT cell whose commitment is the root:
    // its txid and ptxid depend on the root; the root depends only on section bytes.
    CMutableTransaction coinbase;
    coinbase.version = 2;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript() << CScriptNum{100} << CScriptNum{7};
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    const auto cell{modern::MakeMetadataCellScript(static_cast<uint16_t>(modern::PolicyType::MODERN_PAYLOAD_ROOT), 1,
                                                    section_hash /* stands in for payload_root */, {})};
    BOOST_REQUIRE(cell.has_value());
    coinbase.vout.emplace_back(0, *cell);
    const CTransaction cb{coinbase};
    BOOST_CHECK(cb.GetPtxid().ToUint256() == cb.GetHash().ToUint256()); // coinbase without MPA: ptxid == txid
    // Changing the root changes the coinbase txid/ptxid (downstream), never the
    // section hash (upstream): the relation is acyclic.
    CMutableTransaction coinbase2{coinbase};
    coinbase2.vout[1] = CTxOut{0, *modern::MakeMetadataCellScript(8, 1, uint256{"0101010101010101010101010101010101010101010101010101010101010101"}, {})};
    BOOST_CHECK(!(CTransaction{coinbase2}.GetHash() == cb.GetHash()));
    DataStream section3;
    SerializeMpaSection(section3, tx.mpa);
    BOOST_CHECK(section3.str() == sec_str);
}

BOOST_AUTO_TEST_CASE(legacy_unchanged_and_production_fail_closed)
{
    CMutableTransaction l;
    l.version = 1;
    l.nTime = 1'400'000'000;
    l.m_legacy_encoding = true;
    l.vin.resize(1);
    l.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"3333333333333333333333333333333333333333333333333333333333333333"}), 0};
    l.vout.emplace_back(1, CScript() << OP_TRUE);
    const CTransaction tl{l};
    // Legacy: hash = SHA256d(legacy bytes) as before; ptxid == txid; the legacy
    // codec carries no optional data.
    DataStream ss;
    ss << legacy::TX_LEGACY(tl);
    const auto lb{ss.str()};
    BOOST_CHECK(tl.GetHash().ToUint256() == Sha256d(std::vector<unsigned char>(lb.begin(), lb.end())));
    BOOST_CHECK(tl.GetPtxid().ToUint256() == tl.GetHash().ToUint256());
    BOOST_CHECK_EQUAL(tl.GetHash().GetHex(), "0bbfe1ccd68398eb5b810971d3e06c927423b85dd91c2dd76cff965155a62df6");
    // Production: any MPA is still invalid.
    Consensus::Params production{};
    production.legacy_b3coin = true;
    CMutableTransaction m{BaseTx()};
    m.mpa.push_back(Rec(5, Ev(0x01)));
    std::string err;
    BOOST_CHECK(!modern::CheckTransactionMpa(CTransaction{m}, production, err));
    BOOST_CHECK_EQUAL(err, "mpa-not-active");
}

BOOST_AUTO_TEST_SUITE_END()

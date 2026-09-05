// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 7 of the Modern PoS V1 finality plan: MODERN_PAYLOAD_ROOT — the
// Path-B commitment of every MPA section into the block hash. Root
// construction (tagged section hashes, explicit big-endian index leaves, the
// block Merkle algorithm incl. odd-leaf duplication), mutation/integrity
// properties, acyclicity with a coinbase MPA, the coinbase cell rule at block
// level (missing/duplicate/wrong/non-coinbase/params/value), Connect /
// Disconnect / reconsider, and legacy behaviour.

#include <consensus/merkle.h>
#include <hash.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/payload_root.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <validation.h>

#include <test/util/finality_fixture.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <vector>

using namespace b3test;

namespace {

CMpaRecord Rec(const unsigned char fill)
{
    CMpaRecord r;
    r.payload_type = modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
    r.payload_version = 1;
    r.payload.assign(modern::FINALITY_KEY_EVIDENCE_SIZE, fill);
    return r;
}

CMutableTransaction Plain(const unsigned seed)
{
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.resize(1);
    mtx.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{"4444444444444444444444444444444444444444444444444444444444444444"}), seed};
    mtx.vout.emplace_back(100 + seed, CScript() << OP_TRUE);
    return mtx;
}

CMutableTransaction Coinbase(const int height)
{
    CMutableTransaction cb;
    cb.version = 2;
    cb.vin.resize(1);
    cb.vin[0].prevout.SetNull();
    cb.vin[0].scriptSig = CScript() << CScriptNum{height} << CScriptNum{7};
    cb.vout.emplace_back(0, CScript() << OP_TRUE);
    return cb;
}

CBlock MakeBlock(std::vector<CMutableTransaction> txs)
{
    CBlock block;
    for (auto& t : txs) block.vtx.push_back(MakeTransactionRef(std::move(t)));
    return block;
}

std::string Hex(const uint256& u) { return HexStr(std::span<const unsigned char>(u.begin(), 32)); }

} // namespace

BOOST_FIXTURE_TEST_SUITE(payload_root_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(leaf_and_section_hash_construction_pinned)
{
    // Explicit big-endian 4-byte index, never host order.
    const uint256 zero{};
    const uint256 l0{modern::PayloadLeaf(0, zero)};
    const uint256 l1{modern::PayloadLeaf(1, zero)};
    const uint256 l256{modern::PayloadLeaf(256, zero)};
    BOOST_CHECK(l0 != l1 && l1 != l256);
    {
        // Recompute by hand: TaggedHash("B3/MPA/LEAF/V1", 00 00 01 00 || 0^32) for index 256
        HashWriter w{TaggedHash("B3/MPA/LEAF/V1")};
        const unsigned char pre[36]{0x00, 0x00, 0x01, 0x00};
        w << std::span<const unsigned char>(pre, 36);
        BOOST_CHECK(w.GetSHA256() == l256);
    }
    BOOST_CHECK_EQUAL(Hex(l0), "3d93dae34da8e9684ba8bfb3ec6ae5ec4c68ac57d5c6fd374c955f9719810aa1");
    BOOST_CHECK_EQUAL(Hex(l1), "1858c79e4c9458d8ea6c9b79ec9a6656f79ffd528eede449bb77675b2a9ce92e");
    // Section hash: tagged hash of the canonical section bytes; zero without MPA.
    CMutableTransaction tx{Plain(1)};
    BOOST_CHECK(modern::MpaSectionHash(CTransaction{tx}).IsNull());
    BOOST_CHECK(modern::CanonicalMpaSectionBytes(CTransaction{tx}).empty());
    tx.mpa.push_back(Rec(0x5A));
    const CTransaction t{tx};
    const auto section{modern::CanonicalMpaSectionBytes(t)};
    BOOST_CHECK_EQUAL(section.size(), 1u + 2 + 2 + 1 + 244);
    BOOST_CHECK_EQUAL(section[0], 0x01);
    {
        HashWriter w{TaggedHash("B3/MPA/SECTION/V1")};
        w << std::span<const unsigned char>(section);
        BOOST_CHECK(w.GetSHA256() == modern::MpaSectionHash(t));
    }
    BOOST_CHECK_EQUAL(Hex(modern::MpaSectionHash(t)), "65690de15ca0290657afe8451ef4c1b65f7de211b4c757e6fb5a530dd952a2f9");
    // The section hash does not depend on the transaction's inputs/outputs/ptxid.
    CMutableTransaction tx2{tx};
    tx2.vout[0].nValue += 1;
    BOOST_CHECK(modern::MpaSectionHash(CTransaction{tx2}) == modern::MpaSectionHash(t));
}

BOOST_AUTO_TEST_CASE(root_single_odd_even_and_duplication_semantics)
{
    // Single transaction (coinbase only): root == leaf[0].
    {
        const CBlock b{MakeBlock({Coinbase(1)})};
        BOOST_CHECK(modern::ComputePayloadRoot(b) == modern::PayloadLeaf(0, uint256{}));
    }
    // Three transactions: the existing algorithm duplicates the last node on odd
    // levels: root = H(H(l0,l1), H(l2,l2)).
    {
        CMutableTransaction a{Plain(1)}, b{Plain(2)};
        a.mpa.push_back(Rec(0x11));
        const CBlock blk{MakeBlock({Coinbase(1), a, b})};
        const auto leaves{modern::PayloadLeaves(blk)};
        BOOST_REQUIRE_EQUAL(leaves.size(), 3u);
        auto h2 = [](const uint256& x, const uint256& y) { return Hash(x, y); };
        const uint256 expected{h2(h2(leaves[0], leaves[1]), h2(leaves[2], leaves[2]))};
        BOOST_CHECK(modern::ComputePayloadRoot(blk) == expected);
        BOOST_CHECK(modern::ComputePayloadRoot(blk) == ComputeMerkleRoot(leaves));
        // Four transactions: balanced.
        CMutableTransaction c{Plain(3)};
        const CBlock blk4{MakeBlock({Coinbase(1), a, b, c})};
        const auto l4{modern::PayloadLeaves(blk4)};
        BOOST_CHECK(modern::ComputePayloadRoot(blk4) == h2(h2(l4[0], l4[1]), h2(l4[2], l4[3])));
        // Positional leaves are all distinct, so the classic duplicate-tail
        // ambiguity (3 leaves vs [l0,l1,l2,l2]) cannot arise from a real block:
        // appending a fourth transaction changes leaf[3] (index 3 != index 2).
        BOOST_CHECK(l4[3] != l4[2]);
    }
}

BOOST_AUTO_TEST_CASE(mutation_and_integrity_properties)
{
    CMutableTransaction a{Plain(1)}, b{Plain(2)}, c{Plain(3)};
    a.mpa.push_back(Rec(0x11));
    const CBlock base{MakeBlock({Coinbase(1), a, b, c})};
    const uint256 root{modern::ComputePayloadRoot(base)};
    // one byte of an MPA payload
    {
        CMutableTransaction a2{a};
        a2.mpa[0].payload[7] ^= 0x01;
        BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({Coinbase(1), a2, b, c})) != root);
    }
    // which transaction carries the MPA
    {
        CMutableTransaction a3{a}, b3{b};
        b3.mpa = a3.mpa;
        a3.mpa.clear();
        BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({Coinbase(1), a3, b3, c})) != root);
    }
    // transaction position / order
    {
        BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({Coinbase(1), b, a, c})) != root);
        // swapping two MPA-free transactions leaves every (position, section) pair
        // identical, so the payload root is unchanged — their order is committed by
        // the ordinary transaction Merkle root, not by the payload root
        BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({Coinbase(1), a, c, b})) == root);
    }
    // adding / removing an MPA-bearing transaction
    {
        CMutableTransaction d{Plain(4)};
        d.mpa.push_back(Rec(0x22));
        BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({Coinbase(1), a, b, c, d})) != root);
        BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({Coinbase(1), b, c})) != root);
    }
    // ordinary transaction state (inputs, outputs, values, coinbase outputs) with
    // identical positions and identical sections: root unchanged — the ordinary
    // Merkle root commits to that state.
    {
        CMutableTransaction a4{a}, b4{b}, cb{Coinbase(1)};
        a4.vout[0].nValue = 999;
        b4.vin[0].prevout.n = 77;
        cb.vout.emplace_back(0, CScript() << OP_FALSE);
        const CBlock mutated{MakeBlock({cb, a4, b4, c})};
        BOOST_CHECK(modern::ComputePayloadRoot(mutated) == root);
        BOOST_CHECK(BlockMerkleRoot(mutated) != BlockMerkleRoot(base));
    }
}

BOOST_AUTO_TEST_CASE(acyclic_even_when_coinbase_carries_mpa)
{
    // Coinbase MPA bytes -> section hash -> payload_root -> root cell -> coinbase
    // txid -> block Merkle root; never back into the root.
    CMutableTransaction cb{Coinbase(5)};
    cb.mpa.push_back(Rec(0x33));
    CMutableTransaction a{Plain(1)};
    a.mpa.push_back(Rec(0x44));
    // Root computed from a block whose coinbase has NO root cell yet...
    const CBlock probe{MakeBlock({cb, a})};
    const uint256 root{modern::ComputePayloadRoot(probe)};
    // ...equals the root computed after the cell (which depends on the root) is
    // appended: the cell changes the coinbase txid, not the root.
    cb.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
    const CBlock with_cell{MakeBlock({cb, a})};
    BOOST_CHECK(modern::ComputePayloadRoot(with_cell) == root);
    BOOST_CHECK(with_cell.vtx[0]->GetHash() != probe.vtx[0]->GetHash());
    BOOST_CHECK(BlockMerkleRoot(with_cell) != BlockMerkleRoot(probe));
    std::string err;
    BOOST_CHECK(modern::CheckBlockPayloadRoot(with_cell, err));
    // The coinbase's own MPA bytes DO enter the root (section_hash[0]).
    CMutableTransaction cb2{Coinbase(5)};
    cb2.mpa.push_back(Rec(0x34));
    BOOST_CHECK(modern::ComputePayloadRoot(MakeBlock({cb2, a})) != root);
    // And a different root in the cell is simply a mismatch (the root is not
    // recomputed from the cell).
    CMutableTransaction cb3{cb};
    cb3.vout.back() = CTxOut{0, modern::MakePayloadRootCellScript(uint256{"0909090909090909090909090909090909090909090909090909090909090909"})};
    BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb3, a}), err));
    BOOST_CHECK_EQUAL(err, "payload-root-mismatch");
}

BOOST_AUTO_TEST_CASE(cell_rule_unit)
{
    std::string err;
    // no MPA, no cell: fine
    BOOST_CHECK(modern::CheckBlockPayloadRoot(MakeBlock({Coinbase(1), Plain(1)}), err));
    // no MPA, a cell: invalid (no "empty payload root" encoding)
    {
        CMutableTransaction cb{Coinbase(1)};
        cb.vout.emplace_back(0, modern::MakePayloadRootCellScript(uint256{}));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb, Plain(1)}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-without-mpa");
    }
    CMutableTransaction a{Plain(1)};
    a.mpa.push_back(Rec(0x11));
    const uint256 root{modern::ComputePayloadRoot(MakeBlock({Coinbase(1), a}))};
    // missing
    BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({Coinbase(1), a}), err));
    BOOST_CHECK_EQUAL(err, "payload-root-missing");
    // valid
    CMutableTransaction cb{Coinbase(1)};
    cb.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
    BOOST_CHECK(modern::CheckBlockPayloadRoot(MakeBlock({cb, a}), err));
    // duplicate
    {
        CMutableTransaction cb2{cb};
        cb2.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb2, a}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-duplicate");
    }
    // non-coinbase
    {
        CMutableTransaction a2{a};
        a2.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({Coinbase(1), a2}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-not-in-coinbase");
    }
    // non-empty params / wrong version / wrong value
    {
        CMutableTransaction cb2{Coinbase(1)};
        cb2.vout.emplace_back(0, *modern::MakeMetadataCellScript(8, 1, root, std::vector<unsigned char>(4, 0)));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb2, a}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-params");
        CMutableTransaction cb3{Coinbase(1)};
        cb3.vout.emplace_back(0, *modern::MakeMetadataCellScript(8, 2, root, {}));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb3, a}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-version");
        CMutableTransaction cb4{Coinbase(1)};
        cb4.vout.emplace_back(1, modern::MakePayloadRootCellScript(root));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb4, a}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-value");
    }
    // wrong root
    {
        CMutableTransaction cb2{Coinbase(1)};
        cb2.vout.emplace_back(0, modern::MakePayloadRootCellScript(uint256{"0101010101010101010101010101010101010101010101010101010101010101"}));
        BOOST_CHECK(!modern::CheckBlockPayloadRoot(MakeBlock({cb2, a}), err));
        BOOST_CHECK_EQUAL(err, "payload-root-mismatch");
    }
    // The cell is a metadata cell of policy 8: zero value, empty params, not OP_RETURN.
    const CScript cell{modern::MakePayloadRootCellScript(root)};
    const auto parsed{modern::ParseMetadataCell(cell)};
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL(parsed->policy_type, 8);
    BOOST_CHECK(parsed->params.empty());
    BOOST_CHECK(parsed->commitment == root);
    BOOST_CHECK(!cell.IsUnspendable());
}

//! Block level on the synthetic chain: with the test contexts on, an MPA block
//! needs exactly one correct root cell; every other shape is refused; connect /
//! disconnect / reconsider behave; the cell never becomes a coin.
BOOST_FIXTURE_TEST_CASE(block_level_rule_and_reorg, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)};
    const auto b0{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const CMutableTransaction tx{MakeTx(0, {b0.cell}, {b0.record})};
    int64_t delta{60};
    const int base_height{Tip()->nHeight};
    auto expect_reject = [&](const CBlock& block) {
        (void)Submit(block);
        BOOST_CHECK_EQUAL(Tip()->nHeight, base_height);
    };
    // missing root cell
    expect_reject(BuildCorridor(Tip(), {tx}, delta++));
    const uint256 root{modern::ComputePayloadRoot(BuildCorridor(Tip(), {tx}, delta))};
    // wrong root
    expect_reject(BuildCorridor(Tip(), {tx}, delta++, EASY_BITS,
                                {CTxOut{0, modern::MakePayloadRootCellScript(uint256{"0202020202020202020202020202020202020202020202020202020202020202"})}}));
    // duplicate root cells
    expect_reject(BuildCorridor(Tip(), {tx}, delta++, EASY_BITS,
                                {CTxOut{0, modern::MakePayloadRootCellScript(root)}, CTxOut{0, modern::MakePayloadRootCellScript(root)}}));
    // root cell in a non-coinbase transaction
    {
        CMutableTransaction tx2{tx};
        tx2.vout.emplace_back(0, modern::MakePayloadRootCellScript(root));
        const CBlock probe{BuildCorridor(Tip(), {tx2}, delta)};
        const uint256 root2{modern::ComputePayloadRoot(probe)};
        expect_reject(BuildCorridor(Tip(), {tx2}, delta++, EASY_BITS, {CTxOut{0, modern::MakePayloadRootCellScript(root2)}}));
    }
    // non-zero value / non-empty params root cells
    expect_reject(BuildCorridor(Tip(), {tx}, delta++, EASY_BITS, {CTxOut{1, modern::MakePayloadRootCellScript(root)}}));
    expect_reject(BuildCorridor(Tip(), {tx}, delta++, EASY_BITS,
                                {CTxOut{0, *modern::MakeMetadataCellScript(8, 1, root, std::vector<unsigned char>(1, 0))}}));
    // a root cell in a block WITHOUT any MPA
    expect_reject(BuildCorridor(Tip(), {}, delta++, EASY_BITS, {CTxOut{0, modern::MakePayloadRootCellScript(uint256{})}}));
    // the correct block connects; the cell is never a coin; a plain block connects without a cell
    const CBlock good{BuildCorridorWithRoot({tx}, delta++)};
    BOOST_REQUIRE(Submit(good));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, base_height + 1);
    {
        LOCK(cs_main);
        CCoinsViewCache& view{m_node.chainman->ActiveChainstate().CoinsTip()};
        BOOST_CHECK(!view.HaveCoin(COutPoint{good.vtx[0]->GetHash(), 1})); // the root cell
        BOOST_CHECK(view.HaveCoin(COutPoint{good.vtx[1]->GetHash(), 0}));  // ordinary output of tx
    }
    BOOST_REQUIRE(Submit(BuildCorridor(Tip(), {}, 60)));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, base_height + 2);
    // disconnect both, reconsider, same tip
    const uint256 good_hash{good.GetHash()};
    {
        BlockValidationState state;
        CBlockIndex* idx{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(good_hash))};
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, idx));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, base_height);
    BOOST_CHECK_EQUAL(Index().Size(), 0u);
    {
        CBlockIndex* idx{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(good_hash))};
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(idx);
        m_node.chainman->RecalculateBestHeader();
    }
    {
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, base_height + 2);
    BOOST_CHECK_EQUAL(Index().Size(), 1u);
}

//! MPA bytes do not enter a transaction id or the ordinary block Merkle root.
//! A malformed relay copy must therefore be treated like other uncommitted
//! block data: it cannot permanently invalidate the shared header before the
//! authentic body arrives. Once the payload root commits the same malformed
//! bytes, their semantic failure remains consensus-invalid.
BOOST_FIXTURE_TEST_CASE(uncommitted_mpa_cannot_poison_authentic_block, BindingFixture)
{
    Prepare();
    const auto bls_a{Bls(1)};
    const auto binding_a{MakeBinding(m_validator_a, m_vk_a, &bls_a, 0)};
    const CMutableTransaction tx{MakeTx(0, {binding_a.cell}, {binding_a.record})};
    const CBlock authentic{BuildCorridorWithRoot({tx})};

    // Alter only the hash-external MPA. The committed transaction and block
    // identities stay exactly the same as the authentic block.
    CBlock malformed_copy{authentic};
    CMutableTransaction malformed_tx{*malformed_copy.vtx[1]};
    BOOST_REQUIRE(!malformed_tx.mpa.empty());
    BOOST_REQUIRE(!malformed_tx.mpa[0].payload.empty());
    malformed_tx.mpa[0].payload.pop_back();
    malformed_copy.vtx[1] = MakeTransactionRef(std::move(malformed_tx));
    BOOST_REQUIRE(malformed_copy.vtx[1]->GetHash() == authentic.vtx[1]->GetHash());
    BOOST_REQUIRE(malformed_copy.GetHash() == authentic.GetHash());

    {
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(
            m_node.chainman->ActiveChainstate(), malformed_copy,
            /*check_pow=*/false, /*check_merkle_root=*/true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_MUTATED);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-payload-root");
    }

    const int height_before{Tip()->nHeight};
    BOOST_CHECK(!Submit(malformed_copy));
    {
        LOCK(cs_main);
        const CBlockIndex* index{
            m_node.chainman->m_blockman.LookupBlockIndex(authentic.GetHash())};
        BOOST_REQUIRE(index != nullptr);
        BOOST_CHECK(!(index->nStatus & BLOCK_FAILED_VALID));
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
    }

    // The authentic body with the same header is still accepted and stored.
    BOOST_REQUIRE(Submit(authentic));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, height_before + 1);
    BOOST_CHECK(Tip()->GetBlockHash() == authentic.GetHash());

    // Recompute the payload root over a malformed record. Now the header does
    // commit those bytes, so their grammar failure is a consensus failure.
    const auto bls_b{Bls(2)};
    const auto binding_b{MakeBinding(m_validator_b, m_vk_b, &bls_b, 0)};
    CMutableTransaction committed_bad_tx{
        MakeTx(1, {binding_b.cell}, {binding_b.record})};
    BOOST_REQUIRE(!committed_bad_tx.mpa.empty());
    committed_bad_tx.mpa[0].payload.pop_back();
    const CBlock committed_bad{BuildCorridorWithRoot({committed_bad_tx})};
    {
        LOCK(cs_main);
        const BlockValidationState state{TestBlockValidity(
            m_node.chainman->ActiveChainstate(), committed_bad,
            /*check_pow=*/false, /*check_merkle_root=*/true)};
        BOOST_REQUIRE(state.IsInvalid());
        BOOST_CHECK(state.GetResult() == BlockValidationResult::BLOCK_CONSENSUS);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-mpa");
    }
    BOOST_CHECK(!Submit(committed_bad));
    {
        LOCK(cs_main);
        const CBlockIndex* index{
            m_node.chainman->m_blockman.LookupBlockIndex(committed_bad.GetHash())};
        BOOST_REQUIRE(index != nullptr);
        BOOST_CHECK(index->nStatus & BLOCK_FAILED_VALID);
        BOOST_CHECK(!(index->nStatus & BLOCK_HAVE_DATA));
    }
}

//! Legacy era: a root-shaped cell in a legacy block is an ordinary output and
//! no payload-root rule runs below H+1 (legacy blocks cannot carry an MPA).
BOOST_FIXTURE_TEST_CASE(legacy_unchanged, ModernPosSetup)
{
    const Consensus::Params& consensus{m_node.chainman->GetConsensus()};
    Txid coinbase1{};
    for (int height{1}; height <= SYN_H - 1; ++height) {
        const CBlock block{BuildLegacy(Tip(), {})};
        BOOST_REQUIRE(Submit(block));
        if (height == 1) coinbase1 = block.vtx[0]->GetHash();
    }
    CMutableTransaction tx;
    tx.version = 1;
    tx.nTime = static_cast<uint32_t>(Tip()->GetBlockTime() + 17);
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint{coinbase1, 0};
    tx.vout.emplace_back(legacy::GetProofOfWorkReward(0, 1, consensus) - 1, CScript() << OP_TRUE);
    tx.vout.emplace_back(0, modern::MakePayloadRootCellScript(uint256{}));
    const CBlock block_h{BuildLegacy(Tip(), {tx})};
    BOOST_REQUIRE(Submit(block_h));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H);
    LOCK(cs_main);
    BOOST_CHECK(m_node.chainman->ActiveChainstate().CoinsTip().HaveCoin(COutPoint{block_h.vtx[1]->GetHash(), 1}));
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 5 of the Modern PoS V1 finality plan: FINALITY_KEY cell <-> MPA
// evidence binding at block level on the synthetic B3 chain — valid
// bind/rotate/revoke, every binding failure, no partial index mutation,
// Connect/Disconnect/rebuild exactness, txid invariance, and production
// fail-closed behaviour.

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <key.h>
#include <modern/chain_domain.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <node/derived_state.h>
#include <node/finality_binding_index.h>
#include <primitives/transaction.h>
#include <uint256.h>
#include <validation.h>

#include <test/util/finality_fixture.h>
#include <test/util/modern_pos_setup.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

using namespace b3test;
using modern::FinalityKeyEvidence;
using modern::FinalityKeyParams;
using node::FinalityBindingIndex;
using node::FinalityBindingTracker;


BOOST_FIXTURE_TEST_SUITE(finality_key_binding_tests, BasicTestingSetup)

BOOST_FIXTURE_TEST_CASE(valid_bind_rotate_revoke_and_index, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    // bind (seq 0)
    const auto b0{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const CMutableTransaction tx0{MakeTx(0, {b0.cell}, {b0.record})};
    const Txid txid0{CTransaction{tx0}.GetHash()};
    BOOST_REQUIRE(SubmitCorridor({tx0}));
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, SYN_H + 1);
    {
        const auto& index{Index()};
        const auto rec{index.Get(m_vk_a)};
        BOOST_REQUIRE(rec.has_value());
        BOOST_CHECK(rec->bls_pubkey == k1.GetPublicKey().Compressed());
        BOOST_CHECK_EQUAL(rec->seq, 0u);
        BOOST_CHECK_EQUAL(rec->height, SYN_H + 1);
        BOOST_CHECK(index.OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_a});
        LOCK(cs_main);
        CCoinsViewCache& view{m_node.chainman->ActiveChainstate().CoinsTip()};
        BOOST_CHECK(view.HaveCoin(COutPoint{txid0, 0}));   // ordinary output
        BOOST_CHECK(!view.HaveCoin(COutPoint{txid0, 1}));  // the cell: never a coin
    }
    // rotate (seq 1)
    const auto b1{MakeBinding(m_validator_a, m_vk_a, &k2, 1)};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(1, {b1.cell}, {b1.record})}));
    {
        const auto& index{Index()};
        BOOST_CHECK(index.Get(m_vk_a)->bls_pubkey == k2.GetPublicKey().Compressed());
        BOOST_CHECK(!index.OwnerOf(k1.GetPublicKey().Compressed()).has_value());
    }
    // revoke (seq 2): zero key, zero PoP
    const auto r{MakeBinding(m_validator_a, m_vk_a, nullptr, 2)};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(2, {r.cell}, {r.record})}));
    {
        const auto& index{Index()};
        BOOST_CHECK(index.Get(m_vk_a)->IsRevoked());
        BOOST_CHECK_EQUAL(index.Get(m_vk_a)->seq, 2u);
        BOOST_CHECK(index.SnapshotActive().empty());
    }
    // a second validator binds in the same block as the first re-binds (two txs, in-block overlay)
    const auto a3{MakeBinding(m_validator_a, m_vk_a, &k1, 3)};
    const auto bb{MakeBinding(m_validator_b, m_vk_b, &k2, 0)}; // k2 was released by a's revocation
    BOOST_REQUIRE(SubmitCorridor({MakeTx(3, {a3.cell}, {a3.record}), MakeTx(4, {bb.cell}, {bb.record})}));
    {
        const auto& index{Index()};
        BOOST_CHECK(index.OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_a});
        BOOST_CHECK(index.OwnerOf(k2.GetPublicKey().Compressed()) == std::optional{m_vk_b});
        BOOST_CHECK_EQUAL(index.SnapshotActive().size(), 2u);
    }
}

BOOST_FIXTURE_TEST_CASE(binding_failures_are_rejected_without_index_mutation, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    const auto good{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    // Baseline index state: empty.
    BOOST_CHECK_EQUAL(Index().Size(), 0u);
    int64_t delta{60};
    // Structural failures are refused before storage (Submit false); semantic
    // failures are refused at connect time (the block is stored and marked
    // failed, Submit may return true) — in every case the tip must not move
    // and the binding index must stay untouched.
    auto expect_reject = [&](const std::vector<CMutableTransaction>& txs) {
        const int before{Tip()->nHeight};
        (void)SubmitCorridor(txs, delta++); // distinct timestamps -> distinct blocks
        BOOST_CHECK_EQUAL(Tip()->nHeight, before);
        BOOST_CHECK_EQUAL(Index().Size(), 0u); // no partial mutation
    };
    // cell without evidence
    expect_reject({MakeTx(0, {good.cell}, {})});
    // evidence without cell (orphan)
    expect_reject({MakeTx(0, {}, {good.record})});
    // duplicate evidence
    expect_reject({MakeTx(0, {good.cell}, {good.record, good.record})});
    // duplicate cells competing for one evidence
    expect_reject({MakeTx(0, {good.cell, good.cell}, {good.record})});
    // field mismatch: cell for seq 0, evidence for seq 1
    {
        const auto other{MakeBinding(m_validator_a, m_vk_a, &k1, 1)};
        expect_reject({MakeTx(0, {good.cell}, {other.record})});
    }
    // wrong BIP340 signer
    {
        const auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0, &m_validator_b)};
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // wrong domain
    {
        const uint256 other_domain{uint256{"abababababababababababababababababababababababababababababababab"}};
        const auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0, nullptr, &other_domain)};
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // cross-key PoP
    {
        auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
        FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(bad.record.payload)};
        ev.pop = k2.SignPoP().Compressed();
        const auto enc{ev.Encode()};
        bad.record.payload.assign(enc.begin(), enc.end());
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // nonzero key with zero PoP
    {
        auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
        FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(bad.record.payload)};
        ev.pop.fill(0);
        const auto enc{ev.Encode()};
        bad.record.payload.assign(enc.begin(), enc.end());
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // revocation with a nonzero PoP (first transition may be a revocation at seq 0)
    {
        auto bad{MakeBinding(m_validator_a, m_vk_a, nullptr, 0)};
        FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(bad.record.payload)};
        ev.pop = k1.SignPoP().Compressed();
        const auto enc{ev.Encode()};
        bad.record.payload.assign(enc.begin(), enc.end());
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // wrong first seq
    {
        const auto bad{MakeBinding(m_validator_a, m_vk_a, &k1, 1)};
        expect_reject({MakeTx(0, {bad.cell}, {bad.record})});
    }
    // one valid pair + one invalid pair in the same block: whole block rejected, nothing applied
    {
        const auto bad{MakeBinding(m_validator_b, m_vk_b, &k2, 5)}; // bad first seq
        expect_reject({MakeTx(0, {good.cell}, {good.record}), MakeTx(1, {bad.cell}, {bad.record})});
    }
    // duplicate BLS key across validators in one block: second pair rejected -> block rejected
    {
        const auto dup{MakeBinding(m_validator_b, m_vk_b, &k1, 0)};
        expect_reject({MakeTx(0, {good.cell}, {good.record}), MakeTx(1, {dup.cell}, {dup.record})});
    }
    // inactive MPA type inside an otherwise valid transaction
    {
        auto tx{MakeTx(0, {good.cell}, {good.record})};
        CMpaRecord inactive;
        inactive.payload_type = 4; inactive.payload_version = 1; inactive.payload.assign(10, 0);
        tx.mpa.push_back(inactive);
        std::sort(tx.mpa.begin(), tx.mpa.end(), modern::MpaRecordLess);
        expect_reject({tx});
    }
    // Finally the good pair connects, proving the chain/fixture were fine all along.
    BOOST_REQUIRE(SubmitCorridor({MakeTx(0, {good.cell}, {good.record})}, delta));
    BOOST_CHECK_EQUAL(Index().Size(), 1u);
    // ...and a duplicate BLS key across validators in a LATER block is rejected too.
    const auto dup2{MakeBinding(m_validator_b, m_vk_b, &k1, 0)};
    const int height_before{Tip()->nHeight};
    (void)SubmitCorridor({MakeTx(1, {dup2.cell}, {dup2.record})}, delta + 1);
    BOOST_CHECK_EQUAL(Tip()->nHeight, height_before);
    BOOST_CHECK_EQUAL(Index().Size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(connect_disconnect_rebuild_exactness, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    const auto b0{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const auto b1{MakeBinding(m_validator_a, m_vk_a, &k2, 1)};
    const auto bb{MakeBinding(m_validator_b, m_vk_b, &k1, 0)}; // takes k1 after a moved off it
    BOOST_REQUIRE(SubmitCorridor({MakeTx(0, {b0.cell}, {b0.record})}));
    const uint256 h1{Tip()->GetBlockHash()};
    BOOST_REQUIRE(SubmitCorridor({MakeTx(1, {b1.cell}, {b1.record}), MakeTx(2, {bb.cell}, {bb.record})}));
    const uint256 h2{Tip()->GetBlockHash()};
    const auto state_after_2{Index().All()};
    BOOST_CHECK_EQUAL(state_after_2.size(), 2u);
    BOOST_CHECK(Index().OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_b});
    // Disconnect the top block: exact prior state (a bound to k1, b absent).
    {
        BlockValidationState state;
        CBlockIndex* idx{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(h2))};
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, idx));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == h1);
    {
        const auto& index{Index()};
        BOOST_CHECK_EQUAL(index.Size(), 1u);
        BOOST_CHECK(index.Get(m_vk_a)->bls_pubkey == k1.GetPublicKey().Compressed());
        BOOST_CHECK(index.OwnerOf(k1.GetPublicKey().Compressed()) == std::optional{m_vk_a});
        BOOST_CHECK(!index.Get(m_vk_b).has_value());
    }
    // Reconsider: identical state again.
    {
        CBlockIndex* idx{WITH_LOCK(cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(h2))};
        LOCK(cs_main);
        m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(idx);
        m_node.chainman->RecalculateBestHeader();
    }
    {
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
    }
    BOOST_REQUIRE(Tip()->GetBlockHash() == h2);
    BOOST_CHECK(Index().All() == state_after_2);
    // Rebuild from disk (fresh tracker) equals the incremental state.
    {
        LOCK(cs_main);
        FinalityBindingTracker fresh;
        BOOST_REQUIRE(fresh.Sync(m_node.chainman->ActiveChain(), m_node.chainman->m_blockman,
                                 m_node.chainman->GetConsensus(), *Tip()));
        BOOST_CHECK(fresh.Index().All() == state_after_2);
        for (const auto& pk : {k1.GetPublicKey().Compressed(), k2.GetPublicKey().Compressed()}) {
            BOOST_CHECK(fresh.Index().OwnerOf(pk) == Index().OwnerOf(pk));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(txid_unchanged_and_production_fail_closed, BindingFixture)
{
    Prepare();
    const auto k1{Bls(1)}, k2{Bls(2)};
    const auto e1{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    const auto e2{MakeBinding(m_validator_a, m_vk_a, &k1, 0)};
    // Same base transaction, different evidence bytes (a second signature with the
    // same inputs is identical under BIP340 with zero aux... so perturb the PoP
    // by using another key's PoP): txid stays stable, while the full relay
    // identities commit to the changed MPA payload.
    CMutableTransaction ta{MakeTx(0, {e1.cell}, {e1.record})};
    CMutableTransaction tb{MakeTx(0, {e2.cell}, {e2.record})};
    FinalityKeyEvidence ev{*FinalityKeyEvidence::Decode(tb.mpa[0].payload)};
    ev.pop = k2.SignPoP().Compressed();
    const auto enc{ev.Encode()};
    tb.mpa[0].payload.assign(enc.begin(), enc.end());
    const CTransaction tx_a{ta};
    const CTransaction tx_b{tb};
    BOOST_CHECK(tx_a.GetHash() == tx_b.GetHash());
    BOOST_CHECK(tx_a.GetWitnessHash() != tx_b.GetWitnessHash());
    BOOST_CHECK(!(tx_a.GetPtxid() == tx_b.GetPtxid()));
    BOOST_CHECK(!(ta.mpa == tb.mpa));
    // Production: the same block shape is refused while the X-pin
    // configuration is incomplete (the F = M activation predicate is off).
    const auto saved_pos{MutableConsensus().modern_pos};
    MutableConsensus().modern_pos.reset();
    BOOST_CHECK(!SubmitCorridor({ta}));
    BOOST_CHECK(!SubmitCorridor({ta}, 61));
    MutableConsensus().modern_pos = saved_pos;
    BOOST_CHECK_EQUAL(Tip()->nHeight, SYN_H);
    BOOST_CHECK(Consensus::ModernObjectRulesActive(
        CreateChainParams(ArgsManager{}, ChainType::MAIN)->GetConsensus()));
    for (const auto chain : {ChainType::TESTNET, ChainType::TESTNET4,
                             ChainType::SIGNET, ChainType::REGTEST}) {
        const auto chain_params{CreateChainParams(ArgsManager{}, chain)};
        const auto& c{chain_params->GetConsensus()};
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(c));
    }
    // Legacy era: a legacy block cannot carry an MPA at all (codec), and the
    // binding machinery never runs below H+1 (the era gate in ConnectBlock).
    BOOST_CHECK(Consensus::GetB3Era(SYN_H, m_node.chainman->GetConsensus()) == Consensus::B3Era::LEGACY);
}

// B0b: the index encodes to one canonical byte string, undo trail included,
// and decodes only into an index that ConnectBlock could have built. The
// index verifies no cryptography, so plain byte patterns stand in for keys.
namespace {
modern::ValidatorKeyBytes Vk(const unsigned char b)
{
    modern::ValidatorKeyBytes vk;
    vk.fill(b);
    return vk;
}
modern::BlsPubkeyBytes Pk(const unsigned char b)
{
    modern::BlsPubkeyBytes pk;
    pk.fill(b);
    return pk;
}
FinalityBindingIndex::Transition Bind(const modern::ValidatorKeyBytes& vk, const modern::BlsPubkeyBytes& pk,
                                      const uint32_t seq, const int height)
{
    return {vk, modern::BindingRecord{pk, seq, height}};
}
} // namespace

BOOST_AUTO_TEST_CASE(binding_index_codec_round_trip)
{
    const auto a{Vk(0xa1)}, b{Vk(0xb2)}, c{Vk(0xc3)}, d{Vk(0xd4)};
    const modern::BlsPubkeyBytes revoked{};
    const std::vector<std::pair<int, std::vector<FinalityBindingIndex::Transition>>> blocks{
        {10, {Bind(a, Pk(1), 0, 10), Bind(b, Pk(2), 0, 10)}},
        // Two transitions of one validator in one block; c takes a's old key.
        {12, {Bind(a, Pk(3), 1, 12), Bind(a, Pk(4), 2, 12), Bind(c, Pk(1), 0, 12)}},
        {15, {Bind(b, revoked, 1, 15)}},
        {20, {Bind(d, Pk(2), 0, 20)}}, // d takes the key b revoked
    };
    const auto round_trip{[](const FinalityBindingIndex& index, const int last_height) {
        const std::vector<unsigned char> bytes{index.Encode()};
        const auto decoded{FinalityBindingIndex::Decode(bytes, 10, last_height)};
        BOOST_REQUIRE(decoded);
        BOOST_CHECK(decoded->SameState(index));
        BOOST_CHECK(decoded->Encode() == bytes);
        return *decoded;
    }};
    FinalityBindingIndex index;
    std::vector<FinalityBindingIndex> states{index};
    round_trip(index, 9);
    for (const auto& [height, transitions] : blocks) {
        index.ConnectBlock(height, transitions);
        states.push_back(index);
        round_trip(index, height);
    }
    BOOST_CHECK(!states.front().SameState(index));
    // A decoded index keeps working: it disconnects and reconnects every
    // block into exactly the states the original passed through.
    FinalityBindingIndex decoded{round_trip(index, 20)};
    BOOST_CHECK(decoded.OwnerOf(Pk(1)) == std::optional{c});
    BOOST_CHECK(decoded.OwnerOf(Pk(2)) == std::optional{d});
    BOOST_CHECK(decoded.OwnerOf(Pk(4)) == std::optional{a});
    BOOST_CHECK(!decoded.OwnerOf(Pk(3)));
    for (size_t i{blocks.size()}; i-- > 0;) {
        decoded.DisconnectBlock(blocks[i].first);
        BOOST_CHECK(decoded.SameState(states[i]));
    }
    for (size_t i{0}; i < blocks.size(); ++i) {
        decoded.ConnectBlock(blocks[i].first, blocks[i].second);
        BOOST_CHECK(decoded.SameState(states[i + 1]));
    }
    // The undo trail is part of the state: the same records reached by
    // another block are another state.
    FinalityBindingIndex once, twice;
    once.ConnectBlock(10, {Bind(a, Pk(1), 0, 10)});
    twice.ConnectBlock(10, {Bind(a, Pk(1), 0, 10), Bind(a, Pk(1), 0, 10)});
    BOOST_CHECK(once.All() == twice.All());
    BOOST_CHECK(!once.SameState(twice));
}

BOOST_AUTO_TEST_CASE(binding_index_decode_refuses_malformed)
{
    const auto a{Vk(0xa1)}, b{Vk(0xb2)};
    FinalityBindingIndex index;
    index.ConnectBlock(10, {Bind(a, Pk(1), 0, 10)});
    index.ConnectBlock(12, {Bind(a, Pk(2), 1, 12), Bind(b, Pk(3), 0, 12)});
    const std::vector<unsigned char> bytes{index.Encode()};
    // Layout: 1 count byte, two 88-byte records (a at 1, b at 89), 1 count
    // byte at 177; block 10 at 178 (entry a, no previous: flag at 215);
    // block 12 at 216 (entry a with its previous record, whose height sits
    // at 306; entry b at 310, no previous). 343 bytes.
    BOOST_REQUIRE_EQUAL(bytes.size(), 343u);
    BOOST_REQUIRE(FinalityBindingIndex::Decode(bytes, 10, 12));
    const auto refused{[](const std::vector<unsigned char>& doctored) {
        return !FinalityBindingIndex::Decode(doctored, 10, 12).has_value();
    }};
    const auto patched_le32{[&](const std::vector<std::pair<size_t, uint32_t>>& patches) {
        std::vector<unsigned char> doctored{bytes};
        for (const auto& [offset, value] : patches) WriteLE32(doctored.data() + offset, value);
        return doctored;
    }};
    // Every strict prefix, and one byte too many.
    for (size_t n{0}; n < bytes.size(); ++n) {
        BOOST_CHECK(refused({bytes.begin(), bytes.begin() + n}));
    }
    std::vector<unsigned char> longer{bytes};
    longer.push_back(0);
    BOOST_CHECK(refused(longer));
    // Blocks outside [first_height, last_height].
    BOOST_CHECK(!FinalityBindingIndex::Decode(bytes, 11, 12));
    BOOST_CHECK(!FinalityBindingIndex::Decode(bytes, 10, 11));
    // Records out of order: the encoding is no longer canonical.
    {
        std::vector<unsigned char> swapped{bytes};
        std::swap_ranges(swapped.begin() + 1, swapped.begin() + 89, swapped.begin() + 89);
        BOOST_CHECK(refused(swapped));
    }
    // A previous-record flag other than 0 or 1, on the entry that has a
    // previous record: read as a boolean, everything else would decode.
    {
        std::vector<unsigned char> flag{bytes};
        BOOST_REQUIRE_EQUAL(int{flag[253]}, 1);
        flag[253] = 2;
        BOOST_CHECK(refused(flag));
    }
    // Block heights not strictly increasing, with every record consistent
    // with them (both blocks at 10; the current records' heights at 85 and
    // 173 moved to match).
    BOOST_CHECK(refused(patched_le32({{216, 10}, {85, 10}, {173, 10}})));
    // A previous record not set at the validator's entry before it.
    BOOST_CHECK(refused(patched_le32({{306, 11}})));
    // A current record not set at the validator's last entry.
    BOOST_CHECK(refused(patched_le32({{85, 10}})));
    // A trail cut below its first block: a's entry at 12 then names a
    // previous record that no entry set.
    {
        std::vector<unsigned char> cut{bytes.begin(), bytes.begin() + 177};
        cut.push_back(1);
        cut.insert(cut.end(), bytes.begin() + 216, bytes.end());
        BOOST_CHECK(refused(cut));
    }
    // A validator with entries but no record (b's record dropped).
    {
        std::vector<unsigned char> cut;
        cut.push_back(1);
        cut.insert(cut.end(), bytes.begin() + 1, bytes.begin() + 89);
        cut.insert(cut.end(), bytes.begin() + 177, bytes.end());
        BOOST_CHECK(refused(cut));
    }
    // A record for a validator without entries (b's record renamed c).
    {
        std::vector<unsigned char> renamed{bytes};
        std::fill(renamed.begin() + 89, renamed.begin() + 121, 0xc3);
        BOOST_CHECK(refused(renamed));
    }
    // One nonzero key held by two validators.
    FinalityBindingIndex shared;
    shared.ConnectBlock(10, {Bind(a, Pk(1), 0, 10), Bind(b, Pk(1), 0, 10)});
    BOOST_CHECK(!FinalityBindingIndex::Decode(shared.Encode(), 10, 10));
}

// B0b: a record is accepted only whole, from the same build, under the same
// parameters digest; no damaged byte string is ever accepted.
BOOST_AUTO_TEST_CASE(derived_state_record_codec)
{
    using node::DerivedStateCheck;
    node::DerivedStateRecord record;
    record.params_digest = uint256{1};
    record.block_hash = uint256{2};
    record.height = 1234;
    record.payload = {1, 2, 3, 4, 5};
    const std::vector<unsigned char> bytes{node::EncodeDerivedStateRecord(record, "v1-abc")};
    const auto check{[](const std::span<const unsigned char> b, const std::string_view build, const uint256& digest) {
        node::DerivedStateRecord out;
        return node::DecodeDerivedStateRecord(b, build, digest, out);
    }};
    node::DerivedStateRecord decoded;
    BOOST_REQUIRE(node::DecodeDerivedStateRecord(bytes, "v1-abc", uint256{1}, decoded) == DerivedStateCheck::OK);
    BOOST_CHECK(decoded.params_digest == record.params_digest);
    BOOST_CHECK(decoded.block_hash == record.block_hash);
    BOOST_CHECK_EQUAL(decoded.height, 1234);
    BOOST_CHECK(decoded.payload == record.payload);
    BOOST_CHECK(check(bytes, "v1-abd", uint256{1}) == DerivedStateCheck::OTHER_BUILD);
    BOOST_CHECK(check(bytes, "v1-abc", uint256{3}) == DerivedStateCheck::OTHER_PARAMS);
    for (size_t i{0}; i < bytes.size(); ++i) {
        std::vector<unsigned char> flipped{bytes};
        flipped[i] ^= 0x01;
        const DerivedStateCheck result{check(flipped, "v1-abc", uint256{1})};
        BOOST_CHECK(result != DerivedStateCheck::OK);
        if (i == 0) BOOST_CHECK(result == DerivedStateCheck::OTHER_FORMAT);
    }
    for (size_t n{0}; n < bytes.size(); ++n) {
        BOOST_CHECK(check(std::span{bytes}.first(n), "v1-abc", uint256{1}) != DerivedStateCheck::OK);
    }
    std::vector<unsigned char> longer{bytes};
    longer.push_back(0);
    BOOST_CHECK(check(longer, "v1-abc", uint256{1}) == DerivedStateCheck::MALFORMED);
}

BOOST_AUTO_TEST_CASE(derived_state_store_round_trip)
{
    const auto id{node::DerivedStateId::FINALITY_BINDINGS};
    node::DerivedStateStore store{DBParams{
        .path = m_args.GetDataDirNet() / "derivedstate",
        .cache_bytes = 1 << 20,
        .memory_only = true}};
    BOOST_CHECK(!store.Read(id));
    const std::vector<unsigned char> value{9, 8, 7};
    store.Write(id, value);
    BOOST_CHECK(store.Read(id) == std::optional{value});
    store.Erase(id);
    BOOST_CHECK(!store.Read(id));
}

// The digest covers exactly what the binding derivation reads: the era
// boundary and the modern chain domain.
BOOST_AUTO_TEST_CASE(binding_params_digest_reads_four_fields)
{
    Consensus::Params params{CreateChainParams(ArgsManager{}, ChainType::REGTEST)->GetConsensus()};
    params.legacy_b3coin = true;
    params.hard_fork_height = 100;
    params.legacy_final_hash = uint256{7};
    const uint256 base{FinalityBindingTracker::ParamsDigest(params)};
    const auto differs{[&](const auto& mutate) {
        Consensus::Params changed{params};
        mutate(changed);
        return FinalityBindingTracker::ParamsDigest(changed) != base;
    }};
    BOOST_CHECK(differs([](Consensus::Params& p) { p.hashGenesisBlock = uint256{8}; }));
    BOOST_CHECK(differs([](Consensus::Params& p) { p.legacy_b3coin = false; }));
    BOOST_CHECK(differs([](Consensus::Params& p) { p.hard_fork_height = 101; }));
    BOOST_CHECK(differs([](Consensus::Params& p) { p.legacy_final_hash = uint256{9}; }));
    // Unset is not zero.
    const auto digest_with{[&](const auto& mutate) {
        Consensus::Params changed{params};
        mutate(changed);
        return FinalityBindingTracker::ParamsDigest(changed);
    }};
    BOOST_CHECK(digest_with([](Consensus::Params& p) { p.hard_fork_height = 0; }) !=
                digest_with([](Consensus::Params& p) { p.hard_fork_height.reset(); }));
    BOOST_CHECK(digest_with([](Consensus::Params& p) { p.legacy_final_hash = uint256{}; }) !=
                digest_with([](Consensus::Params& p) { p.legacy_final_hash.reset(); }));
    // Parameters the derivation does not read leave the digest alone.
    BOOST_CHECK(!differs([](Consensus::Params& p) { p.h1 = Consensus::H1Params{}; }));
    BOOST_CHECK(!differs([](Consensus::Params& p) { p.transition_pow_length = 7; }));
}

BOOST_AUTO_TEST_CASE(source_build_id_names_clean_sources_only)
{
    if (const auto id{SourceBuildId()}) {
        BOOST_CHECK(!id->empty());
        BOOST_CHECK(!id->ends_with("-dirty"));
        BOOST_CHECK(id->find('$') == std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

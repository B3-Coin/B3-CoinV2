// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
//
// Commit 4 of the Modern PoS V1 finality plan: FINALITY_KEY semantics
// (identity-authorized BLS binding with separate PoP, explicit revocation,
// strict sequence, one active validator per BLS key) and the derived binding
// index (exact connect/disconnect undo, rebuild, immutable snapshots).
// No carrier, no MPA, no eligibility change, no activation: evidence is fed
// to the checker directly (test plumbing); production stays fail-closed.

#include <chainparams.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <key.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <node/finality_binding_index.h>
#include <pubkey.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <optional>
#include <vector>

using modern::BindingRecord;
using modern::BlsPubkeyBytes;
using modern::CheckFinalityKeyTransition;
using modern::FinalityKeyCheck;
using modern::FinalityKeyEvidence;
using modern::FinalityKeyParams;
using modern::ValidatorKeyBytes;
using node::FinalityBindingIndex;

namespace {

const uint256 CHAIN_DOMAIN{uint256{"d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0"}};
const uint256 OTHER_CHAIN_DOMAIN{uint256{"d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1d1"}};

struct Validator {
    CKey identity;
    ValidatorKeyBytes vk{};
    explicit Validator(const unsigned char seed)
    {
        std::vector<unsigned char> data(32, seed);
        identity.Set(data.begin(), data.end(), true);
        const XOnlyPubKey x{identity.GetPubKey()};
        std::copy(x.data(), x.data() + 32, vk.begin());
    }
    uint256 Commitment() const { uint256 c; std::copy(vk.begin(), vk.end(), c.begin()); return c; }
};

bls::SecretKey BlsKey(const unsigned i)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(i);
    ikm[31] = 0x77;
    return *bls::SecretKey::FromIKM(ikm);
}

BlsPubkeyBytes Pk(const bls::SecretKey& sk) { return sk.GetPublicKey().Compressed(); }
const BlsPubkeyBytes ZERO_PK{};

struct Built {
    FinalityKeyParams params;
    FinalityKeyEvidence ev;
};

//! Build a cell+evidence pair. `bls` null => revocation (zero key, zero PoP).
Built Build(const Validator& v, const bls::SecretKey* bls, const uint32_t seq, const uint256& domain = CHAIN_DOMAIN,
            const CKey* signer = nullptr)
{
    Built b;
    b.params.bls_pubkey = bls ? Pk(*bls) : ZERO_PK;
    b.params.seq = seq;
    b.ev.validator_key = v.vk;
    b.ev.bls_pubkey = b.params.bls_pubkey;
    b.ev.seq = seq;
    const uint256 digest{modern::FinalityBindDigest(domain, b.ev.validator_key, b.ev.bls_pubkey, seq)};
    const CKey& k{signer ? *signer : v.identity};
    uint256 aux{};
    BOOST_REQUIRE(k.SignSchnorr(digest, b.ev.bip340_sig, nullptr, aux));
    if (bls) b.ev.pop = bls->SignPoP().Compressed();
    return b;
}

FinalityKeyCheck Check(const Validator& v, const Built& b, const std::optional<BindingRecord>& prev,
                       const FinalityBindingIndex& index, const uint256& domain = CHAIN_DOMAIN)
{
    return CheckFinalityKeyTransition(domain, v.Commitment(), b.params, b.ev, prev, index.OwnerLookup());
}

BindingRecord Rec(const BlsPubkeyBytes& pk, const uint32_t seq, const int height) { return BindingRecord{pk, seq, height}; }

} // namespace

BOOST_FIXTURE_TEST_SUITE(finality_key_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(valid_initial_rotation_revocation)
{
    FinalityBindingIndex index;
    Validator a{1};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)};
    // initial binding seq 0
    const Built b0{Build(a, &k1, 0)};
    BOOST_CHECK(Check(a, b0, std::nullopt, index) == FinalityKeyCheck::OK);
    index.ConnectBlock(10, {{a.vk, Rec(Pk(k1), 0, 10)}});
    BOOST_CHECK(index.OwnerOf(Pk(k1)) == std::optional{a.vk});
    // rotation seq 1 to a new key
    const Built b1{Build(a, &k2, 1)};
    BOOST_CHECK(Check(a, b1, index.Get(a.vk), index) == FinalityKeyCheck::OK);
    index.ConnectBlock(11, {{a.vk, Rec(Pk(k2), 1, 11)}});
    BOOST_CHECK(!index.OwnerOf(Pk(k1)).has_value()); // old key released
    BOOST_CHECK(index.OwnerOf(Pk(k2)) == std::optional{a.vk});
    // re-binding the SAME own key is allowed (seq 2)
    const Built b2{Build(a, &k2, 2)};
    BOOST_CHECK(Check(a, b2, index.Get(a.vk), index) == FinalityKeyCheck::OK);
    index.ConnectBlock(12, {{a.vk, Rec(Pk(k2), 2, 12)}});
    // revocation seq 3: zero key, zero PoP, BIP340 still required
    const Built r{Build(a, nullptr, 3)};
    BOOST_CHECK(modern::IsZeroBlsKey(r.params.bls_pubkey) && modern::IsZeroBlsKey(r.ev.pop));
    BOOST_CHECK(Check(a, r, index.Get(a.vk), index) == FinalityKeyCheck::OK);
    index.ConnectBlock(13, {{a.vk, Rec(ZERO_PK, 3, 13)}});
    BOOST_CHECK(index.Get(a.vk)->IsRevoked());
    BOOST_CHECK(!index.OwnerOf(Pk(k2)).has_value());
    BOOST_CHECK(index.SnapshotActive().empty());
    // after revocation the validator may bind again (seq 4)
    const Built b4{Build(a, &k1, 4)};
    BOOST_CHECK(Check(a, b4, index.Get(a.vk), index) == FinalityKeyCheck::OK);
}

BOOST_AUTO_TEST_CASE(wrong_signer_or_domain)
{
    FinalityBindingIndex index;
    Validator a{1}, mallory{9};
    const auto k1{BlsKey(1)};
    // signed by another identity key
    const Built wrong_signer{Build(a, &k1, 0, CHAIN_DOMAIN, &mallory.identity)};
    BOOST_CHECK(Check(a, wrong_signer, std::nullopt, index) == FinalityKeyCheck::BAD_BIP340_SIGNATURE);
    // signed under another chain domain
    const Built wrong_domain{Build(a, &k1, 0, OTHER_CHAIN_DOMAIN)};
    BOOST_CHECK(Check(a, wrong_domain, std::nullopt, index) == FinalityKeyCheck::BAD_BIP340_SIGNATURE);
    BOOST_CHECK(Check(a, wrong_domain, std::nullopt, index, OTHER_CHAIN_DOMAIN) == FinalityKeyCheck::OK);
    // tampered signature
    Built tampered{Build(a, &k1, 0)};
    tampered.ev.bip340_sig[10] ^= 0x01;
    BOOST_CHECK(Check(a, tampered, std::nullopt, index) == FinalityKeyCheck::BAD_BIP340_SIGNATURE);
    // revocation also needs the identity signature
    const Built bad_revoke{Build(a, nullptr, 0, CHAIN_DOMAIN, &mallory.identity)};
    BOOST_CHECK(Check(a, bad_revoke, std::nullopt, index) == FinalityKeyCheck::BAD_BIP340_SIGNATURE);
    // evidence/cell mismatches are caught first
    Built mism{Build(a, &k1, 0)};
    mism.ev.seq = 1;
    BOOST_CHECK(Check(a, mism, std::nullopt, index) == FinalityKeyCheck::EVIDENCE_SEQ_MISMATCH);
    Built mism2{Build(a, &k1, 0)};
    mism2.ev.bls_pubkey = Pk(BlsKey(2));
    BOOST_CHECK(Check(a, mism2, std::nullopt, index) == FinalityKeyCheck::EVIDENCE_PUBKEY_MISMATCH);
    Built mism3{Build(a, &k1, 0)};
    mism3.ev.validator_key = mallory.vk;
    BOOST_CHECK(Check(a, mism3, std::nullopt, index) == FinalityKeyCheck::EVIDENCE_VALIDATOR_MISMATCH);
}

BOOST_AUTO_TEST_CASE(pop_rules)
{
    FinalityBindingIndex index;
    Validator a{1};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)};
    // cross-key PoP (k2's PoP presented for k1)
    Built cross{Build(a, &k1, 0)};
    cross.ev.pop = k2.SignPoP().Compressed();
    BOOST_CHECK(Check(a, cross, std::nullopt, index) == FinalityKeyCheck::BAD_POP);
    // tampered PoP
    Built tampered{Build(a, &k1, 0)};
    tampered.ev.pop[5] ^= 0x01;
    BOOST_CHECK(Check(a, tampered, std::nullopt, index) == FinalityKeyCheck::BAD_POP);
    // nonzero key with zero (missing) PoP
    Built zero_pop{Build(a, &k1, 0)};
    zero_pop.ev.pop.fill(0);
    BOOST_CHECK(Check(a, zero_pop, std::nullopt, index) == FinalityKeyCheck::BAD_POP);
    // a SIG_DST signature over the key bytes is not a PoP
    Built sig_not_pop{Build(a, &k1, 0)};
    const BlsPubkeyBytes pk1{Pk(k1)};
    std::array<unsigned char, 32> d{};
    std::copy(pk1.begin(), pk1.begin() + 32, d.begin());
    sig_not_pop.ev.pop = k1.Sign(d).Compressed();
    BOOST_CHECK(Check(a, sig_not_pop, std::nullopt, index) == FinalityKeyCheck::BAD_POP);
    // revocation with a nonzero PoP field
    Built revoke_with_pop{Build(a, nullptr, 0)};
    revoke_with_pop.ev.pop = k1.SignPoP().Compressed();
    BOOST_CHECK(Check(a, revoke_with_pop, std::nullopt, index) == FinalityKeyCheck::POP_MUST_BE_ZERO);
    // malformed BLS key bytes (nonzero but not a canonical point): checked before PoP
    Built badkey{Build(a, &k1, 0)};
    badkey.params.bls_pubkey[0] = 0x11; // clears the compression flag -> not canonical compressed form
    badkey.ev.bls_pubkey = badkey.params.bls_pubkey;
    {   // re-sign so the BIP340 step passes and the BLS step is reached
        const uint256 digest{modern::FinalityBindDigest(CHAIN_DOMAIN, badkey.ev.validator_key, badkey.ev.bls_pubkey, 0)};
        uint256 aux{};
        BOOST_REQUIRE(a.identity.SignSchnorr(digest, badkey.ev.bip340_sig, nullptr, aux));
    }
    BOOST_CHECK(Check(a, badkey, std::nullopt, index) == FinalityKeyCheck::BAD_BLS_PUBKEY);
}

BOOST_AUTO_TEST_CASE(sequence_rules)
{
    FinalityBindingIndex index;
    Validator a{1};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)};
    // first binding must be seq 0
    BOOST_CHECK(Check(a, Build(a, &k1, 1), std::nullopt, index) == FinalityKeyCheck::BAD_FIRST_SEQ);
    BOOST_CHECK(Check(a, Build(a, &k1, 0), std::nullopt, index) == FinalityKeyCheck::OK);
    index.ConnectBlock(1, {{a.vk, Rec(Pk(k1), 0, 1)}});
    // duplicate
    BOOST_CHECK(Check(a, Build(a, &k2, 0), index.Get(a.vk), index) == FinalityKeyCheck::BAD_SEQ);
    // gap
    BOOST_CHECK(Check(a, Build(a, &k2, 2), index.Get(a.vk), index) == FinalityKeyCheck::BAD_SEQ);
    // exact next
    BOOST_CHECK(Check(a, Build(a, &k2, 1), index.Get(a.vk), index) == FinalityKeyCheck::OK);
    // overflow: previous at UINT32_MAX can never transition again (bind, rotate or revoke)
    const BindingRecord maxed{Pk(k1), UINT32_MAX, 1};
    BOOST_CHECK(Check(a, Build(a, &k2, 0), maxed, index) == FinalityKeyCheck::SEQ_OVERFLOW);
    BOOST_CHECK(Check(a, Build(a, nullptr, 0), maxed, index) == FinalityKeyCheck::SEQ_OVERFLOW);
    // a revoked record still advances the sequence (seq continues after revocation)
    index.ConnectBlock(2, {{a.vk, Rec(ZERO_PK, 1, 2)}});
    BOOST_CHECK(Check(a, Build(a, &k2, 1), index.Get(a.vk), index) == FinalityKeyCheck::BAD_SEQ);
    BOOST_CHECK(Check(a, Build(a, &k2, 2), index.Get(a.vk), index) == FinalityKeyCheck::OK);
}

BOOST_AUTO_TEST_CASE(one_active_validator_per_bls_key)
{
    FinalityBindingIndex index;
    Validator a{1}, b{2};
    const auto k1{BlsKey(1)};
    index.ConnectBlock(1, {{a.vk, Rec(Pk(k1), 0, 1)}});
    // b cannot bind a's active key
    BOOST_CHECK(Check(b, Build(b, &k1, 0), std::nullopt, index) == FinalityKeyCheck::BLS_KEY_IN_USE);
    // a revokes; now b may bind it
    index.ConnectBlock(2, {{a.vk, Rec(ZERO_PK, 1, 2)}});
    BOOST_CHECK(Check(b, Build(b, &k1, 0), std::nullopt, index) == FinalityKeyCheck::OK);
    index.ConnectBlock(3, {{b.vk, Rec(Pk(k1), 0, 3)}});
    BOOST_CHECK(index.OwnerOf(Pk(k1)) == std::optional{b.vk});
    // a cannot take it back while b holds it
    BOOST_CHECK(Check(a, Build(a, &k1, 2), index.Get(a.vk), index) == FinalityKeyCheck::BLS_KEY_IN_USE);
    // rotating away releases it for a again
    index.ConnectBlock(4, {{b.vk, Rec(Pk(BlsKey(5)), 1, 4)}});
    BOOST_CHECK(Check(a, Build(a, &k1, 2), index.Get(a.vk), index) == FinalityKeyCheck::OK);
}

BOOST_AUTO_TEST_CASE(snapshot_is_immutable_under_later_bindings)
{
    FinalityBindingIndex index;
    Validator a{1}, b{2};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)}, k3{BlsKey(3)};
    index.ConnectBlock(1, {{a.vk, Rec(Pk(k1), 0, 1)}, {b.vk, Rec(Pk(k2), 0, 1)}});
    // "Epoch snapshot": taken at the boundary, then frozen.
    const auto snapshot{index.SnapshotActive()};
    BOOST_REQUIRE_EQUAL(snapshot.size(), 2u);
    BOOST_CHECK(snapshot.at(a.vk).bls_pubkey == Pk(k1));
    // Mid-epoch rebind and revocation mutate the index, never the snapshot.
    index.ConnectBlock(2, {{a.vk, Rec(Pk(k3), 1, 2)}});
    index.ConnectBlock(3, {{b.vk, Rec(ZERO_PK, 1, 3)}});
    BOOST_CHECK(snapshot.at(a.vk).bls_pubkey == Pk(k1));
    BOOST_CHECK(snapshot.count(b.vk) == 1);
    const auto later{index.SnapshotActive()};
    BOOST_CHECK_EQUAL(later.size(), 1u);
    BOOST_CHECK(later.at(a.vk).bls_pubkey == Pk(k3));
    BOOST_CHECK(later != snapshot);
}

BOOST_AUTO_TEST_CASE(index_connect_disconnect_rebuild)
{
    Validator a{1}, b{2}, c{3};
    const auto k1{BlsKey(1)}, k2{BlsKey(2)}, k3{BlsKey(3)};
    using T = FinalityBindingIndex::Transition;
    // A block sequence with every kind of transition, including two transitions
    // of one validator in the same block and a key handed over within a block.
    const std::vector<std::pair<int, std::vector<T>>> blocks{
        {100, {{a.vk, Rec(Pk(k1), 0, 100)}, {b.vk, Rec(Pk(k2), 0, 100)}}},
        {101, {{a.vk, Rec(Pk(k3), 1, 101)}, {a.vk, Rec(Pk(k1), 2, 101)}}},   // rotate twice in one block
        {102, {{b.vk, Rec(ZERO_PK, 1, 102)}, {c.vk, Rec(Pk(k2), 0, 102)}}},  // b releases k2, c takes it
        {103, {{c.vk, Rec(ZERO_PK, 1, 103)}}},
    };
    FinalityBindingIndex incremental;
    std::vector<std::map<ValidatorKeyBytes, BindingRecord>> states_after;
    for (const auto& [h, ts] : blocks) {
        incremental.ConnectBlock(h, ts);
        states_after.push_back(incremental.All());
    }
    BOOST_CHECK(incremental.OwnerOf(Pk(k1)) == std::optional{a.vk});
    BOOST_CHECK(!incremental.OwnerOf(Pk(k2)).has_value());
    BOOST_CHECK(!incremental.OwnerOf(Pk(k3)).has_value());
    BOOST_CHECK_EQUAL(incremental.ConnectedHeight(), 103);
    // Disconnect in reverse: each step restores the exact prior state.
    for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i) {
        incremental.DisconnectBlock(blocks[i].first);
        if (i > 0) {
            BOOST_CHECK(incremental.All() == states_after[i - 1]);
        } else {
            BOOST_CHECK(incremental.All().empty());
            BOOST_CHECK_EQUAL(incremental.ConnectedHeight(), -1);
        }
    }
    BOOST_CHECK(!incremental.OwnerOf(Pk(k1)).has_value());
    // Reconnect: identical again.
    for (size_t i = 0; i < blocks.size(); ++i) {
        incremental.ConnectBlock(blocks[i].first, blocks[i].second);
        BOOST_CHECK(incremental.All() == states_after[i]);
    }
    // Reindex: a fresh index fed the same blocks equals the incremental one,
    // including the owner map (probed through OwnerOf).
    FinalityBindingIndex rebuilt;
    for (const auto& [h, ts] : blocks) rebuilt.ConnectBlock(h, ts);
    BOOST_CHECK(rebuilt.All() == incremental.All());
    for (const auto& pk : {Pk(k1), Pk(k2), Pk(k3)}) BOOST_CHECK(rebuilt.OwnerOf(pk) == incremental.OwnerOf(pk));
    // Partial reorg: drop the top two blocks, replace with a different block.
    incremental.DisconnectBlock(103);
    incremental.DisconnectBlock(102);
    BOOST_CHECK(incremental.All() == states_after[1]);
    incremental.ConnectBlock(102, {{b.vk, Rec(Pk(k3), 1, 102)}}); // b rotates to k3 (free since a moved off it)
    BOOST_CHECK(incremental.OwnerOf(Pk(k3)) == std::optional{b.vk});
    BOOST_CHECK(incremental.OwnerOf(Pk(k2)) == std::optional{b.vk} || !incremental.OwnerOf(Pk(k2)).has_value());
    BOOST_CHECK(!incremental.OwnerOf(Pk(k2)).has_value()); // b moved off k2
}

BOOST_AUTO_TEST_CASE(production_remains_fail_closed)
{
    // No carrier exists for the evidence yet; a FINALITY_KEY cell is invalid in
    // production regardless of any semantics above, and the checker is not
    // reachable from transaction validation.
    Consensus::Params production{};
    production.legacy_b3coin = true;
    BOOST_CHECK(!Consensus::ModernObjectRulesActive(production));
    Validator a{1};
    const auto k1{BlsKey(1)};
    const FinalityKeyParams params{Pk(k1), 0};
    const auto script{modern::MakeMetadataCellScript(static_cast<uint16_t>(modern::PolicyType::FINALITY_KEY),
                                                      modern::POLICY_VERSION_V1, a.Commitment(), params.Encode())};
    BOOST_REQUIRE(script.has_value());
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.emplace_back(0, *script);
    std::string err;
    BOOST_CHECK(!modern::CheckMetadataCellOutputs(CTransaction{mtx}, production, err));
    BOOST_CHECK(err.find("inactive") != std::string::npos);
    BOOST_CHECK(!modern::IsActivatedPolicy(static_cast<uint16_t>(modern::PolicyType::FINALITY_KEY), 1));
    for (const auto chain : {ChainType::MAIN, ChainType::TESTNET, ChainType::REGTEST}) {
        BOOST_CHECK(!Consensus::ModernObjectRulesActive(CreateChainParams(ArgsManager{}, chain)->GetConsensus()));
    }
    // The cell's params decode back to the binding the evidence must match.
    const auto cell{modern::ParseMetadataCell(*script)};
    BOOST_REQUIRE(cell.has_value());
    const auto decoded{FinalityKeyParams::Decode(cell->params)};
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK(decoded->bls_pubkey == Pk(k1) && decoded->seq == 0);
    BOOST_CHECK(cell->commitment == a.Commitment());
}

BOOST_AUTO_TEST_SUITE_END()

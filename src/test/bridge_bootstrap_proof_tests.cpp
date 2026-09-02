// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/bootstrap_proof.h>

#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

bls::SecretKey Key(const unsigned char seed)
{
    std::array<unsigned char, 32> ikm{};
    for (size_t i{0}; i < ikm.size(); ++i) ikm[i] = seed + i;
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

uint64_t AbiWord(std::span<const unsigned char> bytes, const size_t offset)
{
    BOOST_REQUIRE(offset + 32 <= bytes.size());
    for (size_t i{offset}; i < offset + 24; ++i) BOOST_REQUIRE_EQUAL(bytes[i], 0);
    return ReadBE64(bytes.data() + offset + 24);
}

struct Fixture {
    uint256 domain{Filled(0xd0)};
    uint64_t modern_start{811001};
    std::vector<bls::SecretKey> secrets;
    std::vector<bridge::BootstrapIdentity> identities;
    std::vector<bridge::BootstrapSignaturePackage> packages;
    modern::FinalizedBlock snapshot;
    modern::ValidatorSetHeader set0;

    Fixture()
    {
        // Deliberately reverse the input identity order. The builder must sort
        // by validator_key, never by BLS key or input order.
        for (unsigned char i{0}; i < 4; ++i) secrets.push_back(Key(i + 1));
        for (int i{3}; i >= 0; --i) {
            std::array<unsigned char, 32> validator{};
            validator.back() = static_cast<unsigned char>(i + 1);
            identities.push_back({validator, secrets[i].GetPublicKey(),
                                  secrets[i].SignPoP(), 0, 810001});
        }

        std::vector<bls::VerifiedPublicKey> verified;
        for (const auto& secret : secrets) {
            const auto pubkey{secret.GetPublicKey()};
            const auto vk{bls::VerifiedPublicKey::FromPoP(
                pubkey, secret.SignPoP())};
            BOOST_REQUIRE(vk);
            verified.push_back(*vk);
        }
        const auto aggregate{bls::AggregatePublicKeys(verified)};
        BOOST_REQUIRE(aggregate);
        set0.epoch = 0;
        set0.ruleset_version = modern::FINALITY_RULESET_V1;
        set0.validator_count = 4;
        set0.total_weight = 400;
        set0.quorum_weight = modern::QuorumWeightV1(400);
        set0.aggregate_pubkey = aggregate->Compressed();
        set0.members_root = Filled(0x44);

        snapshot.height = modern_start - 1;
        snapshot.block_hash = Filled(0x11);
        snapshot.withdrawal_root = uint256{};
        snapshot.validator_set_hash = modern::ValidatorSetHash(set0);
        snapshot.epoch = 0;
        const uint256 digest{modern::FinalityDigest(domain, snapshot)};

        // Canonical members are key 0..3; member index 2 is absent.
        for (const size_t i : {size_t{0}, size_t{1}, size_t{3}}) {
            std::array<unsigned char, 32> validator{};
            validator.back() = static_cast<unsigned char>(i + 1);
            packages.push_back({validator, secrets[i].GetPublicKey(), snapshot,
                                set0, digest,
                                secrets[i].Sign(std::span<const unsigned char>{
                                    digest.begin(), 32}),
                                0, 810001, domain});
        }
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(bridge_bootstrap_proof_tests)

BOOST_AUTO_TEST_CASE(predeployment_set_matches_later_proof)
{
    Fixture fixture;
    std::string error;
    const auto deployment_set{bridge::BuildBootstrapSet(
        fixture.domain, fixture.identities, fixture.modern_start, error)};
    BOOST_REQUIRE_MESSAGE(deployment_set, error);
    BOOST_CHECK_EQUAL(deployment_set->bootstrap_set.validator_count, 4U);
    BOOST_CHECK_EQUAL(deployment_set->bootstrap_set.total_weight, 4U);
    BOOST_CHECK_EQUAL(deployment_set->bootstrap_set.quorum_weight, 3U);
    // Shared with contracts/test/BlsBootstrapVector.t.sol. This pins the raw
    // validator-key ordering and fixed-width big-endian Solidity header hash.
    BOOST_CHECK_EQUAL(
        HexStr(deployment_set->bootstrap_set.aggregate_pubkey),
        "afea1502f003a9ab0d8bf89c1826bc78d3e01413f9e78e836b0b99dba05c3ce"
        "5d9e81f1478697a4d776aeee83449ec54");
    BOOST_CHECK_EQUAL(
        HexStr(deployment_set->bootstrap_set.members_root),
        "78621e43cecacca419e7d29cb7458f31262616dce6083cf3f32ddec1b67b5910");
    BOOST_CHECK_EQUAL(
        HexStr(deployment_set->bootstrap_set_hash),
        "5188225d2e128c1f51a61f4f6bad92dd8da6566549156bae3ebb81728158bf14");

    const auto proof{bridge::BuildBootstrapProof(
        fixture.domain, fixture.identities, fixture.packages,
        fixture.modern_start, error)};
    BOOST_REQUIRE_MESSAGE(proof, error);
    BOOST_CHECK(deployment_set->bootstrap_set == proof->bootstrap_set);
    BOOST_CHECK(deployment_set->bootstrap_set_hash == proof->bootstrap_set_hash);
}

BOOST_AUTO_TEST_CASE(predeployment_set_rejects_unsafe_manifest)
{
    Fixture duplicate_validator;
    std::string error;
    duplicate_validator.identities[1].validator_key =
        duplicate_validator.identities[0].validator_key;
    BOOST_CHECK(!bridge::BuildBootstrapSet(
        duplicate_validator.domain, duplicate_validator.identities,
        duplicate_validator.modern_start, error));
    BOOST_CHECK_EQUAL(error, "duplicate validator_key in manifest");

    Fixture duplicate_bls;
    duplicate_bls.identities[1].bls_pubkey =
        duplicate_bls.identities[0].bls_pubkey;
    duplicate_bls.identities[1].proof_of_possession =
        duplicate_bls.identities[0].proof_of_possession;
    BOOST_CHECK(!bridge::BuildBootstrapSet(
        duplicate_bls.domain, duplicate_bls.identities,
        duplicate_bls.modern_start, error));
    BOOST_CHECK_EQUAL(error, "duplicate bls_pubkey in manifest");

    Fixture bad_pop;
    bad_pop.identities[0].proof_of_possession = bad_pop.secrets[0].Sign(
        std::span<const unsigned char>{bad_pop.domain.begin(), 32});
    BOOST_CHECK(!bridge::BuildBootstrapSet(
        bad_pop.domain, bad_pop.identities, bad_pop.modern_start, error));
    BOOST_CHECK(error.find("invalid proof of possession") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(real_three_of_four_absent_witness_and_abi)
{
    Fixture fixture;
    std::string error;
    const auto artifacts{bridge::BuildBootstrapProof(
        fixture.domain, fixture.identities, fixture.packages,
        fixture.modern_start, error)};
    BOOST_REQUIRE_MESSAGE(artifacts, error);
    BOOST_CHECK_EQUAL(HexStr(artifacts->signer_bitmap), "0b");
    BOOST_CHECK_EQUAL(artifacts->absent.index, 2U);
    BOOST_CHECK_EQUAL(artifacts->bootstrap_set.validator_count, 4U);
    BOOST_CHECK_EQUAL(artifacts->bootstrap_set.total_weight, 4U);
    BOOST_CHECK_EQUAL(artifacts->bootstrap_set.quorum_weight, 3U);

    uint256 node{modern::ValidatorSetLeaf(
        artifacts->absent.index, artifacts->absent.compressed_pubkey, 1)};
    for (size_t level{0}; level < modern::FINALITY_SET_TREE_DEPTH; ++level) {
        std::array<unsigned char, 64> pair{};
        if ((artifacts->absent.index >> level) & 1) {
            std::copy(artifacts->absent.siblings[level].begin(),
                      artifacts->absent.siblings[level].end(), pair.begin());
            std::copy(node.begin(), node.end(), pair.begin() + 32);
        } else {
            std::copy(node.begin(), node.end(), pair.begin());
            std::copy(artifacts->absent.siblings[level].begin(),
                      artifacts->absent.siblings[level].end(), pair.begin() + 32);
        }
        node = modern::Keccak(pair);
    }
    BOOST_CHECK(node == artifacts->bootstrap_set.members_root);

    std::vector<bls::VerifiedPublicKey> signers;
    for (const size_t i : {size_t{0}, size_t{1}, size_t{3}}) {
        const auto pubkey{fixture.secrets[i].GetPublicKey()};
        const auto verified{bls::VerifiedPublicKey::FromPoP(
            pubkey, fixture.secrets[i].SignPoP())};
        BOOST_REQUIRE(verified);
        signers.push_back(*verified);
    }
    const uint256 digest{modern::FinalityDigest(fixture.domain,
                                                artifacts->snapshot)};
    BOOST_CHECK(bls::FastAggregateVerify(
        signers, std::span<const unsigned char>{digest.begin(), 32},
        artifacts->aggregate_signature));

    // Public proof ABI has four dynamic heads. Its one Absent tuple has the
    // Solidity dynamic-array offset measured after the length word.
    BOOST_CHECK_EQUAL(AbiWord(artifacts->proof_abi, 0), 128U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->proof_abi, 32), 192U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->proof_abi, 64), 480U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->proof_abi, 96), 640U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->proof_abi, 640), 1U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->proof_abi, 672), 32U);
    BOOST_CHECK_EQUAL(artifacts->proof_abi.size(), 1504U);

    // initialize selector + 7-word argument head; Set_0 then proof tails.
    BOOST_CHECK_EQUAL(HexStr(std::span<const unsigned char>{
                          artifacts->initialize_calldata.data(), 4}),
                      "89b50ca1");
    BOOST_CHECK_EQUAL(AbiWord(artifacts->initialize_calldata, 4 + 5 * 32),
                      224U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->initialize_calldata, 4 + 6 * 32),
                      544U);
    BOOST_CHECK_EQUAL(AbiWord(artifacts->initialize_calldata, 4 + 544),
                      artifacts->proof_abi.size());
}

BOOST_AUTO_TEST_CASE(rejects_duplicate_signer_and_bad_pop)
{
    Fixture fixture;
    std::string error;
    fixture.packages[2] = fixture.packages[0];
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        fixture.domain, fixture.identities, fixture.packages,
        fixture.modern_start, error));
    BOOST_CHECK_EQUAL(error, "duplicate signature package");

    Fixture bad_pop;
    bad_pop.identities[0].proof_of_possession = bad_pop.secrets[0].Sign(
        std::span<const unsigned char>{bad_pop.domain.begin(), 32});
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        bad_pop.domain, bad_pop.identities, bad_pop.packages,
        bad_pop.modern_start, error));
    BOOST_CHECK(error.find("invalid proof of possession") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(rejects_mismatched_binding_metadata)
{
    Fixture fixture;
    std::string error;
    fixture.packages[0].chain_domain = Filled(0xaa);
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        fixture.domain, fixture.identities, fixture.packages,
        fixture.modern_start, error));
    BOOST_CHECK_EQUAL(
        error, "signature package chain_domain does not match manifest");

    Fixture late_binding;
    late_binding.packages[0].binding_height = late_binding.snapshot.height + 1;
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        late_binding.domain, late_binding.identities, late_binding.packages,
        late_binding.modern_start, error));
    BOOST_CHECK_EQUAL(
        error, "signature package binding was not active by the M-1 snapshot");

    Fixture wrong_sequence;
    wrong_sequence.packages[0].binding_seq = 1;
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        wrong_sequence.domain, wrong_sequence.identities,
        wrong_sequence.packages, wrong_sequence.modern_start, error));
    BOOST_CHECK_EQUAL(
        error, "signature package binding_seq does not match manifest");

    Fixture wrong_height;
    wrong_height.packages[0].binding_height = 810002;
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        wrong_height.domain, wrong_height.identities,
        wrong_height.packages, wrong_height.modern_start, error));
    BOOST_CHECK_EQUAL(
        error, "signature package binding_height does not match manifest");

    Fixture late_manifest;
    late_manifest.identities[0].binding_height = late_manifest.snapshot.height + 1;
    BOOST_CHECK(!bridge::BuildBootstrapProof(
        late_manifest.domain, late_manifest.identities,
        late_manifest.packages, late_manifest.modern_start, error));
    BOOST_CHECK_EQUAL(
        error, "manifest binding was not active by the M-1 snapshot");
}

BOOST_AUTO_TEST_SUITE_END()

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bridge/regtest_certify.h>

#include <bridge/bootstrap_proof.h>
#include <bridge/bootstrap_v2.h>
#include <chainparams.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <kernel/chainparams.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace bridge;
using namespace bridge::regtest_certify;

namespace {

// contracts/test/vectors/bridge_v2_vectors.json bootstrap_attestation_v2,
// copied byte for byte (gen_v2_vectors.py). Case 0 is the regtest statement,
// case 1 the mainnet-shaped one; both sign finalized_block_v2[0].
constexpr std::string_view FB_ENCODED{
    "1a963944632a75ae707940a80da3ba4625bc32d5a65f3c342ce75e379dd78446000000000000010a3eb15e38c1fe49793d90a4ed8a01f0"
    "cb39c2fd362fc0afc4d32041913bb6442e0000000000000000000000000000000000000000000000000000000000000000d577dae80a4733"
    "e62d77115a58a6f4c26339989bf8cd60c6ab0b8a42a104d425000000000000000000000000000000011243ccb653de3036ff1b508195e5b8"
    "2a90ff7592a269e42cb347f901f11c476900"};

struct VectorCase {
    std::string_view chain_domain;
    uint64_t evm_chain_id;
    unsigned char verifier_byte;
    std::string_view start_set_hash;
    std::string_view message;
    std::string_view digest;
};

constexpr VectorCase VECTOR_CASES[2]{
    {"1a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f43879", 31'337, 0x44,
     "c631d4251ee10b5f2f48fb4b9ad9c91fa9a3ff2bd645876b81771d80760df0d6",
     "1a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f438790000000000000000000000000000000000000000000000"
     "000000000000007a694444444444444444444444444444444444444444c631d4251ee10b5f2f48fb4b9ad9c91fa9a3ff2bd645876b81"
     "771d80760df0d61a963944632a75ae707940a80da3ba4625bc32d5a65f3c342ce75e379dd78446000000000000010a3eb15e38c1fe4979"
     "3d90a4ed8a01f0cb39c2fd362fc0afc4d32041913bb6442e000000000000000000000000000000000000000000000000000000000000000"
     "0d577dae80a4733e62d77115a58a6f4c26339989bf8cd60c6ab0b8a42a104d425000000000000000000000000000000011243ccb653de30"
     "36ff1b508195e5b82a90ff7592a269e42cb347f901f11c476900",
     "8a327426a0b105148b0229505a55b4543f85651b7d20f0c34a71e4e386a82fbe"},
    {"a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a", 1, 0x55,
     "d577dae80a4733e62d77115a58a6f4c26339989bf8cd60c6ab0b8a42a104d425",
     "a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a0000000000000000000000000000000000000000000000"
     "0000000000000000015555555555555555555555555555555555555555d577dae80a4733e62d77115a58a6f4c26339989bf8cd60c6ab0b"
     "8a42a104d4251a963944632a75ae707940a80da3ba4625bc32d5a65f3c342ce75e379dd78446000000000000010a3eb15e38c1fe4979"
     "3d90a4ed8a01f0cb39c2fd362fc0afc4d32041913bb6442e000000000000000000000000000000000000000000000000000000000000000"
     "0d577dae80a4733e62d77115a58a6f4c26339989bf8cd60c6ab0b8a42a104d425000000000000000000000000000000011243ccb653de30"
     "36ff1b508195e5b82a90ff7592a269e42cb347f901f11c476900",
     "914c90a8048681ac42109eba8aa27d8848fc1e3473c61c5d503bbc87147a48c7"},
};

//! sha256("B3/BRIDGE/BOOTSTRAP/V2"), contracts/src/v2/B3TypesV2.sol BOOTSTRAP_TAG.
constexpr std::string_view BOOTSTRAP_TAG_HASH{"1e12d61c624b35a72fb66b8d7c57438dbf5d9324d885ec2e9c3362668e75dd8b"};

// Test-key derivation pins, computed independently with Python's stdlib
// (hashlib/hmac: the IKM, then IETF BLS KeyGen, HKDF-SHA256 with salt
// "BLS-SIG-KEYGEN-SALT-", L = 48, reduced mod r).
constexpr std::string_view BOOTSTRAP_KEY0_IKM{"79c7eb53f09954dfcc8d83c5e67b64b2a68adb387187948dc78ed6e3de8a1fb9"};
constexpr std::string_view BOOTSTRAP_KEY0_SECRET{"51f138cfefc41feb08229080d44295752a5a7f222006c6ca2c1a39c6da272bfc"};
constexpr std::string_view BOOTSTRAP_KEY3_SECRET{"186cb01bbaefa3ee37a16b0bc14eab585f8f2fb2f6a924fc906ad43b2f57a02d"};

// The bootstrap committee pins (the DeployV2 K_OF_N env values). The tool's
// attestation over case 0 was checked once against a BlsDigestProver
// deployed on a local anvil (verifyDigest true; a tampered digest false).
constexpr std::string_view BOOTSTRAP_AGGREGATE_PUBKEY{
    "b3e3014b0e05ba465773b77ae84f580155f13af4c46295651be7e91afe0cf07b5d97ab1b7b9b229b82d30cfa67aca1de"};
constexpr std::string_view BOOTSTRAP_MEMBERS_ROOT{"0df81bd556a9133486858ef44c83823c8ac2c07249666e6fc67c133ce8c4b6d4"};
constexpr std::string_view BOOTSTRAP_SET_HASH{"e7bdf1c135de19196b9acfd2ad57f3eab78117c5a2d215f172ff6bc22486dd85"};

std::vector<unsigned char> Bytes(std::string_view hex)
{
    const auto bytes{TryParseHex<unsigned char>(hex)};
    BOOST_REQUIRE(bytes);
    return *bytes;
}

uint256 Raw(std::string_view hex)
{
    const auto bytes{Bytes(hex)};
    BOOST_REQUIRE_EQUAL(bytes.size(), 32U);
    return uint256{std::span<const unsigned char>{bytes}};
}

EthAddress FilledAddress(const unsigned char value)
{
    EthAddress out;
    out.fill(value);
    return out;
}

modern::FinalizedBlockV2 VectorBlock()
{
    const auto fb{modern::FinalizedBlockV2::Decode(Bytes(FB_ENCODED))};
    BOOST_REQUIRE(fb);
    return *fb;
}

uint64_t AbiWord(std::span<const unsigned char> bytes, const size_t offset)
{
    BOOST_REQUIRE(offset + 32 <= bytes.size());
    for (size_t i{offset}; i < offset + 24; ++i) BOOST_REQUIRE_EQUAL(bytes[i], 0);
    return ReadBE64(bytes.data() + offset + 24);
}

//! BlsDigestProver._memberRoot: the ordered depth-13 fold of one member leaf.
uint256 FoldMember(const BootstrapAbsentWitness& witness)
{
    uint256 node{modern::ValidatorSetLeaf(witness.index, witness.compressed_pubkey, witness.weight)};
    for (size_t level{0}; level < modern::FINALITY_SET_TREE_DEPTH; ++level) {
        std::array<unsigned char, 64> pair{};
        const bool right{((witness.index >> level) & 1U) != 0};
        const uint256& left_node{right ? witness.siblings[level] : node};
        const uint256& right_node{right ? node : witness.siblings[level]};
        std::copy(left_node.begin(), left_node.end(), pair.begin());
        std::copy(right_node.begin(), right_node.end(), pair.begin() + 32);
        node = modern::Keccak(pair);
    }
    return node;
}

KofNAttestation RegtestAttestation(std::optional<std::vector<uint32_t>> signers = std::nullopt)
{
    const VectorCase& c{VECTOR_CASES[0]};
    return BuildKofNAttestation(Raw(c.chain_domain), c.evm_chain_id, FilledAddress(c.verifier_byte),
                                Raw(c.start_set_hash), VectorBlock(), std::move(signers));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(bridge_regtest_certify_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(bootstrap_message_matches_shared_vectors)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> tag_hash{};
    CSHA256().Write(UCharCast(BRIDGE_BOOTSTRAP_V2_TAG.data()), BRIDGE_BOOTSTRAP_V2_TAG.size()).Finalize(tag_hash.data());
    BOOST_CHECK_EQUAL(HexStr(tag_hash), BOOTSTRAP_TAG_HASH);

    const modern::FinalizedBlockV2 fb{VectorBlock()};
    std::vector<uint256> digests;
    for (const VectorCase& c : VECTOR_CASES) {
        const auto message{BootstrapAttestationMessageV2(Raw(c.chain_domain), c.evm_chain_id,
                                                         FilledAddress(c.verifier_byte), Raw(c.start_set_hash), fb)};
        BOOST_CHECK_EQUAL(message.size(), BRIDGE_BOOTSTRAP_V2_MESSAGE_SIZE);
        BOOST_CHECK_EQUAL(HexStr(message), c.message);
        const uint256 digest{BootstrapAttestationDigestV2(Raw(c.chain_domain), c.evm_chain_id,
                                                          FilledAddress(c.verifier_byte), Raw(c.start_set_hash), fb)};
        BOOST_CHECK_EQUAL(HexStr(digest), c.digest);
        // Its own tag: never the digest-V2 committee message for the same fb.
        BOOST_CHECK(digest != modern::FinalityDigestV2(Raw(c.chain_domain), fb));
        digests.push_back(digest);
    }
    BOOST_CHECK(digests[0] != digests[1]);
}

BOOST_AUTO_TEST_CASE(bootstrap_message_layout_binds_every_field)
{
    const modern::FinalizedBlockV2 fb{VectorBlock()};
    const uint256 domain{RegtestModernChainDomain()};
    const uint256 start{Raw(VECTOR_CASES[0].start_set_hash)};
    const EthAddress verifier{FilledAddress(0x44)};
    // A chain id above 2^32 shows the full big-endian u256 word.
    const uint64_t chain_id{0x0102030405060708};
    const auto message{BootstrapAttestationMessageV2(domain, chain_id, verifier, start, fb)};
    BOOST_REQUIRE_EQUAL(message.size(), 301U);
    BOOST_CHECK(std::equal(domain.begin(), domain.end(), message.begin()));
    BOOST_CHECK(std::all_of(message.begin() + 32, message.begin() + 56, [](unsigned char b) { return b == 0; }));
    BOOST_CHECK_EQUAL(HexStr(std::span{message}.subspan(56, 8)), "0102030405060708");
    BOOST_CHECK(std::equal(verifier.begin(), verifier.end(), message.begin() + 64));
    BOOST_CHECK(std::equal(start.begin(), start.end(), message.begin() + 84));
    const auto encoded{fb.Encode()};
    BOOST_CHECK(std::equal(encoded.begin(), encoded.end(), message.begin() + 116));

    // Every bound field moves the digest (OI-12: another verifier, chain or
    // start set cannot reuse an attestation).
    const uint256 base{BootstrapAttestationDigestV2(domain, 31'337, verifier, start, fb)};
    BOOST_CHECK(base != BootstrapAttestationDigestV2(domain, 31'338, verifier, start, fb));
    BOOST_CHECK(base != BootstrapAttestationDigestV2(domain, 31'337, FilledAddress(0x45), start, fb));
    BOOST_CHECK(base != BootstrapAttestationDigestV2(domain, 31'337, verifier, Raw(VECTOR_CASES[1].start_set_hash), fb));
    BOOST_CHECK(base != BootstrapAttestationDigestV2(MainnetChainDomain(), 31'337, verifier, start, fb));
    modern::FinalizedBlockV2 other{fb};
    other.epoch += 1;
    BOOST_CHECK(base != BootstrapAttestationDigestV2(domain, 31'337, verifier, start, other));
}

BOOST_AUTO_TEST_CASE(chain_domain_constants_match_chainparams)
{
    for (const bool flowmesh : {false, true}) {
        CChainParams::RegTestOptions options;
        CChainParams::B3ModernRegTestOptions b3;
        b3.flowmesh_test = flowmesh;
        options.b3_modern = b3;
        const auto chain{CChainParams::RegTest(options)};
        const Consensus::Params& params{chain->GetConsensus()};
        BOOST_CHECK(params.hashGenesisBlock == REGTEST_MODERN_GENESIS_HASH);
        BOOST_REQUIRE(params.legacy_final_hash);
        const auto domain{modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
        BOOST_REQUIRE(domain);
        BOOST_CHECK(*domain == RegtestModernChainDomain());
    }
    // The shared vectors' regtest case names the same domain.
    BOOST_CHECK(RegtestModernChainDomain() == Raw(VECTOR_CASES[0].chain_domain));

    const auto main{CChainParams::Main()};
    const Consensus::Params& main_params{main->GetConsensus()};
    BOOST_REQUIRE(main_params.legacy_final_hash);
    const auto main_domain{modern::ModernChainDomain(main_params.hashGenesisBlock, *main_params.legacy_final_hash)};
    BOOST_REQUIRE(main_domain);
    BOOST_CHECK(*main_domain == MainnetChainDomain());
    BOOST_CHECK(MainnetChainDomain() == Raw(VECTOR_CASES[1].chain_domain));
    BOOST_CHECK(MainnetChainDomain() != RegtestModernChainDomain());

    BOOST_CHECK_NO_THROW(CheckRegtestDomain(RegtestModernChainDomain()));
    BOOST_CHECK_EXCEPTION(CheckRegtestDomain(MainnetChainDomain()), std::runtime_error,
                          HasReason("refusing the B3 mainnet chain domain 0xa651262a"));
    BOOST_CHECK_EXCEPTION(CheckRegtestDomain(uint256{}), std::runtime_error,
                          HasReason("only the -b3modernregtest domain"));
    BOOST_CHECK_NO_THROW(CheckRegtestEvmChainId(31'337));
    for (const uint64_t id : {uint64_t{0}, uint64_t{1}, uint64_t{31'338}, uint64_t{42'161}}) {
        BOOST_CHECK_EXCEPTION(CheckRegtestEvmChainId(id), std::runtime_error,
                              HasReason("refusing EVM chain id " + util::ToString(id)));
    }
}

BOOST_AUTO_TEST_CASE(test_key_derivation_is_deterministic)
{
    BOOST_CHECK_EQUAL(HexStr(TestKeyIkm("bootstrap", 0)), BOOTSTRAP_KEY0_IKM);
    const TestKey key0{DeriveTestKey("bootstrap", 0)};
    BOOST_CHECK_EQUAL(key0.index, 0U);
    BOOST_CHECK_EQUAL(HexStr(key0.secret.Bytes()), BOOTSTRAP_KEY0_SECRET);
    BOOST_CHECK_EQUAL(HexStr(DeriveTestKey("bootstrap", 3).secret.Bytes()), BOOTSTRAP_KEY3_SECRET);

    const TestKey again{DeriveTestKey("bootstrap", 0)};
    BOOST_CHECK(again.pubkey == key0.pubkey);
    BOOST_CHECK(again.pop == key0.pop);
    BOOST_CHECK(key0.pubkey == key0.secret.GetPublicKey());
    BOOST_CHECK(bls::VerifyPoP(key0.pubkey, key0.pop));
    BOOST_CHECK(bls::VerifiedPublicKey::FromPoP(key0.pubkey, key0.pop));
    // Index, label and label length all separate keys.
    BOOST_CHECK(!(DeriveTestKey("bootstrap", 1).pubkey == key0.pubkey));
    BOOST_CHECK(!(DeriveTestKey("rogue", 0).pubkey == key0.pubkey));
    BOOST_CHECK(TestKeyIkm("a", 0) != TestKeyIkm("a-", 0));

    for (const std::string_view good : {"a", "a-1", "epoch-1", "0123456789abcdefghijklmnopqrstuv"}) {
        BOOST_CHECK(ValidLabel(good));
    }
    for (const std::string_view bad : {"", "Bootstrap", "a_b", "a b", "a.b", "0123456789abcdefghijklmnopqrstuvw"}) {
        BOOST_CHECK(!ValidLabel(bad));
        BOOST_CHECK_EXCEPTION(DeriveTestKey(bad, 0), std::runtime_error, HasReason("invalid test-key label"));
    }
}

BOOST_AUTO_TEST_CASE(bootstrap_committee_header_and_set_hash)
{
    const TestCommittee committee{BootstrapCommittee()};
    BOOST_CHECK_EQUAL(committee.label, "bootstrap");
    BOOST_REQUIRE_EQUAL(committee.Size(), 4U);
    const modern::ValidatorSetHeader& header{committee.header};
    BOOST_CHECK_EQUAL(header.epoch, 0U);
    BOOST_CHECK_EQUAL(header.ruleset_version, modern::FINALITY_RULESET_V2);
    BOOST_CHECK_EQUAL(header.validator_count, 4U);
    BOOST_CHECK_EQUAL(header.total_weight, 4U);
    BOOST_CHECK_EQUAL(header.quorum_weight, 3U);
    // B3TypesV2.headerShapeValid: compressed, not infinity.
    BOOST_CHECK_EQUAL(header.aggregate_pubkey[0] & 0xc0, 0x80);
    BOOST_CHECK_EQUAL(HexStr(header.aggregate_pubkey), BOOTSTRAP_AGGREGATE_PUBKEY);
    BOOST_CHECK_EQUAL(HexStr(header.members_root), BOOTSTRAP_MEMBERS_ROOT);
    BOOST_CHECK_EQUAL(HexStr(committee.set_hash), BOOTSTRAP_SET_HASH);
    BOOST_CHECK(committee.set_hash == modern::ValidatorSetHash(header));

    std::vector<uint256> leaves;
    for (uint32_t i{0}; i < 4; ++i) {
        BOOST_CHECK(committee.keys[i].pubkey == DeriveTestKey("bootstrap", i).pubkey);
        BOOST_CHECK_EQUAL(committee.weights[i], 1U);
        leaves.push_back(modern::ValidatorSetLeaf(i, committee.keys[i].pubkey.Compressed(), 1));
        BOOST_CHECK(committee.leaves[i] == leaves.back());
    }
    const auto members_root{modern::ValidatorSetMembersRoot(leaves)};
    BOOST_REQUIRE(members_root);
    BOOST_CHECK(*members_root == header.members_root);
    const auto aggregate{bls::AggregatePublicKeys(committee.verified)};
    BOOST_REQUIRE(aggregate);
    BOOST_CHECK(aggregate->Compressed() == header.aggregate_pubkey);

    // MakeCommittee stamps the requested epoch into the ruleset-2 header.
    const TestCommittee epoch2{MakeCommittee("bootstrap", 2, {1, 1, 1, 1})};
    BOOST_CHECK_EQUAL(epoch2.header.epoch, 2U);
    BOOST_CHECK(epoch2.header.members_root == header.members_root);
    BOOST_CHECK(epoch2.set_hash != committee.set_hash);
    // Weighted committee: quorum floor(2W/3)+1.
    const TestCommittee weighted{MakeCommittee("weighted", 0, {5, 3, 2, 1})};
    BOOST_CHECK_EQUAL(weighted.header.total_weight, 11U);
    BOOST_CHECK_EQUAL(weighted.header.quorum_weight, 8U);
}

BOOST_AUTO_TEST_CASE(kofn_attestation_verifies)
{
    const KofNAttestation a{RegtestAttestation()};
    BOOST_CHECK_EQUAL(HexStr(a.message), VECTOR_CASES[0].message);
    BOOST_CHECK_EQUAL(HexStr(a.digest), VECTOR_CASES[0].digest);
    BOOST_CHECK_EQUAL(HexStr(a.committee.set_hash), BOOTSTRAP_SET_HASH);
    const SignedDigest& s{a.signed_digest};
    BOOST_CHECK(s.signers == (std::vector<uint32_t>{0, 1, 2}));
    BOOST_CHECK(s.bitmap == (std::vector<unsigned char>{0x07}));
    BOOST_CHECK_EQUAL(s.signed_weight, 3U);

    const std::span<const unsigned char> digest{a.digest.begin(), 32};
    const std::vector<bls::VerifiedPublicKey> signers{a.committee.verified.begin(), a.committee.verified.begin() + 3};
    BOOST_CHECK(bls::FastAggregateVerify(signers, digest, s.aggregate_signature));
    // The attestation is bound to its deployment: it does not verify for the
    // same statement on another verifier, nor as a digest-V2 certificate.
    const uint256 other_verifier{BootstrapAttestationDigestV2(RegtestModernChainDomain(), 31'337, FilledAddress(0x45),
                                                              Raw(VECTOR_CASES[0].start_set_hash), VectorBlock())};
    BOOST_CHECK(!bls::FastAggregateVerify(signers, std::span<const unsigned char>{other_verifier.begin(), 32},
                                          s.aggregate_signature));
    const uint256 digest_v2{modern::FinalityDigestV2(RegtestModernChainDomain(), VectorBlock())};
    BOOST_CHECK(!bls::FastAggregateVerify(signers, std::span<const unsigned char>{digest_v2.begin(), 32},
                                          s.aggregate_signature));

    // One absent witness (member 3) that folds to members_root.
    BOOST_REQUIRE_EQUAL(s.absent.size(), 1U);
    const BootstrapAbsentWitness& absent{s.absent[0]};
    BOOST_CHECK_EQUAL(absent.index, 3U);
    BOOST_CHECK_EQUAL(absent.weight, 1U);
    BOOST_CHECK(absent.compressed_pubkey == a.committee.keys[3].pubkey.Compressed());
    BOOST_CHECK(absent.uncompressed_pubkey == a.committee.keys[3].pubkey.Eip2537Uncompressed());
    BOOST_CHECK(FoldMember(absent) == a.committee.header.members_root);

    // The proof is the BlsDigestProver witness ABI over exactly these parts.
    const auto aggregate{bls::PublicKey::Decode(a.committee.header.aggregate_pubkey)};
    BOOST_REQUIRE(aggregate);
    BOOST_CHECK(s.proof == bootstrap_detail::EncodeProof(s.bitmap, s.aggregate_signature.Eip2537Uncompressed(),
                                                         aggregate->Eip2537Uncompressed(), s.absent));
    BOOST_CHECK_EQUAL(AbiWord(s.proof, 0), 128U);
    BOOST_CHECK_EQUAL(AbiWord(s.proof, 128), 1U);
    BOOST_CHECK_EQUAL(s.proof[160], 0x07);

    // All four, and another 3-of-4 choice.
    const KofNAttestation all{RegtestAttestation(std::vector<uint32_t>{3, 2, 1, 0})};
    BOOST_CHECK(all.signed_digest.signers == (std::vector<uint32_t>{0, 1, 2, 3}));
    BOOST_CHECK(all.signed_digest.bitmap == (std::vector<unsigned char>{0x0f}));
    BOOST_CHECK(all.signed_digest.absent.empty());
    BOOST_CHECK(bls::FastAggregateVerify(all.committee.verified, digest, all.signed_digest.aggregate_signature));
    const KofNAttestation last_three{RegtestAttestation(std::vector<uint32_t>{1, 2, 3})};
    BOOST_REQUIRE_EQUAL(last_three.signed_digest.absent.size(), 1U);
    BOOST_CHECK_EQUAL(last_three.signed_digest.absent[0].index, 0U);
    BOOST_CHECK(FoldMember(last_three.signed_digest.absent[0]) == last_three.committee.header.members_root);
    const std::vector<bls::VerifiedPublicKey> keys123{last_three.committee.verified.begin() + 1,
                                                      last_three.committee.verified.end()};
    BOOST_CHECK(bls::FastAggregateVerify(keys123, digest, last_three.signed_digest.aggregate_signature));
}

BOOST_AUTO_TEST_CASE(kofn_attestation_refusals)
{
    const VectorCase& c{VECTOR_CASES[0]};
    const modern::FinalizedBlockV2 fb{VectorBlock()};
    const uint256 domain{Raw(c.chain_domain)};
    const uint256 start{Raw(c.start_set_hash)};
    const EthAddress verifier{FilledAddress(c.verifier_byte)};
    auto build{[&](const uint256& d, uint64_t id, const EthAddress& v, const uint256& s,
                   std::optional<std::vector<uint32_t>> signers) {
        return BuildKofNAttestation(d, id, v, s, fb, std::move(signers));
    }};

    // The mainnet vector case is refused by name; any other domain too.
    const VectorCase& mainnet{VECTOR_CASES[1]};
    BOOST_CHECK_EXCEPTION(build(Raw(mainnet.chain_domain), mainnet.evm_chain_id, FilledAddress(mainnet.verifier_byte),
                                Raw(mainnet.start_set_hash), std::nullopt),
                          std::runtime_error, HasReason("refusing the B3 mainnet chain domain"));
    BOOST_CHECK_EXCEPTION(build(Raw("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"), 31'337,
                                verifier, start, std::nullopt),
                          std::runtime_error, HasReason("only the -b3modernregtest domain"));
    BOOST_CHECK_EXCEPTION(build(domain, 1, verifier, start, std::nullopt), std::runtime_error,
                          HasReason("refusing EVM chain id 1:"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'338, verifier, start, std::nullopt), std::runtime_error,
                          HasReason("refusing EVM chain id 31338"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, FilledAddress(0), start, std::nullopt), std::runtime_error,
                          HasReason("verifier address is zero"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, uint256{}, std::nullopt), std::runtime_error,
                          HasReason("start_set_hash is zero"));

    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, start, std::vector<uint32_t>{0, 1}), std::runtime_error,
                          HasReason("needs at least 3 of the 4 bootstrap signers"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, start, std::vector<uint32_t>{3}), std::runtime_error,
                          HasReason("needs at least 3 of the 4 bootstrap signers"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, start, std::vector<uint32_t>{0, 1, 1}), std::runtime_error,
                          HasReason("duplicate signer index 1"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, start, std::vector<uint32_t>{0, 1, 4}), std::runtime_error,
                          HasReason("bootstrap signer index 4 is outside 0..3"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, start, std::vector<uint32_t>{0, 1, 2, 3, 4}),
                          std::runtime_error, HasReason("bootstrap signer index 4 is outside 0..3"));
    BOOST_CHECK_EXCEPTION(build(domain, 31'337, verifier, start, std::vector<uint32_t>{}), std::runtime_error,
                          HasReason("no signers"));
}

BOOST_AUTO_TEST_CASE(test_certificate_builds_and_verifies)
{
    const uint256 domain{RegtestModernChainDomain()};
    modern::FinalizedBlockV2 fb{VectorBlock()};
    const std::vector<uint64_t> weights{5, 3, 2, 1};
    const TestCommittee committee{MakeCommittee("conflict", fb.epoch, weights)};

    const TestCertificate all{BuildTestCertificate(domain, fb, "conflict", weights, std::nullopt, committee.set_hash)};
    BOOST_CHECK(all.digest == modern::FinalityDigestV2(domain, fb));
    BOOST_CHECK(all.cert_id == modern::FinalityCertIdV2(fb));
    BOOST_CHECK(all.committee.set_hash == committee.set_hash);
    BOOST_CHECK(all.signed_digest.signers == (std::vector<uint32_t>{0, 1, 2, 3}));
    BOOST_CHECK_EQUAL(all.signed_digest.signed_weight, 11U);
    BOOST_CHECK(modern::VerifyFinalityCertificateV2(domain, fb, all.certificate, committee.View(),
                                                    fb.validator_set_hash) == modern::CertificateCheck::OK);
    // The B3 carrier payload decodes back to the same statement and certificate.
    const auto decoded{modern::DecodeCertificatePayloadV2(all.payload, 4)};
    BOOST_REQUIRE(decoded);
    BOOST_CHECK(decoded->first == fb);
    BOOST_CHECK(decoded->second.signer_bitmap == all.certificate.signer_bitmap);
    BOOST_CHECK(decoded->second.aggregate_sig == all.certificate.aggregate_sig);
    // A digest-V2 certificate is not a bootstrap attestation.
    const KofNAttestation attestation{RegtestAttestation()};
    const std::span<const unsigned char> bootstrap_digest{attestation.digest.begin(), 32};
    BOOST_CHECK(!bls::FastAggregateVerify(committee.verified, bootstrap_digest, all.signed_digest.aggregate_signature));

    // A quorum subset (headcount 3 of 4, weight 9 of quorum 8) with absent member 2.
    const TestCertificate subset{
        BuildTestCertificate(domain, fb, "conflict", weights, std::vector<uint32_t>{3, 0, 1}, std::nullopt)};
    BOOST_CHECK(subset.signed_digest.bitmap == (std::vector<unsigned char>{0x0b}));
    BOOST_REQUIRE_EQUAL(subset.signed_digest.absent.size(), 1U);
    BOOST_CHECK_EQUAL(subset.signed_digest.absent[0].index, 2U);
    BOOST_CHECK_EQUAL(subset.signed_digest.absent[0].weight, 2U);
    BOOST_CHECK(FoldMember(subset.signed_digest.absent[0]) == committee.header.members_root);

    // The committee header carries fb.epoch, so another epoch is another set.
    fb.epoch = 1;
    const TestCertificate epoch1{BuildTestCertificate(domain, fb, "conflict", weights, std::nullopt, std::nullopt)};
    BOOST_CHECK_EQUAL(epoch1.committee.header.epoch, 1U);
    BOOST_CHECK(epoch1.committee.set_hash != committee.set_hash);

    // A nine-member committee needs two bitmap bytes; members 6 and 7 are absent.
    const std::vector<uint64_t> nine(9, 1);
    const TestCertificate wide{
        BuildTestCertificate(domain, fb, "rogue", nine, std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 8}, std::nullopt)};
    BOOST_CHECK(wide.signed_digest.bitmap == (std::vector<unsigned char>{0x3f, 0x01}));
    BOOST_REQUIRE_EQUAL(wide.signed_digest.absent.size(), 2U);
    for (const BootstrapAbsentWitness& witness : wide.signed_digest.absent) {
        BOOST_CHECK(FoldMember(witness) == wide.committee.header.members_root);
    }
    BOOST_CHECK_EQUAL(wide.payload.size(), modern::FinalizedBlockV2::SIZE + 2 + modern::BLS_SIGNATURE_SIZE);
}

BOOST_AUTO_TEST_CASE(test_certificate_refusals)
{
    const uint256 domain{RegtestModernChainDomain()};
    const modern::FinalizedBlockV2 fb{VectorBlock()};
    const std::vector<uint64_t> weights{5, 3, 2, 1};
    auto build{[&](const uint256& d, std::string_view label, const std::vector<uint64_t>& w,
                   std::optional<std::vector<uint32_t>> signers, const std::optional<uint256>& expected) {
        return BuildTestCertificate(d, fb, label, w, std::move(signers), expected);
    }};

    // Naming a set that is not exactly the derived test committee.
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", weights, std::nullopt, Raw(VECTOR_CASES[0].start_set_hash)),
                          std::runtime_error, HasReason("refusing a signing set whose keys are not regtest test keys"));
    const TestCommittee bootstrap{BootstrapCommittee()};
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", {1, 1, 1, 1}, std::nullopt, bootstrap.set_hash),
                          std::runtime_error, HasReason("refusing a signing set whose keys are not regtest test keys"));
    BOOST_CHECK_NO_THROW(build(domain, "bootstrap", {1, 1, 1, 1}, std::nullopt, bootstrap.set_hash));

    BOOST_CHECK_EXCEPTION(build(MainnetChainDomain(), "conflict", weights, std::nullopt, std::nullopt),
                          std::runtime_error, HasReason("refusing the B3 mainnet chain domain"));
    BOOST_CHECK_EXCEPTION(build(uint256::ONE, "conflict", weights, std::nullopt, std::nullopt), std::runtime_error,
                          HasReason("only the -b3modernregtest domain"));

    BOOST_CHECK_EXCEPTION(build(domain, "Conflict", weights, std::nullopt, std::nullopt), std::runtime_error,
                          HasReason("invalid test-key label"));
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", {}, std::nullopt, std::nullopt), std::runtime_error,
                          HasReason("a test committee needs 1..8192 members"));
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", {1, 0, 1}, std::nullopt, std::nullopt), std::runtime_error,
                          HasReason("test committee weights must be positive"));
    const uint64_t half{std::numeric_limits<uint64_t>::max() / 2};
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", {half, 1}, std::nullopt, std::nullopt), std::runtime_error,
                          HasReason("test committee total weight is too large"));
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", {std::numeric_limits<uint64_t>::max(), 1}, std::nullopt,
                                std::nullopt),
                          std::runtime_error, HasReason("test committee total weight is too large"));

    BOOST_CHECK_EXCEPTION(build(domain, "conflict", weights, std::vector<uint32_t>{0, 1, 4}, std::nullopt),
                          std::runtime_error, HasReason("signer index 4 is outside 0..3"));
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", weights, std::vector<uint32_t>{0, 0, 1}, std::nullopt),
                          std::runtime_error, HasReason("duplicate signer index 0"));
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", weights, std::vector<uint32_t>{}, std::nullopt),
                          std::runtime_error, HasReason("no signers"));
    // Sub-quorum material is never emitted: weight 6 < 8, then headcount 2 < 3.
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", weights, std::vector<uint32_t>{1, 2, 3}, std::nullopt),
                          std::runtime_error, HasReason("signers do not meet the dual quorum"));
    BOOST_CHECK_EXCEPTION(build(domain, "conflict", weights, std::vector<uint32_t>{0, 1}, std::nullopt),
                          std::runtime_error, HasReason("signers do not meet the dual quorum"));
}

BOOST_AUTO_TEST_SUITE_END()

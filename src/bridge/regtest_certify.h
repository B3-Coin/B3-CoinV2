// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_REGTEST_CERTIFY_H
#define B3COIN_BRIDGE_REGTEST_CERTIFY_H

#include <bridge/bootstrap_proof.h>
#include <bridge/bootstrap_v2.h>
#include <bridge/deposit.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <crypto/sha256.h>
#include <modern/chain_domain.h>
#include <modern/finality_certificate.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <span.h>
#include <uint256.h>
#include <util/overflow.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * REGTEST ONLY bridge V2 certificate material from public deterministic test
 * keys (regtest plan section 3.2 "Certificate tool"; node changes rev 11
 * sections 7-8):
 *
 *  - the K_OF_N 3-of-4 bootstrap attestation of B3FinalityVerifierV2, bound
 *    to the verifier address, the EVM chain id and START_SET_HASH (open issue
 *    OI-12, owner ruling R9 of 2026-10-05; bridge/bootstrap_v2.h);
 *  - digest-V2 committee certificates from any labelled test committee, for
 *    the later forged (rogue committee) and conflicting (keys imported into
 *    B3 wallets with importfinalitykey) drills.
 *
 * Test keys: secret(label, i) = KeyGen(SHA256(TEST_KEY_TAG || u8 len(label)
 * || label || LE32 i)), labels [a-z0-9-]{1,32}. They are public and
 * worthless anywhere else: the tool refuses every chain domain but the
 * -b3modernregtest one (ModernChainDomain(g, g) for its pinned genesis g,
 * the B3 mainnet domain by name) and every EVM chain id but anvil's 31337,
 * and it never takes a secret key as input. A caller that names the set it
 * expects is refused unless that set is exactly the derived test committee,
 * so the tool can never claim to speak for a set of real keys.
 *
 * Every refusal throws std::runtime_error with a stable message. Tooling
 * only: included by b3-bridge-regtest-certify (B3_BRIDGE_TOOLS) and its unit
 * test, never by node code.
 */
namespace bridge {
namespace regtest_certify {

//! Test-key derivation tag.
inline constexpr std::string_view TEST_KEY_TAG{"B3-REGTEST-BRIDGE-V2-TEST-KEY-V1"};
inline constexpr size_t MAX_LABEL_SIZE{32};
//! anvil's chain id, the only EVM chain a bootstrap attestation may name.
inline constexpr uint64_t REGTEST_EVM_CHAIN_ID{31'337};
//! The K_OF_N bootstrap committee: label "bootstrap", four keys of weight 1
//! at epoch 0, so its ruleset-2 header is n = 4, W = 4, quorum 3, exactly the
//! shape B3FinalityVerifierV2's constructor requires.
inline constexpr std::string_view BOOTSTRAP_LABEL{"bootstrap"};
inline constexpr uint32_t BOOTSTRAP_MEMBERS{4};
inline constexpr uint32_t BOOTSTRAP_MIN_SIGNERS{3};
//! getblockhash 0 of a -b3modernregtest node (display order): the
//! legacy-format genesis of kernel/chainparams.cpp, independent of the other
//! regtest knobs (test/functional/test_framework/b3bridge.py
//! REGTEST_MODERN_GENESIS_HASH; the unit test checks it against CChainParams).
inline constexpr uint256 REGTEST_MODERN_GENESIS_HASH{
    "10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a"};
//! The B3 mainnet chain domain in raw (EVM) byte order
//! (contracts/deployments/ethereum-mainnet-v1.1.1.json b3_chain_domain).
inline constexpr std::string_view MAINNET_CHAIN_DOMAIN_HEX{
    "a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a"};

//! 0x-prefixed hex of raw bytes (the EVM byte order every output uses).
inline std::string EvmHex(std::span<const unsigned char> bytes) { return "0x" + HexStr(bytes); }
inline std::string EvmHex(const uint256& value) { return EvmHex(std::span<const unsigned char>{value.begin(), 32}); }

//! uint256 holding exactly the 32 raw bytes of `hex` (no byte reversal).
inline uint256 RawHash(std::string_view hex)
{
    const auto bytes{TryParseHex<unsigned char>(hex)};
    if (!bytes || bytes->size() != 32) throw std::logic_error("raw hash constant is not 32 bytes of hex");
    return uint256{std::span<const unsigned char>{*bytes}};
}

inline uint256 MainnetChainDomain() { return RawHash(MAINNET_CHAIN_DOMAIN_HEX); }

//! ModernChainDomain(g, g) for the -b3modernregtest genesis g (H = 0, X = g).
inline uint256 RegtestModernChainDomain()
{
    const auto domain{modern::ModernChainDomain(REGTEST_MODERN_GENESIS_HASH, REGTEST_MODERN_GENESIS_HASH)};
    if (!domain) throw std::logic_error("regtest chain domain is unavailable");
    return *domain;
}

inline void CheckRegtestDomain(const uint256& chain_domain)
{
    if (chain_domain == MainnetChainDomain()) {
        throw std::runtime_error("refusing the B3 mainnet chain domain " + EvmHex(chain_domain) + " (regtest only)");
    }
    if (chain_domain != RegtestModernChainDomain()) {
        throw std::runtime_error("refusing chain domain " + EvmHex(chain_domain) +
                                 ": only the -b3modernregtest domain " + EvmHex(RegtestModernChainDomain()) +
                                 " is accepted (regtest only)");
    }
}

inline void CheckRegtestEvmChainId(const uint64_t evm_chain_id)
{
    if (evm_chain_id != REGTEST_EVM_CHAIN_ID) {
        throw std::runtime_error("refusing EVM chain id " + util::ToString(evm_chain_id) +
                                 ": only anvil's 31337 is accepted (regtest only)");
    }
}

inline bool ValidLabel(std::string_view label)
{
    if (label.empty() || label.size() > MAX_LABEL_SIZE) return false;
    return std::all_of(label.begin(), label.end(), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

inline void CheckLabel(std::string_view label)
{
    if (!ValidLabel(label)) {
        throw std::runtime_error("invalid test-key label \"" + std::string{label} + "\": expected [a-z0-9-]{1,32}");
    }
}

//! One public deterministic test key with its proof of possession.
struct TestKey {
    uint32_t index;
    bls::SecretKey secret;
    bls::PublicKey pubkey;
    bls::Signature pop;
};

//! SHA256(TEST_KEY_TAG || u8 len(label) || label || LE32 index).
inline std::array<unsigned char, CSHA256::OUTPUT_SIZE> TestKeyIkm(std::string_view label, const uint32_t index)
{
    CheckLabel(label);
    const unsigned char length{static_cast<unsigned char>(label.size())};
    std::array<unsigned char, 4> index_le{};
    WriteLE32(index_le.data(), index);
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> ikm{};
    CSHA256()
        .Write(UCharCast(TEST_KEY_TAG.data()), TEST_KEY_TAG.size())
        .Write(&length, 1)
        .Write(UCharCast(label.data()), label.size())
        .Write(index_le.data(), index_le.size())
        .Finalize(ikm.data());
    return ikm;
}

inline TestKey DeriveTestKey(std::string_view label, const uint32_t index)
{
    const auto secret{bls::SecretKey::FromIKM(TestKeyIkm(label, index))};
    if (!secret) throw std::runtime_error("test key derivation failed");
    const bls::PublicKey pubkey{secret->GetPublicKey()};
    const bls::Signature pop{secret->SignPoP()};
    if (!bls::VerifyPoP(pubkey, pop)) throw std::runtime_error("test key proof of possession failed");
    return TestKey{index, *secret, pubkey, pop};
}

//! A labelled test committee and its ruleset-2 header at one fresh epoch.
struct TestCommittee {
    std::string label;
    std::vector<TestKey> keys;
    std::vector<uint64_t> weights;
    //! modern::ValidatorSetLeaf(i, pk_i, w_i), index order.
    std::vector<uint256> leaves;
    //! PoP-verified keys, index order (the aggregate admits only these).
    std::vector<bls::VerifiedPublicKey> verified;
    modern::ValidatorSetHeader header;
    uint256 set_hash;

    uint32_t Size() const { return static_cast<uint32_t>(keys.size()); }

    modern::ValidatorSetView View() const
    {
        modern::ValidatorSetView view;
        view.validator_count = Size();
        view.quorum_weight = header.quorum_weight;
        view.keys = verified;
        view.weights = weights;
        return view;
    }
};

/**
 * Members 0..n-1 of `label` with the given weights: a ruleset-2 header with
 * quorum floor(2W/3)+1, the aggregate of the PoP-verified keys and the
 * depth-13 ordered members root over ValidatorSetLeaf(i, pk_i, w_i).
 */
inline TestCommittee MakeCommittee(std::string_view label, const uint64_t epoch, const std::vector<uint64_t>& weights)
{
    CheckLabel(label);
    if (weights.empty() || weights.size() > modern::MAX_FINALITY_SET) {
        throw std::runtime_error("a test committee needs 1.." + util::ToString(modern::MAX_FINALITY_SET) + " members");
    }
    uint64_t total{0};
    for (const uint64_t weight : weights) {
        if (weight == 0) throw std::runtime_error("test committee weights must be positive");
        const auto sum{CheckedAdd(total, weight)};
        // QuorumWeightV1 saturates above 2^63; keep W where it is exact.
        if (!sum || *sum > std::numeric_limits<uint64_t>::max() / 2) {
            throw std::runtime_error("test committee total weight is too large");
        }
        total = *sum;
    }
    TestCommittee out;
    out.label = std::string{label};
    out.weights = weights;
    out.keys.reserve(weights.size());
    out.leaves.reserve(weights.size());
    out.verified.reserve(weights.size());
    for (uint32_t i{0}; i < weights.size(); ++i) {
        TestKey key{DeriveTestKey(label, i)};
        const auto verified{bls::VerifiedPublicKey::FromPoP(key.pubkey, key.pop)};
        if (!verified) throw std::runtime_error("test key proof of possession failed");
        out.leaves.push_back(modern::ValidatorSetLeaf(i, key.pubkey.Compressed(), weights[i]));
        out.verified.push_back(*verified);
        out.keys.push_back(std::move(key));
    }
    const auto aggregate{bls::AggregatePublicKeys(out.verified)};
    if (!aggregate) throw std::runtime_error("test committee aggregate key is infinity");
    out.header.epoch = epoch;
    out.header.ruleset_version = modern::FINALITY_RULESET_V2;
    out.header.validator_count = out.Size();
    out.header.total_weight = total;
    out.header.quorum_weight = modern::QuorumWeightV1(total);
    out.header.aggregate_pubkey = aggregate->Compressed();
    uint256 members_root;
    (void)bootstrap_detail::MerklePath(out.leaves, 0, members_root);
    out.header.members_root = members_root;
    out.set_hash = modern::ValidatorSetHash(out.header);
    return out;
}

//! The K_OF_N bootstrap committee: "bootstrap", 4 x weight 1, epoch 0.
inline TestCommittee BootstrapCommittee()
{
    return MakeCommittee(BOOTSTRAP_LABEL, 0, std::vector<uint64_t>(BOOTSTRAP_MEMBERS, 1));
}

//! Signer indices, ascending; empty, out-of-range and duplicate indices are refused.
inline std::vector<uint32_t> NormalizeSigners(std::vector<uint32_t> signers, const uint32_t validator_count)
{
    for (const uint32_t index : signers) {
        if (index >= validator_count) {
            throw std::runtime_error("signer index " + util::ToString(index) + " is outside 0.." +
                                     util::ToString(validator_count - 1));
        }
    }
    std::sort(signers.begin(), signers.end());
    const auto duplicate{std::adjacent_find(signers.begin(), signers.end())};
    if (duplicate != signers.end()) {
        throw std::runtime_error("duplicate signer index " + util::ToString(*duplicate));
    }
    if (signers.empty()) throw std::runtime_error("no signers");
    return signers;
}

//! An aggregate signature over one digest and its BlsDigestProver witness.
struct SignedDigest {
    std::vector<uint32_t> signers;
    std::vector<unsigned char> bitmap;
    uint64_t signed_weight;
    bls::Signature aggregate_signature;
    std::vector<BootstrapAbsentWitness> absent;
    //! abi.encode(bitmap, sigG2(256), aggregatePubkeyG1(128), Absent[]): the
    //! V1 witness ABI BlsDigestProver.verifyDigest decodes.
    std::vector<unsigned char> proof;
};

/**
 * `signers` of `committee` sign `digest`. The signers must meet B3's dual
 * quorum (headcount floor(2n/3)+1 and weight >= quorum_weight), so the tool
 * never emits sub-quorum material. The aggregate is re-verified and every
 * absent witness is folded back to members_root before it is returned.
 */
inline SignedDigest SignDigest(const TestCommittee& committee, const uint256& digest, std::vector<uint32_t> signers)
{
    const uint32_t n{committee.Size()};
    if (n == 0 || committee.weights.size() != n || committee.leaves.size() != n || committee.verified.size() != n) {
        throw std::logic_error("inconsistent test committee");
    }
    signers = NormalizeSigners(std::move(signers), n);

    std::vector<unsigned char> bitmap(modern::SignerBitmapBytes(n), 0);
    std::vector<bls::Signature> signatures;
    std::vector<bls::VerifiedPublicKey> signer_keys;
    uint64_t signed_weight{0};
    const std::span<const unsigned char> message{digest.begin(), 32};
    for (const uint32_t index : signers) {
        bitmap[index / 8] |= static_cast<unsigned char>(1U << (index % 8));
        signatures.push_back(committee.keys[index].secret.Sign(message));
        signer_keys.push_back(committee.verified[index]);
        signed_weight += committee.weights[index];
    }
    if (signers.size() < modern::FinalityHeadcountQuorum(n) || signed_weight < committee.header.quorum_weight) {
        throw std::runtime_error("signers do not meet the dual quorum: " + util::ToString(signers.size()) + " of " +
                                 util::ToString(n) + " members (need " +
                                 util::ToString(modern::FinalityHeadcountQuorum(n)) + "), weight " +
                                 util::ToString(signed_weight) + " (need " +
                                 util::ToString(committee.header.quorum_weight) + ")");
    }
    const auto aggregate{bls::AggregateSignatures(signatures)};
    if (!aggregate) throw std::runtime_error("aggregate signature is infinity");
    if (!bls::FastAggregateVerify(signer_keys, message, *aggregate)) {
        throw std::runtime_error("aggregate signature does not verify");
    }
    const auto full_aggregate{bls::PublicKey::Decode(committee.header.aggregate_pubkey)};
    if (!full_aggregate) throw std::runtime_error("test committee aggregate key is not canonical");

    std::vector<BootstrapAbsentWitness> absent;
    for (uint32_t i{0}; i < n; ++i) {
        if (modern::SignerBit(bitmap, i)) continue;
        BootstrapAbsentWitness witness;
        witness.index = i;
        witness.compressed_pubkey = committee.keys[i].pubkey.Compressed();
        witness.weight = committee.weights[i];
        witness.uncompressed_pubkey = committee.keys[i].pubkey.Eip2537Uncompressed();
        uint256 root;
        witness.siblings = bootstrap_detail::MerklePath(committee.leaves, i, root);
        if (root != committee.header.members_root) {
            throw std::runtime_error("absent-member path does not reproduce the members root");
        }
        absent.push_back(std::move(witness));
    }
    std::vector<unsigned char> proof{bootstrap_detail::EncodeProof(
        bitmap, aggregate->Eip2537Uncompressed(), full_aggregate->Eip2537Uncompressed(), absent)};
    return SignedDigest{std::move(signers), std::move(bitmap), signed_weight, *aggregate, std::move(absent),
                        std::move(proof)};
}

//! The K_OF_N initialize witness (OI-12): message, digest, the bootstrap
//! header and set hash, and the signed proof with its bitmap.
struct KofNAttestation {
    std::vector<unsigned char> message;
    uint256 digest;
    TestCommittee committee;
    SignedDigest signed_digest;
};

/**
 * Sign B3FinalityVerifierV2's K_OF_N bootstrap digest for one deployment:
 * BootstrapAttestationDigestV2(domain, evm_chain_id, verifier, start_set_hash,
 * fb) by bootstrap members `signers` (default 0, 1, 2). Refuses a non-regtest
 * domain or chain id, a zero verifier or start set hash, and fewer than 3,
 * duplicate or out-of-range (>= 4) signers.
 */
inline KofNAttestation BuildKofNAttestation(const uint256& chain_domain, const uint64_t evm_chain_id,
                                            const EthAddress& verifier, const uint256& start_set_hash,
                                            const modern::FinalizedBlockV2& fb,
                                            std::optional<std::vector<uint32_t>> signers = std::nullopt)
{
    CheckRegtestDomain(chain_domain);
    CheckRegtestEvmChainId(evm_chain_id);
    if (std::all_of(verifier.begin(), verifier.end(), [](const unsigned char b) { return b == 0; })) {
        throw std::runtime_error("verifier address is zero");
    }
    if (start_set_hash.IsNull()) throw std::runtime_error("start_set_hash is zero");
    std::vector<uint32_t> chosen{signers ? std::move(*signers) : std::vector<uint32_t>{0, 1, 2}};
    for (const uint32_t index : chosen) {
        if (index >= BOOTSTRAP_MEMBERS) {
            throw std::runtime_error("bootstrap signer index " + util::ToString(index) + " is outside 0..3");
        }
    }
    chosen = NormalizeSigners(std::move(chosen), BOOTSTRAP_MEMBERS);
    if (chosen.size() < BOOTSTRAP_MIN_SIGNERS) {
        throw std::runtime_error("a K_OF_N bootstrap attestation needs at least 3 of the 4 bootstrap signers");
    }
    TestCommittee committee{BootstrapCommittee()};
    std::vector<unsigned char> message{
        BootstrapAttestationMessageV2(chain_domain, evm_chain_id, verifier, start_set_hash, fb)};
    const uint256 digest{BootstrapAttestationDigestV2(chain_domain, evm_chain_id, verifier, start_set_hash, fb)};
    SignedDigest signed_digest{SignDigest(committee, digest, std::move(chosen))};
    return KofNAttestation{std::move(message), digest, std::move(committee), std::move(signed_digest)};
}

//! A digest-V2 certificate from a test committee: the B3 carrier payload
//! (FinalizedBlockV2 || bitmap || sig) and the verifier's prover witness.
struct TestCertificate {
    uint256 digest;
    uint256 cert_id;
    TestCommittee committee;
    SignedDigest signed_digest;
    modern::FinalityCertificate certificate;
    std::vector<unsigned char> payload;
};

/**
 * Sign FinalityDigestV2(domain, fb) with the epoch-fb.epoch committee of
 * `label` (members with `weights`; `signers` default all). With
 * `expected_set_hash` (B3's or the verifier's Set'_{fb.epoch}) the derived
 * committee must be exactly that set, or the call is refused as naming a set
 * of non-test keys; without it the committee stands alone (the rogue-committee
 * drill). The result re-verifies with modern::VerifyFinalityCertificateV2.
 */
inline TestCertificate BuildTestCertificate(const uint256& chain_domain, const modern::FinalizedBlockV2& fb,
                                            std::string_view label, const std::vector<uint64_t>& weights,
                                            std::optional<std::vector<uint32_t>> signers,
                                            const std::optional<uint256>& expected_set_hash)
{
    CheckRegtestDomain(chain_domain);
    TestCommittee committee{MakeCommittee(label, fb.epoch, weights)};
    if (expected_set_hash && *expected_set_hash != committee.set_hash) {
        throw std::runtime_error("refusing a signing set whose keys are not regtest test keys: expected set hash " +
                                 EvmHex(*expected_set_hash) + ", test committee \"" + committee.label +
                                 "\" at epoch " + util::ToString(fb.epoch) + " has " + EvmHex(committee.set_hash));
    }
    std::vector<uint32_t> chosen;
    if (signers) {
        chosen = std::move(*signers);
    } else {
        for (uint32_t i{0}; i < committee.Size(); ++i) chosen.push_back(i);
    }
    const uint256 digest{modern::FinalityDigestV2(chain_domain, fb)};
    SignedDigest signed_digest{SignDigest(committee, digest, std::move(chosen))};
    modern::FinalityCertificate certificate;
    certificate.signer_bitmap = signed_digest.bitmap;
    certificate.aggregate_sig = signed_digest.aggregate_signature.Compressed();
    const auto check{modern::VerifyFinalityCertificateV2(chain_domain, fb, certificate, committee.View(),
                                                         fb.validator_set_hash)};
    if (check != modern::CertificateCheck::OK) {
        throw std::runtime_error(std::string{"test certificate does not verify: "} +
                                 modern::CertificateCheckName(check));
    }
    std::vector<unsigned char> payload{modern::EncodeCertificatePayloadV2(fb, certificate)};
    return TestCertificate{digest,
                           modern::FinalityCertIdV2(fb),
                           std::move(committee),
                           std::move(signed_digest),
                           std::move(certificate),
                           std::move(payload)};
}

} // namespace regtest_certify
} // namespace bridge

#endif // B3COIN_BRIDGE_REGTEST_CERTIFY_H

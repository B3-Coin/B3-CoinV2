// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_BOOTSTRAP_PROOF_H
#define B3COIN_BRIDGE_BOOTSTRAP_PROOF_H

#include <crypto/bls.h>
#include <modern/finality_types.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bridge {

/** One PoP-authenticated member of the immutable four-key bridge committee. */
struct BootstrapIdentity {
    std::array<unsigned char, 32> validator_key{};
    bls::PublicKey bls_pubkey;
    bls::Signature proof_of_possession;
    uint32_t binding_seq{0};
    uint64_t binding_height{0};
};

/** The consensus objects and one wallet's output from signbridgebootstrap. */
struct BootstrapSignaturePackage {
    std::array<unsigned char, 32> validator_key{};
    bls::PublicKey bls_pubkey;
    modern::FinalizedBlock snapshot;
    modern::ValidatorSetHeader set0;
    uint256 digest;
    bls::Signature signature;
    uint32_t binding_seq{0};
    uint64_t binding_height{0};
    uint256 chain_domain;
};

struct BootstrapAbsentWitness {
    uint32_t index{0};
    std::array<unsigned char, bls::PUBKEY_SIZE> compressed_pubkey{};
    uint64_t weight{1};
    std::array<unsigned char, 128> uncompressed_pubkey{};
    std::array<uint256, modern::FINALITY_SET_TREE_DEPTH> siblings{};
};

/** Deterministic deployment pins derived from the four public identities. */
struct BootstrapSetArtifacts {
    modern::ValidatorSetHeader bootstrap_set;
    uint256 bootstrap_set_hash;
};

struct BootstrapProofArtifacts {
    BootstrapProofArtifacts(bls::Signature signature, bls::PublicKey pubkey)
        : aggregate_signature{std::move(signature)},
          aggregate_pubkey{std::move(pubkey)}
    {
    }

    modern::ValidatorSetHeader bootstrap_set;
    uint256 bootstrap_set_hash;
    modern::FinalizedBlock snapshot;
    modern::ValidatorSetHeader set0;
    std::vector<unsigned char> signer_bitmap;
    bls::Signature aggregate_signature;
    bls::PublicKey aggregate_pubkey;
    BootstrapAbsentWitness absent;
    std::vector<unsigned char> proof_abi;
    std::vector<unsigned char> initialize_calldata;
};

namespace bootstrap_detail {

inline void Append(std::vector<unsigned char>& out,
                   std::span<const unsigned char> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline std::array<unsigned char, 32> Word(const uint64_t value)
{
    std::array<unsigned char, 32> out{};
    WriteBE64(out.data() + 24, value);
    return out;
}

inline std::array<unsigned char, 32> Word(const uint256& value)
{
    std::array<unsigned char, 32> out{};
    std::copy(value.begin(), value.end(), out.begin());
    return out;
}

inline std::vector<unsigned char> DynamicBytes(
    std::span<const unsigned char> value)
{
    std::vector<unsigned char> out;
    const auto length{Word(value.size())};
    Append(out, length);
    Append(out, value);
    out.resize((out.size() + 31) & ~size_t{31}, 0);
    return out;
}

inline std::vector<unsigned char> EncodeAbsent(
    const BootstrapAbsentWitness& absent)
{
    // (uint32,bytes,uint64,bytes,bytes32[13]): the tuple head is 17 words.
    constexpr size_t HEAD{17 * 32};
    const auto pubkey{DynamicBytes(absent.compressed_pubkey)};
    const auto uncompressed{DynamicBytes(absent.uncompressed_pubkey)};
    std::vector<unsigned char> out;
    out.reserve(HEAD + pubkey.size() + uncompressed.size());
    Append(out, Word(absent.index));
    Append(out, Word(HEAD));
    Append(out, Word(absent.weight));
    Append(out, Word(HEAD + pubkey.size()));
    for (const uint256& sibling : absent.siblings) Append(out, Word(sibling));
    Append(out, pubkey);
    Append(out, uncompressed);
    return out;
}

inline std::vector<unsigned char> EncodeAbsentArray(
    const std::span<const BootstrapAbsentWitness> absent)
{
    // Dynamic-array element offsets are measured from the element head,
    // immediately after the array length word.
    std::vector<std::vector<unsigned char>> elements;
    elements.reserve(absent.size());
    size_t size{32 + absent.size() * 32};
    for (const BootstrapAbsentWitness& witness : absent) {
        elements.push_back(EncodeAbsent(witness));
        size += elements.back().size();
    }
    std::vector<unsigned char> out;
    out.reserve(size);
    Append(out, Word(absent.size()));
    size_t offset{absent.size() * 32};
    for (const auto& element : elements) {
        Append(out, Word(offset));
        offset += element.size();
    }
    for (const auto& element : elements) Append(out, element);
    return out;
}

inline std::vector<unsigned char> EncodeAbsentArray(
    const BootstrapAbsentWitness& absent)
{
    return EncodeAbsentArray(
        std::span<const BootstrapAbsentWitness>{&absent, 1});
}

inline std::vector<unsigned char> EncodeProof(
    std::span<const unsigned char> bitmap,
    std::span<const unsigned char> signature,
    std::span<const unsigned char> aggregate_pubkey,
    std::span<const BootstrapAbsentWitness> absent)
{
    const auto bitmap_tail{DynamicBytes(bitmap)};
    const auto signature_tail{DynamicBytes(signature)};
    const auto aggregate_tail{DynamicBytes(aggregate_pubkey)};
    const auto absent_tail{EncodeAbsentArray(absent)};
    constexpr size_t HEAD{4 * 32};
    std::vector<unsigned char> out;
    out.reserve(HEAD + bitmap_tail.size() + signature_tail.size() +
                aggregate_tail.size() + absent_tail.size());
    Append(out, Word(HEAD));
    Append(out, Word(HEAD + bitmap_tail.size()));
    Append(out, Word(HEAD + bitmap_tail.size() + signature_tail.size()));
    Append(out, Word(HEAD + bitmap_tail.size() + signature_tail.size() +
                     aggregate_tail.size()));
    Append(out, bitmap_tail);
    Append(out, signature_tail);
    Append(out, aggregate_tail);
    Append(out, absent_tail);
    return out;
}

inline std::vector<unsigned char> EncodeProof(
    std::span<const unsigned char> bitmap,
    std::span<const unsigned char> signature,
    std::span<const unsigned char> aggregate_pubkey,
    const BootstrapAbsentWitness& absent)
{
    return EncodeProof(
        bitmap, signature, aggregate_pubkey,
        std::span<const BootstrapAbsentWitness>{&absent, 1});
}

inline std::vector<unsigned char> EncodeSetHeaderTuple(
    const modern::ValidatorSetHeader& header)
{
    // The aggregate key is the sole dynamic member of this seven-field tuple.
    constexpr size_t HEAD{7 * 32};
    const auto aggregate{DynamicBytes(header.aggregate_pubkey)};
    std::vector<unsigned char> out;
    out.reserve(HEAD + aggregate.size());
    Append(out, Word(header.epoch));
    Append(out, Word(header.ruleset_version));
    Append(out, Word(header.validator_count));
    Append(out, Word(header.total_weight));
    Append(out, Word(header.quorum_weight));
    Append(out, Word(HEAD));
    Append(out, Word(header.members_root));
    Append(out, aggregate);
    return out;
}

inline std::vector<unsigned char> EncodeInitializeCalldata(
    const modern::FinalizedBlock& snapshot,
    const modern::ValidatorSetHeader& set0,
    std::span<const unsigned char> proof)
{
    // initialize((uint64,bytes32,bytes32,bytes32,uint64),
    //            (uint64,uint16,uint32,uint64,uint64,bytes,bytes32),bytes)
    static constexpr std::string_view SIGNATURE{
        "initialize((uint64,bytes32,bytes32,bytes32,uint64),(uint64,uint16,uint32,uint64,uint64,bytes,bytes32),bytes)"};
    uint256 selector_hash;
    Keccak256().Write(std::span<const unsigned char>{
                          reinterpret_cast<const unsigned char*>(SIGNATURE.data()),
                          SIGNATURE.size()})
        .Finalize(selector_hash);

    const auto set_tail{EncodeSetHeaderTuple(set0)};
    const auto proof_tail{DynamicBytes(proof)};
    constexpr size_t HEAD{7 * 32}; // five snapshot words + two offsets
    std::vector<unsigned char> out;
    out.reserve(4 + HEAD + set_tail.size() + proof_tail.size());
    out.insert(out.end(), selector_hash.begin(), selector_hash.begin() + 4);
    Append(out, Word(snapshot.height));
    Append(out, Word(snapshot.block_hash));
    Append(out, Word(snapshot.withdrawal_root));
    Append(out, Word(snapshot.validator_set_hash));
    Append(out, Word(snapshot.epoch));
    Append(out, Word(HEAD));
    Append(out, Word(HEAD + set_tail.size()));
    Append(out, set_tail);
    Append(out, proof_tail);
    return out;
}

inline std::array<uint256, modern::FINALITY_SET_TREE_DEPTH> MerklePath(
    std::vector<uint256> leaves, const size_t index, uint256& root)
{
    leaves.resize(modern::MAX_FINALITY_SET, uint256{});
    std::array<uint256, modern::FINALITY_SET_TREE_DEPTH> siblings{};
    size_t cursor{index};
    for (size_t level{0}; level < modern::FINALITY_SET_TREE_DEPTH; ++level) {
        siblings[level] = leaves[cursor ^ 1];
        std::vector<uint256> next;
        next.reserve(leaves.size() / 2);
        for (size_t i{0}; i < leaves.size(); i += 2) {
            std::array<unsigned char, 64> pair{};
            std::copy(leaves[i].begin(), leaves[i].end(), pair.begin());
            std::copy(leaves[i + 1].begin(), leaves[i + 1].end(),
                      pair.begin() + 32);
            next.push_back(modern::Keccak(pair));
        }
        leaves = std::move(next);
        cursor >>= 1;
    }
    root = leaves.front();
    return siblings;
}

} // namespace bootstrap_detail

/**
 * Validate the four public bootstrap identities and derive the exact synthetic
 * equal-weight 3-of-4 header that is pinned into the Ethereum deployment.
 * This intentionally needs no M-1 signature package, so it can run before M.
 */
inline std::optional<BootstrapSetArtifacts> BuildBootstrapSet(
    const uint256& chain_domain,
    std::vector<BootstrapIdentity> identities,
    const uint64_t modern_start_height,
    std::string& error)
{
    auto fail{[&](std::string message)
              -> std::optional<BootstrapSetArtifacts> {
        error = std::move(message);
        return std::nullopt;
    }};
    if (chain_domain.IsNull()) return fail("chain_domain is zero");
    if (modern_start_height == 0) return fail("modern_start_height is zero");
    if (identities.size() != 4) return fail("manifest must contain exactly four identities");

    std::sort(identities.begin(), identities.end(),
              [](const BootstrapIdentity& a, const BootstrapIdentity& b) {
                  return a.validator_key < b.validator_key;
              });
    std::vector<bls::VerifiedPublicKey> verified;
    verified.reserve(4);
    std::vector<uint256> leaves;
    leaves.reserve(4);
    for (size_t i{0}; i < identities.size(); ++i) {
        if (i != 0 && identities[i - 1].validator_key == identities[i].validator_key) {
            return fail("duplicate validator_key in manifest");
        }
        for (size_t j{0}; j < i; ++j) {
            if (identities[j].bls_pubkey == identities[i].bls_pubkey) {
                return fail("duplicate bls_pubkey in manifest");
            }
        }
        const auto key{bls::VerifiedPublicKey::FromPoP(
            identities[i].bls_pubkey, identities[i].proof_of_possession)};
        if (!key) {
            return fail("invalid proof of possession for manifest member " +
                        std::to_string(i));
        }
        if (identities[i].binding_height > modern_start_height - 1) {
            return fail("manifest binding was not active by the M-1 snapshot");
        }
        verified.push_back(*key);
        leaves.push_back(modern::ValidatorSetLeaf(
            i, identities[i].bls_pubkey.Compressed(), 1));
    }
    const auto aggregate{bls::AggregatePublicKeys(verified)};
    if (!aggregate) return fail("four-key aggregate public key is infinity");

    BootstrapSetArtifacts out;
    out.bootstrap_set.epoch = 0;
    out.bootstrap_set.ruleset_version = modern::FINALITY_RULESET_V1;
    out.bootstrap_set.validator_count = 4;
    out.bootstrap_set.total_weight = 4;
    out.bootstrap_set.quorum_weight = 3;
    out.bootstrap_set.aggregate_pubkey = aggregate->Compressed();
    uint256 members_root;
    (void)bootstrap_detail::MerklePath(leaves, 0, members_root);
    out.bootstrap_set.members_root = members_root;
    out.bootstrap_set_hash = modern::ValidatorSetHash(out.bootstrap_set);
    error.clear();
    return out;
}

/**
 * Build the exact one-time 3-of-4 bootstrap witness and initialize calldata.
 * Every committee member needs a valid PoP: this keeps raw externally supplied
 * keys out of B3's aggregate-key API and makes the published manifest safe to
 * reproduce independently.
 */
inline std::optional<BootstrapProofArtifacts> BuildBootstrapProof(
    const uint256& chain_domain,
    std::vector<BootstrapIdentity> identities,
    std::vector<BootstrapSignaturePackage> packages,
    const uint64_t modern_start_height,
    std::string& error)
{
    auto fail{[&](std::string message)
              -> std::optional<BootstrapProofArtifacts> {
        error = std::move(message);
        return std::nullopt;
    }};
    if (packages.size() != 3) return fail("exactly three signature packages are required");

    const auto deployment_set{BuildBootstrapSet(
        chain_domain, identities, modern_start_height, error)};
    if (!deployment_set) return std::nullopt;

    std::sort(identities.begin(), identities.end(),
              [](const BootstrapIdentity& a, const BootstrapIdentity& b) {
                  return a.validator_key < b.validator_key;
              });
    std::vector<bls::VerifiedPublicKey> verified;
    verified.reserve(4);
    std::vector<uint256> leaves;
    leaves.reserve(4);
    for (size_t i{0}; i < identities.size(); ++i) {
        const auto vk{bls::VerifiedPublicKey::FromPoP(
            identities[i].bls_pubkey, identities[i].proof_of_possession)};
        if (!vk) return fail("validated manifest member lost proof of possession");
        verified.push_back(*vk);
        leaves.push_back(modern::ValidatorSetLeaf(
            i, identities[i].bls_pubkey.Compressed(), 1));
    }
    const auto full_aggregate{bls::PublicKey::Decode(
        deployment_set->bootstrap_set.aggregate_pubkey)};
    if (!full_aggregate) return fail("validated bootstrap aggregate key is invalid");

    const BootstrapSignaturePackage& reference{packages.front()};
    if (reference.snapshot.height != modern_start_height - 1 ||
        reference.snapshot.epoch != 0 ||
        !reference.snapshot.withdrawal_root.IsNull() ||
        reference.snapshot.block_hash.IsNull()) {
        return fail("signature package has an invalid M-1 snapshot");
    }
    if (reference.set0.epoch != 0 || reference.set0.validator_count == 0 ||
        reference.set0.validator_count > modern::MAX_FINALITY_SET ||
        reference.set0.ruleset_version != modern::FINALITY_RULESET_V1 ||
        reference.set0.total_weight == 0 ||
        reference.set0.quorum_weight !=
            modern::QuorumWeightV1(reference.set0.total_weight) ||
        reference.set0.members_root.IsNull()) {
        return fail("signature package contains an invalid Set_0 header");
    }
    const auto set0_pubkey{bls::PublicKey::Decode(reference.set0.aggregate_pubkey)};
    if (!set0_pubkey) return fail("Set_0 aggregate public key is invalid");
    const uint256 set0_hash{modern::ValidatorSetHash(reference.set0)};
    if (reference.snapshot.validator_set_hash != set0_hash) {
        return fail("snapshot validator_set_hash does not commit to Set_0");
    }
    const uint256 digest{modern::FinalityDigest(chain_domain, reference.snapshot)};
    if (reference.digest != digest) return fail("signature package digest does not match chain_domain and snapshot");

    std::array<bool, 4> signed_member{};
    std::vector<bls::Signature> signatures;
    std::vector<bls::VerifiedPublicKey> signer_keys;
    signatures.reserve(3);
    signer_keys.reserve(3);
    for (const auto& package : packages) {
        if (!(package.snapshot == reference.snapshot) ||
            !(package.set0 == reference.set0) || package.digest != digest) {
            return fail("signature packages do not describe one identical message");
        }
        if (package.chain_domain != chain_domain) {
            return fail("signature package chain_domain does not match manifest");
        }
        if (package.binding_height > package.snapshot.height) {
            return fail("signature package binding was not active by the M-1 snapshot");
        }
        size_t member{identities.size()};
        for (size_t i{0}; i < identities.size(); ++i) {
            if (package.validator_key == identities[i].validator_key) {
                member = i;
                break;
            }
        }
        if (member == identities.size() ||
            !(package.bls_pubkey == identities[member].bls_pubkey)) {
            return fail("signature package identity is not in the manifest");
        }
        if (package.binding_seq != identities[member].binding_seq) {
            return fail("signature package binding_seq does not match manifest");
        }
        if (package.binding_height != identities[member].binding_height) {
            return fail("signature package binding_height does not match manifest");
        }
        if (signed_member[member]) return fail("duplicate signature package");
        if (!bls::Verify(package.bls_pubkey,
                         std::span<const unsigned char>{digest.begin(), 32},
                         package.signature)) {
            return fail("invalid bootstrap signature for manifest member " + std::to_string(member));
        }
        signed_member[member] = true;
        signatures.push_back(package.signature);
        signer_keys.push_back(verified[member]);
    }
    const auto aggregate_signature{bls::AggregateSignatures(signatures)};
    if (!aggregate_signature) return fail("aggregate signature is infinity");
    if (!bls::FastAggregateVerify(
            signer_keys, std::span<const unsigned char>{digest.begin(), 32},
            *aggregate_signature)) {
        return fail("aggregate bootstrap signature verification failed");
    }

    BootstrapProofArtifacts out{*aggregate_signature, *full_aggregate};
    out.snapshot = reference.snapshot;
    out.set0 = reference.set0;
    out.signer_bitmap.assign(1, 0);
    size_t absent_index{identities.size()};
    for (size_t i{0}; i < identities.size(); ++i) {
        if (signed_member[i]) {
            out.signer_bitmap[0] |= static_cast<unsigned char>(1U << i);
        } else {
            absent_index = i;
        }
    }
    if (absent_index == identities.size()) return fail("no absent member in 3-of-4 proof");

    uint256 members_root;
    out.absent.index = absent_index;
    out.absent.compressed_pubkey = identities[absent_index].bls_pubkey.Compressed();
    out.absent.uncompressed_pubkey = identities[absent_index].bls_pubkey.Eip2537Uncompressed();
    out.absent.siblings = bootstrap_detail::MerklePath(
        leaves, absent_index, members_root);

    if (members_root != deployment_set->bootstrap_set.members_root) {
        return fail("bootstrap member root changed while building the proof");
    }
    out.bootstrap_set = deployment_set->bootstrap_set;
    out.bootstrap_set_hash = deployment_set->bootstrap_set_hash;

    const auto signature_eip{aggregate_signature->Eip2537Uncompressed()};
    const auto aggregate_eip{full_aggregate->Eip2537Uncompressed()};
    out.proof_abi = bootstrap_detail::EncodeProof(
        out.signer_bitmap, signature_eip, aggregate_eip, out.absent);
    out.initialize_calldata = bootstrap_detail::EncodeInitializeCalldata(
        out.snapshot, out.set0, out.proof_abi);
    return out;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_BOOTSTRAP_PROOF_H

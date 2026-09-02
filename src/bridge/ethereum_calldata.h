// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_ETHEREUM_CALLDATA_H
#define B3COIN_BRIDGE_ETHEREUM_CALLDATA_H

#include <bridge/bootstrap_proof.h>
#include <crypto/bls.h>
#include <modern/finality_certificate.h>
#include <modern/withdrawal_tree.h>
#include <node/validator_set.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bridge {

/** Fully public artifacts needed to call B3FinalityVerifier.submitCertificate. */
struct FinalityRelayArtifacts {
    modern::FinalizedBlock finalized_block{};
    modern::ValidatorSetHeader signing_set{};
    modern::ValidatorSetHeader successor_set{};
    std::vector<unsigned char> signer_bitmap{};
    std::vector<BootstrapAbsentWitness> absent{};
    std::vector<unsigned char> proof_abi{};
    std::vector<unsigned char> submit_calldata{};
    uint64_t signed_weight{0};
    uint32_t signer_count{0};
};

/** Fully public artifacts needed to call B3StakerBridge.release. */
struct WithdrawalRelayArtifacts {
    modern::BridgeWithdrawalV1 withdrawal{};
    uint256 leaf{};
    uint256 root{};
    std::array<uint256, modern::WITHDRAWAL_TREE_DEPTH> path{};
    std::vector<unsigned char> release_calldata{};
};

namespace calldata_detail {

inline std::array<unsigned char, 4> Selector(const std::string_view signature)
{
    uint256 hash;
    Keccak256()
        .Write(std::span<const unsigned char>{
            reinterpret_cast<const unsigned char*>(signature.data()),
            signature.size()})
        .Finalize(hash);
    std::array<unsigned char, 4> out{};
    std::copy_n(hash.begin(), out.size(), out.begin());
    return out;
}

inline std::vector<unsigned char> EncodeFinalityCall(
    const modern::FinalizedBlock& finalized,
    const modern::ValidatorSetHeader& successor,
    std::span<const unsigned char> proof,
    const std::string_view signature)
{
    using namespace bootstrap_detail;
    const auto successor_tail{EncodeSetHeaderTuple(successor)};
    const auto proof_tail{DynamicBytes(proof)};
    // The FinalizedBlock tuple is static (five words). SetHeader is dynamic
    // because it contains bytes, and proof is bytes: seven top-level words.
    constexpr size_t HEAD{7 * 32};
    std::vector<unsigned char> out;
    out.reserve(4 + HEAD + successor_tail.size() + proof_tail.size());
    const auto selector{Selector(signature)};
    Append(out, selector);
    Append(out, Word(finalized.height));
    Append(out, Word(finalized.block_hash));
    Append(out, Word(finalized.withdrawal_root));
    Append(out, Word(finalized.validator_set_hash));
    Append(out, Word(finalized.epoch));
    Append(out, Word(HEAD));
    Append(out, Word(HEAD + successor_tail.size()));
    Append(out, successor_tail);
    Append(out, proof_tail);
    return out;
}

inline std::array<unsigned char, 32> AddressWord(
    const Consensus::BridgeEthAddress& address)
{
    std::array<unsigned char, 32> out{};
    std::copy(address.begin(), address.end(), out.begin() + 12);
    return out;
}

inline uint32_t CountSignerBits(const std::span<const unsigned char> bitmap,
                                const uint32_t validators)
{
    uint32_t count{0};
    for (uint32_t i{0}; i < validators; ++i) {
        if (modern::SignerBit(bitmap, i)) ++count;
    }
    return count;
}

} // namespace calldata_detail

/**
 * Convert one already-valid B3 certificate and its exact signing/successor
 * snapshots to the Solidity verifier ABI. This performs the B3 signature
 * check again and defensively re-enforces the same headcount quorum used by
 * B3 consensus and the immutable Ethereum prover.
 */
inline std::optional<FinalityRelayArtifacts> BuildFinalityRelayArtifacts(
    const uint256& chain_domain,
    const modern::FinalizedBlock& finalized,
    const modern::FinalityCertificate& certificate,
    const node::ValidatorSetSnapshot& signing_set,
    const node::ValidatorSetSnapshot& successor_set,
    const uint32_t max_bridge_validators,
    std::string& error)
{
    auto fail{[&](std::string message)
                  -> std::optional<FinalityRelayArtifacts> {
        error = std::move(message);
        return std::nullopt;
    }};
    if (chain_domain.IsNull()) return fail("chain domain is zero");
    if (signing_set.Epoch() != finalized.epoch) {
        return fail("signing-set epoch does not match certificate");
    }
    if (finalized.epoch == std::numeric_limits<uint64_t>::max() ||
        successor_set.Epoch() != finalized.epoch + 1) {
        return fail("successor-set epoch is not certificate epoch plus one");
    }
    if (finalized.validator_set_hash != successor_set.SetHash()) {
        return fail("certificate does not commit to the supplied successor set");
    }
    if (signing_set.Size() == 0 ||
        signing_set.Size() > max_bridge_validators) {
        return fail("signing set is outside the deployment gas bound");
    }
    const auto check{modern::VerifyFinalityCertificate(
        chain_domain, finalized, certificate, signing_set.View(),
        successor_set.SetHash())};
    if (check != modern::CertificateCheck::OK) {
        return fail(std::string{"B3 certificate verification failed: "} +
                    modern::CertificateCheckName(check));
    }

    FinalityRelayArtifacts out;
    out.finalized_block = finalized;
    out.signing_set = signing_set.Header();
    out.successor_set = successor_set.Header();
    out.signer_bitmap = certificate.signer_bitmap;
    out.signer_count = calldata_detail::CountSignerBits(
        certificate.signer_bitmap,
        static_cast<uint32_t>(signing_set.Size()));
    const uint32_t headcount_quorum{
        static_cast<uint32_t>((signing_set.Size() * 2) / 3 + 1)};
    if (out.signer_count < headcount_quorum) {
        return fail("certificate lacks the two-thirds headcount quorum");
    }

    const auto signature{bls::Signature::Decode(certificate.aggregate_sig)};
    const auto aggregate{
        bls::PublicKey::Decode(signing_set.Header().aggregate_pubkey)};
    if (!signature || !aggregate) {
        return fail("certificate signature or set aggregate key is not canonical BLS");
    }

    out.absent.reserve(signing_set.Size() - out.signer_count);
    for (uint32_t i{0}; i < signing_set.Size(); ++i) {
        const node::ValidatorSetMember& member{signing_set.Members()[i]};
        if (modern::SignerBit(certificate.signer_bitmap, i)) {
            out.signed_weight += member.weight;
            continue;
        }
        const auto pubkey{bls::PublicKey::Decode(member.bls_pubkey)};
        if (!pubkey) return fail("validator set contains a non-canonical BLS key");
        BootstrapAbsentWitness witness;
        witness.index = i;
        witness.compressed_pubkey = member.bls_pubkey;
        witness.weight = member.weight;
        witness.uncompressed_pubkey = pubkey->Eip2537Uncompressed();
        uint256 root;
        witness.siblings = bootstrap_detail::MerklePath(
            signing_set.Leaves(), i, root);
        if (root != signing_set.Header().members_root) {
            return fail("validator membership path does not reproduce set root");
        }
        out.absent.push_back(std::move(witness));
    }
    if (out.signed_weight < signing_set.QuorumWeight()) {
        return fail("certificate signed weight is below quorum");
    }

    const auto signature_eip{signature->Eip2537Uncompressed()};
    const auto aggregate_eip{aggregate->Eip2537Uncompressed()};
    out.proof_abi = bootstrap_detail::EncodeProof(
        out.signer_bitmap, signature_eip, aggregate_eip, out.absent);
    static constexpr std::string_view SIGNATURE{
        "submitCertificate((uint64,bytes32,bytes32,bytes32,uint64),(uint64,uint16,uint32,uint64,uint64,bytes,bytes32),bytes)"};
    out.submit_calldata = calldata_detail::EncodeFinalityCall(
        finalized, successor_set.Header(), out.proof_abi, SIGNATURE);
    error.clear();
    return out;
}

/**
 * Construct the ordered fixed-depth path for one withdrawal in a cumulative
 * prefix and encode B3StakerBridge.release. `withdrawals` must be the exact
 * consecutive prefix committed by `expected_root`.
 */
inline std::optional<WithdrawalRelayArtifacts> BuildWithdrawalRelayArtifacts(
    const std::span<const modern::BridgeWithdrawalV1> withdrawals,
    const uint64_t withdrawal_id,
    const uint256& expected_root,
    std::string& error)
{
    auto fail{[&](std::string message)
                  -> std::optional<WithdrawalRelayArtifacts> {
        error = std::move(message);
        return std::nullopt;
    }};
    if (withdrawal_id >= withdrawals.size()) {
        return fail("withdrawal is not present in the finalized prefix");
    }
    if (withdrawals.size() > modern::MAX_WITHDRAWAL_LEAVES) {
        return fail("withdrawal prefix exceeds the depth-32 tree capacity");
    }
    std::vector<uint256> level;
    level.reserve(withdrawals.size());
    for (size_t i{0}; i < withdrawals.size(); ++i) {
        if (withdrawals[i].withdrawal_id != i) {
            return fail("withdrawal prefix is not consecutive");
        }
        const auto leaf{modern::BridgeWithdrawalLeafV1(withdrawals[i])};
        if (!leaf) return fail("withdrawal prefix contains an invalid leaf");
        level.push_back(*leaf);
    }

    WithdrawalRelayArtifacts out;
    out.withdrawal = withdrawals[withdrawal_id];
    out.leaf = level[withdrawal_id];
    out.root = expected_root;
    size_t cursor{static_cast<size_t>(withdrawal_id)};
    const auto& zeros{modern::WithdrawalZeroHashes()};
    for (unsigned depth{0}; depth < modern::WITHDRAWAL_TREE_DEPTH; ++depth) {
        const size_t sibling{cursor ^ 1U};
        out.path[depth] = sibling < level.size() ? level[sibling]
                                                : zeros[depth];
        std::vector<uint256> next;
        next.reserve((level.size() + 1) / 2);
        for (size_t i{0}; i < level.size(); i += 2) {
            const uint256& right{i + 1 < level.size() ? level[i + 1]
                                                      : zeros[depth]};
            next.push_back(modern::WithdrawalNodeHash(level[i], right));
        }
        level = std::move(next);
        cursor >>= 1;
    }
    if (level.size() != 1 || level.front() != expected_root) {
        return fail("withdrawal prefix does not reproduce the finalized root");
    }

    using namespace bootstrap_detail;
    static constexpr std::string_view SIGNATURE{
        "release((uint64,address,uint256,uint64),bytes32[32])"};
    const auto selector{calldata_detail::Selector(SIGNATURE)};
    out.release_calldata.reserve(4 + (4 + modern::WITHDRAWAL_TREE_DEPTH) * 32);
    Append(out.release_calldata, selector);
    Append(out.release_calldata, Word(out.withdrawal.withdrawal_id));
    Append(out.release_calldata,
           calldata_detail::AddressWord(out.withdrawal.recipient));
    Append(out.release_calldata,
           Word(static_cast<uint64_t>(out.withdrawal.amount)));
    Append(out.release_calldata, Word(out.withdrawal.b3_height));
    for (const uint256& sibling : out.path) {
        Append(out.release_calldata, Word(sibling));
    }
    error.clear();
    return out;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_ETHEREUM_CALLDATA_H

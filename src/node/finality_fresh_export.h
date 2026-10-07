// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_FRESH_EXPORT_H
#define B3COIN_NODE_FINALITY_FRESH_EXPORT_H

#include <bridge/bootstrap_proof.h>
#include <bridge/ethereum_calldata.h>
#include <crypto/bls.h>
#include <modern/finality_certificate.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <node/finality_fresh_lineage.h>
#include <node/validator_set.h>
#include <uint256.h>

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

namespace node {

/**
 * Fresh finality lineage: export of one included fresh-lineage certificate
 * as B3FinalityVerifierV2 calldata (bridge V2 stage S3; the call and its
 * argument order are b3-bridge-v2-contracts-spec.md section 3.3, the bytes
 * section 2; the Solidity declaration is
 * contracts/src/v2/IB3FinalityVerifierV2.sol submitCertificate).
 *
 *   submitCertificate(FinalizedBlockV2 fb, SetHeader signingSet,
 *                     SetHeader successor, bytes32[32] ancestryPath,
 *                     bytes proof)
 *
 * ABI layout after the 4-byte selector: a 44-word head (the 9 static fb
 * words, the offset of signingSet, the offset of successor, the 32 static
 * ancestryPath words and the offset of proof), then the two SetHeader tuples
 * (bridge::bootstrap_detail::EncodeSetHeaderTuple, ruleset 2) and the proof
 * bytes. The proof is the unchanged V1 witness ABI
 * (bitmap, EIP-2537 signature, EIP-2537 aggregate key, absent witnesses) that
 * BlsDigestProver.verifyDigest decodes (contracts spec section 3.6).
 *
 * ancestryPath places historyLeaf(h_prev, hash_prev) at `prev_index` under
 * fb.checkpointHistoryRoot, where (prev_index, h_prev) is the verifier's
 * latest accepted checkpoint (index 0 = GENESIS_HEIGHT before any
 * certificate, which is also the K_OF_N bootstrap's leaf0Path). Both paths
 * are exported so a relayer can serve either verifier state.
 *
 * Pure and header-only (like bridge/ethereum_calldata.h): it re-verifies the
 * certificate, holds no key and touches no chain state.
 */

//! The exact Solidity signature whose keccak selects
//! B3FinalityVerifierV2.submitCertificate (cast sig: 0x6263c1cb).
inline constexpr std::string_view FRESH_SUBMIT_CERTIFICATE_SIGNATURE{
    "submitCertificate((bytes32,uint64,bytes32,bytes32,bytes32,uint64,uint64,bytes32,uint8),"
    "(uint64,uint16,uint32,uint64,uint64,bytes,bytes32),(uint64,uint16,uint32,uint64,uint64,bytes,bytes32),"
    "bytes32[32],bytes)"};
//! Head words of submitCertificate: 9 fb words, two set offsets, 32 path words, the proof offset.
inline constexpr size_t FRESH_SUBMIT_FB_WORDS{9};
inline constexpr size_t FRESH_SUBMIT_HEAD_WORDS{FRESH_SUBMIT_FB_WORDS + 2 + modern::FINALITY_HISTORY_DEPTH + 1};
static_assert(FRESH_SUBMIT_HEAD_WORDS == 44);

/** Fully public artifacts needed to call B3FinalityVerifierV2.submitCertificate. */
struct FreshLineageRelayArtifacts {
    modern::FinalizedBlockV2 fb{};
    modern::ValidatorSetHeader signing_header{};   // ruleset 2, epoch fb.epoch
    modern::ValidatorSetHeader successor_header{}; // ruleset 2, epoch fb.epoch + 1
    uint256 signing_set_hash{};
    uint256 successor_set_hash{};
    std::vector<unsigned char> bitmap{};
    std::vector<bridge::BootstrapAbsentWitness> absent{};
    uint32_t signer_count{0};
    uint64_t signed_weight{0};
    std::vector<unsigned char> proof_abi{};
    //! The verifier-latest leaf the ancestry path places, and where.
    uint64_t prev_index{0};
    uint256 prev_leaf{};
    modern::FinalityHistoryPathV2 ancestry_path{};
    //! historyLeaf(GENESIS_HEIGHT, its hash) at index 0 (leaf0Path).
    uint256 leaf0{};
    modern::FinalityHistoryPathV2 leaf0_path{};
    std::vector<unsigned char> submit_calldata{};
};

namespace fresh_export_detail {

//! submitCertificate calldata: selector || 44-word head || signingSet || successor || proof.
inline std::vector<unsigned char> EncodeSubmitCertificateV2(const modern::FinalizedBlockV2& fb,
                                                            const modern::ValidatorSetHeader& signing,
                                                            const modern::ValidatorSetHeader& successor,
                                                            const modern::FinalityHistoryPathV2& ancestry_path,
                                                            std::span<const unsigned char> proof)
{
    using namespace bridge::bootstrap_detail;
    const auto signing_tail{EncodeSetHeaderTuple(signing)};
    const auto successor_tail{EncodeSetHeaderTuple(successor)};
    const auto proof_tail{DynamicBytes(proof)};
    constexpr size_t HEAD{FRESH_SUBMIT_HEAD_WORDS * 32};
    std::vector<unsigned char> out;
    out.reserve(4 + HEAD + signing_tail.size() + successor_tail.size() + proof_tail.size());
    Append(out, bridge::calldata_detail::Selector(FRESH_SUBMIT_CERTIFICATE_SIGNATURE));
    // FinalizedBlockV2 is a static tuple: its nine words sit in the head.
    Append(out, Word(fb.lineage_id));
    Append(out, Word(fb.height));
    Append(out, Word(fb.block_hash));
    Append(out, Word(fb.withdrawal_root));
    Append(out, Word(fb.validator_set_hash));
    Append(out, Word(fb.epoch));
    Append(out, Word(fb.checkpoint_index));
    Append(out, Word(fb.checkpoint_history_root));
    Append(out, Word(static_cast<uint64_t>(fb.bridge_flags)));
    Append(out, Word(static_cast<uint64_t>(HEAD)));
    Append(out, Word(static_cast<uint64_t>(HEAD + signing_tail.size())));
    // bytes32[32] is static: the 32 siblings sit in the head.
    for (const uint256& sibling : ancestry_path) Append(out, Word(sibling));
    Append(out, Word(static_cast<uint64_t>(HEAD + signing_tail.size() + successor_tail.size())));
    Append(out, signing_tail);
    Append(out, successor_tail);
    Append(out, proof_tail);
    return out;
}

} // namespace fresh_export_detail

/**
 * Convert one already-included fresh-lineage certificate, its exact signing
 * and successor committees and the history leaves 0..k-1 (k =
 * fb.checkpoint_index; leaf i = historyLeaf(G + i * interval, its hash)) to
 * B3FinalityVerifierV2.submitCertificate calldata whose ancestry path places
 * leaf `prev_index` (< k). Re-verifies the certificate with
 * modern::VerifyFinalityCertificateV2, defensively re-checks both quorums,
 * rebuilds the absent-member witnesses against the members root and checks
 * that both history paths fold to fb.checkpointHistoryRoot. nullopt with an
 * error otherwise.
 */
inline std::optional<FreshLineageRelayArtifacts> BuildFreshLineageRelayArtifacts(
    const uint256& chain_domain, const modern::FinalizedBlockV2& fb, const modern::FinalityCertificate& cert,
    const FreshLineageSet& signing, const FreshLineageSet& successor, std::span<const uint256> leaves,
    const uint64_t prev_index, std::string& error)
{
    auto fail{[&](std::string message) -> std::optional<FreshLineageRelayArtifacts> {
        error = std::move(message);
        return std::nullopt;
    }};
    if (chain_domain.IsNull()) return fail("chain domain is zero");
    if (!signing.members || !successor.members || signing.Size() == 0) {
        return fail("signing or successor committee is unavailable");
    }
    if (signing.header.ruleset_version != modern::FINALITY_RULESET_V2 ||
        successor.header.ruleset_version != modern::FINALITY_RULESET_V2) {
        return fail("committee header is not ruleset 2");
    }
    if (signing.Epoch() != fb.epoch) return fail("signing-committee epoch does not match certificate");
    if (fb.epoch == std::numeric_limits<uint64_t>::max() || successor.Epoch() != fb.epoch + 1) {
        return fail("successor-committee epoch is not certificate epoch plus one");
    }
    if (fb.validator_set_hash != successor.set_hash) {
        return fail("certificate does not commit to the supplied successor committee");
    }
    if (fb.checkpoint_index == 0 || fb.checkpoint_index >= modern::MAX_FINALITY_CHECKPOINT_INDEX) {
        return fail("certificate checkpoint index is outside 1..2^32-1");
    }
    if (leaves.size() != fb.checkpoint_index) {
        return fail("history leaves must be exactly leaves 0..checkpoint_index-1");
    }
    if (prev_index >= fb.checkpoint_index) {
        return fail("prev_index must be below the certificate's checkpoint index");
    }
    const auto check{
        modern::VerifyFinalityCertificateV2(chain_domain, fb, cert, signing.View(), successor.set_hash)};
    if (check != modern::CertificateCheck::OK) {
        return fail(std::string{"fresh-lineage certificate verification failed: "} +
                    modern::CertificateCheckName(check));
    }

    FreshLineageRelayArtifacts out;
    out.fb = fb;
    out.signing_header = signing.header;
    out.successor_header = successor.header;
    out.signing_set_hash = signing.set_hash;
    out.successor_set_hash = successor.set_hash;
    out.bitmap = cert.signer_bitmap;
    const auto n{static_cast<uint32_t>(signing.Size())};
    out.signer_count = bridge::calldata_detail::CountSignerBits(cert.signer_bitmap, n);
    if (out.signer_count < modern::FinalityHeadcountQuorum(n)) {
        return fail("certificate lacks the two-thirds headcount quorum");
    }
    const auto signature{bls::Signature::Decode(cert.aggregate_sig)};
    const auto aggregate{bls::PublicKey::Decode(signing.header.aggregate_pubkey)};
    if (!signature || !aggregate) return fail("certificate signature or committee aggregate key is not canonical BLS");

    const ValidatorSetSnapshot& members{*signing.members};
    out.absent.reserve(n - out.signer_count);
    for (uint32_t i{0}; i < n; ++i) {
        const ValidatorSetMember& member{members.Members()[i]};
        if (modern::SignerBit(cert.signer_bitmap, i)) {
            out.signed_weight += member.weight;
            continue;
        }
        const auto pubkey{bls::PublicKey::Decode(member.bls_pubkey)};
        if (!pubkey) return fail("committee contains a non-canonical BLS key");
        bridge::BootstrapAbsentWitness witness;
        witness.index = i;
        witness.compressed_pubkey = member.bls_pubkey;
        witness.weight = member.weight;
        witness.uncompressed_pubkey = pubkey->Eip2537Uncompressed();
        uint256 root;
        witness.siblings = bridge::bootstrap_detail::MerklePath(members.Leaves(), i, root);
        if (root != signing.header.members_root) {
            return fail("committee membership path does not reproduce the members root");
        }
        out.absent.push_back(std::move(witness));
    }
    if (out.signed_weight < signing.header.quorum_weight) return fail("certificate signed weight is below quorum");

    // History paths: the verifier's latest leaf and the genesis leaf (leaf0Path).
    uint256 root;
    const auto ancestry{modern::FinalityHistoryPath(leaves, prev_index, root)};
    if (!ancestry || root != fb.checkpoint_history_root) {
        return fail("history leaves do not reproduce the certificate's checkpoint history root");
    }
    const auto leaf0{modern::FinalityHistoryPath(leaves, 0, root)};
    if (!leaf0 || root != fb.checkpoint_history_root) {
        return fail("history leaves do not reproduce the certificate's checkpoint history root");
    }
    out.prev_index = prev_index;
    out.prev_leaf = leaves[prev_index];
    out.ancestry_path = *ancestry;
    out.leaf0 = leaves[0];
    out.leaf0_path = *leaf0;
    if (modern::FoldFinalityHistoryPath(out.prev_leaf, prev_index, out.ancestry_path) != fb.checkpoint_history_root ||
        modern::FoldFinalityHistoryPath(out.leaf0, 0, out.leaf0_path) != fb.checkpoint_history_root) {
        return fail("history path does not fold to the checkpoint history root");
    }

    out.proof_abi = bridge::bootstrap_detail::EncodeProof(out.bitmap, signature->Eip2537Uncompressed(),
                                                          aggregate->Eip2537Uncompressed(), out.absent);
    out.submit_calldata = fresh_export_detail::EncodeSubmitCertificateV2(fb, out.signing_header,
                                                                         out.successor_header,
                                                                         out.ancestry_path, out.proof_abi);
    error.clear();
    return out;
}

} // namespace node

#endif // B3COIN_NODE_FINALITY_FRESH_EXPORT_H

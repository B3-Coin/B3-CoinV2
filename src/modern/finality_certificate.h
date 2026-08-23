// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_CERTIFICATE_H
#define B3COIN_MODERN_FINALITY_CERTIFICATE_H

#include <crypto/bls.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace modern {

/**
 * FINALITY_CERTIFICATE validation (plan Commit 10) over the frozen format
 * (b3-cross-chain-finality-v1.md sections 3, 3.1, 4):
 *
 *   payload  = FinalizedBlock(112) || signer_bitmap(ceil(n/8)) || aggregate_sig(96)
 *   digest   = TaggedHash("B3/FINALITY/V1", ModernChainDomain || FinalizedBlock)
 *   cell     = FINALITY_CERT (policy 6, metadata, coinbase-only, <= 1 per block):
 *              commitment = TaggedHash("B3/FINALITY/CERT/V1", payload),
 *              params     = epoch u64 || height u64  (== FinalizedBlock.epoch/.height)
 *   record   = MPA type 4, version 1, payload as above (<= 1,232 bytes)
 *
 * Semantic verification (VerifyFinalityCertificate) against the signing set
 * Set_epoch (a ValidatorSetView: n, per-index PoP-verified BLS keys and
 * weights, quorum weight) and the expected successor set hash:
 *   - bitmap well-formed for exactly n (width, zero high bits);
 *   - FinalizedBlock.validator_set_hash == hash(Set_{epoch+1}) (wrong set);
 *   - signed weight = sum of weights of set bits >= quorum_weight;
 *   - FastAggregateVerify over the signers' keys of the digest under the
 *     chain domain (wrong domain / wrong signers / bad signature all fail);
 *   - an empty signer set, an infinity aggregate, a duplicate "signer" (a bit
 *     can be set only once -- a bitmap has no duplicates by construction; bits
 *     beyond n are the malformed case) all fail.
 *
 * Structural binding (MatchFinalityCertificate): exactly one FINALITY_CERT
 * cell <-> one type-4 record in the coinbase, matched by commitment; orphan
 * record, cell without record, duplicates, non-coinbase placement, params
 * mismatch are invalid. NOT wired into block validation here: checkpoint
 * scheduling, epoch rotation, the finality pin, P2P aggregation and activation
 * are later commits (the registry keeps type 4 INACTIVE), so production stays
 * fail-closed and nothing here runs on a real chain yet.
 */

//! The verifier's view of the signing validator set (built from a snapshot).
struct ValidatorSetView {
    uint32_t validator_count{0};
    uint64_t quorum_weight{0};
    std::vector<bls::VerifiedPublicKey> keys; // index i = member i (PoP-verified at binding time)
    std::vector<uint64_t> weights;            // index i = member i
};

enum class CertificateCheck {
    OK,
    MALFORMED_BITMAP,        //!< width != ceil(n/8) or bits >= n set
    WRONG_SUCCESSOR_SET,     //!< FinalizedBlock.validator_set_hash != expected hash(Set_{epoch+1})
    NO_SIGNERS,              //!< empty bitmap
    INSUFFICIENT_WEIGHT,     //!< signed weight < quorum
    BAD_SIGNATURE,           //!< aggregate signature does not verify (wrong domain, wrong signers, tampered)
    SET_VIEW_INCONSISTENT,   //!< keys/weights sizes != n (programming error, fail closed)
};

inline const char* CertificateCheckName(const CertificateCheck c)
{
    switch (c) {
    case CertificateCheck::OK: return "ok";
    case CertificateCheck::MALFORMED_BITMAP: return "malformed-bitmap";
    case CertificateCheck::WRONG_SUCCESSOR_SET: return "wrong-successor-set";
    case CertificateCheck::NO_SIGNERS: return "no-signers";
    case CertificateCheck::INSUFFICIENT_WEIGHT: return "insufficient-weight";
    case CertificateCheck::BAD_SIGNATURE: return "bad-signature";
    case CertificateCheck::SET_VIEW_INCONSISTENT: return "set-view-inconsistent";
    }
    return "unknown";
}

//! Sum of weights of the set bits (caller has checked the bitmap shape).
inline uint64_t SignedWeight(std::span<const unsigned char> bitmap, const ValidatorSetView& set)
{
    uint64_t w{0};
    for (uint32_t i = 0; i < set.validator_count; ++i) {
        if (SignerBit(bitmap, i)) w += set.weights[i];
    }
    return w;
}

/**
 * Verify one certificate against its signing set. Cheap checks first; the
 * aggregate BLS verification last.
 */
inline CertificateCheck VerifyFinalityCertificate(const uint256& chain_domain, const FinalizedBlock& fb,
                                                  const FinalityCertificate& cert, const ValidatorSetView& set,
                                                  const uint256& expected_successor_set_hash)
{
    if (set.keys.size() != set.validator_count || set.weights.size() != set.validator_count || set.validator_count == 0) {
        return CertificateCheck::SET_VIEW_INCONSISTENT;
    }
    if (!IsWellFormedSignerBitmap(cert.signer_bitmap, set.validator_count)) return CertificateCheck::MALFORMED_BITMAP;
    if (fb.validator_set_hash != expected_successor_set_hash) return CertificateCheck::WRONG_SUCCESSOR_SET;
    std::vector<bls::VerifiedPublicKey> signers;
    uint64_t signed_weight{0};
    for (uint32_t i = 0; i < set.validator_count; ++i) {
        if (!SignerBit(cert.signer_bitmap, i)) continue;
        signers.push_back(set.keys[i]);
        signed_weight += set.weights[i];
    }
    if (signers.empty()) return CertificateCheck::NO_SIGNERS;
    if (signed_weight < set.quorum_weight) return CertificateCheck::INSUFFICIENT_WEIGHT;
    const auto sig{bls::Signature::Decode(cert.aggregate_sig)};
    if (!sig) return CertificateCheck::BAD_SIGNATURE;
    const uint256 digest{FinalityDigest(chain_domain, fb)};
    if (!bls::FastAggregateVerify(signers, std::span<const unsigned char>(digest.begin(), 32), *sig)) {
        return CertificateCheck::BAD_SIGNATURE;
    }
    return CertificateCheck::OK;
}

//! One matched FINALITY_CERT cell + certificate record (structural).
struct FinalityCertificatePair {
    size_t cell_index{0};
    size_t record_index{0};
    FinalizedBlock finalized_block{};
    FinalityCertificate certificate{};
    std::vector<unsigned char> payload{};
};

/**
 * Structural binding inside the coinbase: exactly one FINALITY_CERT cell <->
 * one type-4 record, commitment == TaggedHash("B3/FINALITY/CERT/V1", payload),
 * params == (epoch, height) of the decoded FinalizedBlock; payload decoded for
 * the set size `n` of the signing set (bitmap width). Returns nullopt with no
 * error when the coinbase carries neither; false on any violation. Cells or
 * type-4 records anywhere else in the block are invalid (checked by caller
 * over all transactions).
 */
inline bool MatchFinalityCertificate(const CTransaction& coinbase, const uint32_t n,
                                     std::optional<FinalityCertificatePair>& out, std::string& error)
{
    out.reset();
    std::vector<std::pair<size_t, MetadataCell>> cells;
    for (size_t i = 0; i < coinbase.vout.size(); ++i) {
        const auto cell{ParseMetadataCell(coinbase.vout[i].scriptPubKey)};
        if (cell && cell->policy_type == static_cast<uint16_t>(PolicyType::FINALITY_CERT)) cells.emplace_back(i, *cell);
    }
    std::vector<size_t> records;
    for (size_t i = 0; i < coinbase.mpa.size(); ++i) {
        if (coinbase.mpa[i].payload_type == MPA_TYPE_FINALITY_CERTIFICATE && coinbase.mpa[i].payload_version == MPA_VERSION_V1) {
            records.push_back(i);
        }
    }
    if (cells.empty() && records.empty()) return true;
    if (cells.size() > 1) { error = "finality-cert-duplicate-cell"; return false; }
    if (records.size() > 1) { error = "finality-cert-duplicate-record"; return false; }
    if (cells.empty()) { error = "finality-cert-orphan-record"; return false; }
    if (records.empty()) { error = "finality-cert-cell-without-record"; return false; }
    const auto& [cell_index, cell] = cells[0];
    if (cell.policy_version != POLICY_VERSION_V1) { error = "finality-cert-cell-version"; return false; }
    const auto params{FinalityCertParams::Decode(cell.params)};
    if (!params) { error = "finality-cert-cell-params"; return false; }
    const auto& payload{coinbase.mpa[records[0]].payload};
    if (FinalityCertCommitment(payload) != cell.commitment) { error = "finality-cert-commitment-mismatch"; return false; }
    const auto decoded{DecodeCertificatePayload(payload, n)};
    if (!decoded) { error = "finality-cert-malformed-payload"; return false; }
    if (decoded->first.epoch != params->epoch || decoded->first.height != params->height) {
        error = "finality-cert-params-mismatch";
        return false;
    }
    FinalityCertificatePair pair;
    pair.cell_index = cell_index;
    pair.record_index = records[0];
    pair.finalized_block = decoded->first;
    pair.certificate = decoded->second;
    pair.payload = payload;
    out = std::move(pair);
    return true;
}

//! Block-level structural rule: FINALITY_CERT cells and type-4 records may
//! exist only in the coinbase (and at most one pair there).
inline bool CheckFinalityCertificatePlacement(const CBlock& block, std::string& error)
{
    for (size_t t = 1; t < block.vtx.size(); ++t) {
        const CTransaction& tx{*block.vtx[t]};
        for (const auto& out : tx.vout) {
            const auto cell{ParseMetadataCell(out.scriptPubKey)};
            if (cell && cell->policy_type == static_cast<uint16_t>(PolicyType::FINALITY_CERT)) {
                error = "finality-cert-not-in-coinbase";
                return false;
            }
        }
        for (const auto& rec : tx.mpa) {
            if (rec.payload_type == MPA_TYPE_FINALITY_CERTIFICATE) {
                error = "finality-cert-record-not-in-coinbase";
                return false;
            }
        }
    }
    return true;
}

//! Build a certificate payload + cell for a set (used by tests and, later, the aggregator/miner).
inline std::pair<std::vector<unsigned char>, CScript> BuildFinalityCertificate(const FinalizedBlock& fb,
                                                                              const FinalityCertificate& cert)
{
    const auto payload{EncodeCertificatePayload(fb, cert)};
    const FinalityCertParams params{.epoch = fb.epoch, .height = fb.height};
    const auto cell{MakeMetadataCellScript(static_cast<uint16_t>(PolicyType::FINALITY_CERT), POLICY_VERSION_V1,
                                           FinalityCertCommitment(payload), params.Encode())};
    return {payload, *cell};
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_CERTIFICATE_H

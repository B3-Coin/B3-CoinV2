// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_LEGACY_FN_ISSUANCE_H
#define B3COIN_MODERN_LEGACY_FN_ISSUANCE_H

#include <consensus/amount.h>
#include <consensus/boundary.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <legacy/consensus.h>
#include <modern/creation_action.h>
#include <modern/fn.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <tinyformat.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace modern {

/**
 * Legacy FN issuance (owner ruling 2026-08-17;
 * doc/design/b3-legacy-fn-issuance-proposal.md — supersedes the
 * signature-based FnClaimActionV1 claim flow for legacy PoDs):
 *
 * Historical disintegrations stay plain confirmed legacy transactions.
 * ONE archival builder — any party holding complete legacy history; it
 * has no special authority — constructs a canonical PROOF-CARRYING FN
 * issuance per qualifying PoD, holds it privately before the modern
 * activation height and broadcasts it there, "as if someone in the
 * modern era just issued FN Coin". Every node verifies the embedded
 * proof STATELESSLY against the H/X-sealed chain: no legacy rescan, no
 * derived database, no funding-key signatures, no user claim process.
 * Duplicate and competing broadcasts are deduplicated BY PoDID — the
 * one-issuance-per-PoDId rule (future modern consensus state) is the
 * dedup authority. Honest builders additionally produce byte-identical
 * transactions (a practical determinism property the tests pin), but
 * byte identity is NOT relied upon for uniqueness.
 *
 * Trust base of the verifier: the pinned H/X boundary plus the node's
 * own block index, which permanently stores every block's committed
 * hashMerkleRoot even when block data is pruned — consensus state every
 * node already has. A merkle path from a legacy transaction to the
 * committed root of a block at height <= H on the X-anchored chain IS
 * membership in the sealed ledger. Validity of the disintegration
 * itself (inputs existed and were unspent, signatures) is inherited
 * from that ledger: the embedded evidence only reveals committed bytes.
 *
 * NOTHING here is wired into consensus, mempool, wallet or RPC: this
 * header defines the proof format, the eligibility/recipient rules, the
 * stateless verifier and the pure builder — activation-inert libraries.
 */

// ---- Eligibility and the canonical recipient ---------------------------

//! The historical FN identity marker value: exactly 1 old-B3. The final
//! legacy client registered FN identities only from outputs of exactly
//! this value (master fn-activity.cpp SelectCoinsFundamentalnode) in
//! byte-exact P2PKH form paying the registration key (signhelper_mn.h
//! IsVinAssociatedWithPubkey), inside the disintegration transaction
//! itself (main.cpp AcceptableFundamentalTxn).
inline constexpr CAmount LEGACY_FN_MARKER_VALUE{1'000'000};

//! Byte-exact P2PKH form — the only script form the historical client
//! accepted as an FN identity output. Frozen matcher, never a policy.
inline bool IsLegacyFnP2pkh(const CScript& script)
{
    return script.size() == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 &&
           script[2] == 0x14 && script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG;
}

/**
 * The canonical designated recipient of a disintegration: the
 * lowest-index output of exactly LEGACY_FN_MARKER_VALUE in byte-exact
 * P2PKH form — the operator's own historical designation, read from the
 * sealed transaction bytes (later spending of that output changes
 * nothing). Returns std::nullopt when no such output exists: that
 * disintegration is IGNORED — no FN issuance exists for it, ever, and
 * no fallback recipient is derived (owner ruling 2026-08-17; such a
 * transaction could never have registered an FN historically either).
 */
inline std::optional<uint32_t> FindLegacyFnRecipientVout(const CTransaction& tx)
{
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        if (tx.vout[n].nValue == LEGACY_FN_MARKER_VALUE && IsLegacyFnP2pkh(tx.vout[n].scriptPubKey)) {
            return n;
        }
    }
    return std::nullopt;
}

/**
 * The deterministic legacy-beneficiary ownership commitment: the modern
 * ownership-policy commitment representing the historical 1-B3 P2PKH
 * beneficiary WITHOUT the pubkey being known at issuance.
 *
 * Mechanism — the repository's existing approved script-hash ownership
 * form (the same construction LEGACY_LOCK v1 pins as its enforceable
 * spending condition, and STAKE uses as its owner binding): the
 * commitment is SHA256 of the canonical 25-byte P2PKH script, which
 * embeds only HASH160(pubkey). At spend time the owner reveals the
 * script (the SHA256 preimage) plus pubkey and signature, and P2PKH
 * semantics enforce HASH160(pubkey) == the historical beneficiary hash
 * — script execution itself is deferred exactly as LEGACY_LOCK v1
 * defers it (modern/proof.h) until the modern sighash is defined. No
 * pubkey recovery and no HASH160→key conversion exists or is needed.
 */
inline uint256 LegacyFnRecipientCommitment(const CScript& recipient_script)
{
    return LegacyLockCommitment(recipient_script);
}

// ---- The proof format ---------------------------------------------------

//! Proof format version.
inline constexpr uint8_t LEGACY_FN_ISSUANCE_PROOF_VERSION_V1{1};
//! Hard decode bounds: a historical legacy transaction can never exceed
//! the frozen legacy block bound (master main.h MAX_BLOCK_SIZE =
//! 5,000,000 — an evidence bound over sealed history, NOT the future
//! issuance-carrier limit, which remains OPEN), and a merkle path over
//! <= 2^32 leaves never exceeds 32 hashes.
inline constexpr size_t MAX_LEGACY_FN_EVIDENCE_TX_SIZE{5'000'000};
inline constexpr size_t MAX_LEGACY_FN_MERKLE_PATH{32};

/**
 * One embedded legacy transaction with its inclusion proof: the full
 * legacy serialization (its hash IS the legacy txid), the height of the
 * containing block on the sealed chain, the transaction's position in
 * that block and the merkle path to the block's committed root.
 */
struct LegacyFnMerkleProof {
    std::vector<unsigned char> tx_bytes;
    int32_t height{0};
    uint32_t position{0};
    std::vector<uint256> path;

    friend bool operator==(const LegacyFnMerkleProof& a, const LegacyFnMerkleProof& b)
    {
        return a.tx_bytes == b.tx_bytes && a.height == b.height && a.position == b.position &&
               a.path == b.path;
    }
};

/**
 * The complete issuance proof: the disintegration transaction plus one
 * funding entry per DISTINCT prevout txid of the disintegration, in
 * order of first appearance among its inputs. The funding entries are
 * the smallest PROVABLE input-value evidence (undo data is not
 * committed to by the chain); they also reveal the funding scripts.
 *
 * Wire form (canonical compact sizes, strict full-consumption decode;
 * together with the canonical merkle-position rules this removes every
 * KNOWN proof malleability — but issuance uniqueness is enforced by
 * PoDId, never by proof or transaction byte identity):
 *
 *     u8 version (=1)
 *     merkle_proof(pod)
 *     compactSize(n_funding) ; n_funding × merkle_proof
 *
 *     merkle_proof := compactSize(height) ; compactSize(tx_len) ; tx
 *                     ; compactSize(position)
 *                     ; compactSize(path_len) ; path_len × u256
 */
struct LegacyFnIssuanceProofV1 {
    LegacyFnMerkleProof pod;
    std::vector<LegacyFnMerkleProof> funding;

    friend bool operator==(const LegacyFnIssuanceProofV1& a, const LegacyFnIssuanceProofV1& b)
    {
        return a.pod == b.pod && a.funding == b.funding;
    }
};

/**
 * The shared structural bounds of one merkle-proof entry — the SINGLE
 * source of truth for encoder/decoder symmetry: everything the decoder
 * rejects, the encoder refuses to emit, so a proof this codec produces
 * always decodes. (height and position fit their integer types by
 * construction; the wire-level truncation and canonical-compact-size
 * rules are decode-only concerns.)
 */
inline bool ValidateLegacyFnMerkleProof(const LegacyFnMerkleProof& entry, std::string& error)
{
    if (entry.height <= 0) {
        error = "evidence height out of range";
        return false;
    }
    if (entry.tx_bytes.empty() || entry.tx_bytes.size() > MAX_LEGACY_FN_EVIDENCE_TX_SIZE) {
        error = "evidence transaction length out of range";
        return false;
    }
    if (entry.path.size() > MAX_LEGACY_FN_MERKLE_PATH) {
        error = "merkle path length out of range";
        return false;
    }
    return true;
}

namespace detail {

inline void WriteMerkleProof(std::vector<unsigned char>& out, const LegacyFnMerkleProof& proof)
{
    WriteCompact(out, static_cast<uint64_t>(proof.height));
    WriteCompact(out, proof.tx_bytes.size());
    out.insert(out.end(), proof.tx_bytes.begin(), proof.tx_bytes.end());
    WriteCompact(out, proof.position);
    WriteCompact(out, proof.path.size());
    for (const uint256& hash : proof.path) {
        out.insert(out.end(), hash.begin(), hash.end());
    }
}

inline bool ReadMerkleProof(const std::span<const unsigned char> data, size_t& cursor,
                            LegacyFnMerkleProof& out, std::string& error)
{
    uint64_t height{0};
    if (!ReadCompact(data, cursor, height)) {
        error = "truncated evidence height";
        return false;
    }
    if (height == 0 || height > uint64_t{std::numeric_limits<int32_t>::max()}) {
        error = "evidence height out of range";
        return false;
    }
    uint64_t tx_len{0};
    if (!ReadCompact(data, cursor, tx_len)) {
        error = "truncated evidence transaction length";
        return false;
    }
    if (tx_len == 0 || tx_len > MAX_LEGACY_FN_EVIDENCE_TX_SIZE) {
        error = "evidence transaction length out of range";
        return false;
    }
    if (data.size() - cursor < tx_len) {
        error = "truncated evidence transaction";
        return false;
    }
    out.tx_bytes.assign(data.begin() + cursor, data.begin() + cursor + tx_len);
    cursor += tx_len;
    uint64_t position{0};
    if (!ReadCompact(data, cursor, position)) {
        error = "truncated evidence position";
        return false;
    }
    if (position > std::numeric_limits<uint32_t>::max()) {
        error = "evidence position out of range";
        return false;
    }
    uint64_t path_len{0};
    if (!ReadCompact(data, cursor, path_len)) {
        error = "truncated merkle path length";
        return false;
    }
    if (path_len > MAX_LEGACY_FN_MERKLE_PATH) {
        error = "merkle path length out of range";
        return false;
    }
    if ((data.size() - cursor) / 32 < path_len) {
        error = "truncated merkle path";
        return false;
    }
    out.path.resize(path_len);
    for (uint64_t i{0}; i < path_len; ++i) {
        std::copy(data.begin() + cursor, data.begin() + cursor + 32, out.path[i].begin());
        cursor += 32;
    }
    out.height = static_cast<int32_t>(height);
    out.position = static_cast<uint32_t>(position);
    // Symmetry by construction: the shared structural validator (below)
    // is the final word on every decoded entry, exactly as it is on
    // every encoded one.
    return ValidateLegacyFnMerkleProof(out, error);
}

} // namespace detail

//! Serialize an issuance proof. Returns std::nullopt (with `error` set)
//! for any structurally invalid proof — the encoder never emits bytes
//! its own decoder would reject.
inline std::optional<std::vector<unsigned char>> EncodeLegacyFnIssuanceProof(
    const LegacyFnIssuanceProofV1& proof, std::string& error)
{
    error.clear();
    if (!ValidateLegacyFnMerkleProof(proof.pod, error)) return std::nullopt;
    for (const LegacyFnMerkleProof& entry : proof.funding) {
        if (!ValidateLegacyFnMerkleProof(entry, error)) return std::nullopt;
    }
    std::vector<unsigned char> out;
    out.push_back(LEGACY_FN_ISSUANCE_PROOF_VERSION_V1);
    detail::WriteMerkleProof(out, proof.pod);
    detail::WriteCompact(out, proof.funding.size());
    for (const LegacyFnMerkleProof& entry : proof.funding) {
        detail::WriteMerkleProof(out, entry);
    }
    return out;
}

//! Strict decode: exact version, canonical compact sizes, bounded reads,
//! full consumption. Any violation fails with `out` unspecified.
inline bool DecodeLegacyFnIssuanceProof(const std::span<const unsigned char> data,
                                        LegacyFnIssuanceProofV1& out, std::string& error)
{
    error.clear();
    size_t cursor{0};
    if (data.empty() || data[cursor++] != LEGACY_FN_ISSUANCE_PROOF_VERSION_V1) {
        error = "unknown legacy FN issuance proof version";
        return false;
    }
    if (!detail::ReadMerkleProof(data, cursor, out.pod, error)) return false;
    uint64_t n_funding{0};
    if (!detail::ReadCompact(data, cursor, n_funding)) {
        error = "truncated funding entry count";
        return false;
    }
    // Each funding entry occupies at least 4 bytes of frame; bound the
    // allocation BEFORE reserving.
    if (n_funding > (data.size() - cursor) / 4 + 1) {
        error = "funding entry count exceeds the payload";
        return false;
    }
    out.funding.clear();
    out.funding.reserve(n_funding);
    for (uint64_t i{0}; i < n_funding; ++i) {
        LegacyFnMerkleProof entry;
        if (!detail::ReadMerkleProof(data, cursor, entry, error)) return false;
        out.funding.push_back(std::move(entry));
    }
    if (cursor != data.size()) {
        error = "trailing bytes after the issuance proof";
        return false;
    }
    return true;
}

// ---- Embedded legacy transaction codec ----------------------------------

inline std::vector<unsigned char> EncodeLegacyTx(const CTransaction& tx)
{
    DataStream stream;
    stream << TX_LEGACY_B3(tx);
    const auto bytes{MakeUCharSpan(stream)};
    return {bytes.begin(), bytes.end()};
}

//! Strict full-consumption decode of an embedded legacy transaction.
//! Rejects the 64-byte serialization outright: a 64-byte "transaction"
//! is byte-compatible with an inner merkle node (the classic leaf/inner
//! ambiguity), so it can never serve as evidence.
inline bool DecodeLegacyTx(const std::span<const unsigned char> bytes, CMutableTransaction& out,
                           std::string& error)
{
    if (bytes.size() == 64) {
        error = "64-byte evidence transaction (merkle leaf/inner-node ambiguity)";
        return false;
    }
    try {
        DataStream stream{bytes};
        stream >> TX_LEGACY_B3(out);
        if (!stream.empty()) {
            error = "trailing bytes after evidence transaction";
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string{"undecodable evidence transaction: "} + e.what();
        return false;
    }
    return true;
}

// ---- The stateless verifier ---------------------------------------------

/**
 * The facts a valid proof establishes — everything the issuance consumer
 * (the future height-M validation) needs, all recomputed from proven
 * bytes and consensus parameters, nothing trusted from the proof frame.
 */
struct LegacyFnIssuanceFacts {
    Txid pod_id{};
    int32_t height{0};
    CAmount tier{0};
    CAmount disintegrated{0};
    uint32_t recipient_vout{0};
    CScript recipient_script{};
};

/**
 * Read-only view of the ACTIVE chain, bound by the caller to its own
 * block index (every node holds it, pruned or not). Both lookups must
 * answer from the SAME chain. The verifier trusts the view's merkle
 * roots ONLY after confirming, through block_hash_at, that the view's
 * block at the final legacy height H is exactly the pinned X — a view
 * bound to any other chain (an off-anchor tip before recovery, a
 * different network) verifies nothing.
 */
struct LegacyChainView {
    std::function<std::optional<uint256>(int height)> block_hash_at;
    std::function<std::optional<uint256>(int height)> merkle_root_at;
};

/**
 * Canonical membership fold: ComputeMerkleRootFromPath plus the two
 * rules that make one (position, path) encoding unique for a committed
 * tree. (1) `position` must carry no bits above the path length —
 * folding ignores such bits, so they would be free malleability. (2) At
 * a level whose sibling equals the running hash (the odd-tree
 * self-duplication), the node must sit on the LEFT (bit 0): Hash(h, h)
 * is order-independent, so the right-side encoding would be a second
 * verifying byte sequence for the same fact.
 */
inline bool CanonicalMerkleRootFromPath(const uint256& leaf, const std::vector<uint256>& path,
                                        const uint32_t position, uint256& root,
                                        std::string& error)
{
    if (path.size() < 32 && (position >> path.size()) != 0) {
        error = "merkle position carries bits above the path length";
        return false;
    }
    uint256 hash{leaf};
    uint32_t pos{position};
    for (const uint256& sibling : path) {
        if ((pos & 1) && sibling == hash) {
            error = "merkle position is not canonical at a duplicated level";
            return false;
        }
        hash = (pos & 1) ? Hash(sibling, hash) : Hash(hash, sibling);
        pos >>= 1;
    }
    root = hash;
    return true;
}

/**
 * Verify one issuance proof against the sealed legacy chain. Fails
 * closed with a specific error; on success fills `facts`. Requires the
 * boundary to be pinned (H and X configured) AND the supplied chain
 * view to carry exactly X at H — the view's merkle roots are trusted
 * only after that anchor confirmation.
 */
inline bool VerifyLegacyFnIssuanceProof(const LegacyFnIssuanceProofV1& proof,
                                        const Consensus::Params& params,
                                        const LegacyChainView& view,
                                        LegacyFnIssuanceFacts& facts, std::string& error)
{
    error.clear();
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (!final_height || !params.legacy_final_hash) {
        error = "legacy boundary is not pinned; no sealed ledger to verify against";
        return false;
    }
    // ---- Anchor the view itself before trusting any of its roots.
    const std::optional<uint256> hash_at_h{view.block_hash_at ? view.block_hash_at(*final_height)
                                                              : std::nullopt};
    if (!hash_at_h || *hash_at_h != *params.legacy_final_hash) {
        error = "chain view does not carry X at the final legacy height";
        return false;
    }

    // ---- The disintegration transaction and its membership.
    CMutableTransaction pod_mtx;
    if (!DecodeLegacyTx(proof.pod.tx_bytes, pod_mtx, error)) return false;
    const CTransaction pod_tx{std::move(pod_mtx)};
    if (pod_tx.IsCoinBase() || pod_tx.IsCoinStake()) {
        error = "disintegration evidence is a coinbase or coinstake";
        return false;
    }
    if (proof.pod.height <= 0 || proof.pod.height > *final_height) {
        error = "disintegration height lies outside the sealed legacy prefix";
        return false;
    }
    const auto check_membership{[&](const LegacyFnMerkleProof& entry, const Txid& txid,
                                    const char* what) {
        const std::optional<uint256> root{view.merkle_root_at ? view.merkle_root_at(entry.height)
                                                              : std::nullopt};
        if (!root) {
            error = std::string{what} + " height has no committed merkle root on the active chain";
            return false;
        }
        uint256 folded;
        std::string fold_error;
        if (!CanonicalMerkleRootFromPath(txid.ToUint256(), entry.path, entry.position, folded,
                                         fold_error)) {
            error = std::string{what} + " " + fold_error;
            return false;
        }
        if (folded != *root) {
            error = std::string{what} + " merkle path does not reach the committed block root";
            return false;
        }
        return true;
    }};
    if (!check_membership(proof.pod, pod_tx.GetHash(), "disintegration")) return false;

    // ---- The canonical recipient (or the PoD is ignored entirely).
    const std::optional<uint32_t> recipient_vout{FindLegacyFnRecipientVout(pod_tx)};
    if (!recipient_vout) {
        error = "disintegration carries no 1-coin P2PKH designation; ignored — no FN issuance exists for it";
        return false;
    }

    // ---- Funding evidence: distinct prevout txids, first-appearance order.
    std::vector<Txid> expected;
    for (const CTxIn& in : pod_tx.vin) {
        const Txid& prev{in.prevout.hash};
        bool seen{false};
        for (const Txid& txid : expected) {
            if (txid == prev) {
                seen = true;
                break;
            }
        }
        if (!seen) expected.push_back(prev);
    }
    if (proof.funding.size() != expected.size()) {
        error = "funding evidence count does not match the disintegration's distinct inputs";
        return false;
    }
    std::vector<CTransaction> funding_txs;
    funding_txs.reserve(expected.size());
    for (size_t i{0}; i < expected.size(); ++i) {
        CMutableTransaction funding_mtx;
        if (!DecodeLegacyTx(proof.funding[i].tx_bytes, funding_mtx, error)) return false;
        CTransaction funding_tx{std::move(funding_mtx)};
        if (funding_tx.GetHash() != expected[i]) {
            error = "funding evidence does not match the referenced input transaction";
            return false;
        }
        if (proof.funding[i].height <= 0 || proof.funding[i].height > *final_height) {
            error = "funding evidence height lies outside the sealed legacy prefix";
            return false;
        }
        if (!check_membership(proof.funding[i], funding_tx.GetHash(), "funding")) return false;
        funding_txs.push_back(std::move(funding_tx));
    }

    // ---- The disintegration gap, from proven values only.
    CAmount value_in{0};
    for (const CTxIn& in : pod_tx.vin) {
        const CTransaction* funding{nullptr};
        for (size_t i{0}; i < expected.size(); ++i) {
            if (expected[i] == in.prevout.hash) {
                funding = &funding_txs[i];
                break;
            }
        }
        if (!funding) { // unreachable: every prevout txid is in `expected`
            error = "internal funding evidence lookup failure";
            return false;
        }
        if (in.prevout.n >= funding->vout.size()) {
            error = "disintegration input references a nonexistent funding output";
            return false;
        }
        const CAmount prev_value{funding->vout[in.prevout.n].nValue};
        if (prev_value < 0 || prev_value > MAX_MONEY || prev_value > MAX_MONEY - value_in) {
            error = "funding evidence value out of range";
            return false;
        }
        value_in += prev_value;
    }
    CAmount value_out{0};
    for (const CTxOut& out : pod_tx.vout) {
        if (out.nValue < 0 || out.nValue > MAX_MONEY || out.nValue > MAX_MONEY - value_out) {
            error = "disintegration output value out of range";
            return false;
        }
        value_out += out.nValue;
    }
    if (value_in < value_out) {
        error = "disintegration outputs exceed its proven inputs";
        return false;
    }
    const CAmount gap{value_in - value_out};
    const CAmount tier{legacy::GetFNCollateral(proof.pod.height, params)};
    if (gap < tier) {
        error = "disintegration gap lies below the collateral tier";
        return false;
    }

    facts.pod_id = pod_tx.GetHash();
    facts.height = proof.pod.height;
    facts.tier = tier;
    facts.disintegrated = gap;
    facts.recipient_vout = *recipient_vout;
    facts.recipient_script = pod_tx.vout[*recipient_vout].scriptPubKey;
    return true;
}

// ---- The issuance action (creation-action type 2) -----------------------

/**
 * The one-time archival/historical issuance proof carrier
 * (CREATION_ACTION_LEGACY_FN_ISSUANCE, version 1): the proof plus the
 * index of the FN output it authorizes. Contains no funding-key
 * signatures, no user claim authorization, no administrator
 * authorization — the proof IS the entitlement. Payload wire form:
 *
 *     compactSize(fn_output_index) || issuance-proof bytes (§4 codec)
 *
 * The generic creation-action payload bound applies at encode time; the
 * final issuance-carrier limit is OPEN pending the real-chain proof-size
 * measurement.
 */
struct LegacyFnIssuanceActionV1 {
    uint32_t fn_output_index{0};
    LegacyFnIssuanceProofV1 proof;

    friend bool operator==(const LegacyFnIssuanceActionV1& a, const LegacyFnIssuanceActionV1& b)
    {
        return a.fn_output_index == b.fn_output_index && a.proof == b.proof;
    }
};

//! Serialize into the generic CreationAction frame. Returns std::nullopt
//! when the proof is structurally invalid (the encoder never emits what
//! the decoder rejects) or the payload exceeds the generic bound.
inline std::optional<CreationAction> EncodeLegacyFnIssuanceAction(
    const LegacyFnIssuanceActionV1& action)
{
    std::vector<unsigned char> payload;
    detail::WriteCompact(payload, action.fn_output_index);
    std::string proof_error;
    const std::optional<std::vector<unsigned char>> proof_bytes{
        EncodeLegacyFnIssuanceProof(action.proof, proof_error)};
    if (!proof_bytes) return std::nullopt;
    payload.insert(payload.end(), proof_bytes->begin(), proof_bytes->end());
    if (payload.size() > MAX_CREATION_ACTION_PAYLOAD) return std::nullopt;
    CreationAction out;
    out.action_type = CREATION_ACTION_LEGACY_FN_ISSUANCE;
    out.action_version = LEGACY_FN_ISSUANCE_ACTION_VERSION_V1;
    out.payload = std::move(payload);
    return out;
}

//! Strict decode: exactly (type 2, version 1) — the RESERVED superseded
//! FnClaimActionV1 (type 1) and every other pair are rejected here, so
//! old bytes can never acquire issuance meaning — canonical sizes, full
//! consumption.
inline bool DecodeLegacyFnIssuanceAction(const CreationAction& action,
                                         LegacyFnIssuanceActionV1& out, std::string& error)
{
    if (action.action_type != CREATION_ACTION_LEGACY_FN_ISSUANCE ||
        action.action_version != LEGACY_FN_ISSUANCE_ACTION_VERSION_V1) {
        error = "not a legacy FN issuance action (superseded or unknown action type)";
        return false;
    }
    if (action.payload.size() > MAX_CREATION_ACTION_PAYLOAD) {
        error = "issuance action payload exceeds the bound";
        return false;
    }
    const std::span<const unsigned char> data{action.payload};
    size_t cursor{0};
    uint64_t index{0};
    if (!detail::ReadCompact(data, cursor, index)) {
        error = "truncated fn_output_index";
        return false;
    }
    if (index > std::numeric_limits<uint32_t>::max()) {
        error = "fn_output_index out of range";
        return false;
    }
    if (!DecodeLegacyFnIssuanceProof(data.subspan(cursor), out.proof, error)) return false;
    out.fn_output_index = static_cast<uint32_t>(index);
    return true;
}

// ---- The mint authorization (pure verification result) ------------------

//! What one valid historical issuance authorizes — nothing more: +1 unit
//! of the global FN asset at one output for one PoDId. A pure result;
//! no persistent issued[pod_id] state exists in this commit.
struct LegacyFnMintAuthorization {
    Txid pod_id{};
    uint32_t fn_output_index{0};
    CAmount amount{1};
    uint256 recipient_policy_commitment{};
};

/**
 * Verify one complete inactive historical FN issuance. PRECISELY WHAT
 * THIS CHECKS: (1)-(5) the H/X-sealed historical proof through
 * VerifyLegacyFnIssuanceProof — anchored membership of canonical
 * evidence, historical collateral rules, the designated 1-B3 P2PKH
 * beneficiary, the canonically derived PoDId; (6) the caller-supplied
 * duplicate predicate `already_issued(PoDId)` — the future
 * issued[pod_id] nullifier state stands behind it at activation; (7)
 * `fn_output_index` references exactly one existing output; (8)-(9)
 * that output is exactly {asset = the chain's FN_ASSET_ID (never
 * native), amount = 1, policy FN v1, canonical empty v1 params}; (10)
 * its policy commitment equals the deterministic legacy-beneficiary
 * ownership commitment; (11) the authorized output mints no native B3
 * (it is the FN asset by (8)).
 *
 * DELIBERATELY NOT HERE (activation-phase, the height-M spec):
 * persistent issued[pod_id] / issued-total / live-supply state,
 * transition-level conservation, relay/mempool rules, activation.
 *
 * THE FN ASSET ID IS NEVER CALLER-SUPPLIED: the verifier derives the
 * chain's one FN asset internally — ModernChainDomain(genesis, X)
 * followed by FnAssetId — and fails closed when the domain cannot be
 * derived. No caller can bless an arbitrary asset.
 */
inline bool VerifyLegacyFnIssuanceAction(const LegacyFnIssuanceActionV1& action,
                                         const std::vector<ModernOutput>& outputs,
                                         const Consensus::Params& params,
                                         const LegacyChainView& view,
                                         const std::function<bool(const Txid&)>& already_issued,
                                         LegacyFnMintAuthorization& out, std::string& error)
{
    LegacyFnIssuanceFacts facts;
    if (!VerifyLegacyFnIssuanceProof(action.proof, params, view, facts, error)) return false;
    if (already_issued(facts.pod_id)) {
        error = "an FN unit was already issued for this PoDId";
        return false;
    }
    // Derive the chain's ONE FN asset from consensus parameters; fail
    // closed without a derivable modern chain domain.
    if (!params.legacy_final_hash) { // unreachable: the proof check requires the pin
        error = "legacy boundary is not pinned";
        return false;
    }
    const std::optional<uint256> domain{
        ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)};
    if (!domain) {
        error = "the modern chain domain cannot be derived; no FN asset exists";
        return false;
    }
    const AssetId fn_asset_id{FnAssetId(*domain)};
    if (action.fn_output_index >= outputs.size()) {
        error = "issuance references a nonexistent output";
        return false;
    }
    std::string parse_error;
    const std::optional<FnOutputView> fn_view{
        ParseFnOutput(outputs[action.fn_output_index], fn_asset_id, parse_error)};
    if (!fn_view) {
        error = "issuance references a malformed FN output: " + parse_error;
        return false;
    }
    if (fn_view->amount != 1) {
        error = "one issuance authorizes exactly one FN unit";
        return false;
    }
    if (fn_view->owner_commitment != LegacyFnRecipientCommitment(facts.recipient_script)) {
        error = "FN output is not bound to the historical beneficiary";
        return false;
    }
    out.pod_id = facts.pod_id;
    out.fn_output_index = action.fn_output_index;
    out.amount = 1;
    out.recipient_policy_commitment = fn_view->owner_commitment;
    return true;
}

// ---- The pure builder ---------------------------------------------------

//! Read the block at a height of the sealed chain (builder-side data
//! access — an archival node's block files, or fixture blocks in tests).
using LegacyBlockAt = std::function<bool(int height, CBlock&)>;
//! Locate the (height, position) of a legacy transaction by txid.
using LegacyTxLocator = std::function<std::optional<std::pair<int32_t, uint32_t>>(const Txid&)>;

/**
 * Build the canonical issuance proof for the disintegration at
 * (pod_height, pod_position). Pure: all data access goes through the
 * provided callbacks. The builder SELF-VERIFIES its product through
 * VerifyLegacyFnIssuanceProof (against the supplied anchored chain
 * view) before returning it — it never emits anything a verifier would
 * reject (including ineligible PoDs: gap below tier, or no 1-coin P2PKH
 * designation → "ignored"). Two honest builders produce byte-identical
 * proofs; uniqueness is nevertheless enforced by PoDId, not bytes.
 */
inline bool BuildLegacyFnIssuanceProof(const int32_t pod_height, const uint32_t pod_position,
                                       const Consensus::Params& params,
                                       const LegacyBlockAt& block_at,
                                       const LegacyTxLocator& locate_tx,
                                       const LegacyChainView& view,
                                       LegacyFnIssuanceProofV1& out,
                                       LegacyFnIssuanceFacts& facts, std::string& error)
{
    error.clear();
    const auto build_entry{[&](const int32_t height, const uint32_t position,
                               LegacyFnMerkleProof& entry, CTransactionRef& tx) {
        CBlock block;
        if (!block_at(height, block)) {
            error = strprintf("block data unavailable at height %d", height);
            return false;
        }
        if (position >= block.vtx.size()) {
            error = strprintf("no transaction at position %u of height %d", position, height);
            return false;
        }
        tx = block.vtx[position];
        entry.tx_bytes = EncodeLegacyTx(*tx);
        // The bytes must qualify as evidence at all (notably: never the
        // 64-byte leaf/inner-node ambiguity shape) — checked here so the
        // builder rejects them before doing any further work.
        CMutableTransaction evidence_check;
        if (!DecodeLegacyTx(entry.tx_bytes, evidence_check, error)) return false;
        entry.height = height;
        entry.position = position;
        entry.path = TransactionMerklePath(block, position);
        return true;
    }};

    LegacyFnIssuanceProofV1 proof;
    CTransactionRef pod_tx;
    if (!build_entry(pod_height, pod_position, proof.pod, pod_tx)) return false;

    std::vector<Txid> distinct;
    for (const CTxIn& in : pod_tx->vin) {
        bool seen{false};
        for (const Txid& txid : distinct) {
            if (txid == in.prevout.hash) {
                seen = true;
                break;
            }
        }
        if (!seen) distinct.push_back(in.prevout.hash);
    }
    for (const Txid& txid : distinct) {
        const auto location{locate_tx(txid)};
        if (!location) {
            error = strprintf("funding transaction %s not found in the sealed chain",
                              txid.ToString());
            return false;
        }
        LegacyFnMerkleProof entry;
        CTransactionRef funding_tx;
        if (!build_entry(location->first, location->second, entry, funding_tx)) return false;
        if (funding_tx->GetHash() != txid) {
            error = strprintf("located transaction does not hash to %s", txid.ToString());
            return false;
        }
        proof.funding.push_back(std::move(entry));
    }

    // Self-verification: the builder refuses to emit anything the
    // stateless verifier would reject.
    if (!VerifyLegacyFnIssuanceProof(proof, params, view, facts, error)) return false;
    out = std::move(proof);
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_LEGACY_FN_ISSUANCE_H

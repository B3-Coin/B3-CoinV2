// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_MODERN_FINALITY_FRESH_KEY_H
#define B3COIN_MODERN_FINALITY_FRESH_KEY_H

#include <crypto/bls.h>
#include <crypto/common.h>
#include <hash.h>
#include <modern/creation_action.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace modern {

/**
 * FRESH FINALITY KEY: the fresh lineage's per-epoch committee key binding
 * (B-37; b3-bridge-v2-finality-dependency.md section 6.4). It is a binding
 * kind of its own, separate from V1's FINALITY_KEY (modern/finality_key.h):
 * V1 bindings, the V1 signer and V1 producer snapshots never read it, and the
 * fresh committee never reads a V1 binding as a key.
 *
 *   cell (metadata policy 7 FINALITY_KEY, policy version 2, zero value):
 *       commitment = validator_key (the BIP340 x-only identity key)
 *       params     = bls_pubkey[48] || target_epoch u64 BE        (56 bytes)
 *   evidence (MPA record type 12, version 1, exactly 248 bytes):
 *       validator_key[32] || bls_pubkey[48] || target_epoch u64 BE
 *       || bip340_sig[64] || pop[96]
 *
 * Rules (CheckFreshKeyTransition), in order:
 *   - evidence fields equal the cell's commitment and params;
 *   - the key is nonzero (there is no revocation and no sequence: a fresh
 *     key names one target epoch and is never rebound);
 *   - bip340_sig = Schnorr by validator_key over
 *       TaggedHash("B3/FINALITY/FRESHBIND/V1",
 *                  chain_domain || validator_key || bls_pubkey || u64 target_epoch)
 *     The tag differs from V1's "B3/FINALITY/BIND/V1", so a V1 binding
 *     signature never authorizes a fresh binding (and vice versa). The
 *     lineage id is not bound: it is unknown before R_f, and genesis
 *     bindings are made between R_key and R_f - 1;
 *   - the key decodes canonically (crypto/bls.h) and `pop` is the key's
 *     IDENTITY-BOUND proof of possession: a BLS signature by bls_pubkey,
 *     under the signature suite (bls::SIG_DST), over
 *       TaggedHash("B3/FINALITY/FRESHPOP/V1",
 *                  chain_domain || validator_key || bls_pubkey || u64 target_epoch)
 *     The message contains the key itself, so it proves possession of the
 *     secret exactly as the IETF PoP does (rogue-key safety for the
 *     aggregates; no vote digest uses this tag). Unlike the IETF PoP
 *     (sign(sk, pk) under POP_DST, which V1's type-5 evidence carries), it
 *     also names the validator, the chain and the target epoch: a copy of a
 *     pending binding's key and `pop` under another identity, epoch or chain
 *     fails here, and a fresh binding never publishes a bare IETF PoP that
 *     a V1 FINALITY_KEY binding of the same key could reuse. So only the
 *     key's holder can claim a fresh key, under either kind, and nobody
 *     can burn a member's pending key by front-running it.
 *
 * What this header does NOT decide (node side, later steps): whether a key
 * was ever bound before under either kind (the first claim wins and a reuse
 * is inert), which binding window an epoch accepts, the seat rules
 * (double-binding void, carry-over) and erasure. Those live in the fresh key
 * index and the fresh committee rule; consensus here is structure and
 * cryptography only.
 *
 * ACTIVATION: the cell (7, 2) and record type 12 are active only from R_key
 * (Consensus::Params::FreshKeyRulesActive) on top of the Modern-object gate.
 * No shipped network configures R_key; regtest's -b3freshkeyheight is the
 * only setter. Without it a (7, 2) cell stays an inactive metadata policy and
 * a type-12 record is a known-but-inactive MPA type: invalid either way.
 */

//! Record version of MPA type 12.
inline constexpr uint16_t FRESH_FINALITY_KEY_EVIDENCE_VERSION_V1{1};
//! Exact evidence record size: 32 + 48 + 8 + 64 + 96.
inline constexpr size_t FRESH_FINALITY_KEY_EVIDENCE_SIZE{32 + BLS_PUBKEY_SIZE + 8 + BIP340_SIG_SIZE + BLS_SIGNATURE_SIZE};
static_assert(FRESH_FINALITY_KEY_EVIDENCE_SIZE == 248);
//! Tagged-hash domain of the identity authorization (BIP340 TaggedHash convention).
inline constexpr const char* FINALITY_FRESH_BIND_TAG{"B3/FINALITY/FRESHBIND/V1"};
//! Tagged-hash domain of the key's identity-bound proof of possession (BLS, SIG_DST).
inline constexpr const char* FINALITY_FRESH_POP_TAG{"B3/FINALITY/FRESHPOP/V1"};

//! FINALITY_KEY policy-version-2 cell params: bls_pubkey || target_epoch. 56 bytes (<= 80).
struct FreshFinalityKeyParams {
    static constexpr size_t SIZE{BLS_PUBKEY_SIZE + 8};
    BlsPubkeyBytes bls_pubkey{};
    uint64_t target_epoch{0};

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        std::copy(bls_pubkey.begin(), bls_pubkey.end(), out.data());
        WriteBE64(out.data() + BLS_PUBKEY_SIZE, target_epoch);
        return out;
    }
    static std::optional<FreshFinalityKeyParams> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        FreshFinalityKeyParams p;
        std::copy(in.begin(), in.begin() + BLS_PUBKEY_SIZE, p.bls_pubkey.begin());
        p.target_epoch = ReadBE64(in.data() + BLS_PUBKEY_SIZE);
        return p;
    }
    friend bool operator==(const FreshFinalityKeyParams& a, const FreshFinalityKeyParams& b)
    {
        return a.bls_pubkey == b.bls_pubkey && a.target_epoch == b.target_epoch;
    }
};
static_assert(FreshFinalityKeyParams::SIZE <= MAX_POLICY_PARAMS_SIZE);

//! FRESH_FINALITY_KEY_EVIDENCE record payload. 248 bytes exactly.
struct FreshFinalityKeyEvidence {
    static constexpr size_t SIZE{FRESH_FINALITY_KEY_EVIDENCE_SIZE};
    ValidatorKeyBytes validator_key{};
    BlsPubkeyBytes bls_pubkey{};
    uint64_t target_epoch{0};
    std::array<unsigned char, BIP340_SIG_SIZE> bip340_sig{};
    //! Identity-bound proof of possession (FreshFinalityPopDigest), never the
    //! bare IETF PoP of the key.
    std::array<unsigned char, BLS_SIGNATURE_SIZE> pop{};

    std::array<unsigned char, SIZE> Encode() const
    {
        std::array<unsigned char, SIZE> out{};
        size_t o{0};
        std::copy(validator_key.begin(), validator_key.end(), out.data() + o); o += 32;
        std::copy(bls_pubkey.begin(), bls_pubkey.end(), out.data() + o); o += BLS_PUBKEY_SIZE;
        WriteBE64(out.data() + o, target_epoch); o += 8;
        std::copy(bip340_sig.begin(), bip340_sig.end(), out.data() + o); o += BIP340_SIG_SIZE;
        std::copy(pop.begin(), pop.end(), out.data() + o);
        return out;
    }
    static std::optional<FreshFinalityKeyEvidence> Decode(std::span<const unsigned char> in)
    {
        if (in.size() != SIZE) return std::nullopt;
        FreshFinalityKeyEvidence e;
        size_t o{0};
        std::copy(in.begin() + o, in.begin() + o + 32, e.validator_key.begin()); o += 32;
        std::copy(in.begin() + o, in.begin() + o + BLS_PUBKEY_SIZE, e.bls_pubkey.begin()); o += BLS_PUBKEY_SIZE;
        e.target_epoch = ReadBE64(in.data() + o); o += 8;
        std::copy(in.begin() + o, in.begin() + o + BIP340_SIG_SIZE, e.bip340_sig.begin()); o += BIP340_SIG_SIZE;
        std::copy(in.begin() + o, in.begin() + o + BLS_SIGNATURE_SIZE, e.pop.begin());
        return e;
    }
    friend bool operator==(const FreshFinalityKeyEvidence& a, const FreshFinalityKeyEvidence& b)
    {
        return a.Encode() == b.Encode();
    }
};

//! TaggedHash(tag, chain_domain || validator_key || bls_pubkey || u64 BE target_epoch).
inline uint256 FreshFinalityBindingHash(const char* tag, const uint256& chain_domain,
                                        const ValidatorKeyBytes& validator_key, const BlsPubkeyBytes& bls_pubkey,
                                        const uint64_t target_epoch)
{
    HashWriter w{TaggedHash(tag)};
    w << std::span<const unsigned char>(chain_domain.begin(), 32);
    w << std::span<const unsigned char>(validator_key) << std::span<const unsigned char>(bls_pubkey);
    unsigned char e[8];
    WriteBE64(e, target_epoch);
    w << std::span<const unsigned char>(e, 8);
    return w.GetSHA256();
}

//! The message the BIP340 identity key signs to authorize a fresh binding.
inline uint256 FreshFinalityBindDigest(const uint256& chain_domain, const ValidatorKeyBytes& validator_key,
                                       const BlsPubkeyBytes& bls_pubkey, const uint64_t target_epoch)
{
    return FreshFinalityBindingHash(FINALITY_FRESH_BIND_TAG, chain_domain, validator_key, bls_pubkey, target_epoch);
}

/**
 * The message the fresh BLS key itself signs (bls::SecretKey::Sign, SIG_DST)
 * as its identity-bound proof of possession: the same fields as the bind
 * digest under its own tag. Binding the validator, the chain and the epoch
 * is what stops a copied `pop` from serving another claim of the key.
 */
inline uint256 FreshFinalityPopDigest(const uint256& chain_domain, const ValidatorKeyBytes& validator_key,
                                      const BlsPubkeyBytes& bls_pubkey, const uint64_t target_epoch)
{
    return FreshFinalityBindingHash(FINALITY_FRESH_POP_TAG, chain_domain, validator_key, bls_pubkey, target_epoch);
}

enum class FreshKeyCheck {
    OK,
    EVIDENCE_VALIDATOR_MISMATCH, //!< evidence.validator_key != cell commitment
    EVIDENCE_PUBKEY_MISMATCH,    //!< evidence.bls_pubkey != params.bls_pubkey
    EVIDENCE_EPOCH_MISMATCH,     //!< evidence.target_epoch != params.target_epoch
    ZERO_KEY,                    //!< the all-zero key: the fresh kind has no revocation
    BAD_BIP340_SIGNATURE,        //!< identity authorization invalid
    BAD_BLS_PUBKEY,              //!< key does not decode canonically
    BAD_POP,                     //!< invalid (or zero) identity-bound proof of possession
};

inline const char* FreshKeyCheckName(const FreshKeyCheck c)
{
    switch (c) {
    case FreshKeyCheck::OK: return "ok";
    case FreshKeyCheck::EVIDENCE_VALIDATOR_MISMATCH: return "evidence-validator-mismatch";
    case FreshKeyCheck::EVIDENCE_PUBKEY_MISMATCH: return "evidence-pubkey-mismatch";
    case FreshKeyCheck::EVIDENCE_EPOCH_MISMATCH: return "evidence-epoch-mismatch";
    case FreshKeyCheck::ZERO_KEY: return "zero-key";
    case FreshKeyCheck::BAD_BIP340_SIGNATURE: return "bad-bip340-signature";
    case FreshKeyCheck::BAD_BLS_PUBKEY: return "bad-bls-pubkey";
    case FreshKeyCheck::BAD_POP: return "bad-pop";
    }
    return "unknown";
}

/**
 * Validate one fresh binding. Cheap field and shape checks run before any
 * cryptography; BIP340 before BLS. Context-free apart from the chain domain:
 * reuse, windows and seats are node-side rules (see the header comment).
 *
 * @param chain_domain    ModernChainDomain (the bind digest is chain-bound)
 * @param cell_commitment the cell's commitment = validator_key
 * @param params          the cell's params (bls_pubkey || target_epoch)
 * @param evidence        the 248-byte evidence record
 */
inline FreshKeyCheck CheckFreshKeyTransition(const uint256& chain_domain, const uint256& cell_commitment,
                                             const FreshFinalityKeyParams& params,
                                             const FreshFinalityKeyEvidence& evidence)
{
    // 1. evidence <-> cell consistency
    if (!std::equal(evidence.validator_key.begin(), evidence.validator_key.end(), cell_commitment.begin())) {
        return FreshKeyCheck::EVIDENCE_VALIDATOR_MISMATCH;
    }
    if (evidence.bls_pubkey != params.bls_pubkey) return FreshKeyCheck::EVIDENCE_PUBKEY_MISMATCH;
    if (evidence.target_epoch != params.target_epoch) return FreshKeyCheck::EVIDENCE_EPOCH_MISMATCH;
    // 2. no revocation form
    if (IsZeroBlsKey(params.bls_pubkey)) return FreshKeyCheck::ZERO_KEY;
    // 3. identity authorization (BIP340 by validator_key over the fresh bind digest)
    const uint256 digest{FreshFinalityBindDigest(chain_domain, evidence.validator_key, evidence.bls_pubkey,
                                                 evidence.target_epoch)};
    const XOnlyPubKey identity{std::span<const unsigned char>(evidence.validator_key)};
    if (!identity.VerifySchnorr(digest, evidence.bip340_sig)) return FreshKeyCheck::BAD_BIP340_SIGNATURE;
    // 4. canonical key + identity-bound proof of possession (the key signs
    //    this exact claim, so a copy under another identity never verifies)
    const auto pk{bls::PublicKey::Decode(evidence.bls_pubkey)};
    if (!pk) return FreshKeyCheck::BAD_BLS_PUBKEY;
    const auto pop{bls::Signature::Decode(evidence.pop)};
    const uint256 pop_digest{FreshFinalityPopDigest(chain_domain, evidence.validator_key, evidence.bls_pubkey,
                                                    evidence.target_epoch)};
    if (!pop || !bls::Verify(*pk, std::span<const unsigned char>(pop_digest.begin(), 32), *pop)) {
        return FreshKeyCheck::BAD_POP;
    }
    return FreshKeyCheck::OK;
}

//! One matched fresh FINALITY_KEY (v2) cell + type-12 evidence record.
struct FreshFinalityKeyPair {
    size_t cell_index{0};    // tx.vout index of the cell
    size_t record_index{0};  // tx.mpa index of the evidence record
    uint256 commitment{};    // = validator_key
    FreshFinalityKeyParams params{};
    FreshFinalityKeyEvidence evidence{};
};

//! Deterministic matching key: validator_key || bls_pubkey || target_epoch (BE).
using FreshFinalityKeyMatchKey = std::array<unsigned char, 32 + FreshFinalityKeyParams::SIZE>;

inline FreshFinalityKeyMatchKey MakeFreshFinalityKeyMatchKey(const ValidatorKeyBytes& validator_key,
                                                            const FreshFinalityKeyParams& params)
{
    FreshFinalityKeyMatchKey key{};
    std::copy(validator_key.begin(), validator_key.end(), key.begin());
    const auto p{params.Encode()};
    std::copy(p.begin(), p.end(), key.begin() + 32);
    return key;
}

//! Is this output a FINALITY_KEY policy-version-2 (fresh) cell? Grammar only.
inline std::optional<MetadataCell> ParseFreshFinalityKeyCell(const CScript& script)
{
    auto cell{ParseMetadataCell(script)};
    if (!cell || cell->policy_type != static_cast<uint16_t>(PolicyType::FINALITY_KEY) ||
        cell->policy_version != FRESH_FINALITY_KEY_POLICY_VERSION) {
        return std::nullopt;
    }
    return cell;
}

/**
 * Exact one-to-one matching of fresh FINALITY_KEY cells (policy version 2)
 * and type-12 evidence records (version 1) within one transaction, by the
 * deterministic key (validator_key, bls_pubkey, target_epoch). No positional
 * guessing. Fails closed on malformed cell params, malformed evidence,
 * duplicate cells or duplicate evidence sharing a key, evidence without a
 * cell (orphan) and a cell without evidence. V1 cells (version 1) and type-5
 * records are not looked at (MatchFinalityKeyPairs owns them, and it skips
 * version-2 cells). Transactions with neither are trivially fine. `out` is in
 * key order.
 */
inline bool MatchFreshFinalityKeyPairs(const CTransaction& tx, std::vector<FreshFinalityKeyPair>& out,
                                       std::string& error)
{
    out.clear();
    std::map<FreshFinalityKeyMatchKey, FreshFinalityKeyPair> cells;
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        const auto cell{ParseFreshFinalityKeyCell(tx.vout[i].scriptPubKey)};
        if (!cell) continue;
        const auto params{FreshFinalityKeyParams::Decode(cell->params)};
        if (!params) {
            error = "fresh-key-cell-params";
            return false;
        }
        FreshFinalityKeyPair pair;
        pair.cell_index = i;
        pair.commitment = cell->commitment;
        pair.params = *params;
        ValidatorKeyBytes vk;
        std::copy(cell->commitment.begin(), cell->commitment.end(), vk.begin());
        if (!cells.emplace(MakeFreshFinalityKeyMatchKey(vk, *params), pair).second) {
            error = "fresh-key-duplicate-cell";
            return false;
        }
    }
    std::map<FreshFinalityKeyMatchKey, size_t> records;
    for (size_t i = 0; i < tx.mpa.size(); ++i) {
        const CMpaRecord& rec{tx.mpa[i]};
        if (rec.payload_type != CREATION_ACTION_FRESH_FINALITY_KEY_EVIDENCE ||
            rec.payload_version != FRESH_FINALITY_KEY_EVIDENCE_VERSION_V1) {
            continue;
        }
        const auto ev{FreshFinalityKeyEvidence::Decode(rec.payload)};
        if (!ev) {
            error = "fresh-key-evidence-malformed";
            return false;
        }
        FreshFinalityKeyParams p;
        p.bls_pubkey = ev->bls_pubkey;
        p.target_epoch = ev->target_epoch;
        const auto key{MakeFreshFinalityKeyMatchKey(ev->validator_key, p)};
        if (!records.emplace(key, i).second) {
            error = "fresh-key-duplicate-evidence";
            return false;
        }
        const auto cell_it{cells.find(key)};
        if (cell_it == cells.end()) {
            error = "fresh-key-orphan-evidence";
            return false;
        }
        cell_it->second.record_index = i;
        cell_it->second.evidence = *ev;
    }
    if (records.size() != cells.size()) {
        error = "fresh-key-cell-without-evidence";
        return false;
    }
    out.reserve(cells.size());
    for (auto& [key, pair] : cells) out.push_back(pair);
    return true;
}

//! Canonical zero-value fresh cell script for (validator_key, params).
inline CScript MakeFreshFinalityKeyCell(const ValidatorKeyBytes& validator_key, const FreshFinalityKeyParams& params)
{
    uint256 commitment;
    std::copy(validator_key.begin(), validator_key.end(), commitment.begin());
    // 56-byte params always fit the 80-byte bound (static_assert above).
    return *MakeMetadataCellScript(static_cast<uint16_t>(PolicyType::FINALITY_KEY),
                                   FRESH_FINALITY_KEY_POLICY_VERSION, commitment, params.Encode());
}

//! The type-12 MPA record carrying `evidence`.
inline CMpaRecord MakeFreshFinalityKeyRecord(const FreshFinalityKeyEvidence& evidence)
{
    CMpaRecord record;
    record.payload_type = CREATION_ACTION_FRESH_FINALITY_KEY_EVIDENCE;
    record.payload_version = FRESH_FINALITY_KEY_EVIDENCE_VERSION_V1;
    const auto enc{evidence.Encode()};
    record.payload.assign(enc.begin(), enc.end());
    return record;
}

/**
 * Structure + cryptography of every fresh binding in a block: the matcher and
 * CheckFreshKeyTransition for each transaction, in block order. Errors are the
 * matcher's names or "fresh-key-<check name>". The caller gates it on
 * Consensus::Params::FreshKeyRulesActive; reuse, windows and seats are not
 * consensus-invalidating and are not checked here.
 */
inline bool VerifyFreshKeyBindingsInBlock(const CBlock& block, const uint256& chain_domain, std::string& error)
{
    for (const CTransactionRef& tx : block.vtx) {
        std::vector<FreshFinalityKeyPair> pairs;
        if (!MatchFreshFinalityKeyPairs(*tx, pairs, error)) return false;
        for (const FreshFinalityKeyPair& pair : pairs) {
            const FreshKeyCheck check{CheckFreshKeyTransition(chain_domain, pair.commitment, pair.params, pair.evidence)};
            if (check != FreshKeyCheck::OK) {
                error = std::string{"fresh-key-"} + FreshKeyCheckName(check);
                return false;
            }
        }
    }
    return true;
}

} // namespace modern

#endif // B3COIN_MODERN_FINALITY_FRESH_KEY_H

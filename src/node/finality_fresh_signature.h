// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_FRESH_SIGNATURE_H
#define B3COIN_NODE_FINALITY_FRESH_SIGNATURE_H

#include <consensus/params.h>
#include <modern/finality_types.h>
#include <modern/finality_v2.h>
#include <node/finality_tracker.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CBlockIndex;
class CChain;
struct CMutableTransaction;

namespace node {

class BridgeStateIndex;

/**
 * Fresh-lineage vote message (bridge V2 stage S3, regtest staging signer;
 * b3-bridge-v2-finality-dependency.md sections 6.5 "Votes" and 8). LIVENESS
 * ONLY, exactly like the V1 `finsig` path: nothing here mutates consensus
 * state. A fresh checkpoint becomes final only through an MPA type-11 record
 * included in a valid block and judged by node::JudgeFreshCoinbaseCertificate.
 *
 * Serialization (fixed 148 bytes, integers little-endian like `finsig`):
 *   32 B lineage id || u64 epoch || u64 height || u32 index || 96 B BLS signature
 *
 * V1 `finsig` names no lineage; this message does, so a vote for another
 * fresh lineage (another R_f, another genesis block or another Set'_0) fails
 * cheaply instead of failing BLS. (epoch, height, index) then identify the
 * signed object on the receiver's own chain: the FinalizedBlockV2 is
 * reconstructed locally (node::ExpectedFinalizedBlockV2: block hash at
 * `height`, zero withdrawal root and flags in this slice, ruleset-2
 * successor hash, checkpoint index and history root), and the signer is
 * member `index` of Set'_epoch.
 *
 * Transport gap (deviation from section 6.5, which asks for a P2P message
 * naming the lineage): this slice carries the message over the hidden RPCs
 * getfinalitylineagevotes / submitfinalitylineagevote only, so a regtest
 * network aggregates a V2 certificate only while a harness shuttles votes
 * between nodes. A lineage-naming message mirroring `finsig` in
 * net_processing.cpp, feeding FreshLineageSignaturePool::Submit, is an open
 * item before RT4 (or an explicit ruling that the RT4 harness shuttles).
 */
struct FreshLineageSig {
    static constexpr size_t SERIALIZED_SIZE{32 + 8 + 8 + 4 + modern::BLS_SIGNATURE_SIZE};

    uint256 lineage_id{};
    uint64_t epoch{0};
    uint64_t height{0};
    uint32_t index{0};
    std::array<unsigned char, modern::BLS_SIGNATURE_SIZE> signature{};

    SERIALIZE_METHODS(FreshLineageSig, obj)
    {
        READWRITE(obj.lineage_id, obj.epoch, obj.height, obj.index, obj.signature);
    }
    friend bool operator==(const FreshLineageSig& a, const FreshLineageSig& b)
    {
        return a.lineage_id == b.lineage_id && a.epoch == b.epoch && a.height == b.height &&
               a.index == b.index && a.signature == b.signature;
    }
};
static_assert(FreshLineageSig::SERIALIZED_SIZE == 148);

/**
 * Collects verified fresh-lineage votes per (fresh epoch, checkpoint height)
 * for the fresh lineage of the active chain; a mirror of
 * node::FinalitySignaturePool over FinalityDigestV2.
 *
 * Validation order (cheap first, BLS last): fresh lineage configured and
 * bootstrapped -> lineage id -> height on the fresh grid strictly above
 * GENESIS_HEIGHT and not above the tip -> depth -> strictly above the fresh
 * finalized height -> epoch in {current, current-1} -> fresh epoch of the
 * height -> member index -> reconstruct the exact FinalizedBlockV2 and evict
 * every slot whose object changed (pre-finality reorg) -> duplicate -> pool
 * bounds -> single BLS verification. All slots are dropped when the fresh
 * lineage id of the chain changes (a reorg across R_f - 1).
 */
class FreshLineageSignaturePool
{
public:
    //! Distinct (epoch, height) slots tracked at once (DoS bound; finalized
    //! slots are pruned and a newer verified checkpoint replaces the oldest).
    static constexpr size_t MAX_TRACKED_CHECKPOINTS{8};

    enum class Accept {
        ACCEPTED,
        DUPLICATE,        //!< exact signature bytes already verified for this index and current digest
        STALE,            //!< at or below the fresh finalized height, broken lineage, or object not derivable
        UNCONFIGURED,     //!< no fresh lineage on this chain (knob absent, below R_f, or not bootstrapped)
        LINEAGE_MISMATCH, //!< the message names another fresh lineage id
        UNKNOWN_EPOCH,    //!< outside {current, current-1} of the fresh lineage
        NOT_CHECKPOINT,   //!< not on the fresh grid above GENESIS_HEIGHT, above the tip, or in another epoch's span
        TOO_SHALLOW,      //!< tip - height < CHECKPOINT_DEPTH: not signable yet
        BAD_INDEX,        //!< member index >= n
        POOL_FULL,        //!< bounded pool is full of checkpoints newer than this one
        BAD_SIGNATURE,    //!< BLS verification failed (wrong branch, wrong key, V1 digest, garbage)
    };
    static const char* AcceptName(Accept a);

    /**
     * Validate and store one vote. `tracker` must be synced to the active
     * tip of `chain`; the caller holds cs_main. Cheap checks precede the BLS
     * verification.
     */
    Accept Submit(const FreshLineageSig& sig, const FinalityTracker& tracker, const CChain& chain,
                  const Consensus::Params& params, const BridgeStateIndex* bridge_index = nullptr);

    /**
     * The highest tracked checkpoint whose collected votes meet both quorums
     * (stake weight and headcount floor(2n/3)+1) of its fresh signing
     * committee, aggregated into (FinalizedBlockV2, certificate). When
     * `candidate_parent` is supplied every candidate must also pass
     * FinalityTracker::JudgeCandidateCertificateV2 for the block extending
     * it (the tracker must be synced to that parent); a rejected candidate
     * is skipped, never erased. nullopt when nothing qualifies.
     */
    std::optional<std::pair<modern::FinalizedBlockV2, modern::FinalityCertificate>>
    BestCertificate(const FinalityTracker& tracker, const CChain& chain, const Consensus::Params& params,
                    const BridgeStateIndex* bridge_index = nullptr,
                    const CBlockIndex* candidate_parent = nullptr) const;

    /** Exact already-verified votes that are still useful on this chain
     * (not finalized, still derivable, deep enough), newest checkpoint first.
     * For RPC export and relay; never signs or mutates. Same chain/locking
     * preconditions as Submit. */
    std::vector<FreshLineageSig> VerifiedSignatures(const FinalityTracker& tracker, const CChain& chain,
                                                    const Consensus::Params& params,
                                                    const BridgeStateIndex* bridge_index = nullptr) const;

    //! Drop every slot at or below `finalized_height`.
    void Prune(int finalized_height);
    size_t TrackedCheckpoints() const { return m_slots.size(); }
    size_t SignatureCount(uint64_t epoch, uint64_t height) const;
    //! The fresh lineage id the retained slots belong to (null when empty).
    const uint256& LineageId() const { return m_lineage_id; }

    /**
     * Node-local status of the staging signer (not consensus, never
     * relayed). Every FinalitySigner::MaybeSignFresh pass that reaches the
     * fresh lineage reports its anti-repeat watermark and its current
     * refusal here, so getfinalitylineage shows a fresh signer that is
     * disabled (absent journal, orphan lock, journal failure) while the V1
     * staking status stays healthy. The last reporting signer wins; a node
     * runs one signer.
     */
    struct LocalSignerStatus {
        int last_signed_height{-1};
        std::string last_error;
    };
    void ReportLocalSigner(int last_signed_height, std::string last_error)
    {
        m_local_signer = LocalSignerStatus{last_signed_height, std::move(last_error)};
    }
    //! nullopt until a signer has run a fresh-lineage pass into this pool.
    const std::optional<LocalSignerStatus>& LocalSigner() const { return m_local_signer; }

private:
    struct Slot {
        //! Exact FinalityDigestV2 shared by every signature in this slot.
        uint256 digest{};
        std::map<uint32_t, std::array<unsigned char, modern::BLS_SIGNATURE_SIZE>> sigs; // index -> signature
    };
    uint256 m_lineage_id{};
    std::map<std::pair<uint64_t, uint64_t>, Slot> m_slots;
    std::optional<LocalSignerStatus> m_local_signer;
};

/**
 * Block assembly hook (regtest knob only): when `pool` holds a fresh-lineage
 * certificate that the identical consensus rule accepts for the block
 * extending `parent`, append it to `coinbase` as the MPA type-11 record
 * {type 11, version 1, FinalizedBlockV2 || bitmap || aggregate signature}
 * and restore canonical record order. Type 11 has no metadata cell: the
 * caller's MODERN_PAYLOAD_ROOT commitment binds the record. Returns true when
 * a record was appended. The tracker must be synced to `parent`; the caller
 * holds cs_main.
 */
bool AppendFreshLineageCertificate(const FinalityTracker& tracker, const FreshLineageSignaturePool& pool,
                                   const CChain& chain, const Consensus::Params& params, const CBlockIndex& parent,
                                   const BridgeStateIndex* bridge_index, CMutableTransaction& coinbase);

} // namespace node

#endif // B3COIN_NODE_FINALITY_FRESH_SIGNATURE_H

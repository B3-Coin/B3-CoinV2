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
#include <functional>
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
 * Transport (section 6.5): these exact 148 bytes are the payload of the
 * lineage-naming P2P message `finsigv2` (NetMsgType::FINSIGV2), handled in
 * net_processing.cpp only on a chain that configures a fresh lineage, with
 * its own wire and verification budgets and a per-connection replay of the
 * verified pool (node/finality_fresh_transport.h). The hidden RPCs
 * getfinalitylineagevotes / submitfinalitylineagevote remain for drills.
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
 * B-37 key status of one validator's fresh-lineage signer (node-local, never
 * consensus, never relayed; finality dependency section 6.4 "Erasure and its
 * limit"). Only in fresh-key mode (regtest -b3freshkeyheight).
 */
struct FreshSignerKeyEntry {
    //! Target epoch: the store record's (a persistent signer generates every
    //! key for one epoch), or for an offline signer's key the target of its
    //! chain binding; nullopt for an offline key the chain has not bound.
    std::optional<uint64_t> epoch;
    modern::BlsPubkeyBytes bls_pubkey{};
    //! The signer holds the secret; false once the store ERASED it.
    bool held{false};
    //! The retained committees (Set'_k, k in [e-2, e+1]) that seat this
    //! validator on this key, ascending.
    std::vector<uint64_t> seated_epochs;
    friend bool operator==(const FreshSignerKeyEntry&, const FreshSignerKeyEntry&) = default;
};
//! Set'_epoch seats this validator on `bls_pubkey`, whose secret the signer
//! does not hold.
struct FreshMissingSeat {
    uint64_t epoch{0};
    modern::BlsPubkeyBytes bls_pubkey{};
    friend bool operator==(const FreshMissingSeat&, const FreshMissingSeat&) = default;
};
struct FreshSignerKeyStatus {
    //! Every key of the signer, in its order (store records: creation order).
    std::vector<FreshSignerKeyEntry> fresh_keys;
    //! Seats in Set'_{e-1}, Set'_e and Set'_{e+1} without a held secret
    //! (ascending epoch). A Set'_{e-1} key the store erased is not missing:
    //! the erasure policy removes it only once the handover into e is deep.
    std::vector<FreshMissingSeat> missing_seats;
    /**
     * Human-readable alerts, in order:
     *  - every counted chain binding of this validator (target >= e-2, or
     *    any target before the lineage exists) whose key the signer neither
     *    holds nor erased: someone else holds this validator's identity key
     *    (an old wallet backup, or a leak) and binds keys with it;
     *  - every held key of the signer that the chain recorded under
     *    another first claim (FreshKeyBindingIndex::EverBound without
     *    OwnFreshEpoch): this validator's binding of it is inert; under
     *    identity-bound proofs of possession only a holder of the secret
     *    can make such a claim;
     *  - every target in [e-2, e+2] (target 0 before the lineage exists)
     *    with two or more counted bindings of this validator in its window
     *    (FreshSeatWindow): the seat for that epoch is VOID.
     */
    std::vector<std::string> alerts;
};

/**
 * Compute FreshSignerKeyStatus for `validator_key` from the signer's `keys`
 * (seated_epochs is recomputed; a missing epoch is filled from the key's
 * chain binding), the fresh key index (null: no binding facts), and the fresh
 * state of the tip (null: before the lineage; the projected state for R_f
 * at R_f - 1). Pure.
 */
FreshSignerKeyStatus ComputeFreshSignerKeyStatus(const modern::ValidatorKeyBytes& validator_key,
                                                 std::vector<FreshSignerKeyEntry> keys,
                                                 const FreshKeyBindingIndex* index, const FreshLineageState* state,
                                                 const Consensus::Params& params, int tip_height);

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
        VERIFICATION_DEFERRED, //!< caller declined the expensive verification budget; retry is permitted
        BAD_SIGNATURE,    //!< BLS verification failed (wrong branch, wrong key, V1 digest, garbage)
    };
    static const char* AcceptName(Accept a);

    /**
     * Validate and store one vote. `tracker` must be synced to the active
     * tip of `chain`; the caller holds cs_main. Cheap checks precede the BLS
     * verification. If supplied, consume_verification_budget is called once,
     * immediately before BLS decoding/verification (after the DUPLICATE and
     * POOL_FULL checks), never for cheap rejects or exact verified
     * duplicates; a false result returns VERIFICATION_DEFERRED and stores
     * nothing (the P2P path charges its verification bucket here; RPC and
     * the local signer pass none).
     */
    Accept Submit(const FreshLineageSig& sig, const FinalityTracker& tracker, const CChain& chain,
                  const Consensus::Params& params, const BridgeStateIndex* bridge_index = nullptr,
                  const std::function<bool()>& consume_verification_budget = {});

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
     * runs one signer. In fresh-key mode (B-37, regtest -b3freshkeyheight)
     * the signer also reports its keys, its seats without a held secret and
     * its alerts (FreshSignerKeyStatus); they stay empty otherwise.
     */
    struct LocalSignerStatus {
        int last_signed_height{-1};
        std::string last_error;
        std::vector<FreshSignerKeyEntry> fresh_keys;
        std::vector<FreshMissingSeat> missing_seats;
        std::vector<std::string> alerts;
    };
    void ReportLocalSigner(LocalSignerStatus status) { m_local_signer = std::move(status); }
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

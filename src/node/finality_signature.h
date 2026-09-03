// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_SIGNATURE_H
#define B3COIN_NODE_FINALITY_SIGNATURE_H

#include <consensus/params.h>
#include <crypto/bls.h>
#include <modern/finality_types.h>
#include <node/finality_signer_store.h>
#include <node/finality_tracker.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CChain;

namespace node {

class BridgeStateIndex;

/**
 * BLS finality message path (plan Commit 15; b3-cross-chain-finality-v1.md
 * section 4 "Signing", "Transport"). LIVENESS ONLY: nothing here mutates
 * consensus state -- a checkpoint becomes final exclusively through a
 * FINALITY_CERT included in a valid block and judged by consensus. The pool
 * merely collects individually verified signatures so that any node (no
 * privileged aggregator, no leader) can assemble a certificate once both the
 * stake-weight and validator-headcount quorums are present.
 *
 * Wire message `finsig` (fixed 116 bytes):
 *   u64 epoch || u64 height || u32 index || 96 B BLS signature
 * (epoch, height, index) deterministically identify the signed object on the
 * receiver's own chain: the FinalizedBlock is reconstructed locally (block
 * hash at `height`, zero withdrawal root, hash(Set_{epoch+1}) as derived
 * here), the signer is member `index` of Set_epoch. Anything that does not
 * match this node's chain/epoch state fails cheaply or fails BLS.
 *
 * Validation order (cheap first, BLS last): feature configured -> epoch in
 * {current, current-1} -> checkpoint schedule -> depth (tip - h >= D) ->
 * strictly above the finalized height -> index < n -> reconstruct this
 * branch's exact digest and evict obsolete branch slots -> duplicate -> pool
 * bounds -> single BLS verify. One slot per (epoch, height), explicitly bound
 * to that digest. A permitted pre-finality reorg replaces an obsolete slot;
 * a signature over the old branch then fails verification against the new
 * digest and is dropped.
 */
struct FinalitySig {
    uint64_t epoch{0};
    uint64_t height{0};
    uint32_t index{0};
    std::array<unsigned char, modern::BLS_SIGNATURE_SIZE> signature{};

    SERIALIZE_METHODS(FinalitySig, obj) { READWRITE(obj.epoch, obj.height, obj.index, obj.signature); }
    friend bool operator==(const FinalitySig& a, const FinalitySig& b)
    {
        return a.epoch == b.epoch && a.height == b.height && a.index == b.index && a.signature == b.signature;
    }
};

/** Collects verified finality signatures per (epoch, checkpoint height). */
class FinalitySignaturePool
{
public:
    //! Distinct (epoch, height) slots tracked at once (DoS bound; finalized
    //! slots are pruned and a newer verified checkpoint replaces the oldest).
    static constexpr size_t MAX_TRACKED_CHECKPOINTS{8};

    enum class Accept {
        ACCEPTED,
        DUPLICATE,      //!< already have this (epoch, height, index)
        STALE,          //!< at or below the finalized height, or unconfigured
        UNKNOWN_EPOCH,  //!< outside {current, current-1} or no set on this chain
        NOT_CHECKPOINT, //!< height not on the schedule, above the tip, or in the wrong epoch span
        TOO_SHALLOW,    //!< tip - height < CHECKPOINT_DEPTH: not signable yet
        BAD_INDEX,      //!< index >= n
        POOL_FULL,      //!< bounded pool is full of checkpoints newer than this one
        BAD_SIGNATURE,  //!< BLS verification failed (wrong branch, wrong key, garbage)
    };
    static const char* AcceptName(Accept a);

    /**
     * Validate and store one signature. `tracker` must be synced to the
     * active tip of `chain`. Cheap checks precede the BLS verification.
     */
    Accept Submit(const FinalitySig& sig, const FinalityTracker& tracker, const CChain& chain,
                  const Consensus::Params& params,
                  const BridgeStateIndex* bridge_index = nullptr);

    /**
     * The highest tracked checkpoint whose collected signatures meet both
     * quorums of its signing set, assembled into (FinalizedBlock, certificate
     * with signer bitmap). Verification-ready; the caller (block assembly)
     * still runs the consensus judge before emitting it. nullopt when no slot
     * has both quorums.
     */
    std::optional<std::pair<modern::FinalizedBlock, modern::FinalityCertificate>>
    BestCertificate(const FinalityTracker& tracker, const CChain& chain,
                    const Consensus::Params& params,
                    const BridgeStateIndex* bridge_index = nullptr) const;

    //! Drop every slot at or below `finalized_height`.
    void Prune(int finalized_height);
    size_t TrackedCheckpoints() const { return m_slots.size(); }
    size_t SignatureCount(uint64_t epoch, uint64_t height) const;

    //! Reconstruct the FinalizedBlock this node expects for (epoch, height);
    //! nullopt when the slot is not derivable from the current state.
    static std::optional<modern::FinalizedBlock> ExpectedFinalizedBlock(uint64_t epoch, uint64_t height,
                                                                        const FinalityTracker::State& state,
                                                                        const CChain& chain,
                                                                        const Consensus::Params& params,
                                                                        const BridgeStateIndex* bridge_index = nullptr);

private:
    struct Slot {
        //! Exact branch/root/set digest shared by every signature in this
        //! (epoch,height) slot. A pre-finality reorg may reuse the coordinates
        //! for a different object, in which case the old slot is discarded.
        uint256 digest{};
        std::map<uint32_t, std::array<unsigned char, modern::BLS_SIGNATURE_SIZE>> sigs; // index -> signature
    };
    std::map<std::pair<uint64_t, uint64_t>, Slot> m_slots;
};

/**
 * Produces this validator's finality signatures (validator behaviour,
 * spec section 4 "Signing"): only scheduled checkpoints, only once the
 * depth is reached, strictly increasing heights, only on the active chain
 * (descendants of the latest certified checkpoint), and only with the BLS
 * key the ACTIVE epoch snapshot records for this validator -- a mid-epoch
 * rotation signs nothing until its snapshot takes effect.
 */
class FinalitySigner
{
public:
    void SetKey(const bls::SecretKey& key, const modern::ValidatorKeyBytes& validator_key)
    {
        m_key = key;
        m_validator_key = validator_key;
        m_store = FinalitySignerStore{};
        m_last_signed = -1;
        m_error.clear();
        m_permanent_error = false;
    }
    /** Arm a production signer with its durable, validator-identity-scoped
     * journal. A corrupt, unreadable, foreign, or otherwise unsafe existing
     * record is rejected before the key is armed. */
    bool SetKeyPersistent(const bls::SecretKey& key,
                          const modern::ValidatorKeyBytes& validator_key,
                          const uint256& chain_domain,
                          const fs::path& store_directory,
                          std::string& error);
    bool HasKey() const { return m_key.has_value(); }
    int LastSignedHeight() const { return m_last_signed; }
    const std::string& LastError() const { return m_error; }

    /**
     * Sign every checkpoint now signable and not yet signed; the produced
     * messages are already submitted to `pool` (self-aggregation) and are
     * returned for network relay. `tracker` must be synced to the tip.
     */
    std::vector<FinalitySig> MaybeSign(const FinalityTracker& tracker, const CChain& chain,
                                       const Consensus::Params& params,
                                       FinalitySignaturePool& pool,
                                       const BridgeStateIndex* bridge_index = nullptr);

private:
    bool EnsurePersistentSafety(const FinalityTracker& tracker,
                                const CChain& chain,
                                const Consensus::Params& params,
                                const BridgeStateIndex* bridge_index);
    void Fail(std::string error, bool permanent = true);

    std::optional<bls::SecretKey> m_key;
    modern::ValidatorKeyBytes m_validator_key{};
    int m_last_signed{-1};
    FinalitySignerStore m_store;
    std::string m_error;
    bool m_permanent_error{false};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_SIGNATURE_H

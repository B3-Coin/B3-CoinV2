// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_SIGNATURE_H
#define B3COIN_NODE_FINALITY_SIGNATURE_H

#include <consensus/params.h>
#include <crypto/bls.h>
#include <modern/finality_types.h>
#include <node/finality_signer_store.h>
#include <node/finality_signing_policy.h>
#include <node/finality_tracker.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CChain;

namespace interfaces {
struct FinalityRecoveryStatus;
}

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
 * bounds -> optional verification budget -> single BLS verify. One slot per (epoch, height), explicitly bound
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
        DUPLICATE,      //!< exact signature bytes already verified for this index and current digest
        STALE,          //!< at or below the finalized height, or unconfigured
        UNKNOWN_EPOCH,  //!< outside {current, current-1} or no set on this chain
        NOT_CHECKPOINT, //!< height not on the schedule, above the tip, or in the wrong epoch span
        TOO_SHALLOW,    //!< tip - height < CHECKPOINT_DEPTH: not signable yet
        BAD_INDEX,      //!< index >= n
        POOL_FULL,      //!< bounded pool is full of checkpoints newer than this one
        VERIFICATION_DEFERRED, //!< caller declined the expensive verification budget; retry is permitted
        BAD_SIGNATURE,  //!< BLS verification failed (wrong branch, wrong key, garbage)
    };
    static const char* AcceptName(Accept a);

    /**
     * Validate and store one signature. `tracker` must be synced to the
     * active tip of `chain`. Cheap checks precede the BLS verification.
     * If supplied, consume_verification_budget is called once, immediately
     * before BLS decoding/verification, never for cheap rejects or exact
     * verified duplicates. A false result leaves all still-valid signatures
     * untouched (ordinary finalized/fork cleanup may already have occurred).
     */
    Accept Submit(const FinalitySig& sig, const FinalityTracker& tracker, const CChain& chain,
                  const Consensus::Params& params,
                  const BridgeStateIndex* bridge_index = nullptr,
                  const std::function<bool()>& consume_verification_budget = {});

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

    struct CheckpointStatus {
        modern::FinalizedBlock checkpoint;
        uint256 signing_set_hash;
        uint32_t validator_count{0};
        uint32_t quorum_count{0};
        uint64_t total_weight{0};
        uint64_t quorum_weight{0};
        uint64_t signed_weight{0};
        std::vector<uint32_t> signer_indices;
    };

    /** Verified local observations, newest first. Excludes finalized, shallow,
     * orphaned, or no-longer-retained epoch slots, even before the next Submit.
     * Does not count unverified transport buffers or imply online status.
     * Caller holds cs_main and has synced the supplied tracker to this chain. */
    std::vector<CheckpointStatus> VerifiedCheckpoints(
        const FinalityTracker& tracker, const CChain& chain,
        const Consensus::Params& params,
        const BridgeStateIndex* bridge_index = nullptr) const;

    /** Exact already-verified wire messages eligible for bounded network
     * retransmission. Never signs, changes journals, or mutates consensus.
     * Same chain/locking preconditions as VerifiedCheckpoints. */
    std::vector<FinalitySig> RelayableSignatures(
        const FinalityTracker& tracker, const CChain& chain,
        const Consensus::Params& params,
        const BridgeStateIndex* bridge_index = nullptr) const;

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
 * key the checkpoint's epoch snapshot records for this validator. Retained
 * rotation keys share one validator-scoped journal and anti-repeat watermark.
 */
class FinalitySigner
{
public:
    static constexpr size_t MAX_KEYS{4};

    //! Default timing is unchanged for offline/scaled tests. Live mainnet
    //! staking explicitly supplies its additional local signing wait.
    explicit FinalitySigner(FinalitySigningPolicy policy = {}) : m_signing_policy{policy} {}

    void SetKey(const bls::SecretKey& key, const modern::ValidatorKeyBytes& validator_key)
    {
        std::string error;
        SetKeys({key}, validator_key, error);
    }
    //! Offline/test setup. Invalid empty/oversized collections leave the signer unchanged.
    bool SetKeys(const std::vector<bls::SecretKey>& keys,
                 const modern::ValidatorKeyBytes& validator_key, std::string& error);
    /** Arm a production signer with its durable, validator-identity-scoped
     * journal. A corrupt, unreadable, foreign, or otherwise unsafe existing
     * record is rejected before the key is armed. */
    bool SetKeyPersistent(const bls::SecretKey& key,
                          const modern::ValidatorKeyBytes& validator_key,
                          const uint256& chain_domain,
                          const fs::path& store_directory,
                          std::string& error);
    bool SetKeysPersistent(const std::vector<bls::SecretKey>& keys,
                           const modern::ValidatorKeyBytes& validator_key,
                           const uint256& chain_domain,
                           const fs::path& store_directory,
                           std::string& error);
    /** Explicit, local operator trust for one exact, intact orphan-vote
     * journal. This is NOT a quorum proof and does not revoke an old vote or
     * enforce a block checkpoint. The validator target is mandatory. No
     * journal write occurs here; all active-chain conditions are rechecked
     * before moving only the lock. A failed setter leaves the signer and any
     * previous plan unchanged. The plan is absent by default, is not persisted,
     * and is cleared on successful key reload or recovery. */
    bool SetOperatorTrustedRecovery(const Consensus::FinalitySignerRecovery& recovery,
                                    std::string& error);
    bool HasKey() const { return !m_keys.empty(); }
    int LastSignedHeight() const { return m_last_signed; }
    const std::string& LastError() const { return m_error.empty() ? m_key_error : m_error; }

    //! Read the already-open journal's public state only. Never opens a store,
    //! signs, rebases a lock, or changes the recovery policy. Caller holds
    //! cs_main and has synced tracker to chain; call after MaybeSign to report
    //! the actual post-attempt state, including a failed durable write.
    interfaces::FinalityRecoveryStatus RecoveryStatus(
        const FinalityTracker& tracker, const CChain& chain,
        const Consensus::Params& params,
        const BridgeStateIndex* bridge_index = nullptr) const;

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
    /**
     * The chain-pinned one-time recovery (Consensus::FinalitySignerRecovery)
     * for a journal whose ancestry lock is exactly the pinned orphaned vote.
     * NOT_APPLICABLE when any pinned fact does not match this journal, this
     * chain, this epoch state, or the active chain (the caller then fails
     * closed as usual; `reason` names the refusal when the journal itself is
     * the pinned incident, so an operator can tell a pending recovery from
     * an inapplicable one); APPLIED after the durable lock moved; FAILED
     * after a durable-write failure (already reported through Fail()).
     */
    enum class PinnedRecovery { NOT_APPLICABLE, APPLIED, FAILED };
    PinnedRecovery TryPinnedRecovery(
        const Consensus::FinalitySignerRecovery& pin,
        const FinalitySignerState& persisted,
        const FinalityTracker::State& state,
        const CChain& chain,
        const Consensus::Params& params,
        const uint256& chain_domain,
        const BridgeStateIndex* bridge_index,
        std::string& reason);
    PinnedRecovery TryOperatorTrustedRecovery(
        const FinalitySignerState& persisted,
        const FinalityTracker::State& state,
        const CChain& chain,
        const Consensus::Params& params,
        const uint256& chain_domain,
        const BridgeStateIndex* bridge_index,
        std::string& reason);
    enum class RecoveryTrust { HARDENED_CHECKPOINT, OPERATOR_TRUSTED };
    PinnedRecovery TryRecoveryAnchor(
        const Consensus::FinalitySignerRecovery& pin,
        const FinalitySignerState& persisted,
        const FinalityTracker::State& state,
        const CChain& chain,
        const Consensus::Params& params,
        const uint256& chain_domain,
        const BridgeStateIndex* bridge_index,
        RecoveryTrust trust,
        std::string& reason);
    void Fail(std::string error, bool permanent = true);
    const bls::SecretKey* KeyFor(const std::array<unsigned char, bls::PUBKEY_SIZE>& pubkey) const;

    struct SigningKey {
        bls::SecretKey secret;
        std::array<unsigned char, bls::PUBKEY_SIZE> pubkey;
    };
    FinalitySigningPolicy m_signing_policy;
    std::vector<SigningKey> m_keys;
    std::optional<Consensus::FinalitySignerRecovery> m_operator_trusted_recovery;
    modern::ValidatorKeyBytes m_validator_key{};
    int m_last_signed{-1};
    FinalitySignerStore m_store;
    std::string m_error;
    //! Recoverable key availability is separate from durable safety failures.
    std::string m_key_error;
    bool m_permanent_error{false};
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_SIGNATURE_H

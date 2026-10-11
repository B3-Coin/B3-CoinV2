// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.
#ifndef B3COIN_NODE_FINALITY_SIGNER_STORE_H
#define B3COIN_NODE_FINALITY_SIGNER_STORE_H

#include <consensus/finality_signer_recovery.h>
#include <modern/finality_key.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <optional>
#include <limits>
#include <string>

namespace node {

/**
 * Which finality lineage a signer journal protects. Each namespace has its own
 * file name and its own file magic, so a journal written for one namespace is
 * never opened, read, reset or replaced as the other: a V1 orphan lock cannot
 * block fresh-lineage signing, and no fresh-lineage write can move a V1 lock.
 *
 *  - CHAIN_DOMAIN_V1: the V1 lineage (modern::FinalityDigest). The scope id is
 *    the chain domain. File "finality_signer_v1_<domain>_<vk>.dat", magic
 *    "B3FSLOCK". This is every journal written before bridge V2 S3, unchanged.
 *  - FRESH_LINEAGE_V2: the fresh bridge-V2 lineage (modern::FinalityDigestV2,
 *    finality-dependency section 6.4). The scope id is the lineage id. File
 *    "finality_signer_v2_lineage_<lineage id>_<vk>.dat", magic "B3FSLIN2".
 *
 * Both hex fields of a file name use uint256::GetHex(). The record layout,
 * checksum, version byte and every safety rule are the same in both.
 */
enum class FinalitySignerNamespace : uint8_t {
    CHAIN_DOMAIN_V1 = 1,
    FRESH_LINEAGE_V2 = 2,
};

/**
 * Crash-safe anti-equivocation state for one (scope id, validator key) in one
 * FinalitySignerNamespace. No private key material is ever written.
 *
 * `last_signed_*` prevents signing the same or a lower height with a
 * different object. `lock_*` additionally prevents signing a higher
 * checkpoint on a non-descendant fork. Normally both advance together. The
 * lock may move ahead independently after the node has validated a newer,
 * exact-same-set quorum certificate included on the active chain, or through
 * the separate exact-incident recovery write below. The caller must establish
 * that recovery's hardened-checkpoint or explicit operator-trust conditions;
 * the store itself cannot establish agreement on the chosen chain.
 */
struct FinalitySignerState {
    //! Scope id: the chain domain (CHAIN_DOMAIN_V1) or the fresh lineage id
    //! (FRESH_LINEAGE_V2). The field keeps its V1 name and disk position.
    uint256 chain_domain{};
    modern::ValidatorKeyBytes validator_key{};
    int32_t last_signed_height{-1};
    uint256 last_signed_block_hash{};
    uint256 last_signed_digest{};
    int32_t lock_height{-1};
    uint256 lock_block_hash{};
    uint256 lock_digest{};
    uint64_t lock_epoch{std::numeric_limits<uint64_t>::max()};
    uint256 lock_signing_set_hash{};
    uint256 lock_successor_set_hash{};

    friend bool operator==(const FinalitySignerState& a,
                           const FinalitySignerState& b)
    {
        return a.chain_domain == b.chain_domain &&
               a.validator_key == b.validator_key &&
               a.last_signed_height == b.last_signed_height &&
               a.last_signed_block_hash == b.last_signed_block_hash &&
               a.last_signed_digest == b.last_signed_digest &&
               a.lock_height == b.lock_height &&
               a.lock_block_hash == b.lock_block_hash &&
               a.lock_digest == b.lock_digest &&
               a.lock_epoch == b.lock_epoch &&
               a.lock_signing_set_hash == b.lock_signing_set_hash &&
               a.lock_successor_set_hash == b.lock_successor_set_hash;
    }
};

/**
 * Atomic, checksummed journal. Open() distinguishes an absent file from an
 * invalid one; an invalid/mismatched file always fails closed. Every update
 * verifies the on-disk predecessor before fsync + rename-over + directory
 * sync, so a stale process cannot silently roll the watermark backward.
 */
class FinalitySignerStore
{
public:
    /** Open the journal of (`scope_id`, `validator_key`) in namespace `ns`.
     * The default namespace keeps every V1 caller byte-identical. A file of
     * the other namespace is never consulted; one copied to this path fails
     * closed on its magic. */
    bool Open(const fs::path& directory, const uint256& scope_id,
              const modern::ValidatorKeyBytes& validator_key,
              std::string& error,
              FinalitySignerNamespace ns = FinalitySignerNamespace::CHAIN_DOMAIN_V1);

    bool IsOpen() const { return m_open; }
    bool IsAbsent() const { return m_open && !m_state.has_value(); }
    const std::optional<FinalitySignerState>& State() const { return m_state; }
    const fs::path& Path() const { return m_path; }
    //! The scope id; under FRESH_LINEAGE_V2 this is the lineage id.
    const uint256& ChainDomain() const { return m_chain_domain; }
    const uint256& ScopeId() const { return m_chain_domain; }
    FinalitySignerNamespace Namespace() const { return m_namespace; }

    /** Establish a durable pre-signing marker. Only an absent file may become
     * empty; an existing valid state is left untouched. */
    bool InitializeEmpty(std::string& error);

    /** Persist a vote before it is submitted locally or relayed. Equal
     * height is idempotent only for the exact same block and digest. */
    bool CommitSignedCheckpoint(int height, const uint256& block_hash,
                                const uint256& digest, uint64_t epoch,
                                const uint256& signing_set_hash,
                                const uint256& successor_set_hash,
                                std::string& error);

    /** Move only the ancestry lock to a strictly newer, already validated and
     * included finality checkpoint signed by the exact same epoch/set as the
     * old lock. The last actual vote is retained. */
    bool CommitCertifiedAnchor(int height, const uint256& block_hash,
                               const uint256& digest, uint64_t epoch,
                               const uint256& signing_set_hash,
                               const uint256& successor_set_hash,
                               std::string& error);

    /** FRESH_LINEAGE_V2 only: advance only the ancestry lock by exactly one
     * fresh epoch, to a strictly higher, already validated and included
     * checkpoint whose signing set is the durable lock's exact successor.
     * The caller must establish that the old lock is canonical and validate
     * the historical handover and descendant certificate evidence before
     * each step; this store cannot validate that evidence itself. An orphaned
     * lock still requires CommitCertifiedAnchor's exact-same-epoch/set rule.
     * No last_signed_* field is changed, so the last actual vote survives
     * every catch-up step and restart. Skipped epochs, duplicate transitions
     * and a changed on-disk predecessor are refused. CHAIN_DOMAIN_V1 refuses
     * this helper entirely; its existing lineage rules are unchanged. */
    bool CommitLineageCatchUp(int height, const uint256& block_hash,
                              const uint256& digest, uint64_t epoch,
                              const uint256& signing_set_hash,
                              const uint256& successor_set_hash,
                              std::string& error);

    /** The exact-incident one-time recovery of an orphaned vote (see
     * Consensus::FinalitySignerRecovery). Refuses unless the durable state is
     * EXACTLY the pinned incident: the same checkpoint as both the last vote
     * and the ancestry lock, the same epoch and validator sets, and no newer
     * record. Then moves ONLY the ancestry lock to the pinned anchor (strictly
     * newer); the last actual vote is retained. `anchor_digest` is the exact
     * finality digest this node derives for the anchor checkpoint. The signer
     * must validate the selected trust mode before calling this write.
     * CHAIN_DOMAIN_V1 only: a fresh-lineage journal refuses it. */
    bool CommitPinnedRecoveryAnchor(
        const Consensus::FinalitySignerRecovery& recovery,
        const uint256& anchor_digest, std::string& error);

    /** The journal path of (`scope_id`, `validator_key`) in namespace `ns`;
     * empty for a value outside FinalitySignerNamespace. */
    static fs::path StatePath(
        const fs::path& directory, const uint256& scope_id,
        const modern::ValidatorKeyBytes& validator_key,
        FinalitySignerNamespace ns = FinalitySignerNamespace::CHAIN_DOMAIN_V1);

    /** Read-only configuration preflight. Unlike Open(), creates no directory
     * and never initializes a missing journal. Checks the exact intact incident
     * only, not anchor agreement or chain eligibility. The live signer and
     * durable write must recheck their own current state before applying it.
     * Both recovery helpers apply to the CHAIN_DOMAIN_V1 journal only. */
    static bool CheckOperatorRecoveryIncident(
        const fs::path& directory, const uint256& chain_domain,
        const modern::ValidatorKeyBytes& validator_key,
        const Consensus::FinalitySignerRecovery& recovery, std::string& error);
    static bool MatchesOperatorRecoveryIncident(
        const FinalitySignerState& state,
        const Consensus::FinalitySignerRecovery& recovery, std::string& error);

private:
    bool Commit(const FinalitySignerState& next, std::string& error);

    bool m_open{false};
    FinalitySignerNamespace m_namespace{FinalitySignerNamespace::CHAIN_DOMAIN_V1};
    fs::path m_path;
    uint256 m_chain_domain{};
    modern::ValidatorKeyBytes m_validator_key{};
    std::optional<FinalitySignerState> m_state;
};

} // namespace node

#endif // B3COIN_NODE_FINALITY_SIGNER_STORE_H

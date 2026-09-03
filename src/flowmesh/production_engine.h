// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_PRODUCTION_ENGINE_H
#define B3COIN_FLOWMESH_PRODUCTION_ENGINE_H

#include <crypto/bls.h>
#include <flowmesh/batch.h>
#include <flowmesh/bls_certificate.h>
#include <flowmesh/deposit.h>
#include <flowmesh/fee_allocation.h>
#include <flowmesh/market.h>
#include <flowmesh/production_commitment.h>
#include <flowmesh/state.h>
#include <modern/flowmesh_checkpoint.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace flowmesh {

/**
 * Production FlowMesh epoch log.
 *
 * This is deliberately parallel to, and independent of, the older regtest
 * Schnorr prototype in microblock.h/sync.h. Production entries are bound to
 * an anchored all-FN BLS seat set, use the fixed 2/3+1 BLS certificate, carry
 * a MarketId and epoch, and can only execute through a mandatory fee context
 * derived from that exact seat set.
 */
inline constexpr int32_t FLOWMESH_PRODUCTION_MIN_ANCHOR_DEPTH{30};
inline constexpr const char* FLOWMESH_PRODUCTION_ACTIONS_TAG{
    "B3/FLOWMESH/ACTIONS/V1"};
inline constexpr const char* FLOWMESH_PRODUCTION_HANDOFF_TAG{
    "B3/FLOWMESH/HANDOFF/V1"};
inline constexpr const char* FLOWMESH_PRODUCTION_PROPOSAL_TAG{
    "B3/FLOWMESH/PROPOSAL/V1"};
inline constexpr const char* FLOWMESH_PRODUCTION_RESULT_TAG{
    "B3/FLOWMESH/RESULT/V1"};
inline constexpr const char* FLOWMESH_PRODUCTION_DEPOSIT_ACCEPTANCE_TAG{
    "B3/FLOWMESH/DEPOSIT-ACCEPTANCE/V1"};
inline constexpr const char* FLOWMESH_PRODUCTION_WITHDRAWAL_SHARD_TAG{
    "B3/FLOWMESH/WITHDRAWAL-SHARD/V1"};
inline constexpr const char* FLOWMESH_SEAT_REWARD_CLAIM_TAG{
    "B3/FLOWMESH/SEAT-REWARD-CLAIM/V1"};
inline constexpr size_t FLOWMESH_SEAT_REWARD_CREDENTIAL_SIZE{
    bls::SIGNATURE_SIZE};

struct ProductionEntryCore {
    uint16_t version{FLOWMESH_PRODUCTION_ENTRY_VERSION_V1};
    uint8_t kind{static_cast<uint8_t>(ProductionEntryKind::EXECUTION)};
    uint256 domain;
    MarketId market_id;
    uint64_t epoch{0};
    //! Recomputed from ActiveFnBlsSeatSet; never caller-selected.
    uint256 seat_set_hash;
    //! Global production-log sequence (handoffs consume a sequence too).
    uint64_t sequence{0};
    uint256 parent_hash;
    AnchorRef anchor;
    uint256 previous_state_root;
    std::vector<Action> actions;
    uint256 actions_root;
    uint256 result_root;
    uint256 state_root;
    //! Global per-market typed-effect range produced by this exact entry.
    uint64_t effect_start{0};
    uint32_t effect_count{0};
    uint256 effect_root;

    //! EPOCH_HANDOFF-only commitment. All three fields are null/zero for an
    //! EXECUTION entry.
    uint64_t next_epoch{0};
    AnchorRef next_anchor;
    uint256 next_seat_set_hash;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << version << kind << domain << market_id << epoch << seat_set_hash
          << sequence << parent_hash << anchor << previous_state_root;
        WriteCompactSize(s, actions.size());
        for (const Action& action : actions) s << action;
        s << actions_root << result_root << state_root << effect_start
          << effect_count << effect_root << next_epoch << next_anchor
          << next_seat_set_hash;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        // `DecodeProductionEntry` checks the original byte span before this
        // method is entered. Keep the same check here so direct stream use
        // cannot bypass the 2 MiB pre-allocation bound.
        if (s.size() > FLOWMESH_V1_MAX_MICROBLOCK_BYTES) {
            throw std::ios_base::failure("FlowMesh production entry exceeds 2 MiB");
        }
        s >> version >> kind >> domain >> market_id >> epoch >> seat_set_hash
          >> sequence >> parent_hash >> anchor >> previous_state_root;
        const uint64_t count{ReadCompactSize(s)};
        if (count > FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS) {
            throw std::ios_base::failure("FlowMesh production entry has too many actions");
        }
        actions.clear();
        actions.reserve(count);
        for (uint64_t i{0}; i < count; ++i) {
            Action action;
            s >> action;
            actions.push_back(std::move(action));
        }
        s >> actions_root >> result_root >> state_root >> effect_start
          >> effect_count >> effect_root >> next_epoch >> next_anchor
          >> next_seat_set_hash;
    }

    std::optional<ProductionEntryCommitmentV1> Commitment() const;
    uint256 GetHash() const;
};

uint256 ComputeProductionActionsRoot(std::span<const Action> actions);
uint256 ComputeProductionHandoffResultRoot(const ProductionEntryCore& entry);

//! Derive the exact append-only checkpoint effects produced by one execution.
//! All context fields come from `entry`; all value facts come from canonical
//! execution, never a caller-supplied effect object.
std::optional<std::vector<modern::FlowMeshEffectV1>>
DeriveProductionEffects(const ProductionEntryCore& entry,
                        const BatchResult& result);

//! Commit the legacy execution result plus the exact typed production effects.
std::optional<uint256> ComputeProductionExecutionResultRoot(
    const uint256& batch_result_commitment,
    std::span<const modern::FlowMeshEffectV1> effects);

//! Frozen deterministic ids/shards used by DeriveProductionEffects.
uint256 ComputeProductionDepositAcceptanceId(
    const modern::FlowMeshDepositAcceptanceV1& acceptance_without_id);
uint16_t ComputeProductionWithdrawalChangeShard(
    const VaultId& vault_id, const uint256& receipt_id);

//! Exact codec entry points. Decode refuses >2 MiB before constructing a
//! stream or allocating an action vector and requires complete consumption.
std::optional<std::vector<unsigned char>> EncodeProductionEntry(
    const ProductionEntryCore& entry);
std::optional<ProductionEntryCore> DecodeProductionEntry(
    std::span<const unsigned char> bytes);

struct ProductionAnchorContext {
    //! Height of the canonical B3 tip used for the explicit 30-block check.
    int32_t b3_tip_height{-1};
    //! Last accepted production-log anchor, if any.
    std::optional<AnchorRef> previous_anchor;
    //! Supplies canonical-hash and any stricter network-specific checks.
    const AnchorPolicy* policy{nullptr};
};

enum class ProductionAnchorCheck : uint8_t {
    OK = 0,
    NULL_ANCHOR,
    FUTURE_ANCHOR,
    NOT_DEEP_ENOUGH,
    NON_MONOTONIC,
    SAME_HEIGHT_DIFFERENT_HASH,
    NOT_CANONICAL,
    POLICY_REJECTED,
};

ProductionAnchorCheck CheckProductionAnchor(
    const AnchorRef& anchor, const ProductionAnchorContext& context);

enum class ProductionEntryCheck : uint8_t {
    OK = 0,
    BAD_VERSION_OR_KIND,
    ENTRY_TOO_LARGE,
    TOO_MANY_ACTIONS,
    NON_CANONICAL_ACTIONS,
    INVALID_SEAT_SET,
    WRONG_DOMAIN,
    WRONG_MARKET,
    WRONG_EPOCH,
    WRONG_SEAT_SET,
    WRONG_SEQUENCE,
    WRONG_PARENT,
    BAD_ANCHOR,
    WRONG_PREVIOUS_STATE_ROOT,
    WRONG_ACTIONS_ROOT,
    WRONG_RESULT_ROOT,
    WRONG_STATE_ROOT,
    WRONG_EFFECT_RANGE,
    CHAIN_SETTLEMENT_MISMATCH,
    BAD_FEE_CONTEXT,
    EXECUTION_FAILED,
    BAD_HANDOFF_FIELDS,
    INVALID_NEXT_SEAT_SET,
    WRONG_NEXT_EPOCH,
    WRONG_NEXT_ANCHOR,
    WRONG_NEXT_SEAT_SET,
    BAD_CERTIFICATE,
    HANDOFF_NOT_CONNECTED,
    EPOCH_PAUSED,
};

const char* ProductionEntryCheckName(ProductionEntryCheck check);

//! The only production fee-context constructor. It copies every anchored
//! active member, in the certificate bitmap order, into the fee allocation
//! context and rejects any mismatch/non-canonical set.
std::optional<FlowMeshFeeContext> BuildProductionFlowMeshFeeContext(
    const uint256& domain, const ActiveFnBlsSeatSet& active_seats,
    const uint256& treasury_owner_commitment);

struct BuiltProductionExecution {
    ProductionEntryCore entry;
    FlowMeshState next_state;
    BatchResult result;
    std::vector<modern::FlowMeshEffectV1> effects;
    std::vector<WithdrawalSettlementFactV1> settlements;
};

struct ExecutedProductionEntry {
    FlowMeshState next_state;
    BatchResult result;
    std::vector<modern::FlowMeshEffectV1> effects;
    std::vector<WithdrawalSettlementFactV1> settlements;
};

enum class SeatRewardClaimCredentialCheck : uint8_t {
    OK = 0,
    NOT_REWARD_CLAIM,
    MALFORMED_ACTION,
    INVALID_HISTORICAL_SEAT_SET,
    MEMBER_NOT_IN_SET,
    WRONG_REWARD_ACCOUNT,
    WRONG_KEY,
    MALFORMED_SIGNATURE,
    BAD_SIGNATURE,
};

/**
 * Reward claims use the historical epoch member's BLS key. The digest binds
 * the complete historical set identity and semantic action, so spending or
 * rebinding the current FN output cannot revoke an already-earned reward and
 * cannot redirect it.
 */
uint256 ProductionSeatRewardClaimDigest(
    const uint256& domain, const uint256& execution_config_id,
    const MarketId& market_id, const ActiveFnBlsSeatSet& historical_seats,
    const ActiveFnBlsSeat& historical_member, const Action& action);

SeatRewardClaimCredentialCheck CheckProductionSeatRewardClaimCredential(
    const Action& action, const uint256& domain,
    const uint256& execution_config_id, const MarketId& market_id,
    const ActiveFnBlsSeatSet& historical_seats,
    const ActiveFnBlsSeat& historical_member);

bool SignProductionSeatRewardClaim(
    const bls::SecretKey& key, const uint256& domain,
    const uint256& execution_config_id, const MarketId& market_id,
    const ActiveFnBlsSeatSet& historical_seats,
    const ActiveFnBlsSeat& historical_member, Action& action);

//! Production admission helper: deposits remain chain-authorized, reward
//! claims use historical BLS membership, and every other signed action keeps
//! the existing BIP340 credential rule.
bool CheckProductionActionCredential(
    const Action& action, const uint256& domain,
    const uint256& execution_config_id, const MarketId& market_id,
    const ActiveFnBlsSeatSet* historical_reward_seats = nullptr,
    const ActiveFnBlsSeat* historical_reward_member = nullptr);

class ProductionEpochGate;

std::optional<BuiltProductionExecution> BuildProductionExecutionEntry(
    const FlowMeshState& previous_state, const uint256& domain,
    const MarketId& market_id, const ActiveFnBlsSeatSet& active_seats,
    const ProductionEpochGate& epoch_gate, uint64_t sequence,
    uint64_t effect_start,
    const uint256& parent_hash, const AnchorRef& anchor,
    const ProductionAnchorContext& anchor_context,
    const uint256& treasury_owner_commitment, std::span<const Action> actions,
    const DepositVerifier* deposits, ProductionEntryCheck& check);

std::optional<ExecutedProductionEntry> ExecuteProductionEntry(
    const FlowMeshState& previous_state, const ProductionEntryCore& entry,
    const uint256& expected_domain, const MarketId& expected_market,
    const ActiveFnBlsSeatSet& active_seats,
    const ProductionEpochGate& epoch_gate, uint64_t expected_sequence,
    uint64_t expected_effect_start, const uint256& expected_parent,
    const ProductionAnchorContext& anchor_context,
    const uint256& treasury_owner_commitment, const DepositVerifier* deposits,
    ProductionEntryCheck& check);

std::optional<ProductionEntryCore> BuildProductionHandoffEntry(
    const FlowMeshState& current_state, const uint256& domain,
    const MarketId& market_id, const ActiveFnBlsSeatSet& outgoing_seats,
    const ActiveFnBlsSeatSet& next_seats, uint64_t sequence,
    uint64_t effect_start, const uint256& parent_hash, const AnchorRef& anchor,
    const ProductionAnchorContext& anchor_context, ProductionEntryCheck& check);

//! Existing aggregate certificate primitive applied to a production entry.
BlsCertificateContext ProductionCertificateContext(
    const ProductionEntryCore& entry);
BlsCertificateCheck CheckProductionEntryCertificate(
    const ProductionEntryCore& entry, const ActiveFnBlsSeatSet& active_seats,
    const BlsMicroblockCertificate& certificate);
BlsCertificateAssemblyCheck AssembleProductionEntryCertificate(
    const ProductionEntryCore& entry, const ActiveFnBlsSeatSet& active_seats,
    std::span<const IndexedBlsSignature> partials,
    BlsMicroblockCertificate& certificate_out);

/**
 * Construct the unique compact type-8 publication of one certified
 * production entry. The B3 checkpoint link is publication-only; every other
 * field is copied from, or transitively fixed by, the one-round production
 * certificate and exact anchored seat set.
 */
std::optional<modern::FlowMeshCheckpointRecordV1>
BuildProductionCheckpointRecord(
    const ProductionEntryCore& entry,
    const BlsMicroblockCertificate& certificate,
    const ActiveFnBlsSeatSet& active_seats,
    const modern::FlowMeshCheckpointId& previous_checkpoint_id);

struct ProductionSignPosition {
    uint64_t epoch{0};
    uint64_t sequence{0};

    friend bool operator<(const ProductionSignPosition& a,
                          const ProductionSignPosition& b)
    {
        return a.epoch < b.epoch ||
               (a.epoch == b.epoch && a.sequence < b.sequence);
    }
};

enum class ProductionLockResult : uint8_t {
    LOCKED = 0,
    ALREADY_LOCKED_SAME,
    CONFLICT,
    STORAGE_FAILURE,
};

/**
 * Durable compare-and-set journal. Implementations must persist a first lock
 * before returning LOCKED. There is intentionally no erase/unlock operation:
 * a `(epoch, sequence)` lock is permanent.
 */
class DurableProductionLockJournal
{
public:
    virtual ~DurableProductionLockJournal() = default;
    virtual ProductionLockResult LockOnce(const ProductionSignPosition& position,
                                          const uint256& entry_hash) = 0;
};

class ProductionSigningGuard
{
public:
    explicit ProductionSigningGuard(DurableProductionLockJournal& journal)
        : m_journal{journal}
    {
    }

    ProductionLockResult Lock(const ProductionEntryCore& entry);

private:
    DurableProductionLockJournal& m_journal;
};

struct ProductionProposalEnvelope {
    ProductionEntryCore entry;
    uint32_t round{0};
    uint32_t proposer_seat_index{0};
    std::array<unsigned char, bls::SIGNATURE_SIZE> proposer_signature{};
};

uint32_t ProductionProposerSeatIndex(uint64_t sequence, uint32_t round,
                                     size_t seat_count);
uint256 ProductionProposalDigest(const ProductionEntryCore& entry,
                                 uint32_t round);

enum class ProductionProposalCheck : uint8_t {
    OK = 0,
    INVALID_SEAT_SET,
    WRONG_DOMAIN,
    WRONG_MARKET,
    WRONG_EPOCH,
    WRONG_SEAT_SET,
    WRONG_ROUND,
    WRONG_PROPOSER,
    NOT_PROPOSER_KEY,
    LOCK_CONFLICT,
    LOCK_STORAGE_FAILURE,
    MALFORMED_SIGNATURE,
    BAD_SIGNATURE,
};

std::optional<ProductionProposalEnvelope> SignProductionProposal(
    const bls::SecretKey& proposer_key, const ProductionEntryCore& entry,
    uint32_t round, const ActiveFnBlsSeatSet& active_seats,
    ProductionSigningGuard& guard, ProductionProposalCheck& check);

ProductionProposalCheck CheckProductionProposal(
    const ProductionProposalEnvelope& proposal, const uint256& expected_domain,
    const MarketId& expected_market, uint64_t expected_epoch,
    uint32_t expected_round, const ActiveFnBlsSeatSet& active_seats);

//! Guarded indexed attestation helper. Every production signer must durably
//! lock `(epoch,sequence)->entry_hash` before a signature is returned.
std::optional<IndexedBlsSignature> SignProductionEntryAttestation(
    const bls::SecretKey& key, uint32_t seat_index,
    const ProductionEntryCore& entry, const ActiveFnBlsSeatSet& active_seats,
    ProductionSigningGuard& guard, ProductionLockResult& lock_result);

/**
 * Epoch activation gate. A certified outgoing handoff stages a next set and
 * pauses execution. Only an explicit caller report that the exact handoff is
 * B3-connected switches the active epoch/set. Invalid under-cap next sets
 * also pause; the old set may not run past an unsatisfied epoch boundary.
 */
class ProductionEpochGate
{
public:
    ProductionEpochGate(const uint256& domain, const MarketId& market_id,
                        const ActiveFnBlsSeatSet& initial_seats);

    bool Valid() const { return m_valid; }
    bool Paused() const { return m_paused; }
    uint64_t ActiveEpoch() const { return m_active_epoch; }
    const uint256& ActiveSeatSetHash() const { return m_active_set_hash; }
    bool CanExecute(const ActiveFnBlsSeatSet& seats) const;

    ProductionEntryCheck StageHandoff(
        const FlowMeshState& current_state, const ProductionEntryCore& handoff,
        const ActiveFnBlsSeatSet& outgoing_seats,
        const ActiveFnBlsSeatSet& next_seats,
        const BlsMicroblockCertificate& outgoing_certificate,
        uint64_t expected_sequence, uint64_t expected_effect_start,
        const uint256& expected_parent,
        const ProductionAnchorContext& anchor_context);

    //! Trusted B3 integration reports that this exact handoff entry connected.
    //! Wrong hashes do nothing. There is no alternate activation path.
    bool MarkHandoffB3Connected(const uint256& handoff_entry_hash);

private:
    struct Pending {
        uint256 handoff_hash;
        uint64_t next_epoch{0};
        uint256 next_set_hash;
    };

    uint256 m_domain;
    MarketId m_market_id;
    bool m_valid{false};
    bool m_paused{false};
    uint64_t m_active_epoch{0};
    uint256 m_active_set_hash;
    std::optional<Pending> m_pending;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_PRODUCTION_ENGINE_H

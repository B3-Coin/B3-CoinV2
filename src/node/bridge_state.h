// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_BRIDGE_STATE_H
#define B3COIN_NODE_BRIDGE_STATE_H

#include <bridge/admission.h>
#include <bridge/burn_gate_v2.h>
#include <bridge/eth_light_client.h>
#include <bridge/proof.h>
#include <bridge/reserve_v2.h>
#include <bridge/state_observation.h>
#include <consensus/params.h>
#include <modern/withdrawal_tree.h>
#include <modern/withdrawal_v2.h>
#include <node/finality_fresh_lineage.h>
#include <primitives/block.h>
#include <primitives/transaction_identifier.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

class CBlockIndex;
class CChain;

namespace node {

class BlockManager;
class BridgeStateIndex;
class FinalityTracker;

/** Parent-derived ability of B3's current and next validator sets to produce
 * a withdrawal root accepted by the pinned Ethereum verifier. UNAVAILABLE is
 * operational (the parent finality state could not be reconstructed), while
 * NOT_READY is a deterministic consensus condition. */
enum class BridgeBurnReadiness {
    UNAVAILABLE,
    NOT_READY,
    READY,
};

//! True only for a syntactically decoded decentralized BRIDGE_BURN record,
//! decoded under the bridge V2 record set, so a burn carrying a route (d3)
//! exit block counts as well.
bool HasDecentralizedBridgeBurn(const CTransaction& tx);
bool HasDecentralizedBridgeBurn(const CBlock& block);

/** Fail closed for a decoded BRIDGE_BURN unless parent/projected finality is
 * READY. Malformed type-10 records remain the bridge verifier's responsibility
 * and cannot pass it. */
bool CheckBridgeBurnReadiness(const CTransaction& tx,
                              BridgeBurnReadiness readiness,
                              std::string& error);
bool CheckBridgeBurnReadiness(const CBlock& block,
                              BridgeBurnReadiness readiness,
                              std::string& error);

//! True for a transaction or block carrying a syntactically decoded record
//! that is judged on the fresh lineage (decoded under the bridge V2 record
//! set): kind 9 ETH_STATE_OBSERVATION (a LIVE VERIFIER_E verdict), kind 13
//! FINALITY_CONFLICT or kind 16 VAULT_SWEEP (the retired vault's gate).
//! Mempool admission, block connection, block assembly and wallet
//! prevalidation build the lineage context for such a record.
bool HasBridgeV2LineageRecord(const CTransaction& tx);
bool HasBridgeV2LineageRecord(const CBlock& block);

//! True for a transaction carrying a syntactically decoded kind 16
//! VAULT_SWEEP (decoded under the bridge V2 record set). A sweep passes the
//! retired vault's gate, so once a sticky conflict is set (row C4) it can
//! never be valid again on that chain, and ConnectTip evicts it from the
//! mempool with every V2 burn and kind 13 record.
bool HasBridgeV2VaultSweep(const CTransaction& tx);

//! True for a transaction carrying a decoded kind 6 BRIDGE_BURN that names
//! the B-23 retired launch vault entry at `height`: the migration is active
//! there (Consensus::Params::BridgeV2MigrationActive) and the burn's
//! registry_id is the launch entry id. Such a burn is refused at every
//! height from h_m (node changes section 7.6, "names the retired vault
//! entry"), so ConnectTip evicts it from the mempool once the block below
//! h_m connects. False on every network without a pinned successor.
bool HasBridgeV2RetiredEntryBurn(const CTransaction& tx,
                                 const Consensus::Params& params, int height);

//! True for a transaction carrying a syntactically decoded kind 13
//! FINALITY_CONFLICT (decoded under the bridge V2 record set). Once a
//! sticky conflict is set such a record can never be valid again on that
//! chain, so ConnectTip evicts it from the mempool with every V2 burn.
bool HasBridgeV2FinalityConflict(const CTransaction& tx);

/** Number of kind 9 subjects: VERIFIER_E, RESERVE_E and TOKEN_FLAGS
 * (bridge::ETH_OBSERVATION_SUBJECT_*, values 1-3) and the B-23 successor's
 * RESERVE_E_SUCCESSOR and TOKEN_FLAGS_SUCCESSOR (0x12 and 0x13, node changes
 * section 7.6). */
inline constexpr size_t BRIDGE_V2_OBSERVATION_SUBJECTS{5};

/** The slot of a kind 9 subject in BridgeObservationSlotsV2: 1, 2, 3, 0x12
 * and 0x13 map to 0..4; nullopt for any other value. */
inline constexpr std::optional<size_t> BridgeObservationSlotV2(
    const uint8_t subject)
{
    switch (subject) {
    case bridge::ETH_OBSERVATION_SUBJECT_VERIFIER: return 0;
    case bridge::ETH_OBSERVATION_SUBJECT_RESERVE: return 1;
    case bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS: return 2;
    case bridge::ETH_OBSERVATION_SUBJECT_RESERVE_SUCCESSOR: return 3;
    case bridge::ETH_OBSERVATION_SUBJECT_TOKEN_FLAGS_SUCCESSOR: return 4;
    }
    return std::nullopt;
}

/**
 * One connected kind 9 observation (node changes section 8.2): the anchor
 * it was proven against, the anchor's execution timestamp `t_obs`, the
 * recorded verdict byte (VERIFIER_E only, else zero), the decoded state and
 * the active B3 block that connected it.
 */
struct BridgeObservationRecordV2 {
    uint8_t subject{0};
    uint256 anchor_hash{};
    uint64_t anchor_block{0};
    int64_t t_obs{0};
    uint8_t verdict{0};
    bridge::ObservedStateV2 state{};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeObservationRecordV2&,
                           const BridgeObservationRecordV2&) = default;
};

//! The latest observation per subject, indexed by BridgeObservationSlotV2
//! (subject - 1 for subjects 1-3, then 0x12 and 0x13).
using BridgeObservationSlotsV2 =
    std::array<std::optional<BridgeObservationRecordV2>,
               BRIDGE_V2_OBSERVATION_SUBJECTS>;

//! One proven Vault V2 deposit, minted as bUSD V2 (node changes section 7.1).
struct BridgeProvenDepositV2 {
    uint64_t deposit_id{0};
    CAmount amount{0};

    friend bool operator==(const BridgeProvenDepositV2&,
                           const BridgeProvenDepositV2&) = default;
};

/**
 * The sticky bridge V2 conflict (node changes sections 8.4-8.5): the first
 * E5 (a LIVE VERIFIER_E whose latestCertId is not B3's certificate), E8
 * (a RESERVE_E failing invariant R) or kind 13 (FINALITY_CONFLICT evidence,
 * JudgeFinalityConflictV2) on the active chain. It closes the burn gate
 * (row C4) until the block that set it is disconnected; nothing else clears
 * it, and once one is set no other can replace it (first wins).
 *
 * `reason` is bridge::BRIDGE_V2_CONFLICT_E5, _E8 or _KIND13. `anchor_block`
 * is the execution block of the E5 or E8 observation and 0 for kind 13. The
 * evidence fields are kind 13's only (zero otherwise): the certId
 * (keccak256 of the FinalizedBlockV2) of the conflicting certificate, its
 * checkpoint index and its epoch. They are the trailing members, so
 * positional initializers of an E5 or E8 conflict stay valid.
 */
struct BridgeV2Conflict {
    uint8_t reason{0};
    int height{-1};
    uint256 block_hash{};
    uint64_t anchor_block{0};
    uint256 evidence_cert_id{};
    uint64_t evidence_checkpoint_index{0};
    uint64_t evidence_epoch{0};

    friend bool operator==(const BridgeV2Conflict&,
                           const BridgeV2Conflict&) = default;
};

/**
 * What the bridge V2 rules read of the fresh finality lineage for one
 * candidate block (node changes section 7.4): the fresh state projected for
 * the candidate height and the candidate chain's block hashes (`hash_at`
 * answers heights up to the parent).
 *
 * Block connection, mempool admission, block assembly, wallet
 * prevalidation and the deposit status (BridgeV2DepositsClosedReason through
 * interfaces::Chain::bridgeV2DepositStatus and getbridgev2info) build it
 * with MakeBridgeV2LineageContext. Replay of blocks
 * that were already connected (BridgeStateTracker) passes REPLAY_LINEAGE
 * instead: it carries no lineage and trusts each recorded VERIFIER_E
 * verdict and each kind 13 FINALITY_CONFLICT record, which ConnectBlock
 * judged with a full context before the block was stored. A context is
 * never a burn-gate input when it trusts verdicts.
 */
struct BridgeV2LineageContext {
    bool trust_recorded_verdicts{false};
    std::optional<FreshLineageState> fresh{};
    modern::FreshHashAtFn hash_at{};
};

//! The replay context (trust_recorded_verdicts, no lineage).
extern const BridgeV2LineageContext REPLAY_LINEAGE;

/**
 * The lineage context for the block that would extend `parent`: syncs
 * `finality` to `parent` with `bridge_index` (which must already be synced
 * to `parent`) and projects the fresh lineage for parent + 1; `hash_at`
 * walks `parent`'s ancestors. nullopt, with `reason`, without a bridge V2
 * envelope or when the finality state cannot be reconstructed. Without a
 * configured fresh lineage the context carries no fresh state, so the gate
 * reports C2 and a LIVE VERIFIER_E cannot be judged. Under cs_main.
 */
std::optional<BridgeV2LineageContext> MakeBridgeV2LineageContext(
    FinalityTracker& finality, const CChain& chain,
    const BlockManager& blockman, const Consensus::Params& params,
    const CBlockIndex& parent, const BridgeStateIndex& bridge_index,
    std::string& reason);

/**
 * The VERIFIER_E verdict B3 records for a LIVE verifier observation in a
 * block at `height` (node changes section 8.4 rows E4-E5), against `index`
 * connected at height - 1:
 * - bit 0 SETS_MISMATCH: currentSetHash / nextSetHash differ from
 *   setHash(Set'_{e_E}) / setHash(Set'_{e_E+1}) of the lineage;
 * - bit 1 CERT_CONFLICT: Latest.index is 0, Latest.height is not
 *   G + index * checkpoint_interval, e_E is not the epoch of Latest.height
 *   (B3 signs a checkpoint only in that epoch, as JudgeFinalityConflictV2
 *   check 9), or latestCertId is not FinalityCertIdV2 of B3's canonical
 *   epoch-e_E statement at Latest.height (ExpectedFinalizedBlockV2). Both
 *   functions thus realize the same CertIdAt(index).
 * nullopt, with a stable `error`, when the record cannot be judged and is
 * therefore invalid: no bootstrapped, unbroken fresh lineage; e_E outside
 * [e_B3 - 2, e_B3]; Latest.height above height - 1; or B3 unable to form
 * its statement.
 *
 * The window is the fresh lineage's retained committees, Set'_{e-2} ..
 * Set'_{e+1} (node::FRESH_LINEAGE_RETAINED_PAST_EPOCHS), so a newly observed
 * verifier up to two epochs behind is judged and E3's tolerance is section
 * 8.4's max_epoch_gap = 2 (rows E3 and 8.6). A larger max_epoch_gap does not
 * widen the window: a verifier further behind cannot be judged, no new
 * observation of it is valid, and E1 closes the gate once the last one is
 * stale.
 *
 * Cost: ExpectedFinalizedBlockV2 builds the history root over leaves
 * 0..Latest.index - 1. While Latest.index is below the lineage's finalized
 * index (a verifier lagging B3, the usual case) FreshHistoryRootAt rebuilds
 * it from leaf 0: O(Latest.index) hash_at lookups and keccak calls, one
 * per fresh checkpoint since R_f (about 52,600 a year at mainnet's grid of
 * ten 60-second blocks) per LIVE VERIFIER_E record, in mempool admission, block
 * assembly, block validation and TestBlockValidity alike. Kind 9 records
 * are bounded by the per-block weight, and a cache of roots by index in the
 * fresh lineage would remove the rebuild (a later slice). Kind 13
 * (JudgeFinalityConflictV2) pays the same rebuild only for a statement of
 * the checkpoint's own epoch that equals B3's in every other field, below
 * the finalized index: in practice an older canonical certificate of the
 * current epoch wrapped as evidence, which is refused after a BLS check and
 * the rebuild. The same cache removes it.
 */
std::optional<uint8_t> VerifierVerdictV2(
    const bridge::VerifierStateV2& observed,
    const BridgeV2LineageContext& lineage, const Consensus::Params& params,
    const BridgeStateIndex& index, int height, std::string& error);

/**
 * Consensus judgement of a kind 13 FINALITY_CONFLICT record in a block at
 * `height` (node changes section 8.5), against `index` connected at
 * height - 1 and the lineage projected for `height`. The record is valid
 * evidence when the certificate is signed by the lineage's CURRENT
 * (fb.epoch == e_B3) or NEXT (fb.epoch == e_B3 + 1) committee, as Verifier
 * V2's reportConflict accepts it, and differs from B3's canonical statement
 * at its checkpoint. Older committees are refused: with no unbonding or
 * slashing a rotated-out quorum can be cheap to buy.
 *
 * Checks, in this order (the first failure sets a stable `error`):
 *  1. modern PoS checkpoint parameters and a pinned chain domain;
 *  2. a bootstrapped, unbroken fresh lineage with a hash_at;
 *  3. the certificate decodes for the record's validator count;
 *  4. fb.lineage_id is the lineage's id;
 *  5. fb.epoch selects the current or next committee (else refused);
 *  6. the validator count is that committee's size;
 *  7. 1 <= fb.checkpoint_index < 2^32, H = G + index * interval fits an
 *     int and H <= height - 1 (an index above the parent is invalid until
 *     reached);
 *  8. modern::VerifyFinalityCertificateV2 against the committee, with the
 *     statement's own successor hash: the successor field is deliberately
 *     unchecked, because a quorum that signed a wrong successor signed a
 *     non-canonical statement too;
 *  9. if fb.epoch is not the epoch of H, the statement differs from B3's
 *     by construction; otherwise it must differ from B3's canonical
 *     statement at H (ExpectedFinalizedBlockV2) in some field. The fields
 *     B3 reads cheaply (height, block hash, withdrawal root, successor,
 *     bridgeFlags) are compared first; only a statement equal in all of
 *     them reads B3's history root: the finalized checkpoint's recorded
 *     root at the finalized index, else FreshHistoryRootAt. An equal history
 *     root too is B3's own statement and refused. Fields B3 cannot form
 *     refuse the record.
 * Cheap checks precede the BLS verification, which precedes check 9, whose
 * history-root read can be an O(index) rebuild (VerifierVerdictV2's cost
 * note). fb.height is not required to equal H: B3's statement at that index
 * names H, so any other height differs from it.
 * Returns the conflicting statement on success.
 */
std::optional<modern::FinalizedBlockV2> JudgeFinalityConflictV2(
    const bridge::BridgeFinalityConflictV2& record,
    const BridgeV2LineageContext& lineage, const Consensus::Params& params,
    const BridgeStateIndex& index, int height, std::string& error);

/** One evaluation of the bridge V2 burn gate. */
struct BridgeV2GateResult {
    BridgeBurnReadiness readiness{BridgeBurnReadiness::UNAVAILABLE};
    bridge::BurnGateRowV2 row{bridge::BurnGateRowV2::NONE};
    std::string reason{};
    //! READY only: the first now_eff at which a time row closes the gate.
    std::optional<int64_t> closes_at{};
    int64_t now_eff{0};

    //! "ready", "<row>: <reason>" or "unavailable: <reason>".
    std::string Describe() const;
};

/**
 * The bridge V2 burn gate (node changes section 8.4 rows C1-C4 and E1-E8,
 * bridge::EvaluateBurnGateV2) for a burn in the block at `height` with
 * block time `candidate_time`, from `index` connected at height - 1:
 * - now_eff = max(candidate_time, the V2 light client's finalized execution
 *   timestamp);
 * - the lineage rows read lineage->fresh (MakeLineageV2View; the finalized
 *   checkpoint's epoch for E6);
 * - E1-E8 read the index's latest VERIFIER_E, TOKEN_FLAGS and RESERVE_E,
 *   and C4 its sticky conflict.
 * The gate is the entry's that takes burns at `height`
 * (modern::ActiveBridgeV2Role): the launch entry v below a B-23 migration
 * height h_m, the successor v' from h_m, whose E7 and E8 read
 * TOKEN_FLAGS_SUCCESSOR (0x13) and RESERVE_E_SUCCESSOR (0x12). A burn naming
 * the retired v is refused by its record check, so the block-level gate
 * always follows the entry burns name.
 * UNAVAILABLE without a bridge V2 envelope, without a lineage context, with
 * the replay context, or when `index` is not connected at height - 1.
 * The block-level evaluation shared by mempool admission, block connection,
 * block assembly and wallet prevalidation. VerifyBlock (with a full
 * context) evaluates the same rows again at each burn's position, on the
 * observations and conflict after the block's earlier records, and applies
 * the per-burn reserve and wind-down bounds.
 */
BridgeV2GateResult EvaluateBridgeV2BurnGate(
    const BridgeV2LineageContext* lineage, const BridgeStateIndex& index,
    const Consensus::Params& params, int height, int64_t candidate_time);

/**
 * The gate of a kind 16 VAULT_SWEEP in the block at `height` (node changes
 * section 7.6): the retired launch entry v's burn gate "but for
 * retirement", i.e. EvaluateBridgeV2BurnGate's rows evaluated for v
 * (subjects 1, 2 and 3, the shared lineage and sticky conflict) rather than
 * for the entry that takes burns. UNAVAILABLE where EvaluateBridgeV2BurnGate
 * is, and below the migration height h_m (no sweep exists there).
 * VerifyBlock evaluates the same rows again at each sweep's position.
 */
BridgeV2GateResult EvaluateBridgeV2SweepGate(
    const BridgeV2LineageContext* lineage, const BridgeStateIndex& index,
    const Consensus::Params& params, int height, int64_t candidate_time);

/**
 * Why B3 has closed bridge V2 deposits for good (node changes sections 7.5
 * and 8.5, "Deposits stop too"), or nullopt while they are open. The first
 * match of:
 * - the sticky conflict (gate row C4): "sticky <E5|E8|kind 13> conflict at
 *   height N";
 * - a broken fresh lineage (row C2's permanent case), read from
 *   lineage->fresh; skipped without a lineage or fresh state;
 * - the latest VERIFIER_E showing an impaired verifier (row C1,
 *   bridge::VerifierImpairmentV2: halted, cancelled or expired at the
 *   observation) or Verifier V2 depositsClosed (Ethereum's own close).
 * Each is permanent: nothing but a disconnect of the block that connected
 * it reopens deposits. Rows that can recover (C3, E1-E8) never close them.
 * B3 still mints every proven deposit (the vault cannot refund), so wallets
 * and relayers read this to refuse new deposits and flushes before the next
 * relayed certificate carries bridgeFlags bit 0 (FinalityBridgeFlagsV2)
 * and Verifier V2 closes deposits on Ethereum. nullopt without a bridge V2
 * envelope (mainnet, testnet).
 */
std::optional<std::string> BridgeV2DepositsClosedReason(
    const BridgeStateIndex& index, const BridgeV2LineageContext* lineage,
    const Consensus::Params& params);

/** CheckBridgeBurnReadiness for the V2 regime: refuses a decoded
 * BRIDGE_BURN unless `readiness` is READY. Callers apply it only from the
 * bridge V2 height and append the gate's row and reason. */
bool CheckBridgeV2BurnReadiness(const CTransaction& tx,
                                BridgeBurnReadiness readiness,
                                std::string& error);
bool CheckBridgeV2BurnReadiness(const CBlock& block,
                                BridgeBurnReadiness readiness,
                                std::string& error);

/** Recent active-chain blocks whose bridge effects can be undone in memory. */
inline constexpr size_t BRIDGE_STATE_UNDO_BLOCKS{288};

/** A receipts-root anchor reached from the verified finalized Ethereum head. */
struct BridgeExecutionAnchor {
    uint64_t block_number{0};
    uint256 block_hash{};
    uint256 receipts_root{};
    uint64_t source_finalized_beacon_slot{0};
    uint64_t source_finalized_execution_block{0};
    uint64_t execution_timestamp{0};
    int connected_height{-1};
    uint256 connected_block{};
    //! The execution header's stateRoot, recorded for EIP-1186 account and
    //! storage proofs (bridge/state_proof.h). It is the last member so
    //! positional initializers that predate it stay valid. No V1 rule reads
    //! it: an anchor still needs only a non-null receipts_root. Null when the
    //! anchor's header carried no 32-byte stateRoot.
    uint256 state_root{};

    friend bool operator==(const BridgeExecutionAnchor&,
                           const BridgeExecutionAnchor&) = default;
};

/** Active B3 block that connected one persistent bridge-state fact. */
struct BridgeStateConnection {
    int height{-1};
    uint256 block_hash{};

    friend bool operator==(const BridgeStateConnection&,
                           const BridgeStateConnection&) = default;
};

/**
 * One exact Ethereum light-client store and the active B3 block that made it
 * current. The bridge index retains one old floor snapshot in addition to its
 * bounded undo history so a stream of newer, not-yet-B3-finalized updates
 * cannot make every exportable snapshot disappear.
 */
struct BridgeLightClientSnapshot {
    bridge::LightClientStore store{};
    BridgeStateConnection connection{};
};

/** A type-10 MINT authorization bound to the full evidence-bearing tx bytes. */
struct BridgeTxMintAuthorization {
    Ptxid transaction_id{};
    uint32_t output_index{0};
    bridge::BridgeMintAuthorization authorization{};

    friend bool operator==(const BridgeTxMintAuthorization& a,
                           const BridgeTxMintAuthorization& b)
    {
        return a.transaction_id == b.transaction_id &&
               a.output_index == b.output_index &&
               a.authorization.asset == b.authorization.asset &&
               a.authorization.amount == b.authorization.amount &&
               a.authorization.recipient_script ==
                   b.authorization.recipient_script &&
               a.authorization.nullifier == b.authorization.nullifier;
    }
};

/**
 * A managed-v1 reserve-release request created by one exact bUSD burn. The
 * stable base Txid is safe because its policy-9 output commits the type-10
 * record; witness re-signing therefore cannot create a second request id.
 */
struct BridgeManagedWithdrawalRequest {
    Txid transaction_id{};
    uint32_t burn_output_index{0};
    modern::AssetId asset{};
    CAmount amount{0};
    bridge::EthAddress ethereum_recipient{};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeManagedWithdrawalRequest&,
                           const BridgeManagedWithdrawalRequest&) = default;
};

/**
 * One consensus-derived BRIDGE_BURN leaf. The record supplies only the
 * recipient and names the exact burn; every other Ethereum-facing field is
 * derived from pinned bridge parameters and active-chain position.
 */
struct BridgeDecentralizedWithdrawalRequest {
    Txid transaction_id{};
    uint32_t burn_output_index{0};
    modern::BridgeWithdrawalV1 withdrawal{};
    uint256 leaf{};
    int connected_height{-1};
    uint256 connected_block{};

    friend bool operator==(const BridgeDecentralizedWithdrawalRequest&,
                           const BridgeDecentralizedWithdrawalRequest&) = default;
};

/**
 * One consensus-derived bridge V2 leaf (node changes sections 7.2-7.3). As
 * for V1, the kind 6 record supplies only the recipient and names the exact
 * bUSD V2 burn; the id, destination chain, asset, vault, token and B3 height
 * come from the V2 entry and the active-chain position.
 *
 * `entry` is the index of the vault entry whose ledger counts the leaf in
 * burned_v (Consensus::BridgeV2EntryRole: 0 the launch entry v, 1 the B-23
 * successor v'). `sweep` marks a kind 16 VAULT_SWEEP leaf (section 7.6):
 * it names the retired v (entry 0), pays the successor vault v' and adds
 * its amount to transit_v'. A sweep's `burn_output_index` is the index of
 * the carrier's binding output, the one output that names the record.
 *
 * `exit` is the route (d3) exit block of a kind 6 burn that carried one
 * (node changes section 7.2 item 5, C-7): the exit forwarder parameters p
 * that check 5 verified, stored with the leaf and undone with it, so
 * getbridgewithdrawals and getbridgewithdrawalproofv2 return p without
 * reading the burn block. Never set on a sweep. All three are trailing
 * members, so positional initializers of a kind 6 leaf stay valid.
 */
struct BridgeWithdrawalV2Request {
    Txid transaction_id{};
    uint32_t burn_output_index{0};
    modern::BridgeWithdrawalV2 withdrawal{};
    uint256 leaf{};
    int connected_height{-1};
    uint256 connected_block{};
    uint8_t entry{0};
    bool sweep{false};
    std::optional<modern::ExitForwarderParamsV2> exit{};

    friend bool operator==(const BridgeWithdrawalV2Request&,
                           const BridgeWithdrawalV2Request&) = default;
};

struct BridgeWithdrawalId {
    Txid transaction_id{};
    uint32_t burn_output_index{0};

    friend bool operator==(const BridgeWithdrawalId&,
                           const BridgeWithdrawalId&) = default;
    friend bool operator<(const BridgeWithdrawalId& a,
                          const BridgeWithdrawalId& b)
    {
        if (a.transaction_id != b.transaction_id) {
            return a.transaction_id < b.transaction_id;
        }
        return a.burn_output_index < b.burn_output_index;
    }
};

/** The sole bridge semantic result one transaction can carry. */
struct BridgeTxAuthorization {
    std::optional<BridgeTxMintAuthorization> mint{};
    std::optional<BridgeManagedWithdrawalRequest> withdrawal{};
    std::optional<BridgeDecentralizedWithdrawalRequest>
        decentralized_withdrawal{};
    std::optional<BridgeWithdrawalV2Request> withdrawal_v2{};

    bool HasWithdrawal() const
    {
        return withdrawal.has_value() ||
               decentralized_withdrawal.has_value() ||
               withdrawal_v2.has_value();
    }
};

/**
 * Owner ruling R10 (open issue OI-8): sponsored native-B3 fees. True when a
 * transaction carrying this bridge V2 leaf at `height` is script-checked
 * under SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS instead of
 * SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL: the owner may sign SIGHASH_ALL or
 * SIGHASH_ALL|SIGHASH_ANYONECANPAY (both commit every output, so the
 * BRIDGE_RECORD binding and with it the whole record), which lets a relayer
 * add a native fee input; NONE and SINGLE stay refused.
 *
 * Only a kind 6 burn (routes d1 and d3) qualifies, and only from the
 * envelope's own sponsored sighash pin (BridgeV2Params::
 * sponsored_sighash_height >= V; Consensus::Params::
 * BridgeV2SponsoredSighashActive) on a chain whose bridge_v2 envelope is set
 * and Valid(). A complete envelope without that pin never selects the rule,
 * so pinning bridge V2 alone does not turn it on. Only the regtest
 * -b3bridgepins v2 section sets the envelope and the pin, so this is false
 * at every height on mainnet, testnet, testnet4 and signet. It never applies
 * to a kind 16 sweep (permissionless, no bUSD owner input, native fee paid
 * by the submitter: nothing to sponsor) or to a managed-V1 or
 * decentralized-V1 withdrawal, which keep plain SIGHASH_ALL.
 */
//! Inline so the standalone wallet tool, which does not link the node
//! library, can use it through wallet RPC.
inline bool BridgeV2SponsoredSighashAllowed(const BridgeWithdrawalV2Request& withdrawal,
                                            const Consensus::Params& params,
                                            const int height)
{
    return !withdrawal.sweep && params.BridgeV2SponsoredSighashActive(height);
}
//! The same for a transaction's whole bridge result: true only for a
//! bridge V2 leaf with no mint and no managed or V1 withdrawal beside it.
inline bool BridgeV2SponsoredSighashAllowed(const BridgeTxAuthorization& authorization,
                                            const Consensus::Params& params,
                                            const int height)
{
    if (!authorization.withdrawal_v2 || authorization.mint ||
        authorization.withdrawal || authorization.decentralized_withdrawal) {
        return false;
    }
    return BridgeV2SponsoredSighashAllowed(*authorization.withdrawal_v2,
                                           params, height);
}

/** Exact before/after accounting for one configured asset epoch. */
struct BridgeEpochMintChange {
    modern::AssetId asset{};
    uint64_t epoch{0};
    CAmount before{0};
    CAmount after{0};

    friend bool operator==(const BridgeEpochMintChange&,
                           const BridgeEpochMintChange&) = default;
};

/**
 * Complete reversible effect of one active bridge block, including the prior
 * connected identity. Empty active blocks are retained so disconnect identity
 * remains exact. Light-client snapshots are present only when the block
 * changed that store.
 *
 * From the bridge V2 activation height V (`v2_store`), the light-client
 * fields and `anchors_added` refer to the separate V2 store, mints use
 * nullifier namespace 2, the V1 withdrawal tree is frozen and V2 burns
 * append to the V2 tree (node changes section 3.4). The V2 tree fields are
 * carried at every height; below V they stay the empty tree.
 *
 * The bridge V2 accounting fields (node changes sections 7.1-7.2, 7.6,
 * 8.2, 8.4 and 8.5) are likewise carried at every height and change only
 * from V: the latest observation per subject before the block and the kind
 * 9 records it connected (in order), the reserve ledger before and after,
 * the deposits its V2 mints proved, the sticky conflict before and after,
 * and the certIds of the kind 13 FINALITY_CONFLICT records it accepted (at
 * most one, since the first sets the sticky conflict and every later one is
 * refused).
 *
 * The B-23 successor v' (section 7.6) keeps a ledger and proven deposits of
 * its own: the trailing `reserve_v2_successor_*` and
 * `deposits_v2_successor_added` fields, unchanged and empty below V and
 * until the migration height. `reserve_v2_*` and `deposits_v2_added` stay
 * the launch entry v's.
 */
struct BridgeBlockDelta {
    int height{-1};
    uint256 block_hash{};
    int previous_height{-1};
    uint256 previous_block_hash{};
    std::optional<bridge::LightClientStore> light_client_before{};
    std::optional<bridge::LightClientStore> light_client_after{};
    std::optional<BridgeStateConnection> light_client_connection_before{};
    std::optional<BridgeStateConnection> light_client_connection_after{};
    std::vector<BridgeExecutionAnchor> anchors_added{};
    std::vector<BridgeTxMintAuthorization> mint_authorizations{};
    std::vector<bridge::BridgeDepositKey> nullifiers_added{};
    std::vector<BridgeEpochMintChange> epoch_mint_changes{};
    std::vector<BridgeManagedWithdrawalRequest> withdrawals_added{};
    std::vector<BridgeDecentralizedWithdrawalRequest>
        decentralized_withdrawals_added{};
    modern::WithdrawalTreeState withdrawal_tree_before{};
    modern::WithdrawalTreeState withdrawal_tree_after{};
    bool v2_store{false};
    std::vector<BridgeWithdrawalV2Request> withdrawals_v2_added{};
    modern::WithdrawalTreeState withdrawal_tree_v2_before{};
    modern::WithdrawalTreeState withdrawal_tree_v2_after{};
    BridgeObservationSlotsV2 observations_v2_before{};
    std::vector<BridgeObservationRecordV2> observations_v2_added{};
    bridge::ReserveLedgerV2 reserve_v2_before{};
    bridge::ReserveLedgerV2 reserve_v2_after{};
    std::vector<BridgeProvenDepositV2> deposits_v2_added{};
    std::optional<BridgeV2Conflict> conflict_v2_before{};
    std::optional<BridgeV2Conflict> conflict_v2_after{};
    std::vector<uint256> finality_conflicts_v2_added{};
    bridge::ReserveLedgerV2 reserve_v2_successor_before{};
    bridge::ReserveLedgerV2 reserve_v2_successor_after{};
    std::vector<BridgeProvenDepositV2> deposits_v2_successor_added{};
};

/**
 * Incremental, non-mutating bridge verification for one candidate block.
 *
 * The preview borrows the connected BridgeStateIndex as an immutable base and
 * owns only candidate-local overlays. TryAppend is atomic: a rejected chunk
 * leaves all previously accepted candidate effects intact.
 */
class BridgeBlockPreview
{
public:
    ~BridgeBlockPreview();

    BridgeBlockPreview(const BridgeBlockPreview&) = delete;
    BridgeBlockPreview& operator=(const BridgeBlockPreview&) = delete;

    bool TryAppend(std::span<const CTransactionRef> transactions,
                   std::string& error);

private:
    struct Impl;
    explicit BridgeBlockPreview(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;

    friend class BridgeStateIndex;
};

/**
 * Consensus bridge authorization state. Candidate verification operates on
 * base-state lookups plus per-candidate overlays and yields a complete delta;
 * live state changes only after that delta has been fully validated.
 */
class BridgeStateIndex
{
public:
    /**
     * Verify a candidate block against this state (connected at height - 1).
     * `lineage` judges kind 9 VERIFIER_E verdicts and kind 13
     * FINALITY_CONFLICT records (MakeBridgeV2LineageContext for new blocks,
     * &REPLAY_LINEAGE for blocks already connected once). Without one a LIVE
     * VERIFIER_E fails closed ("verdict cannot be judged here"), as does a
     * kind 13 record ("cannot be judged here").
     *
     * With a full context every V2 burn also passes the burn gate at its
     * position in the block (node changes section 7.2 check 4, per record):
     * EvaluateBridgeV2BurnGate's rows on that context, the block-level
     * now_eff, and the kind 9 observations and sticky conflict after the
     * block's earlier records, so an earlier record that impairs the
     * verifier, pauses the token or sets the conflict closes the gate for
     * every later burn of the same block. The caller still evaluates the
     * block-level gate on the connected state (validation.cpp, miner.cpp,
     * interfaces.cpp), so a burn needs READY both there and at its
     * position: observations in the same block can close the gate for a
     * burn, never open it. REPLAY_LINEAGE skips the per-burn gate (the block
     * passed it when it first connected). Without a context the per-burn
     * gate is left to the caller while no earlier record of the block
     * changed an observation or the conflict, and a burn after such a
     * change fails closed; every consensus caller passes a context whenever
     * the block carries a burn.
     *
     * A kind 16 VAULT_SWEEP (B-23, node changes section 7.6) passes the
     * retired launch entry's gate but for retirement at its position in the
     * same way (EvaluateBridgeV2SweepGate's rows on the block's state after
     * its earlier records). It has no block-level gate: without a context
     * it is refused, and REPLAY_LINEAGE skips its gate. Every consensus
     * caller builds a context for it (HasBridgeV2LineageRecord).
     */
    bool VerifyBlock(const CBlock& block, int height,
                     const uint256& block_hash,
                     const Consensus::Params& params,
                     BridgeBlockDelta& out, std::string& error,
                     const BridgeV2LineageContext* lineage = nullptr) const;

    /** Mempool/miner preview against the currently connected state. */
    bool VerifyTransaction(const CTransaction& tx, int candidate_height,
                           int64_t candidate_time,
                           const Consensus::Params& params,
                           BridgeTxAuthorization& out,
                           std::string& error,
                           const BridgeV2LineageContext* lineage = nullptr) const;

    /**
     * Begin an incremental block preview against this exact connected state.
     * The index must outlive the preview and remain unchanged while it is in
     * use (block assembly satisfies this while holding cs_main). `lineage`
     * is copied into the preview; without one, LIVE VERIFIER_E and kind 13
     * records are refused (and so skipped by the assembler).
     */
    std::unique_ptr<BridgeBlockPreview> BeginBlockPreview(
        int candidate_height, int64_t candidate_time,
        const uint256& preview_block_id, const Consensus::Params& params,
        std::string& error,
        const BridgeV2LineageContext* lineage = nullptr) const;

    bool ConnectBlock(const BridgeBlockDelta& delta, std::string& error);
    bool DisconnectBlock(int height, const uint256& block_hash,
                         std::string& error);
    void Clear();

    bool IsNullified(const bridge::BridgeDepositKey& key) const
    {
        return m_nullifiers.contains(key);
    }
    std::optional<BridgeStateConnection> NullifierConnection(
        const bridge::BridgeDepositKey& key) const;
    std::optional<BridgeExecutionAnchor> Anchor(
        const uint256& block_hash) const;
    /**
     * Nearest retained execution anchor at or after `target_block_number`
     * whose B3 connection is covered by `finalized_b3_height` and whose
     * directly-finalized Ethereum origin can still reach the target within
     * the cumulative backfill limit.
     *
     * The index contains active-chain state only. Callers exposing this
     * across a trust boundary must additionally match connected_block to the
     * active B3 chain at connected_height.
     */
    std::optional<BridgeExecutionAnchor> AnchorForTarget(
        uint64_t target_block_number, int finalized_b3_height) const;
    std::optional<BridgeManagedWithdrawalRequest> Withdrawal(
        const BridgeWithdrawalId& id) const;
    std::optional<BridgeDecentralizedWithdrawalRequest>
    DecentralizedWithdrawal(uint64_t withdrawal_id) const;
    /**
     * Resolve a confirmed active-chain burn source to its consensus-assigned
     * withdrawal. Unlike a mempool preview, this identity survives unrelated
     * transaction ordering and disappears automatically on active-chain undo.
     */
    std::optional<BridgeDecentralizedWithdrawalRequest>
    DecentralizedWithdrawal(const BridgeWithdrawalId& source) const;
    /** Exact consecutive withdrawal prefix connected through `height`. */
    std::optional<std::vector<BridgeDecentralizedWithdrawalRequest>>
    DecentralizedWithdrawalsThrough(int height) const;
    std::optional<uint256> WithdrawalRootAtHeight(int height) const;
    const modern::WithdrawalTreeState& WithdrawalTree() const
    {
        return m_withdrawal_tree;
    }
    const std::optional<bridge::LightClientStore>& LightClient() const
    {
        return m_light_client;
    }
    const std::optional<BridgeStateConnection>&
    LightClientLastConnected() const
    {
        return m_light_client_connection;
    }
    /**
     * Newest retained store whose connection height is covered by B3
     * finality. Recent candidates come from the bounded undo history; the
     * first active-chain store is retained as a starvation-proof floor.
     * Callers must still verify connection.block_hash against the active chain.
     * This is the V1 store; V2-store deltas are skipped.
     */
    std::optional<BridgeLightClientSnapshot>
    FinalizedLightClientSnapshot(int finalized_b3_height) const;
    CAmount EpochMinted(const modern::AssetId& asset, uint64_t epoch) const;
    int ConnectedHeight() const { return m_connected_height; }
    uint256 ConnectedHash() const { return m_connected_hash; }
    //! Every nullifier, both namespaces.
    size_t NullifierCount() const { return m_nullifiers.size(); }
    //! Nullifiers of one namespace (bridge::BRIDGE_NULLIFIER_NAMESPACE_*).
    size_t NullifierCount(uint8_t nullifier_namespace) const;
    size_t AnchorCount() const { return m_anchors.size(); }
    size_t WithdrawalCount() const { return m_withdrawals.size(); }
    size_t DecentralizedWithdrawalCount() const
    {
        return m_decentralized_withdrawals.size();
    }
    const std::deque<BridgeBlockDelta>& History() const { return m_history; }

    // -- Bridge V2 (node changes sections 3.4, 4 and 7) -------------------
    // The V2 light-client store and anchors start empty at V; the V1 store,
    // nullifiers and tree freeze at V. Withdrawal roots V2 are recorded for
    // every connected height from V only; below V (and on every chain
    // without a bridge V2 envelope) FinalityWithdrawalRootV2 is zero without
    // consulting the index.

    const std::optional<bridge::LightClientStore>& LightClientV2() const
    {
        return m_light_client_v2;
    }
    const std::optional<BridgeStateConnection>&
    LightClientV2LastConnected() const
    {
        return m_light_client_connection_v2;
    }
    //! FinalizedLightClientSnapshot over the V2 store only.
    std::optional<BridgeLightClientSnapshot>
    FinalizedLightClientSnapshotV2(int finalized_b3_height) const;
    std::optional<BridgeExecutionAnchor> AnchorV2(
        const uint256& block_hash) const;
    //! AnchorForTarget over the V2 anchors (same rules and caveats).
    std::optional<BridgeExecutionAnchor> AnchorForTargetV2(
        uint64_t target_block_number, int finalized_b3_height) const;
    size_t AnchorCountV2() const { return m_anchors_v2.size(); }
    std::optional<BridgeWithdrawalV2Request> WithdrawalV2(
        uint64_t withdrawal_id) const;
    //! Confirmed active-chain V2 burn source -> its consensus leaf.
    std::optional<BridgeWithdrawalV2Request> WithdrawalV2(
        const BridgeWithdrawalId& source) const;
    //! Exact consecutive V2 withdrawal prefix connected through `height`.
    std::optional<std::vector<BridgeWithdrawalV2Request>>
    WithdrawalsV2Through(int height) const;
    std::optional<uint256> WithdrawalRootV2AtHeight(int height) const;
    const modern::WithdrawalTreeState& WithdrawalTreeV2() const
    {
        return m_withdrawal_tree_v2;
    }
    size_t WithdrawalV2Count() const { return m_withdrawals_v2.size(); }

    // -- Bridge V2 reserve and liveness state (node changes 7.1, 8.2-8.5) --
    // All empty below V and on every chain without a bridge V2 envelope.

    //! The latest connected observation of `subject` (1-3, 0x12 or 0x13),
    //! if any (BridgeObservationSlotV2).
    std::optional<BridgeObservationRecordV2> LatestObservationV2(
        uint8_t subject) const;
    const BridgeObservationSlotsV2& ObservationsV2() const
    {
        return m_observations_v2;
    }
    //! The reserve ledger of vault entry `role`: the launch entry v, or the
    //! B-23 successor v' (all zero until a migration moves anything).
    const bridge::ReserveLedgerV2& ReserveLedgerV2(
        const Consensus::BridgeV2EntryRole role) const
    {
        return role == Consensus::BridgeV2EntryRole::SUCCESSOR
                   ? m_reserve_v2_successor
                   : m_reserve_v2;
    }
    //! The launch entry's reserve ledger.
    const bridge::ReserveLedgerV2& ReserveLedgerV2() const
    {
        return m_reserve_v2;
    }
    //! Proven deposit amounts by Vault V2 deposit id, of entry `role`.
    const bridge::ProvenDepositMapV2& ProvenDepositsV2(
        const Consensus::BridgeV2EntryRole role) const
    {
        return role == Consensus::BridgeV2EntryRole::SUCCESSOR
                   ? m_deposits_v2_successor
                   : m_deposits_v2;
    }
    //! The launch entry's proven deposits.
    const bridge::ProvenDepositMapV2& ProvenDepositsV2() const
    {
        return m_deposits_v2;
    }
    //! The sum of ProvenDepositsV2(role), kept under undo.
    CAmount ProvenDepositsTotalV2(const Consensus::BridgeV2EntryRole role) const
    {
        return role == Consensus::BridgeV2EntryRole::SUCCESSOR
                   ? m_deposits_v2_successor_total
                   : m_deposits_v2_total;
    }
    //! The sum of ProvenDepositsV2(), kept under undo.
    CAmount ProvenDepositsTotalV2() const { return m_deposits_v2_total; }
    //! P_pre and P_post of entry `role`'s connected deposits at `cut`
    //! (bridge::SplitProvenDepositsAtCut).
    std::optional<bridge::ProvenDepositSumsV2> ProvenDepositSumsAtV2(
        const Consensus::BridgeV2EntryRole role, const uint64_t cut) const
    {
        return bridge::SplitProvenDepositsAtCut(ProvenDepositsV2(role),
                                                ProvenDepositsTotalV2(role),
                                                {}, 0, cut);
    }
    //! The launch entry's P_pre and P_post at `cut`.
    std::optional<bridge::ProvenDepositSumsV2> ProvenDepositSumsAtV2(
        const uint64_t cut) const
    {
        return ProvenDepositSumsAtV2(Consensus::BridgeV2EntryRole::LAUNCH,
                                     cut);
    }
    const std::optional<BridgeV2Conflict>& ConflictV2() const
    {
        return m_conflict_v2;
    }
    /**
     * bridgeFlags of B3's fresh-lineage statement at checkpoint `height`
     * (node changes sections 7.4 and 8.5; contracts spec sections 2.1 and
     * 3.3 step 11): modern::FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED once the
     * sticky conflict was set at or below `height`, else 0. The conflict is
     * sticky and first-wins, so the flag at `height` depends only on blocks
     * at or below it, and this state answers it exactly for every connected
     * height. The sticky conflict is one for the bridge: the launch entry
     * and a B-23 successor share one verifier, whose depositsClosed the flag
     * sets, so every conflict covers both entries.
     * FinalityBridgeFlagsV2 adds the activation and fail-closed rules.
     */
    uint8_t BridgeFlagsV2AtHeight(int height) const;
    //! The highest-numbered retained V2 anchor carrying a stateRoot, the
    //! anchor a relayer proves new observations against.
    std::optional<BridgeExecutionAnchor> NewestStateAnchorV2() const;

private:
    //! Unit tests seed reserve ledgers directly (a funded ledger under a
    //! hand-built delta that skips the deposits, or an arbitrary transit).
    friend struct BridgeStateIndexTestAccess;

    std::optional<BridgeLightClientSnapshot> FinalizedSnapshot(
        int finalized_b3_height, bool v2_store) const;

    std::optional<bridge::LightClientStore> m_light_client{};
    std::optional<BridgeStateConnection> m_light_client_connection{};
    std::optional<BridgeLightClientSnapshot> m_light_client_floor{};
    std::map<uint256, BridgeExecutionAnchor> m_anchors{};
    std::map<uint64_t, uint256> m_anchor_by_height{};
    std::set<bridge::BridgeDepositKey> m_nullifiers{};
    std::map<bridge::BridgeDepositKey, BridgeStateConnection>
        m_nullifier_connections{};
    std::map<std::pair<modern::AssetId, uint64_t>, CAmount> m_epoch_minted{};
    std::map<BridgeWithdrawalId, BridgeManagedWithdrawalRequest> m_withdrawals{};
    std::map<uint64_t, BridgeDecentralizedWithdrawalRequest>
        m_decentralized_withdrawals{};
    std::map<BridgeWithdrawalId, uint64_t> m_decentralized_withdrawal_sources{};
    modern::WithdrawalTreeState m_withdrawal_tree{};
    std::map<int, uint256> m_withdrawal_roots{};
    std::optional<bridge::LightClientStore> m_light_client_v2{};
    std::optional<BridgeStateConnection> m_light_client_connection_v2{};
    std::optional<BridgeLightClientSnapshot> m_light_client_floor_v2{};
    std::map<uint256, BridgeExecutionAnchor> m_anchors_v2{};
    std::map<uint64_t, uint256> m_anchor_by_height_v2{};
    std::map<uint64_t, BridgeWithdrawalV2Request> m_withdrawals_v2{};
    std::map<BridgeWithdrawalId, uint64_t> m_withdrawal_v2_sources{};
    modern::WithdrawalTreeState m_withdrawal_tree_v2{};
    std::map<int, uint256> m_withdrawal_v2_roots{};
    BridgeObservationSlotsV2 m_observations_v2{};
    bridge::ReserveLedgerV2 m_reserve_v2{};
    bridge::ProvenDepositMapV2 m_deposits_v2{};
    CAmount m_deposits_v2_total{0};
    std::optional<BridgeV2Conflict> m_conflict_v2{};
    bridge::ReserveLedgerV2 m_reserve_v2_successor{};
    bridge::ProvenDepositMapV2 m_deposits_v2_successor{};
    CAmount m_deposits_v2_successor_total{0};
    int m_connected_height{-1};
    uint256 m_connected_hash{};
    std::deque<BridgeBlockDelta> m_history{};
};

/**
 * Exact withdrawal root a finality certificate must carry at `height`.
 * Managed or pre-inbound-B epochs use zero. From inbound B onward a
 * decentralized bridge requires the indexed canonical cumulative root even
 * while the separate irreversible-burn height W remains unset; the empty
 * depth-32 tree root is nonzero. An unavailable index fails closed.
 */
std::optional<uint256> FinalityWithdrawalRoot(
    int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index);

/**
 * withdrawalRoot a fresh-lineage (digest V2) certificate must carry at
 * `height` (node changes section 7.4; contracts spec section 2.4): zero below
 * V or without a complete bridge V2 envelope, from V the indexed V2 tree root
 * (the empty depth-32 root is non-zero). An index lacking the height fails
 * closed, as does a missing index from V.
 */
std::optional<uint256> FinalityWithdrawalRootV2(
    int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index);

/**
 * bridgeFlags a fresh-lineage (digest V2) certificate must carry at `height`
 * (node changes sections 7.4 and 8.5; contracts spec section 2.1): 0 below V
 * or without a complete bridge V2 envelope (mainnet and testnet), from V
 * BridgeStateIndex::BridgeFlagsV2AtHeight, i.e. bit 0 from the first
 * checkpoint at or after the block that set the sticky conflict (kind 13,
 * E5 or E8). A certificate carrying bit 0 makes Verifier V2 close deposits
 * for good (contracts spec section 3.3 step 11). From V a missing index, or
 * an index that has not connected `height` (no V2 withdrawal root there),
 * fails closed (nullopt), exactly as FinalityWithdrawalRootV2. Since the
 * flag depends only on blocks at or below `height`, every signer, carrier
 * judge and replay that has connected `height` agrees on it.
 */
std::optional<uint8_t> FinalityBridgeFlagsV2(
    int height, const Consensus::Params& params,
    const BridgeStateIndex* bridge_index);

/**
 * Active-chain replay/undo driver. Restart and reindex correctness comes from
 * deterministic replay beginning at bridge activation. A production disk
 * sidecar must only replace this with a strict config-digest marker and one
 * synchronous state+undo batch; partial durability is intentionally absent.
 * Replay verifies with REPLAY_LINEAGE: every block it applies was connected
 * once by validation.cpp, which judged its VERIFIER_E verdicts with a full
 * lineage context.
 */
class BridgeStateTracker
{
public:
    bool Sync(const CChain& chain, const BlockManager& blockman,
              const Consensus::Params& params, const CBlockIndex& target);
    void BlockConnected(const CBlock& block, const CBlockIndex& index,
                        const Consensus::Params& params);
    void BlockDisconnected(const CBlockIndex& index,
                           const Consensus::Params& params);
    void MarkDirty() { m_dirty = true; }
    bool Synced(const uint256& tip_hash) const
    {
        return !m_dirty && m_synced_tip == tip_hash;
    }
    const BridgeStateIndex& Index() const { return m_index; }

private:
    bool ApplyBlock(const CBlock& block, const CBlockIndex& index,
                    const Consensus::Params& params);

    BridgeStateIndex m_index{};
    uint256 m_synced_tip{};
    int m_synced_height{-1};
    bool m_dirty{true};
};

} // namespace node

#endif // B3COIN_NODE_BRIDGE_STATE_H

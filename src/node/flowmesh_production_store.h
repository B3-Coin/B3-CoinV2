// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_PRODUCTION_STORE_H
#define B3COIN_NODE_FLOWMESH_PRODUCTION_STORE_H

#include <dbwrapper.h>
#include <flowmesh/production_engine.h>
#include <modern/flowmesh_checkpoint.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace node {

/**
 * Chain-backed lookup used during restart replay. The returned object is
 * still fully recomputed and checked by the store; the source cannot supply a
 * caller-selected seat-set hash.
 */
class ProductionSeatSetSource
{
public:
    virtual ~ProductionSeatSetSource() = default;
    virtual std::optional<flowmesh::ActiveFnBlsSeatSet> GetSeatSet(
        const uint256& domain, const flowmesh::MarketId& market_id,
        uint64_t epoch, const uint256& seat_set_hash) const = 0;
};

struct StoredProductionEntry {
    //! Redundant on disk by design: restart rejects a record whose recorded
    //! verification epoch differs from either the entry or certificate.
    uint64_t verified_epoch{0};
    flowmesh::ProductionEntryCore entry;
    flowmesh::BlsMicroblockCertificate certificate;
    //! Exact canonical effects whose root/count are signed by `entry`.
    std::vector<modern::FlowMeshEffectV1> effects;
    //! Exact anchor-derived withdrawals consumed by this execution. Nonempty
    //! batches are dedicated transitions and always require type-8 publication.
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;
};

struct ProductionCheckpointCandidate {
    StoredProductionEntry stored;
    modern::FlowMeshCheckpointId previous_checkpoint_id;
};

/**
 * The exact still-uncommitted candidate protected by a permanent signing
 * lock. `evidence` contains one transport/authentication Action for every
 * credential-free semantic action in `entry`, in the same canonical order.
 * It is deliberately separate from the production identity: credentials do
 * not change an entry hash, but are required to safely resume signing after a
 * restart.
 */
struct StoredLockedProductionCandidate {
    flowmesh::ProductionEntryCore entry;
    std::vector<flowmesh::Action> evidence;
};

/** Exact B3 block that connected one durable type-8 checkpoint. */
struct ProductionB3Connection {
    int32_t height{-1};
    uint256 block_hash;

    friend bool operator==(const ProductionB3Connection&,
                           const ProductionB3Connection&) = default;
};

/**
 * A handoff publication becomes an activation fact only after the exact B3
 * block that contains it is buried by the ratified FlowMesh anchor depth.
 * Keep this predicate shared by the runtime, service, and durable store so no
 * caller can accidentally activate the incoming committee on first connect.
 */
bool FlowMeshHandoffConnectionMature(
    const ProductionB3Connection& connection, int32_t canonical_tip_height);

/**
 * Production FlowMesh durable log, format v3.
 *
 * This is a separate per-market store. The older FlowMeshStore format-v2
 * regtest spike remains intact and has no migration path into this class.
 * Entry+marker appends, checkpoint-connection+marker transitions, and each
 * first lock+restart-candidate retention use one synchronous LevelDB batch.
 * Safety locks are permanent compare-and-set records keyed by
 * `(epoch, sequence)`; there is deliberately no unlock API. The bounded
 * candidate/evidence record is erased when that exact entry commits.
 */
class FlowMeshProductionStore final : public flowmesh::DurableProductionLockJournal
{
public:
    static constexpr int32_t FORMAT_VERSION{3};

    struct Marker {
        int32_t version{FORMAT_VERSION};
        uint256 domain;
        flowmesh::MarketId market_id;
        uint64_t current_epoch{0};
        flowmesh::AnchorRef current_anchor;
        uint256 current_seat_set_hash;
        uint64_t next_sequence{0};
        //! Authoritative global cursor for the next typed checkpoint effect.
        uint64_t next_effect_index{0};
        uint256 last_microblock_hash;
        uint256 state_root;
        modern::FlowMeshCheckpointId last_b3_checkpoint;

        SERIALIZE_METHODS(Marker, obj)
        {
            READWRITE(obj.version, obj.domain, obj.market_id,
                      obj.current_epoch, obj.current_anchor,
                      obj.current_seat_set_hash, obj.next_sequence,
                      obj.next_effect_index,
                      obj.last_microblock_hash, obj.state_root,
                      obj.last_b3_checkpoint);
        }
    };

    explicit FlowMeshProductionStore(DBParams db_params);

    bool ReadMarker(std::optional<Marker>& out, std::string& error);

    /**
     * Validate marker and every production namespace without writing. A
     * missing marker is fresh only when the entry, connection, lock and
     * locked-candidate namespaces are all empty.
     */
    bool CheckForMarket(const uint256& domain,
                        const flowmesh::MarketId& market_id,
                        bool& fresh_out, std::string& error);

    /**
     * Bind a genuinely fresh database to one initial anchored seat set and
     * state root, or strictly reopen an existing v3 database. An old v2
     * marker is rejected; no migration is attempted.
     */
    bool OpenForMarket(const uint256& domain,
                       const flowmesh::MarketId& market_id,
                       const flowmesh::ActiveFnBlsSeatSet& initial_seats,
                       const uint256& initial_state_root,
                       std::string& error);

    /**
     * Re-execute, certify and atomically append the next EXECUTION entry.
     * `next_state_out` is assigned only after the synchronous batch commits.
     */
    bool AppendExecution(
        const flowmesh::ProductionEntryCore& entry,
        const flowmesh::BlsMicroblockCertificate& certificate,
        const flowmesh::ActiveFnBlsSeatSet& active_seats,
        const flowmesh::FlowMeshState& current_state,
        const flowmesh::ProductionAnchorContext& anchor_context,
        const uint256& treasury_owner_commitment,
        const flowmesh::DepositVerifier* deposits,
        flowmesh::FlowMeshState& next_state_out, std::string& error);

    /**
     * Validate and atomically append one outgoing-set EPOCH_HANDOFF. The
     * marker remains on the outgoing epoch, making both sets unable to append,
     * until the exact on-chain checkpoint is marked connected.
     */
    bool AppendHandoff(
        const flowmesh::ProductionEntryCore& handoff,
        const flowmesh::BlsMicroblockCertificate& certificate,
        const flowmesh::ActiveFnBlsSeatSet& outgoing_seats,
        const flowmesh::ActiveFnBlsSeatSet& next_seats,
        const flowmesh::FlowMeshState& current_state,
        const flowmesh::ProductionAnchorContext& anchor_context,
        std::string& error);

    /** Record an ordinary connected type-8 checkpoint and advance only the
     * marker's checkpoint head. */
    bool MarkExecutionCheckpointConnected(
        const modern::FlowMeshCheckpointRecordV1& checkpoint,
        const flowmesh::ActiveFnBlsSeatSet& active_seats,
        const ProductionB3Connection& connection,
        std::string& error);

    /**
     * Persist the handoff checkpoint fact and switch epoch/anchor/set in one
     * synchronous batch. The handoff must be the current log tip and the
     * checkpoint must carry the exact stored certificate and extend the
     * stored B3-checkpoint head. `canonical_tip_height` is the caller's exact
     * active-chain snapshot and must bury `connection` by the FlowMesh anchor
     * depth; an immature publication never enters durable connection state.
     */
    bool MarkHandoffCheckpointConnected(
        const modern::FlowMeshCheckpointRecordV1& checkpoint,
        const flowmesh::ActiveFnBlsSeatSet& outgoing_seats,
        const flowmesh::ActiveFnBlsSeatSet& next_seats,
        const ProductionB3Connection& connection,
        int32_t canonical_tip_height,
        std::string& error);

    /**
     * Return the distinct B3 heights named by durable checkpoint connections.
     * This is available before OpenForMarket so startup can reconcile a reorg
     * before trying to resolve the marker's (possibly rolled-back) committee.
     */
    bool ConnectedB3Heights(std::vector<int32_t>& out, std::string& error);

    /**
     * Atomically erase the first connection not present at the exact recorded
     * B3 height/hash and every checkpoint that depends on it. `canonical_blocks`
     * must contain an observation for every height returned by
     * ConnectedB3Heights; a null hash means that height is above the active tip.
     *
     * An ordinary rollback only rewinds the checkpoint head. A handoff rollback
     * also restores the outgoing epoch/anchor/seat set. If entries were already
     * appended after that handoff, exact rollback is no longer possible and the
     * store remains fail-closed until operator recovery.
     */
    bool ReconcileCheckpointConnections(
        const std::map<int32_t, uint256>& canonical_blocks,
        bool& rolled_back, std::string& error);

    bool ReadEntry(uint64_t sequence,
                   const flowmesh::ActiveFnBlsSeatSet& active_seats,
                   std::optional<StoredProductionEntry>& out,
                   std::string& error);

    /**
     * Return the earliest entry after the last connected type-8 that must be
     * published: sequence-zero market genesis, every handoff, or the first
     * effect- or settlement-bearing execution. Ordinary zero-effect
     * executions after genesis are intentionally skippable in v1.
     */
    bool NextCheckpointCandidate(
        const flowmesh::ActiveFnBlsSeatSet& active_seats,
        std::optional<ProductionCheckpointCandidate>& out,
        std::string& error);

    /**
     * Rebuild from the caller's canonical initial state. Every stored entry,
     * BLS certificate, anchor, fee-bearing execution and epoch checkpoint is
     * revalidated. Output state/hash change only after the complete replay
     * agrees with the authoritative marker.
     */
    bool Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                const ProductionSeatSetSource& seat_sets,
                const flowmesh::ProductionAnchorContext& anchor_context,
                const uint256& treasury_owner_commitment,
                const flowmesh::DepositVerifier* deposits,
                std::string& error);

    /**
     * Atomically retain an exact candidate plus its already-authenticated
     * action evidence and install the permanent `(epoch, sequence)` signing
     * lock. The real store never creates a first lock through hash-only
     * LockOnce; this is the mandatory pre-signing path.
     */
    flowmesh::ProductionLockResult LockCandidate(
        const flowmesh::ProductionEntryCore& entry,
        std::span<const flowmesh::Action> authenticated_evidence);

    /** Return the pending candidate for this exact lock position, if any. */
    bool ReadLockedCandidate(
        const flowmesh::ProductionSignPosition& position,
        std::optional<StoredLockedProductionCandidate>& out,
        std::string& error);

    /**
     * Hash-only compare-and-set remains the generic signing-guard interface,
     * but in this production store it can only confirm a lock already created
     * by LockCandidate. A hash-only first lock fails closed.
     */
    flowmesh::ProductionLockResult LockOnce(
        const flowmesh::ProductionSignPosition& position,
        const uint256& entry_hash) override;

    bool ReadLock(const flowmesh::ProductionSignPosition& position,
                  std::optional<uint256>& out, std::string& error);

private:
    CDBWrapper m_db;
    std::mutex m_mutex;
    bool m_open{false};
    //! Fresh/empty stores are ready after their initial binding is checked.
    //! A reopened nonempty store becomes ready only after full replay.
    bool m_ready{false};
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_PRODUCTION_STORE_H

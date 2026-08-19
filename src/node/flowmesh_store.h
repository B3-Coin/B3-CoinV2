// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_STORE_H
#define B3COIN_NODE_FLOWMESH_STORE_H

#include <dbwrapper.h>
#include <flowmesh/certificate.h>
#include <flowmesh/deposit.h>
#include <flowmesh/recovery.h>
#include <flowmesh/sync.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <mutex>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace node {

//! Canonical commitment to the quorum configuration a log's
//! certificates were verified against (sorted seats + threshold).
//! Static for v1: seat ROTATION persistence is deliberately unsupported
//! until the FN seat lifecycle (owner decision) exists.
uint256 QuorumHash(const std::set<XOnlyPubKey>& seats, uint64_t threshold);

/**
 * Durable FlowMesh certified log: the append-only sequence of finalized
 * (microblock, certificate, evidence) entries, a single marker (format
 * version, domain, quorum commitment, next sequence, last hash), the
 * safety-critical lock journal, and an optional certificate-verified
 * state snapshot. Every append commits the entry and the marker in ONE
 * atomic batch; every stored object decodes STRICTLY (exact
 * consumption, trailing bytes rejected) with bounds enforced before
 * allocation.
 */
class FlowMeshStore
{
public:
    static constexpr int32_t FORMAT_VERSION{2};

    struct Marker {
        int32_t version{FORMAT_VERSION};
        uint256 domain;
        uint256 quorum_hash;
        uint64_t next_sequence{0};
        uint256 last_hash;

        SERIALIZE_METHODS(Marker, obj)
        {
            READWRITE(obj.version, obj.domain, obj.quorum_hash, obj.next_sequence,
                      obj.last_hash);
        }
    };

    explicit FlowMeshStore(DBParams db_params);

    bool ReadMarker(std::optional<Marker>& out, std::string& error);

    //! Bind the store to `domain` under the given quorum configuration.
    bool OpenForDomain(const uint256& domain, const std::set<XOnlyPubKey>& seats,
                       uint64_t threshold, std::string& error);

    //! Append the next finalized entry (sequence and parent must extend
    //! the stored tip exactly).
    [[nodiscard]] bool Append(const flowmesh::CertifiedEntry& entry, std::string& error);

    std::optional<flowmesh::CertifiedEntry> ReadEntry(uint64_t sequence);

    // ---- Safety-critical lock journal -----------------------------------

    /**
     * COMPARE-AND-SET, never overwrite: with no existing lock for
     * `sequence` the hash is durably written; an identical existing
     * lock is idempotent success; a DIFFERENT existing lock is refused
     * (the caller must halt — a conflicting durable safety lock must
     * never be replaced). Holds across restart.
     */
    [[nodiscard]] bool WriteLock(uint64_t sequence, const uint256& microblock_hash);
    [[nodiscard]] bool ClearLocksThrough(uint64_t sequence);
    //! All journaled locks (restart restore). Strictly validates the
    //! full lock namespace and the iterator/database status.
    bool ReadLocks(std::map<uint64_t, uint256>& out, std::string& error);

    // ---- Reconstruction --------------------------------------------------

    /**
     * Deterministic reconstruction from genesis: re-verifies every
     * entry's certificate (against the recorded quorum), parent
     * linkage, admission evidence, execution claims, and — when
     * `anchors` is supplied — B3 anchor canonicality. When
     * `anchors_out` is non-null it receives every anchor the replayed
     * history relies on (with its newest sequence), so a restarted node
     * cannot forget the dependencies needed to detect a later B3 reorg.
     */
    bool Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                const flowmesh::ActionAuthenticator& auth,
                const flowmesh::DepositVerifier* deposits, const std::set<XOnlyPubKey>& seats,
                uint64_t threshold, const flowmesh::AnchorPolicy* anchors, std::string& error,
                std::map<std::pair<int32_t, uint256>, uint64_t>* anchors_out = nullptr);

    //! Persist a snapshot of the state reached AFTER entries
    //! [0, upto_sequence); fail-closed against the certified root.
    bool WriteSnapshot(uint64_t upto_sequence, const flowmesh::FlowMeshState& state,
                       std::string& error);

    /**
     * Reconstruct via the stored snapshot when one is usable. The
     * snapshot is validated against certified history INCLUDING the
     * full skipped prefix's B3 dependencies: the prefix entries'
     * parent-hash chain is walked up to the certificate-verified
     * snapshot-tip entry (authenticating every prefix anchor without
     * re-execution), and each prefix anchor must still be canonical.
     * Any defect discards the snapshot and falls back to full verified
     * replay. `anchors_out` as in Replay (prefix + tail).
     */
    bool ReplayFromBestSnapshot(flowmesh::FlowMeshState& state, uint256& last_hash,
                                const flowmesh::ActionAuthenticator& auth,
                                const flowmesh::DepositVerifier* deposits,
                                const std::set<XOnlyPubKey>& seats, uint64_t threshold,
                                const flowmesh::AnchorPolicy* anchors, std::string& error,
                                std::map<std::pair<int32_t, uint256>, uint64_t>* anchors_out =
                                    nullptr);

private:
    bool ReplayRange(flowmesh::FlowMeshState& state, uint256& last_hash, uint64_t from_sequence,
                     const Marker& marker, const flowmesh::ActionAuthenticator& auth,
                     const flowmesh::DepositVerifier* deposits,
                     const std::set<XOnlyPubKey>& seats, uint64_t threshold,
                     const flowmesh::AnchorPolicy* anchors, std::string& error,
                     std::map<std::pair<int32_t, uint256>, uint64_t>* anchors_out);

    CDBWrapper m_db;
    //! Serializes lock-journal compare-and-set and clearing: the CAS
    //! read-check-write must be atomic with respect to concurrent
    //! callers in this process (LevelDB's LOCK file excludes other
    //! processes).
    std::mutex m_lock_mutex;
};

//! MeshNode -> store bridge: OnCommit succeeds only after the atomic
//! durable append succeeded.
class StoreCommitSink final : public flowmesh::CommitSink
{
public:
    explicit StoreCommitSink(FlowMeshStore& store) : m_store{store} {}

    [[nodiscard]] bool OnCommit(const flowmesh::CertifiedEntry& entry) override
    {
        std::string error;
        if (!m_store.Append(entry, error)) {
            m_last_error = error;
            return false;
        }
        return true;
    }

    const std::optional<std::string>& LastError() const { return m_last_error; }

private:
    FlowMeshStore& m_store;
    std::optional<std::string> m_last_error;
};

//! Durable compare-and-set lock journal over the store.
class StoreLockJournal final : public flowmesh::LockJournal
{
public:
    explicit StoreLockJournal(FlowMeshStore& store) : m_store{store} {}
    [[nodiscard]] bool WriteLock(uint64_t sequence, const uint256& microblock_hash) override
    {
        return m_store.WriteLock(sequence, microblock_hash);
    }
    [[nodiscard]] bool ClearLocksThrough(uint64_t sequence) override
    {
        return m_store.ClearLocksThrough(sequence);
    }

private:
    FlowMeshStore& m_store;
};

//! Serve certified history from the durable log.
class StoreCatchupSource final : public flowmesh::CatchupSource
{
public:
    explicit StoreCatchupSource(FlowMeshStore& store) : m_store{store} {}
    std::optional<flowmesh::CertifiedEntry> EntryAt(uint64_t sequence) const override
    {
        return m_store.ReadEntry(sequence);
    }

private:
    FlowMeshStore& m_store;
};

/**
 * THE production signing-validator lifecycle (Codex item 7): a node
 * with a signing key must not become active until the store-backed
 * startup restored ALL safety-critical state. This factory wires the
 * durable sink, lock journal and catch-up source, reconstructs
 * tip/state (snapshot-accelerated, fully verified), restores the
 * journaled locks AND the committed-anchor dependency set, and only
 * then constructs the node. Callers never pass restored state by hand.
 */
struct ValidatorRuntime {
    std::unique_ptr<StoreCommitSink> sink;
    std::unique_ptr<StoreLockJournal> journal;
    std::unique_ptr<StoreCatchupSource> history;
    std::unique_ptr<flowmesh::MeshNode> mesh_node;
};

bool StartValidator(FlowMeshStore& store, flowmesh::MeshNode::Config config,
                    flowmesh::FlowMeshState genesis, ValidatorRuntime& out, std::string& error);

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_STORE_H

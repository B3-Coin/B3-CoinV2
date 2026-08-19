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
#include <map>
#include <optional>
#include <set>
#include <string>

namespace node {

//! Canonical commitment to the quorum configuration a log's
//! certificates were verified against (sorted seats + threshold).
//! Static for v1: seat ROTATION persistence is deliberately unsupported
//! until the FN seat lifecycle (owner decision) exists — a log opened
//! under a different quorum fails closed instead of re-judging history
//! against the wrong seats.
uint256 QuorumHash(const std::set<XOnlyPubKey>& seats, uint64_t threshold);

/**
 * Durable FlowMesh certified log: the append-only sequence of finalized
 * (microblock, certificate) entries, a single marker (format version,
 * domain, quorum commitment, next sequence, last hash), the
 * safety-critical lock journal, and an optional certificate-verified
 * state snapshot. Every append commits the entry and the marker in ONE
 * atomic batch; every stored object decodes STRICTLY (exact
 * consumption, trailing bytes rejected) with bounds enforced before
 * allocation.
 *
 * The log IS the persistence model: state is reconstructed by
 * deterministic replay that re-executes and re-verifies every entry
 * (roots, parent linkage, certificates against the recorded quorum,
 * and B3 anchor canonicality when an anchor policy is supplied).
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

    //! Marker when present and canonical; nullopt when truly missing.
    //! A present-but-undecodable marker (wrong bytes, wrong version, or
    //! trailing bytes) fails closed via `error`.
    bool ReadMarker(std::optional<Marker>& out, std::string& error);

    //! Bind the store to `domain` under the given quorum configuration:
    //! initialize an empty log's marker, or verify an existing log
    //! matches domain, format AND quorum.
    bool OpenForDomain(const uint256& domain, const std::set<XOnlyPubKey>& seats,
                       uint64_t threshold, std::string& error);

    //! Append the next finalized entry. The entry's sequence must be
    //! exactly the marker's next sequence and its parent hash the
    //! marker's last hash — the log cannot skip, repeat or fork.
    [[nodiscard]] bool Append(const flowmesh::CertifiedEntry& entry, std::string& error);

    std::optional<flowmesh::CertifiedEntry> ReadEntry(uint64_t sequence);

    // ---- Safety-critical lock journal -----------------------------------

    [[nodiscard]] bool WriteLock(uint64_t sequence, const uint256& microblock_hash);
    [[nodiscard]] bool ClearLocksThrough(uint64_t sequence);
    //! All journaled locks (restart restore). Fails closed on any
    //! undecodable journal entry.
    bool ReadLocks(std::map<uint64_t, uint256>& out, std::string& error);

    // ---- Reconstruction --------------------------------------------------

    /**
     * Deterministic reconstruction from genesis: re-execute every stored
     * entry in order, re-verifying certificates against
     * `seats`/`threshold` (which must match the recorded quorum), parent
     * linkage, every execution claim, and — when `anchors` is supplied —
     * that every entry's B3 anchor is still canonical (an orphaned
     * anchor fails the replay: certified history derived from it must
     * not silently remain accepted).
     */
    bool Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                const flowmesh::ActionAuthenticator& auth,
                const flowmesh::DepositVerifier* deposits, const std::set<XOnlyPubKey>& seats,
                uint64_t threshold, const flowmesh::AnchorPolicy* anchors, std::string& error);

    /**
     * Persist a snapshot of the state reached AFTER entries [0,
     * upto_sequence). Fail-closed at write time: the snapshot's root
     * must equal the CERTIFIED resulting_state_root of entry
     * upto_sequence-1 already in this log.
     */
    bool WriteSnapshot(uint64_t upto_sequence, const flowmesh::FlowMeshState& state,
                       std::string& error);

    /**
     * Reconstruct via the stored snapshot when one is usable: strict
     * bounded decode, certificate re-verification of the snapshot-tip
     * entry against the recorded quorum, root equality with that
     * CERTIFIED resulting root, anchor canonicality of the snapshot-tip
     * entry (when `anchors` is supplied), then tail replay under the
     * same rules. Any defect discards the snapshot and falls back to
     * the full verified replay from genesis — a snapshot can never
     * become authoritative merely because its root matches bytes stored
     * beside it.
     */
    bool ReplayFromBestSnapshot(flowmesh::FlowMeshState& state, uint256& last_hash,
                                const flowmesh::ActionAuthenticator& auth,
                                const flowmesh::DepositVerifier* deposits,
                                const std::set<XOnlyPubKey>& seats, uint64_t threshold,
                                const flowmesh::AnchorPolicy* anchors, std::string& error);

private:
    bool ReplayRange(flowmesh::FlowMeshState& state, uint256& last_hash, uint64_t from_sequence,
                     const Marker& marker, const flowmesh::ActionAuthenticator& auth,
                     const flowmesh::DepositVerifier* deposits,
                     const std::set<XOnlyPubKey>& seats, uint64_t threshold,
                     const flowmesh::AnchorPolicy* anchors, std::string& error);

    CDBWrapper m_db;
};

//! MeshNode -> store bridge with the mandatory ordering: OnCommit
//! returns success only after the atomic durable append succeeded, so
//! the node's live tip can never advance past its durable tip.
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

//! Durable lock journal over the store (write-ahead of attestations).
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

//! Serve certified history from the durable log (catch-up after
//! restart, when in-memory history is gone).
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

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_STORE_H

// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_STORE_H
#define B3COIN_NODE_FLOWMESH_STORE_H

#include <dbwrapper.h>
#include <flowmesh/certificate.h>
#include <flowmesh/sync.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace node {

/**
 * Durable FlowMesh certified log: the append-only sequence of finalized
 * (microblock, certificate) entries plus a single marker (format
 * version, domain, next sequence, last microblock hash). Every append
 * commits the entry and the marker in ONE atomic batch, so a crash can
 * never leave the log and the marker disagreeing.
 *
 * The log IS the persistence model: state is reconstructed by
 * deterministic replay that re-executes and re-verifies every entry
 * (roots, parent linkage, certificates). Nothing reconstructable is
 * persisted separately; a state snapshot store is a later optimization,
 * not a correctness need.
 */
class FlowMeshStore
{
public:
    static constexpr int32_t FORMAT_VERSION{1};

    struct Marker {
        int32_t version{FORMAT_VERSION};
        uint256 domain;
        uint64_t next_sequence{0};
        uint256 last_hash;

        SERIALIZE_METHODS(Marker, obj)
        {
            READWRITE(obj.version, obj.domain, obj.next_sequence, obj.last_hash);
        }
    };

    explicit FlowMeshStore(DBParams db_params);

    //! Marker when present and canonical; nullopt when truly missing.
    //! Distinguishes absence from damage: a present-but-undecodable
    //! marker (including trailing bytes) fails closed via `error`.
    bool ReadMarker(std::optional<Marker>& out, std::string& error);

    //! Bind the store to `domain`: initialize an empty log's marker, or
    //! verify an existing log belongs to this domain and format.
    bool OpenForDomain(const uint256& domain, std::string& error);

    //! Append the next finalized entry. The entry's sequence must be
    //! exactly the marker's next sequence and its parent hash the
    //! marker's last hash — the log cannot skip, repeat or fork.
    bool Append(const flowmesh::CertifiedEntry& entry, std::string& error);

    std::optional<flowmesh::CertifiedEntry> ReadEntry(uint64_t sequence);

    /**
     * Deterministic reconstruction: starting from `state` (the genesis
     * FlowMesh state), re-execute every stored entry in order,
     * re-verifying certificates against `seats`/`threshold`, parent
     * linkage, and every execution claim (ExecuteCandidate). On success
     * `state`/`last_hash` hold the reconstructed position. Any
     * violation fails closed with `state` left at the last successfully
     * verified position and a description in `error`.
     */
    bool Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                const flowmesh::ActionAuthenticator& auth,
                const flowmesh::DepositVerifier* deposits, const std::set<XOnlyPubKey>& seats,
                uint64_t threshold, std::string& error);

private:
    CDBWrapper m_db;
};

//! MeshNode -> store bridge: persists every committed entry. An append
//! failure is recorded (and must halt further FlowMesh participation at
//! the caller's level); it can never corrupt the store thanks to the
//! atomic marker+entry batch.
class StoreCommitSink final : public flowmesh::CommitSink
{
public:
    explicit StoreCommitSink(FlowMeshStore& store) : m_store{store} {}

    void OnCommit(const flowmesh::CertifiedEntry& entry) override
    {
        std::string error;
        if (!m_store.Append(entry, error)) m_last_error = error;
    }

    const std::optional<std::string>& LastError() const { return m_last_error; }

private:
    FlowMeshStore& m_store;
    std::optional<std::string> m_last_error;
};

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_STORE_H

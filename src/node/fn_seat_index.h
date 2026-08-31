// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FN_SEAT_INDEX_H
#define B3COIN_NODE_FN_SEAT_INDEX_H

#include <consensus/params.h>
#include <crypto/bls.h>
#include <flowmesh/seat_id.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class CBlockIndex;
class CChain;
namespace node { class BlockManager; }

namespace node {

using FnSeatBlsKey = std::array<unsigned char, bls::PUBKEY_SIZE>;
using FnSeatBlsPop = std::array<unsigned char, bls::SIGNATURE_SIZE>;

/** One live, PoP-verified FN-v2 seat derived from a connected output. */
struct FnSeatRecord {
    COutPoint outpoint;
    flowmesh::SeatId seat_id;
    FnSeatBlsKey bls_pubkey{};
    FnSeatBlsPop proof_of_possession{};
    int created_height{-1};
    uint256 created_block;

    flowmesh::FlowMeshSeatSetMember SetMember() const
    {
        return {seat_id, outpoint, bls_pubkey};
    }

    friend bool operator==(const FnSeatRecord& a, const FnSeatRecord& b)
    {
        return a.outpoint == b.outpoint && a.seat_id == b.seat_id &&
               a.bls_pubkey == b.bls_pubkey &&
               a.proof_of_possession == b.proof_of_possession &&
               a.created_height == b.created_height &&
               a.created_block == b.created_block;
    }
};

/**
 * Net state change of one block. The verifier still evaluates every
 * transaction in strict block order; same-block outputs that are later spent
 * cancel out of this durable delta. Empty A2+ blocks are retained too.
 */
struct FnSeatBlockDelta {
    int height{-1};
    uint256 block_hash;
    std::vector<FnSeatRecord> removed; // original-chain input order
    std::vector<FnSeatRecord> added;   // transaction/vout creation order

    friend bool operator==(const FnSeatBlockDelta& a, const FnSeatBlockDelta& b)
    {
        return a.height == b.height && a.block_hash == b.block_hash &&
               a.removed == b.removed && a.added == b.added;
    }
};

/** Immutable active membership at one exact active-chain anchor hash. */
struct FnSeatSnapshot {
    int anchor_height{-1};
    uint256 anchor_hash;
    std::vector<FnSeatRecord> members; // strict (SeatId,outpoint) order

    bool FlowMeshReady() const { return members.size() >= 4; }

    std::vector<flowmesh::FlowMeshSeatSetMember> SetMembers() const;
    std::optional<uint256> SetHash(const uint256& domain,
                                   const uint256& market_id,
                                   uint64_t epoch) const;
};

/**
 * Rebuildable active FN-seat state. Candidate verification runs against a
 * scratch copy and produces a complete delta; the live maps change only when
 * that already-validated delta is connected.
 */
class FnSeatIndex
{
public:
    std::optional<FnSeatRecord> Get(const COutPoint& outpoint) const;
    std::optional<COutPoint> OwnerOf(const FnSeatBlsKey& key) const;

    bool VerifyBlock(const CBlock& block, int height, const uint256& block_hash,
                     const Consensus::Params& params, FnSeatBlockDelta& out,
                     std::string& error) const;
    bool ConnectBlock(const FnSeatBlockDelta& delta, std::string& error);
    bool DisconnectBlock(int height, const uint256& block_hash,
                         std::string& error);

    std::optional<FnSeatSnapshot> SnapshotAt(const CBlockIndex& anchor) const;
    std::optional<FnSeatSnapshot> AnchoredSnapshot(
        const CChain& chain, const CBlockIndex& anchor, int candidate_height,
        const Consensus::Params& params, std::string& error) const;
    /**
     * Return the consensus-unique epoch-zero bootstrap snapshot: the earliest
     * canonical block at or after `first_height` whose post-block membership
     * contains at least four seats. The chosen block must already satisfy the
     * frozen FlowMesh anchor depth at `candidate_height`.
     *
     * Readiness transitions are indexed as blocks connect, so a long interval
     * of empty seat history does not make first-checkpoint validation linear
     * in chain height. Snapshot reconstruction remains hash-key cached.
     */
    std::optional<FnSeatSnapshot> EarliestFlowMeshReadySnapshot(
        const CChain& chain, int first_height, int candidate_height,
        const Consensus::Params& params, std::string& error) const;
    void Clear();

    int ConnectedHeight() const
    {
        return m_history.empty() ? -1 : m_history.back().height;
    }
    uint256 ConnectedHash() const
    {
        return m_history.empty() ? uint256{} : m_history.back().block_hash;
    }
    size_t Size() const { return m_by_outpoint.size(); }
    const std::map<COutPoint, FnSeatRecord>& All() const { return m_by_outpoint; }
    const std::vector<FnSeatBlockDelta>& History() const { return m_history; }

private:
    std::map<COutPoint, FnSeatRecord> m_by_outpoint;
    std::map<FnSeatBlsKey, COutPoint> m_owner_of_key;
    std::vector<FnSeatBlockDelta> m_history;
    //! Heights where post-block membership crosses the four-seat boundary.
    std::vector<std::pair<int, bool>> m_readiness_transitions;
    mutable std::map<uint256, FnSeatSnapshot> m_snapshots;
};

/** Active-chain driver. Replay begins exactly at A2 and rechecks every PoP. */
class FnSeatTracker
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
    const FnSeatIndex& Index() const { return m_index; }
    //! Immutable active-branch anchor view; full FlowMesh must be active and
    //! candidate_height-anchor_height must satisfy the frozen depth.
    std::optional<FnSeatSnapshot> AnchoredSnapshot(
        const CChain& chain, const CBlockIndex& anchor, int candidate_height,
        const Consensus::Params& params, std::string& error) const;

private:
    bool ApplyBlock(const CBlock& block, const CBlockIndex& index,
                    const Consensus::Params& params);

    FnSeatIndex m_index;
    uint256 m_synced_tip;
    int m_synced_height{-1};
    bool m_dirty{true};
};

} // namespace node

#endif // B3COIN_NODE_FN_SEAT_INDEX_H

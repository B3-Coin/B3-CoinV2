// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_DEV_H
#define B3COIN_NODE_FLOWMESH_DEV_H

#include <flowmesh/auth.h>
#include <flowmesh/recovery.h>
#include <node/flowmesh_anchor.h>
#include <node/flowmesh_store.h>
#include <pubkey.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>

#include <memory>
#include <string>

class ChainstateManager;

namespace node {

/**
 * REGTEST-ONLY development runtime for the FlowMesh validator lifecycle
 * (the -b3flowmeshdev spike). This is NOT a production wiring: it exists
 * to exercise node::StartValidator against a live regtest chainstate —
 * store creation under the datadir, verified restart restore, the
 * chain-backed anchor policy, and clean shutdown. It runs a synthetic
 * single-seat market derived from fixed dev constants, uses the
 * fail-closed deposit verifier, and is never reachable on any other
 * chain (init refuses the flag outside regtest).
 *
 * Nothing here touches block validation, mempool policy, or any
 * consensus rule. No transport exists, so the MeshNode never proposes,
 * attests, or commits — it only starts, reports, and stops.
 *
 * THREADING: MeshNode is not thread-safe; every access to `runtime`
 * must hold `mutex`. In this slice the only reader is the hidden
 * getflowmeshinfo RPC.
 */
struct FlowMeshDevRuntime {
    // Immutable synthetic market identity (reported by getflowmeshinfo).
    uint256 domain;
    uint256 vault_commitment;
    uint256 base_asset;
    uint256 quote_asset;
    uint256 config_id;
    XOnlyPubKey seat;
    uint64_t threshold{0};
    fs::path store_path;

    // Long-lived dependencies the MeshNode config points into.
    std::unique_ptr<FlowMeshStore> store;
    std::unique_ptr<flowmesh::RoundRobinSchedule> schedule;
    std::unique_ptr<flowmesh::SchnorrActionAuthenticator> auth;
    std::unique_ptr<ChainAnchorPolicy> anchors;
    UnavailableDepositVerifier deposits;

    // The production-lifecycle validator runtime (node::StartValidator).
    Mutex mutex;
    ValidatorRuntime runtime GUARDED_BY(mutex);
};

/**
 * Start the dev validator: derive the synthetic single-seat market from
 * fixed dev constants, open (or restore) the durable store at
 * `store_path`, and run the full node::StartValidator lifecycle against
 * the live chainstate's anchor policy (FLOWMESH_ANCHOR_DEPTH). Returns
 * nullptr with `error` set on any failure; a failed startup leaves a
 * fresh store byte-identical (StartValidator's store-neutral contract).
 *
 * The returned runtime must be destroyed before the ChainstateManager
 * it anchors against.
 */
std::unique_ptr<FlowMeshDevRuntime> StartFlowMeshDev(const ChainstateManager& chainman,
                                                     const fs::path& store_path,
                                                     std::string& error);

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_DEV_H

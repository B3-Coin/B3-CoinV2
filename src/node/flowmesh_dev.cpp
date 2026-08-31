// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_dev.h>

#include <dbwrapper.h>
#include <flowmesh/certificate.h>
#include <flowmesh/state.h>
#include <flowmesh/sync.h>
#include <hash.h>
#include <key.h>
#include <logging.h>
#include <modern/policy.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace node {

namespace {

//! Curve bound of the synthetic dev market (matches the unit fixtures).
constexpr size_t DEV_MAX_CURVE_POINTS{8};

//! Deterministic, obviously-synthetic dev constants: tagged hashes under
//! a dev-only domain string. These can never collide with a real chain
//! domain, asset id, or vault commitment (all of which use their own
//! tags and, for assets, a pinned chain domain that fails closed while X
//! is unset).
uint256 DevTag(const std::string& what)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/dev/v1"} << what;
    return h.GetHash();
}

//! The dev seat key: a fixed, deterministic REGTEST-ONLY secret. It
//! signs nothing of value anywhere (this runtime never reaches any
//! network other than an isolated regtest, and the slice has no
//! transport at all).
bool MakeDevSeatKey(CKey& key_out)
{
    const uint256 seed{DevTag("seat-key")};
    key_out.Set(seed.begin(), seed.end(), /*fCompressedIn=*/true);
    return key_out.IsValid();
}

} // namespace

std::unique_ptr<FlowMeshDevRuntime> StartFlowMeshDev(const ChainstateManager& chainman,
                                                     const fs::path& store_path,
                                                     std::string& error)
{
    auto dev{std::make_unique<FlowMeshDevRuntime>()};
    dev->domain = DevTag("domain");
    dev->vault_commitment = DevTag("vault");
    dev->base_asset = DevTag("base-asset");
    dev->quote_asset = modern::NativeAsset();
    dev->config_id = flowmesh::ComputeExecutionConfigId(dev->vault_commitment, dev->base_asset,
                                                        dev->quote_asset,
                                                        DEV_MAX_CURVE_POINTS);
    dev->store_path = store_path;

    CKey seat_key;
    if (!MakeDevSeatKey(seat_key)) {
        error = "flowmesh dev seat key derivation failed";
        return nullptr;
    }
    dev->seat = XOnlyPubKey{seat_key.GetPubKey()};

    // Single-seat committee with the smallest safe threshold under f=0.
    const std::optional<uint64_t> threshold{flowmesh::MinCertificateThreshold(1, 0)};
    if (!threshold) {
        error = "flowmesh dev quorum has no valid threshold";
        return nullptr;
    }
    dev->threshold = *threshold;

    dev->schedule = std::make_unique<flowmesh::RoundRobinSchedule>(
        std::vector<XOnlyPubKey>{dev->seat});
    dev->auth =
        std::make_unique<flowmesh::SchnorrActionAuthenticator>(dev->domain, dev->config_id);
    // The REAL chain-backed anchor policy at the ratified depth: on a
    // fresh regtest chain (< FLOWMESH_ANCHOR_DEPTH blocks) Current()
    // reports the null anchor, exactly as production would.
    dev->anchors = std::make_unique<ChainAnchorPolicy>(chainman, FLOWMESH_ANCHOR_DEPTH);

    try {
        dev->store = std::make_unique<FlowMeshStore>(
            DBParams{.path = store_path, .cache_bytes = size_t{1} << 20});
    } catch (const std::exception& e) {
        error = std::string{"flowmesh dev store open failed: "} + e.what();
        return nullptr;
    }

    flowmesh::MeshNode::Config config;
    config.domain = dev->domain;
    config.seats = std::set<XOnlyPubKey>{dev->seat};
    config.threshold = dev->threshold;
    config.schedule = dev->schedule.get();
    config.auth = dev->auth.get();
    config.deposits = &dev->deposits;
    config.anchors = dev->anchors.get();
    config.seat_key = seat_key;

    LOCK(dev->mutex);
    if (!StartValidator(*dev->store, std::move(config), dev->vault_commitment, dev->base_asset,
                        dev->quote_asset, DEV_MAX_CURVE_POINTS, dev->runtime, error)) {
        return nullptr;
    }
    LogInfo("FlowMesh dev validator started (REGTEST spike): store=%s sequence=%d\n",
            fs::PathToString(store_path), dev->runtime.mesh_node->Sequence());
    return dev;
}

} // namespace node

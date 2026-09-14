// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_SETTLEMENT_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_SETTLEMENT_H

#include <interfaces/chain.h>

#include <optional>
#include <span>
#include <string>

class ChainstateManager;

namespace node {
//! Verify an untrusted type-8 payload against the current local B3 history.
//! This is a read-only preflight, not a transaction broadcast or reservation.
std::optional<interfaces::FlowMeshPendingCheckpoint> VerifyClientCheckpoint(
    ChainstateManager& chainman, const uint256& expected_market,
    std::span<const unsigned char> payload, std::string& error);

//! Verify a type-9 proof and select its exact live inputs from local B3 state.
//! No endpoint-supplied input, amount, asset or destination row is trusted.
//! Normal transaction validation must recheck this snapshot when spending.
std::optional<interfaces::FlowMeshVaultOperation> VerifyClientVaultOperation(
    ChainstateManager& chainman, const uint256& expected_market,
    const std::optional<uint256>& expected_effect,
    std::span<const unsigned char> proof_bytes, std::string& error);
} // namespace node
#endif

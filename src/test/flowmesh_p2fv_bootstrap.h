// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#ifndef B3COIN_TEST_FLOWMESH_P2FV_BOOTSTRAP_H
#define B3COIN_TEST_FLOWMESH_P2FV_BOOTSTRAP_H

class CRPCTable;

// Compiled only with the explicit TEST option, and callable only on regtest.
// Read-only verification of a pinned V1 base; NOT authority to switch protocols,
// execute, sign, migrate stores, settle, or accept a P2FV decision certificate.
void RegisterFlowMeshP2fvBootstrapRPCCommands(CRPCTable& table);

#endif // B3COIN_TEST_FLOWMESH_P2FV_BOOTSTRAP_H

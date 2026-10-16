// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CHAIN_B3D_ADAPTER_H
#define B3_FLOWMESH2_CHAIN_B3D_ADAPTER_H

#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>

#include <cstdint>
#include <memory>

/** The b3d chain adapters (slice R10: interfaces and stubs; plan section 3.3).
 *
 * Both return a ChainAdapter (chain/chain_adapter.h) that reads the facts of
 * a fully validating b3d. Neither exists yet: the node side (ConnectTip and
 * DisconnectTip hooks, the synchronous authority epoch MB1-ADP-07, block
 * scans) lives outside src/flowmesh2/ and is owner-gated. Guard rule 1 keeps
 * every node header out of fm2 code, so a real adapter will receive its
 * chain data through a narrow link that b3d implements; these factories are
 * where that link enters.
 *
 * - B3dTrustedFactsAdapter (DEX-1 regtest mode, D8): "trusted facts", depth
 *   only, no finality, same operator. Create refuses every chain type but
 *   REGTEST with UNSUPPORTED_NETWORK (plan 3.3, Security M4: refused at
 *   start-up; the compile-time refusal comes with the node link). On
 *   REGTEST it is the stub MB1-ADP-06.
 * - B3dFinalizedAdapter: facts usable once their block is M7-finalized AND
 *   at least `min_depth` (30, TRD-4) blocks deep, with anchor proofs. The
 *   stub M7-ADP-01 for every network.
 *
 * Config: min_depth is the confirmation depth a fact's block needs.
 * PROVISIONAL; the trusted-facts depth is MB1-ADP-06's to choose and the
 * finalized depth is 30 (TRD-4).
 */
namespace flowmesh2::chain {

struct B3dAdapterConfig {
    uint32_t min_depth{0};
};

class B3dTrustedFactsAdapter
{
public:
    B3dTrustedFactsAdapter() = delete;
    //! UNSUPPORTED_NETWORK unless REGTEST; then the stub MB1-ADP-06.
    static Result<std::unique_ptr<ChainAdapter>> Create(const NetworkId& network, const B3dAdapterConfig& config);
};

class B3dFinalizedAdapter
{
public:
    B3dFinalizedAdapter() = delete;
    //! Stub M7-ADP-01.
    static Result<std::unique_ptr<ChainAdapter>> Create(const NetworkId& network, const B3dAdapterConfig& config);
};

} // namespace flowmesh2::chain

#endif // B3_FLOWMESH2_CHAIN_B3D_ADAPTER_H

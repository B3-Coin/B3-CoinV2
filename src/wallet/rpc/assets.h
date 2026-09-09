// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_RPC_ASSETS_H
#define BITCOIN_WALLET_RPC_ASSETS_H

#include <bridge/proof.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <consensus/params.h>
#include <modern/policy.h>

#include <cstdint>
#include <optional>
#include <string>

class UniValue;

namespace wallet {

/** Exact positive atomic asset units from a canonical integer JSON number or
 * string. No floating-point conversion or display-decimal scaling. Throws a
 * JSON RPC error for malformed values or amounts outside [1, MAX_MONEY]. */
CAmount AssetUnitsFromValue(const UniValue& value, const std::string& name);

/** Return the spend policy used by an existing wallet-owned asset at the
 * next block height, or throw the same activation error exposed by the asset
 * RPCs. The configured bridge asset follows its independent bridge gate and
 * does not wait for generic colored-asset issuance to activate. */
uint16_t AssetOwnerPolicy(const modern::AssetId& asset,
                          const Consensus::Params& params, int next_height);

/** Build the canonical type-10 withdrawal record selected by the pinned
 * bridge mode. Managed-v1 and decentralized-v1 deliberately share the wallet
 * command and exact burn/output layout; only their consensus payload kind
 * differs. */
std::optional<CMpaRecord> BuildBridgeWithdrawalMpaRecord(
    Consensus::BridgeWithdrawalMode mode, const uint256& registry_id,
    uint32_t burn_output_index, uint64_t raw_amount,
    const bridge::EthAddress& ethereum_recipient);

} // namespace wallet

#endif // BITCOIN_WALLET_RPC_ASSETS_H

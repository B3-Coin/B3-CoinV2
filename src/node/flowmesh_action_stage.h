// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_ACTION_STAGE_H
#define B3COIN_NODE_FLOWMESH_ACTION_STAGE_H

#include <interfaces/chain.h>

#include <string>

namespace node {

/**
 * The single source of a receipt's display stage. Qt maps it and never
 * re-derives it. Only the backend that owns `may_have_been_sent` (the
 * remote client's outbox; false for the local engine, which has none) calls
 * it. A stage is never evidence: "outcome_verified" and "not_executed"
 * follow the receipt's verified outcome, and "certified" its verified
 * inclusion.
 */
inline std::string FlowMeshReceiptStage(const interfaces::FlowMeshActionReceipt& receipt,
                                        const bool may_have_been_sent, const bool retained)
{
    if (receipt.outcome && receipt.outcome_verified && receipt.outcome->status == "not_executed") return "not_executed";
    if (receipt.outcome && receipt.outcome_verified) return "outcome_verified";
    if (receipt.state == "certified_inclusion" && receipt.certificate_verified) return "certified";
    if (receipt.state == "admitted") return "admitted";
    if (receipt.state == "queued") return "queued";
    // A backend reports "rejected" only for a definite refusal of an attempt
    // (an uncertain one is "unknown").
    if (receipt.state == "rejected") return "refused";
    if (may_have_been_sent) return "sent";
    if (retained) return "signed";
    return "unknown";
}

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_ACTION_STAGE_H

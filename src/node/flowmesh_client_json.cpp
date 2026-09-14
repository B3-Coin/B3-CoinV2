// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_client.h>
#include <algorithm>
namespace node {
static UniValue MarketCurvesJson(const std::vector<flowmesh::ClearingEngine::CurveView>& curves)
{
    UniValue out{UniValue::VARR};
    for (const auto& curve : curves) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("account_id", curve.account_id.GetHex());
        item.pushKV("side", curve.side == flowmesh::ClearingEngine::Side::BID ? "bid" : "ask");
        item.pushKV("status", curve.filled_quantity ? "partially-filled" : "open");
        item.pushKV("filled_quantity", curve.filled_quantity);
        item.pushKV("remaining_quantity", curve.remaining_quantity);
        item.pushKV("reserved_amount", curve.reserved_amount);
        UniValue points{UniValue::VARR};
        for (const auto& point : curve.points) {
            UniValue row{UniValue::VOBJ};
            row.pushKV("price", point.price);
            row.pushKV("quantity", point.qty);
            points.push_back(std::move(row));
        }
        item.pushKV("points", std::move(points));
        out.push_back(std::move(item));
    }
    return out;
}
UniValue FlowMeshClientMarketDataJson(const flowmesh::MarketData& data)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("domain", data.domain.GetHex());
    out.pushKV("market_id", data.market_id.GetHex());
    out.pushKV("base_asset_id", data.base_asset_id.GetHex());
    out.pushKV("quote_asset", "B3");
    out.pushKV("execution_config_id", data.execution_config_id.GetHex());
    out.pushKV("matching_model", "uniform-price-curve-auction");
    out.pushKV("quantity_lot_raw", 1);
    out.pushKV("price_tick_raw", 1);
    out.pushKV("unchanged", data.unchanged);
    const auto& source{data.snapshot};
    UniValue snapshot{UniValue::VOBJ};
    snapshot.pushKV("certified", source.certified);
    snapshot.pushKV("next_microblock_sequence", source.next_microblock_sequence);
    snapshot.pushKV("last_microblock_hash", source.last_microblock_hash.GetHex());
    snapshot.pushKV("state_root", source.state_root.GetHex());
    snapshot.pushKV("epoch", source.epoch);
    snapshot.pushKV("anchor_height", source.anchor_height);
    snapshot.pushKV("anchor_hash", source.anchor_hash.GetHex());
    snapshot.pushKV("active_seats", static_cast<uint64_t>(source.active_seats));
    snapshot.pushKV("quorum_required", static_cast<uint64_t>(source.quorum_required));
    snapshot.pushKV("running", source.running);
    snapshot.pushKV("paused", source.paused);
    snapshot.pushKV("chain_reconciling", source.chain_reconciling);
    snapshot.pushKV("observer_only", source.observer_only);
    snapshot.pushKV("pending_handoff", source.pending_handoff);
    snapshot.pushKV("halt", source.halt);
    snapshot.pushKV("error", source.error);
    snapshot.pushKV("pending_actions", static_cast<uint64_t>(source.pending_actions));
    snapshot.pushKV("checkpoint_status_known", false);
    if (source.local_observed_at) snapshot.pushKV("local_observed_at", *source.local_observed_at);
    const auto& diagnostics{source.runtime};
    UniValue runtime{UniValue::VOBJ};
    runtime.pushKV("round", diagnostics.round);
    runtime.pushKV("candidate_count", static_cast<uint64_t>(diagnostics.candidate_count));
    runtime.pushKV("max_verified_attestations", static_cast<uint64_t>(diagnostics.max_verified_attestations));
    runtime.pushKV("active_catchup_requests", static_cast<uint64_t>(diagnostics.active_catchup_requests));
    runtime.pushKV("proposals_missing_evidence", diagnostics.proposals_missing_evidence);
    runtime.pushKV("proposals_rejected_round", diagnostics.proposals_rejected_round);
    runtime.pushKV("proposals_verified_different_round", diagnostics.proposals_verified_different_round);
    runtime.pushKV("proposals_conflicting_lock", diagnostics.proposals_conflicting_lock);
    runtime.pushKV("attestations_without_candidate", diagnostics.attestations_without_candidate);
    if (diagnostics.last_message_observed_at) runtime.pushKV("last_message_observed_at", *diagnostics.last_message_observed_at);
    if (diagnostics.local_locked_candidate) runtime.pushKV("local_locked_candidate", diagnostics.local_locked_candidate->GetHex());
    snapshot.pushKV("runtime", std::move(runtime));
    out.pushKV("snapshot", std::move(snapshot));
    UniValue verification{UniValue::VOBJ};
    verification.pushKV("source", data.remote ? "remote_endpoint" : "local_engine");
    verification.pushKV("endpoint", data.endpoint);
    verification.pushKV("certificate_verified", data.remote ? data.certificate_verified : data.snapshot.certified);
    verification.pushKV("account_state_verified", data.remote ? data.account_state_verified : data.snapshot.certified);
    verification.pushKV("execution_result_verified", data.remote ? data.execution_result_verified : data.snapshot.certified);
    verification.pushKV("b3_checkpoint_confirmed", data.b3_checkpoint_confirmed);
    verification.pushKV("history_endpoint_reported", data.remote && !data.execution_result_verified);
    verification.pushKV("event_gap", data.event_gap);
    verification.pushKV("freshest_network_head_proven", false);
    out.pushKV("verification", std::move(verification));
    if (data.unchanged) return out;

    UniValue liquidity{UniValue::VOBJ};
    liquidity.pushKV("curves", MarketCurvesJson(data.liquidity.curves));
    liquidity.pushKV("total_curves", static_cast<uint64_t>(data.liquidity.total_curves));
    liquidity.pushKV("complete", data.liquidity.complete);
    if (data.liquidity.next_cursor) {
        liquidity.pushKV("next_cursor", std::string{data.liquidity.next_cursor->first == flowmesh::ClearingEngine::Side::BID ? "bid:" : "ask:"} + data.liquidity.next_cursor->second.GetHex());
    }
    out.pushKV("liquidity", std::move(liquidity));
    if (data.account) {
        const auto& source_account{*data.account};
        UniValue account{UniValue::VOBJ};
        account.pushKV("account_id", source_account.account_id.GetHex());
        account.pushKV("next_sequence", source_account.next_sequence);
        account.pushKV("base_available", source_account.base_available);
        account.pushKV("base_reserved", source_account.base_reserved);
        account.pushKV("b3_available_atoms", source_account.b3_available_atoms);
        account.pushKV("b3_reserved_atoms", source_account.b3_reserved_atoms);
        account.pushKV("curves", MarketCurvesJson(source_account.curves));
        out.pushKV("account", std::move(account));
    }
    UniValue history{UniValue::VOBJ};
    history.pushKV("available", data.history.available);
    history.pushKV("scope", "bounded-certified-log-cache");
    history.pushKV("truncated", data.history.truncated);
    if (data.history.oldest_retained_sequence) history.pushKV("oldest_retained_sequence", *data.history.oldest_retained_sequence);
    if (data.history.next_before_sequence) history.pushKV("next_before_sequence", *data.history.next_before_sequence);
    UniValue entries{UniValue::VARR};
    for (const auto& entry : data.history.entries) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("sequence", entry.sequence);
        item.pushKV("microblock_hash", entry.microblock_hash.GetHex());
        item.pushKV("epoch", entry.epoch);
        item.pushKV("anchor_height", entry.anchor_height);
        item.pushKV("anchor_hash", entry.anchor_hash.GetHex());
        item.pushKV("kind", entry.handoff ? "handoff" : "execution");
        item.pushKV("cleared", entry.cleared);
        item.pushKV("price", entry.price);
        item.pushKV("quantity", entry.quantity);
        item.pushKV("notional_atoms", entry.notional_atoms);
        item.pushKV("fee_atoms", entry.fee_atoms);
        const auto fill{data.account ? std::find_if(entry.account_fills.begin(), entry.account_fills.end(),
            [&](const auto& candidate) { return candidate.account_id == data.account->account_id; }) : entry.account_fills.end()};
        const bool found{fill != entry.account_fills.end()};
        item.pushKV("account_fills_known", data.account.has_value() && (entry.account_fills_complete || found));
        item.pushKV("account_bid_fill", found ? fill->bid_quantity : CAmount{0});
        item.pushKV("account_ask_fill", found ? fill->ask_quantity : CAmount{0});
        entries.push_back(std::move(item));
    }
    history.pushKV("entries", std::move(entries));
    out.pushKV("history", std::move(history));
    return out;
}
UniValue FlowMeshClientReceiptJson(const interfaces::FlowMeshActionReceipt& receipt)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("action_id", receipt.action_id.GetHex());
    if (receipt.account_id) out.pushKV("account_id", receipt.account_id->GetHex());
    out.pushKV("accepted", receipt.Admitted());
    out.pushKV("receipt_state", receipt.state);
    out.pushKV("reason", receipt.reason);
    out.pushKV("endpoint", receipt.endpoint);
    out.pushKV("certificate_verified", receipt.certificate_verified);
    out.pushKV("outcome_verified", receipt.outcome_verified);
    if (!receipt.microblock_hash.IsNull()) {
        out.pushKV("microblock_hash", receipt.microblock_hash.GetHex());
        out.pushKV("microblock_sequence", receipt.microblock_sequence);
    }
    return out;
}
UniValue FlowMeshClientStatusJson(const interfaces::FlowMeshClientStatus& status)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("backend", status.backend);
    out.pushKV("engine_enabled", status.engine_enabled);
    out.pushKV("active_endpoint", status.active_endpoint);
    out.pushKV("event_gaps", status.event_gaps);
    out.pushKV("pending_actions", status.pending_actions);
    UniValue endpoints{UniValue::VARR};
    for (const auto& endpoint : status.endpoints) {
        UniValue row{UniValue::VOBJ};
        row.pushKV("url", endpoint.url);
        row.pushKV("available", endpoint.available);
        row.pushKV("last_error", endpoint.last_error);
        endpoints.push_back(std::move(row));
    }
    out.pushKV("endpoints", std::move(endpoints));
    return out;
}
} // namespace node

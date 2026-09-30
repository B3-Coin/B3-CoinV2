// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_client.h>

#include <flowmesh/action_reason.h>
#include <flowmesh/observed_time.h>

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
//! Present only when requested, so every earlier reply is unchanged. Remote
//! ticks are endpoint-reported; the client checked only the tick at its
//! verified head against that head's hash.
static UniValue ClearingTicksJson(const flowmesh::ClearingTickPage& page, const bool remote)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", page.available);
    if (!page.available) out.pushKV("unavailable_reason", page.unavailable_reason);
    out.pushKV("source", remote ? "endpoint_reported" : "local_engine");
    if (page.oldest_retained_sequence) out.pushKV("oldest_retained_sequence", *page.oldest_retained_sequence);
    out.pushKV("gap", page.gap);
    out.pushKV("more", page.more);
    UniValue ticks{UniValue::VARR};
    for (const auto& tick : page.ticks) {
        UniValue row{UniValue::VOBJ};
        row.pushKV("sequence", tick.sequence);
        row.pushKV("microblock_hash", tick.microblock_hash.GetHex());
        row.pushKV("kind", flowmesh::ClearingTickKindName(tick.kind));
        row.pushKV("cleared", tick.cleared);
        row.pushKV("price", tick.price);
        row.pushKV("volume", tick.volume);
        row.pushKV("imbalance", tick.imbalance);
        row.pushKV("imbalance_side", flowmesh::ImbalanceSideName(tick.imbalance_side));
        ticks.push_back(std::move(row));
    }
    out.pushKV("ticks", std::move(ticks));
    return out;
}
//! Present only when requested. Never certified: computed by this node from
//! the verified standing curves it holds, without any pending action.
static UniValue IndicativeJson(const flowmesh::IndicativeClearing& value)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", value.available);
    if (!value.available) out.pushKV("unavailable_reason", value.unavailable_reason);
    out.pushKV("certified", false);
    out.pushKV("excludes_pending_actions", true);
    out.pushKV("basis", value.draft ? "verified_standing_curves_with_draft" : "verified_standing_curves");
    out.pushKV("head", value.head.GetHex());
    if (value.draft) {
        UniValue draft{UniValue::VOBJ};
        draft.pushKV("side", value.draft->side == flowmesh::ClearingEngine::Side::BID ? "bid" : "ask");
        draft.pushKV("price", value.draft->price);
        draft.pushKV("quantity", value.draft->quantity);
        draft.pushKV("account_known", value.draft->account.has_value());
        draft.pushKV("replaces_standing_curve", value.draft_replaces_curve);
        draft.pushKV("assumes_admission", true);
        out.pushKV("draft", std::move(draft));
    }
    out.pushKV("cleared", value.cleared);
    out.pushKV("price", value.price);
    out.pushKV("volume", value.volume);
    out.pushKV("imbalance", value.imbalance);
    out.pushKV("imbalance_side", flowmesh::ImbalanceSideName(value.imbalance_side));
    if (value.best_bid) out.pushKV("best_bid", *value.best_bid);
    if (value.best_ask) out.pushKV("best_ask", *value.best_ask);
    if (value.draft_fill_min) out.pushKV("draft_fill_min", *value.draft_fill_min);
    if (value.draft_fill_max) out.pushKV("draft_fill_max", *value.draft_fill_max);
    return out;
}
//! Present only when requested, so every earlier reply is unchanged.
//! Operator-observed time: when the source applied each entry by its own
//! clock, never a certified execution time. A LIVE row has observed_ms; a
//! DELAYED row only applied_here_ms, an upper bound that is never a trade
//! time; an UNTIMED row neither. delay_reason appears only where the source
//! kept it (this node's own RPC), never in a public reply.
static UniValue ObservedTimesJson(const flowmesh::ObservedTimes& value)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", value.available);
    if (!value.available) out.pushKV("unavailable_reason", value.unavailable_reason);
    out.pushKV("source", value.source);
    out.pushKV("source_kind", "operator_observed");
    out.pushKV("certified", false);
    out.pushKV("clock_suspect", value.clock_suspect);
    if (value.recorded_since_sequence) out.pushKV("recorded_since_sequence", *value.recorded_since_sequence);
    UniValue rows{UniValue::VARR};
    for (const auto& stamp : value.rows) {
        UniValue row{UniValue::VOBJ};
        row.pushKV("sequence", stamp.sequence);
        row.pushKV("microblock_hash", stamp.hash.GetHex());
        row.pushKV("mode", flowmesh::ObservedModeName(stamp.mode));
        if (stamp.mode == flowmesh::ObservedMode::LIVE) row.pushKV("observed_ms", stamp.ms);
        if (stamp.mode == flowmesh::ObservedMode::DELAYED) row.pushKV("applied_here_ms", stamp.ms);
        UniValue flags{UniValue::VARR};
        for (const auto& name : flowmesh::ObservedFlagNames(stamp.flags, /*public_only=*/false)) flags.push_back(name);
        row.pushKV("flags", std::move(flags));
        if (stamp.delay != flowmesh::ObservedDelay::NONE) row.pushKV("delay_reason", flowmesh::ObservedDelayName(stamp.delay));
        rows.push_back(std::move(row));
    }
    out.pushKV("rows", std::move(rows));
    return out;
}
std::string FlowMeshUnsigned128Text(util::Unsigned128 value)
{
    std::string out;
    do {
        out.insert(out.begin(), static_cast<char>('0' + static_cast<unsigned>(value % 10)));
        value /= 10;
    } while (value != 0);
    return out;
}
UniValue FlowMeshLevelGroupingJson(const flowmesh::LevelGrouping& grouping)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("exact", grouping.exact);
    if (grouping.exact) return out;
    out.pushKV("orientation", grouping.inverse ? "inverse" : "canonical");
    out.pushKV("mantissa", grouping.mantissa);
    out.pushKV("exponent", grouping.exponent);
    out.pushKV("decimals", grouping.decimals);
    return out;
}
static UniValue BookLevelsSideJson(const flowmesh::BookLevelsSide& side)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", side.available);
    if (!side.available) out.pushKV("unavailable_reason", side.unavailable_reason);
    out.pushKV("more", side.more);
    if (!side.truncated_reason.empty()) out.pushKV("truncated_reason", side.truncated_reason);
    UniValue levels{UniValue::VARR};
    for (const auto& level : side.levels) {
        UniValue row{UniValue::VOBJ};
        row.pushKV("bucket", FlowMeshUnsigned128Text(level.bucket));
        row.pushKV("zero_price", level.zero_price);
        row.pushKV("low", level.low);
        row.pushKV("high", level.high);
        row.pushKV("size", level.size);
        row.pushKV("cumulative", level.cumulative);
        if (level.notional) row.pushKV("notional", FlowMeshUnsigned128Text(*level.notional));
        row.pushKV("curves", uint64_t{level.curves});
        row.pushKV("own", level.own);
        levels.push_back(std::move(row));
    }
    out.pushKV("levels", std::move(levels));
    return out;
}
//! Present only when requested and computed. Never certified: aggregated by
//! this node from the verified standing curves it holds, without any
//! pending action; public replies never carry it.
UniValue FlowMeshBookLevelsJson(const flowmesh::BookLevels& value)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", value.available);
    if (!value.available) out.pushKV("unavailable_reason", value.unavailable_reason);
    out.pushKV("certified", false);
    out.pushKV("excludes_pending_actions", true);
    out.pushKV("basis", "verified_standing_curves");
    out.pushKV("head", value.head.GetHex());
    out.pushKV("depth", static_cast<uint64_t>(value.depth));
    out.pushKV("grouping", FlowMeshLevelGroupingJson(value.grouping));
    if (!value.available) return out;
    out.pushKV("total_curves", static_cast<uint64_t>(value.total_curves));
    if (value.best_bid) out.pushKV("best_bid", *value.best_bid);
    if (value.best_ask) out.pushKV("best_ask", *value.best_ask);
    out.pushKV("bids", BookLevelsSideJson(value.bids));
    out.pushKV("asks", BookLevelsSideJson(value.asks));
    out.pushKV("standing", IndicativeJson(value.standing));
    return out;
}
UniValue FlowMeshObservedCandlePageJson(const flowmesh::ObservedCandlePage& page)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", page.available);
    if (!page.available) out.pushKV("unavailable_reason", page.unavailable_reason);
    out.pushKV("source", page.source);
    out.pushKV("source_kind", "operator_observed");
    out.pushKV("certified", false);
    out.pushKV("interval", flowmesh::CandleIntervalName(page.interval));
    out.pushKV("interval_ms", flowmesh::CandleIntervalMs(page.interval));
    out.pushKV("from_ms", page.from_ms);
    out.pushKV("before_ms", page.before_ms);
    out.pushKV("more", page.more);
    UniValue candles{UniValue::VARR};
    for (const auto& candle : page.candles) {
        UniValue row{UniValue::VOBJ};
        row.pushKV("open_ms", candle.open_ms);
        row.pushKV("first_sequence", candle.first_sequence);
        row.pushKV("last_sequence", candle.last_sequence);
        row.pushKV("entries", uint64_t{candle.entries});
        row.pushKV("clearings", uint64_t{candle.clearings});
        if (candle.clearings > 0) {
            row.pushKV("open", candle.open);
            row.pushKV("high", candle.high);
            row.pushKV("low", candle.low);
            row.pushKV("close", candle.close);
        }
        row.pushKV("base_volume", FlowMeshUnsigned128Text(candle.base_volume));
        row.pushKV("quote_volume", FlowMeshUnsigned128Text(candle.quote_volume));
        UniValue flags{UniValue::VARR};
        for (const auto& name : flowmesh::ObservedCandleFlagNames(candle.flags)) flags.push_back(name);
        row.pushKV("flags", std::move(flags));
        candles.push_back(std::move(row));
    }
    out.pushKV("candles", std::move(candles));
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
    if (data.ticks) out.pushKV("clearing_ticks", ClearingTicksJson(*data.ticks, data.remote));
    if (data.indicative) out.pushKV("indicative", IndicativeJson(*data.indicative));
    if (data.observed) out.pushKV("observed_time", ObservedTimesJson(*data.observed));
    if (data.levels) out.pushKV("book_levels", FlowMeshBookLevelsJson(*data.levels));
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
UniValue FlowMeshClientReceiptJson(const interfaces::FlowMeshActionReceipt& receipt,
                                   const FlowMeshReceiptJsonScope scope)
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
    if (scope != FlowMeshReceiptJsonScope::PUBLIC_V1 && !receipt.reason_code.empty()) {
        out.pushKV("reason_code", receipt.reason_code);
    }
    if (scope != FlowMeshReceiptJsonScope::LOCAL_RPC) return out;
    if (!receipt.stage.empty()) out.pushKV("stage", receipt.stage);
    if (receipt.outcome && receipt.outcome_verified) out.pushKV("outcome", FlowMeshActionOutcomeJson(*receipt.outcome));
    return out;
}
UniValue FlowMeshActionOutcomeJson(const interfaces::FlowMeshActionOutcome& outcome)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("status", outcome.status);
    out.pushKV("final", outcome.final);
    out.pushKV("reason_code", outcome.reason_code);
    const auto* info{flowmesh::ActionReasonLookup(outcome.reason_code)};
    out.pushKV("reason_text", info ? std::string{info->ui_text} : std::string{"Unrecognised reason; outcome unchanged."});
    out.pushKV("evidence", outcome.evidence);
    out.pushKV("microblock_sequence", outcome.microblock_sequence);
    out.pushKV("microblock_hash", outcome.microblock_hash.GetHex());
    if (outcome.fill) {
        UniValue fill{UniValue::VOBJ};
        fill.pushKV("side", outcome.fill->side);
        fill.pushKV("cleared", outcome.fill->cleared);
        fill.pushKV("superseded_in_entry", outcome.fill->superseded_in_entry);
        fill.pushKV("quantity", outcome.fill->quantity);
        fill.pushKV("price", outcome.fill->price);
        fill.pushKV("quote_atoms", outcome.fill->quote_atoms);
        fill.pushKV("fee_atoms", outcome.fill->fee_atoms);
        fill.pushKV("net_quote_atoms", outcome.fill->net_quote_atoms);
        out.pushKV("fill", std::move(fill));
    }
    if (outcome.withdrawal) {
        UniValue withdrawal{UniValue::VOBJ};
        withdrawal.pushKV("receipt_id", outcome.withdrawal->receipt_id.GetHex());
        withdrawal.pushKV("asset", outcome.withdrawal->asset.GetHex());
        withdrawal.pushKV("amount", outcome.withdrawal->amount);
        out.pushKV("withdrawal", std::move(withdrawal));
    }
    if (outcome.deposit) {
        UniValue deposit{UniValue::VOBJ};
        deposit.pushKV("txid", outcome.deposit->outpoint.hash.GetHex());
        deposit.pushKV("vout", outcome.deposit->outpoint.n);
        deposit.pushKV("asset", outcome.deposit->asset.GetHex());
        deposit.pushKV("amount", outcome.deposit->amount);
        out.pushKV("deposit", std::move(deposit));
    }
    if (outcome.consumed_by) {
        UniValue consumer{UniValue::VOBJ};
        consumer.pushKV("action_id", outcome.consumed_by->action_id.GetHex());
        consumer.pushKV("microblock_sequence", outcome.consumed_by->microblock_sequence);
        consumer.pushKV("microblock_hash", outcome.consumed_by->microblock_hash.GetHex());
        out.pushKV("consumed_by", std::move(consumer));
    }
    return out;
}
UniValue FlowMeshClientStatusJson(const interfaces::FlowMeshClientStatus& status)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("backend", status.backend);
    out.pushKV("engine_enabled", status.engine_enabled);
    out.pushKV("active_endpoint", status.active_endpoint);
    out.pushKV("selected_endpoint", status.selected_endpoint);
    out.pushKV("event_gaps", status.event_gaps);
    out.pushKV("pending_actions", status.pending_actions);
    UniValue endpoints{UniValue::VARR};
    for (const auto& endpoint : status.endpoints) {
        UniValue row{UniValue::VOBJ};
        row.pushKV("url", endpoint.url);
        row.pushKV("available", endpoint.available);
        row.pushKV("last_error", endpoint.last_error);
        row.pushKV("transport_available", endpoint.transport_available);
        row.pushKV("last_attempt_ms", endpoint.last_attempt_ms);
        row.pushKV("retry_after_ms", endpoint.retry_after_ms);
        row.pushKV("consecutive_failures", endpoint.consecutive_failures);
        row.pushKV("readiness", endpoint.readiness);
        row.pushKV("readiness_market", endpoint.readiness_market);
        UniValue not_ready{UniValue::VARR};
        for (const auto& code : endpoint.not_ready) not_ready.push_back(code);
        row.pushKV("not_ready", std::move(not_ready));
        row.pushKV("readiness_observed_ms", endpoint.readiness_observed_ms);
        row.pushKV("live_updates_ms", uint64_t{endpoint.live_updates_ms});
        endpoints.push_back(std::move(row));
    }
    out.pushKV("endpoints", std::move(endpoints));
    return out;
}
} // namespace node

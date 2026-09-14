// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/context.h>
#include <node/flowmesh_service.h>
#include <rpc/server_util.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <univalue.h>

static RPCResult TrafficResult(const std::string& name)
{
    return {RPCResult::Type::OBJ, name, "Local transport counters; socket writes are not peer receipt", {
        {RPCResult::Type::NUM, "queued_bytes", "Currently reserved unsent frame bytes"},
        {RPCResult::Type::NUM, "queued_messages", "Currently queued frames"},
        {RPCResult::Type::NUM, "admitted", "Per-peer queue admissions"},
        {RPCResult::Type::NUM, "rejected", "Per-peer queue refusals"},
        {RPCResult::Type::NUM, "socket_written", "Completed local socket writes"},
        {RPCResult::Type::NUM, "failed", "Admitted frames cancelled or lost before local write completion"},
        {RPCResult::Type::NUM, "received", "Frames received and transport-authenticated, before application verification"},
        {RPCResult::Type::NUM, "ingress_retries", "Attempts to admit a retained received frame to the runtime"},
        {RPCResult::Type::NUM, "sent_bytes", "Local socket payload bytes written"},
        {RPCResult::Type::NUM, "received_bytes", "Local socket payload bytes read"},
        {RPCResult::Type::NUM, "max_pass_work_us", "Largest observed non-preemptible channel processing pass, local microseconds"},
        {RPCResult::Type::NUM, "max_pass_bytes", "Largest observed channel processing pass, socket bytes"},
        {RPCResult::Type::NUM, "max_pass_operations", "Largest observed channel processing pass, completed or retried frame operations"},
    }};
}

static UniValue TrafficJSON(const node::FlowMeshNetTraffic& traffic)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("queued_bytes", uint64_t{traffic.queued_bytes});
    out.pushKV("queued_messages", uint64_t{traffic.queued_messages});
    out.pushKV("admitted", traffic.admitted);
    out.pushKV("rejected", traffic.rejected);
    out.pushKV("socket_written", traffic.socket_written);
    out.pushKV("failed", traffic.failed);
    out.pushKV("received", traffic.received);
    out.pushKV("ingress_retries", traffic.ingress_retries);
    out.pushKV("sent_bytes", traffic.sent_bytes);
    out.pushKV("received_bytes", traffic.received_bytes);
    out.pushKV("max_pass_work_us", traffic.max_pass_work_us);
    out.pushKV("max_pass_bytes", traffic.max_pass_bytes);
    out.pushKV("max_pass_operations", traffic.max_pass_operations);
    return out;
}

static RPCHelpMan getflowmeshnetworkinfo()
{
    return RPCHelpMan{
        "getflowmeshnetworkinfo",
        "Read independent FlowMesh transport status without a wallet. Connected or authenticated is NOT proof of FN eligibility, signing, quorum, or execution. Only public operator identities are returned; no private keys are exposed.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Local transport observation", {
            {RPCResult::Type::STR, "mode", "legacy, dual or independent"},
            {RPCResult::Type::BOOL, "legacy_enabled", "B3-carried FlowMesh transport enabled"},
            {RPCResult::Type::BOOL, "running", "Independent network worker running"},
            {RPCResult::Type::BOOL, "listening", "Independent listener open"},
            {RPCResult::Type::STR, "operator_pubkey", "Public network identity, not an FN voting key"},
            {RPCResult::Type::STR, "bind_address", "Independent bind endpoint"},
            {RPCResult::Type::STR, "error", "Local network error, if any"},
            {RPCResult::Type::BOOL, "connection_is_quorum_proof", "Always false"},
            {RPCResult::Type::NUM, "pending_ingress_bytes", "Bounded decoded messages waiting for runtime admission"},
            {RPCResult::Type::NUM, "outbox_queued_bytes", "Application bytes waiting in the independent outbox"},
            {RPCResult::Type::NUM, "ingress_retries", "Runtime admission retries, not additional signatures"},
            {RPCResult::Type::NUM, "ingress_discarded_messages", "Pending messages discarded after disconnect or retry expiry"},
            {RPCResult::Type::NUM, "egress_rejected_messages", "Outgoing messages refused by transport admission"},
            {RPCResult::Type::STR, "last_ingress_error", "Most recent pending-ingress failure"},
            {RPCResult::Type::STR, "last_egress_error", "Most recent outgoing admission failure"},
            {RPCResult::Type::NUM, "notification_refused", "Completion events refused by the bounded runtime event queue"},
            {RPCResult::Type::NUM, "trace_events_dropped", "Process-lifetime BENCH debug events omitted after the 32 MiB transport trace budget; not dropped network messages"},
            {RPCResult::Type::NUM, "receive_idle_timeouts", "Connections closed after no socket-byte progress"},
            {RPCResult::Type::NUM, "receive_frame_timeouts", "Incomplete frames exceeding the whole-frame deadline"},
            {RPCResult::Type::STR, "last_disconnect_reason", "Most recent explicit connection-close reason"},
            TrafficResult("critical"),
            TrafficResult("action"),
            TrafficResult("bulk"),
            {RPCResult::Type::ARR, "peers", "Bounded independent logical peers", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::NUM, "id", "Connection-local independent peer id"},
                    {RPCResult::Type::STR, "address", "Observed endpoint"},
                    {RPCResult::Type::STR, "operator_pubkey", "Authenticated public network identity"},
                    {RPCResult::Type::STR, "role", "Advertised transport role; not eligibility"},
                    {RPCResult::Type::BOOL, "inbound", "Peer initiated connection"},
                    {RPCResult::Type::BOOL, "live", "Critical channel connected (compatibility name)"},
                    {RPCResult::Type::BOOL, "actions", "Independent action channel connected"},
                    {RPCResult::Type::BOOL, "bulk", "History channel connected"},
                    {RPCResult::Type::BOOL, "authenticated", "Challenge authentication completed"},
                    {RPCResult::Type::NUM, "queued_bytes", "Local unsent bytes"},
                    {RPCResult::Type::NUM, "sent_messages", "Completed local socket writes, not peer receipt"},
                    {RPCResult::Type::NUM, "received_messages", "Transport-authenticated frames, before application verification"},
                    {RPCResult::Type::NUM, "dropped_messages", "Messages dropped by transport limits"},
                    {RPCResult::Type::NUM, "pending_ingress_bytes", "Decoded bytes retained for runtime admission"},
                    {RPCResult::Type::NUM, "ingress_retries", "Runtime admission retries for this peer"},
                    {RPCResult::Type::STR, "last_ingress_retry", "Most recent runtime admission retry reason"},
                    TrafficResult("critical_traffic"),
                    TrafficResult("action_traffic"),
                    TrafficResult("bulk_traffic"),
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getflowmeshnetworkinfo", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto& service{EnsureAnyNodeContext(request.context).flowmesh};
            if (!service) throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlowMesh service is not available yet");
            const auto snapshot{service->NetworkSnapshot()};
            UniValue out{UniValue::VOBJ};
            out.pushKV("mode", service->TransportMode());
            out.pushKV("legacy_enabled", service->LegacyTransportEnabled());
            out.pushKV("running", snapshot.running);
            out.pushKV("listening", snapshot.listening);
            out.pushKV("operator_pubkey", snapshot.operator_pubkey);
            out.pushKV("bind_address", snapshot.bind_address);
            out.pushKV("error", snapshot.error);
            out.pushKV("connection_is_quorum_proof", false);
            out.pushKV("pending_ingress_bytes", uint64_t{snapshot.pending_ingress_bytes});
            out.pushKV("outbox_queued_bytes", uint64_t{snapshot.outbox_queued_bytes});
            out.pushKV("ingress_retries", snapshot.ingress_retries);
            out.pushKV("ingress_discarded_messages", snapshot.ingress_discarded_messages);
            out.pushKV("egress_rejected_messages", snapshot.egress_rejected_messages);
            out.pushKV("last_ingress_error", snapshot.last_ingress_error);
            out.pushKV("last_egress_error", snapshot.last_egress_error);
            out.pushKV("notification_refused", snapshot.notification_refused);
            out.pushKV("trace_events_dropped", snapshot.trace_events_dropped);
            out.pushKV("receive_idle_timeouts", snapshot.receive_idle_timeouts);
            out.pushKV("receive_frame_timeouts", snapshot.receive_frame_timeouts);
            out.pushKV("last_disconnect_reason", snapshot.last_disconnect_reason);
            out.pushKV("critical", TrafficJSON(snapshot.traffic[0]));
            out.pushKV("action", TrafficJSON(snapshot.traffic[1]));
            out.pushKV("bulk", TrafficJSON(snapshot.traffic[2]));
            UniValue peers{UniValue::VARR};
            for (const auto& peer : snapshot.peers) {
                UniValue item{UniValue::VOBJ};
                item.pushKV("id", peer.id);
                item.pushKV("address", peer.address);
                item.pushKV("operator_pubkey", peer.operator_pubkey);
                item.pushKV("role", peer.role);
                item.pushKV("inbound", peer.inbound);
                item.pushKV("live", peer.live);
                item.pushKV("actions", peer.actions);
                item.pushKV("bulk", peer.bulk);
                item.pushKV("authenticated", peer.authenticated);
                item.pushKV("queued_bytes", uint64_t{peer.queued_bytes});
                item.pushKV("sent_messages", peer.sent_messages);
                item.pushKV("received_messages", peer.received_messages);
                item.pushKV("dropped_messages", peer.dropped_messages);
                item.pushKV("pending_ingress_bytes", uint64_t{peer.pending_ingress_bytes});
                item.pushKV("ingress_retries", peer.ingress_retries);
                item.pushKV("last_ingress_retry", peer.last_ingress_retry);
                item.pushKV("critical_traffic", TrafficJSON(peer.traffic[0]));
                item.pushKV("action_traffic", TrafficJSON(peer.traffic[1]));
                item.pushKV("bulk_traffic", TrafficJSON(peer.traffic[2]));
                peers.push_back(std::move(item));
            }
            out.pushKV("peers", std::move(peers));
            return out;
        }};
}

static RPCHelpMan getflowmeshdeliveryinfo()
{
    return RPCHelpMan{
        "getflowmeshdeliveryinfo",
        "Read bounded local runtime delivery diagnostics without a wallet. A queue admission or socket write is not peer receipt, certification or durable application. Counts reset when the process restarts. Remote client support is not implied.\n",
        {{"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Limit to one market; otherwise all installed markets"}},
        RPCResult{RPCResult::Type::OBJ, "", "Local observations", {
            {RPCResult::Type::STR, "mode", "Local transport mode"},
            {RPCResult::Type::BOOL, "remote_receipt_proven", "Always false"},
            {RPCResult::Type::ARR, "markets", "Runtime snapshots", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "market_id", "Market identity"},
                    {RPCResult::Type::NUM, "sampled_monotonic_us", "Local monotonic clock sampled while taking this market snapshot; not synchronized across hosts"},
                    {RPCResult::Type::NUM, "last_event_id", "Latest local per-market event cursor; resets with runtime history"},
                    {RPCResult::Type::NUM, "events_dropped", "Older events evicted from the bounded recent-event ring"},
                    {RPCResult::Type::NUM, "trace_bytes", "Bytes charged to the opt-in per-market BENCH trace budget"},
                    {RPCResult::Type::NUM, "trace_events_dropped", "BENCH events omitted at its byte cap; not dropped network messages"},
                    {RPCResult::Type::NUM, "trace_global_bytes", "Node-global process-lifetime BENCH runtime trace bytes, repeated in each row"},
                    {RPCResult::Type::NUM, "trace_global_events_dropped", "Node-global process-lifetime BENCH runtime trace cap refusals, repeated in each row"},
                    {RPCResult::Type::NUM, "created", "Outgoing runtime message creation events"},
                    {RPCResult::Type::NUM, "admitted", "Local service and transport admissions"},
                    {RPCResult::Type::NUM, "refused", "Local refusal events"},
                    {RPCResult::Type::NUM, "retried", "Exact-object retry attempts"},
                    {RPCResult::Type::NUM, "socket_written", "Completed local writes"},
                    {RPCResult::Type::NUM, "verified", "Runtime cryptographic/application verification events"},
                    {RPCResult::Type::NUM, "certificate_formed", "Locally assembled certificates"},
                    {RPCResult::Type::NUM, "durably_applied", "Certified entries applied to the durable local store"},
                    {RPCResult::Type::NUM, "catchup_started", "Local catch-up starts"},
                    {RPCResult::Type::NUM, "catchup_completed", "Applied catch-up pages with tail slack; not proof of the network tip or signing readiness"},
                    {RPCResult::Type::NUM, "catchup_timeouts", "Expired local catch-up requests; subsequent requests may use smaller pages"},
                    {RPCResult::Type::NUM, "catchup_replies_refused", "Unmatched, late, mismatched or invalid catch-up replies; reasons are recorded in events"},
                    {RPCResult::Type::NUM, "catchup_partial_pages", "Catch-up pages not fully applied; never a completion or signing-readiness claim"},
                    {RPCResult::Type::NUM, "completion_timeouts", "Missing local completion feedback deadlines"},
                    {RPCResult::Type::NUM, "retention_refused", "Exact-object retention capacity refusals"},
                    {RPCResult::Type::NUM, "cancelled", "Retired delivery objects"},
                    {RPCResult::Type::NUM, "event_queue_overflows", "Node-global completion feedback queue refusals, repeated in each market row"},
                    {RPCResult::Type::NUM, "pending_objects", "Currently retained outgoing objects"},
                    {RPCResult::Type::NUM, "pending_bytes", "Bytes charged to retained outgoing objects"},
                    {RPCResult::Type::NUM, "receive_deferred", "Critical messages deferred after admission because B3 reconciliation started"},
                    {RPCResult::Type::NUM, "receive_refused", "Deferred ingress capacity or expiry refusals"},
                    {RPCResult::Type::NUM, "deferred_objects", "Currently retained critical incoming objects"},
                    {RPCResult::Type::NUM, "deferred_bytes", "Bytes charged to retained critical incoming objects"},
                    {RPCResult::Type::STR, "current_reason", "Most recent delivery/refusal/retry reason"},
                    {RPCResult::Type::STR_HEX, "last_target_hash", "Last recorded target object"},
                    {RPCResult::Type::ARR, "events", "At most 128 recent events, not a complete audit journal", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::NUM, "monotonic_us", "Microseconds on this process's monotonic clock"},
                            {RPCResult::Type::NUM, "event_id", "Local per-market event cursor for gap detection"},
                            {RPCResult::Type::NUM, "epoch", "Observed runtime epoch"},
                            {RPCResult::Type::STR_HEX, "seat_set_hash", "Observed runtime seat-set hash"},
                            {RPCResult::Type::NUM, "round", /*optional=*/true, "Observed proposer round, only when known"},
                            {RPCResult::Type::NUM, "seat_index", /*optional=*/true, "Verified or locally signing seat index when known"},
                            {RPCResult::Type::NUM, "delivery_id", /*optional=*/true, "Local relay attempt identifier, not a wire sequence or signature"},
                            {RPCResult::Type::STR_HEX, "related_object_id", /*optional=*/true, "Related action or candidate for correlation"},
                            {RPCResult::Type::STR_HEX, "bls_key_hash", /*optional=*/true, "SHA256d of the compressed public BLS key"},
                            {RPCResult::Type::STR_HEX, "signature_hash", /*optional=*/true, "SHA256d of the exact public BLS signature"},
                            {RPCResult::Type::STR_HEX, "wire_hash", /*optional=*/true, "BENCH-only SHA256d of exact encoded inner wire bytes"},
                            {RPCResult::Type::STR, "stage", "Observed stage, not an inferred remote outcome"},
                            {RPCResult::Type::STR, "kind", "Application command"},
                            {RPCResult::Type::NUM, "sequence", "Relevant microblock sequence when known"},
                            {RPCResult::Type::STR_HEX, "object_id", "Relevant object hash when known"},
                            {RPCResult::Type::NUM, "peer", /*optional=*/true, "Connection-local peer identifier when applicable"},
                            {RPCResult::Type::STR, "reason", "Precise event reason when applicable"},
                        }},
                    }},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getflowmeshdeliveryinfo", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto& service{EnsureAnyNodeContext(request.context).flowmesh};
            if (!service) throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlowMesh service is not available yet");
            std::optional<flowmesh::MarketId> market;
            if (!request.params[0].isNull()) market = ParseHashV(request.params[0], "market_id");
            UniValue out{UniValue::VOBJ};
            out.pushKV("mode", service->TransportMode());
            out.pushKV("remote_receipt_proven", false);
            UniValue markets{UniValue::VARR};
            for (const auto& snapshot : service->DeliverySnapshots(market)) {
                UniValue item{UniValue::VOBJ};
                item.pushKV("market_id", snapshot.market_id.GetHex());
                item.pushKV("sampled_monotonic_us", snapshot.sampled_monotonic_us);
                item.pushKV("last_event_id", snapshot.last_event_id);
                item.pushKV("events_dropped", snapshot.events_dropped);
                item.pushKV("trace_bytes", snapshot.trace_bytes);
                item.pushKV("trace_events_dropped", snapshot.trace_events_dropped);
                item.pushKV("trace_global_bytes", snapshot.trace_global_bytes);
                item.pushKV("trace_global_events_dropped", snapshot.trace_global_events_dropped);
                item.pushKV("created", snapshot.created);
                item.pushKV("admitted", snapshot.admitted);
                item.pushKV("refused", snapshot.refused);
                item.pushKV("retried", snapshot.retried);
                item.pushKV("socket_written", snapshot.socket_written);
                item.pushKV("verified", snapshot.verified);
                item.pushKV("certificate_formed", snapshot.certificate_formed);
                item.pushKV("durably_applied", snapshot.durably_applied);
                item.pushKV("catchup_started", snapshot.catchup_started);
                item.pushKV("catchup_completed", snapshot.catchup_completed);
                item.pushKV("catchup_timeouts", snapshot.catchup_timeouts);
                item.pushKV("catchup_replies_refused", snapshot.catchup_replies_refused);
                item.pushKV("catchup_partial_pages", snapshot.catchup_partial_pages);
                item.pushKV("completion_timeouts", snapshot.completion_timeouts);
                item.pushKV("retention_refused", snapshot.retention_refused);
                item.pushKV("cancelled", snapshot.cancelled);
                item.pushKV("event_queue_overflows", snapshot.event_queue_overflows);
                item.pushKV("pending_objects", uint64_t{snapshot.pending_objects});
                item.pushKV("pending_bytes", uint64_t{snapshot.pending_bytes});
                item.pushKV("receive_deferred", snapshot.receive_deferred);
                item.pushKV("receive_refused", snapshot.receive_refused);
                item.pushKV("deferred_objects", uint64_t{snapshot.deferred_objects});
                item.pushKV("deferred_bytes", uint64_t{snapshot.deferred_bytes});
                item.pushKV("current_reason", snapshot.current_reason);
                item.pushKV("last_target_hash", snapshot.last_target_hash.GetHex());
                UniValue events{UniValue::VARR};
                for (const auto& event : snapshot.events) {
                    UniValue row{UniValue::VOBJ};
                    row.pushKV("monotonic_us", event.monotonic_us);
                    row.pushKV("event_id", event.event_id);
                    row.pushKV("epoch", event.epoch);
                    row.pushKV("seat_set_hash", event.seat_set_hash.GetHex());
                    if (event.round) row.pushKV("round", *event.round);
                    if (event.seat_index) row.pushKV("seat_index", *event.seat_index);
                    if (event.delivery_id) row.pushKV("delivery_id", *event.delivery_id);
                    if (event.related_object_id) row.pushKV("related_object_id", event.related_object_id->GetHex());
                    if (event.bls_key_hash) row.pushKV("bls_key_hash", event.bls_key_hash->GetHex());
                    if (event.signature_hash) row.pushKV("signature_hash", event.signature_hash->GetHex());
                    if (event.wire_hash) row.pushKV("wire_hash", event.wire_hash->GetHex());
                    row.pushKV("stage", event.stage);
                    row.pushKV("kind", std::string{flowmesh::WireCommand(event.kind)});
                    row.pushKV("sequence", event.sequence);
                    row.pushKV("object_id", event.object_id.GetHex());
                    if (event.peer) row.pushKV("peer", *event.peer);
                    row.pushKV("reason", event.reason);
                    events.push_back(std::move(row));
                }
                item.pushKV("events", std::move(events));
                markets.push_back(std::move(item));
            }
            out.pushKV("markets", std::move(markets));
            return out;
        }};
}

void RegisterFlowMeshNetworkRPCCommands(CRPCTable& table)
{
    static const CRPCCommand commands[]{
        {"flowmesh", &getflowmeshnetworkinfo},
        {"flowmesh", &getflowmeshdeliveryinfo},
    };
    for (const auto& command : commands) table.appendCommand(command.name, &command);
}
